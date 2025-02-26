/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/ParallelMarking.h"

#include "gc/GCLock.h"
#include "gc/ParallelWork.h"
#include "vm/GeckoProfiler.h"
#include "vm/HelperThreadState.h"
#include "vm/Runtime.h"

using namespace js;
using namespace js::gc;

using mozilla::Maybe;
using mozilla::TimeDuration;
using mozilla::TimeStamp;

class AutoAddTimeDuration {
  TimeStamp start;
  TimeDuration& result;

 public:
  explicit AutoAddTimeDuration(TimeDuration& result)
      : start(TimeStamp::Now()), result(result) {}
  ~AutoAddTimeDuration() { result += TimeSince(start); }
};

ParallelMarker::ParallelMarker(GCRuntime* gc) : gc(gc) {}

size_t ParallelMarker::workerCount() const { return gc->markers.length(); }

bool ParallelMarker::mark(SliceBudget& sliceBudget) {
#ifdef DEBUG
  {
    AutoLockHelperThreadState lock;
    MOZ_ASSERT(workerCount() <= HelperThreadState().maxGCParallelThreads(lock));

    // TODO: Even if the thread limits checked above are correct, there may not
    // be enough threads available to start our mark tasks immediately due to
    // other runtimes in the same process running GC.
  }
#endif

  if (markOneColor(MarkColor::Black, sliceBudget) == NotFinished) {
    return false;
  }
  MOZ_ASSERT(!hasWork(MarkColor::Black));

  if (markOneColor(MarkColor::Gray, sliceBudget) == NotFinished) {
    return false;
  }
  MOZ_ASSERT(!hasWork(MarkColor::Gray));

  // Handle any delayed marking, which is not performed in parallel.
  if (gc->hasDelayedMarking()) {
    gc->markAllDelayedChildren(ReportMarkTime);
  }

  return true;
}

bool ParallelMarker::markOneColor(MarkColor color, SliceBudget& sliceBudget) {
  // Run a marking slice and return whether the stack is now empty.

  if (!hasWork(color)) {
    return true;
  }

  gcstats::AutoPhase ap(gc->stats(), gcstats::PhaseKind::PARALLEL_MARK);

  MOZ_ASSERT(workerCount() <= MaxParallelWorkers);
  mozilla::Maybe<ParallelMarkTask> tasks[MaxParallelWorkers];

  for (size_t i = 0; i < workerCount(); i++) {
    GCMarker* marker = gc->markers[i].get();
    tasks[i].emplace(this, marker, color, sliceBudget);

    // Attempt to populate empty mark stacks.
    //
    // TODO: When supporting more than two markers we will need a more
    // sophisticated approach.
    if (!marker->hasEntries(color) && gc->marker().canDonateWork()) {
      GCMarker::moveWork(marker, &gc->marker());
    }
  }

  {
    AutoLockGC lock(gc);

    activeTasks = 0;
    for (size_t i = 0; i < workerCount(); i++) {
      ParallelMarkTask& task = *tasks[i];
      if (task.hasWork()) {
        incActiveTasks(&task, lock);
      }
    }
  }

  {
    AutoLockHelperThreadState lock;

    for (size_t i = 0; i < workerCount(); i++) {
      gc->startTask(*tasks[i], lock);
    }

    for (size_t i = 0; i < workerCount(); i++) {
      gc->joinTask(*tasks[i], lock);
    }
  }

#ifdef DEBUG
  {
    AutoLockGC lock(gc);
    MOZ_ASSERT(waitingTasks.ref().isEmpty());
    MOZ_ASSERT(waitingTaskCount == 0);
    MOZ_ASSERT(activeTasks == 0);
  }
#endif

  return !hasWork(color);
}

bool ParallelMarker::hasWork(MarkColor color) const {
  for (const auto& marker : gc->markers) {
    if (marker->hasEntries(color)) {
      return true;
    }
  }

  return false;
}

ParallelMarkTask::ParallelMarkTask(ParallelMarker* pm, GCMarker* marker,
                                   MarkColor color, const SliceBudget& budget)
    : GCParallelTask(pm->gc, gcstats::PhaseKind::PARALLEL_MARK, GCUse::Marking),
      pm(pm),
      marker(marker),
      color(*marker, color),
      budget(budget) {
  marker->enterParallelMarkingMode(pm);
}

ParallelMarkTask::~ParallelMarkTask() {
  MOZ_ASSERT(!isWaiting.refNoCheck());
  marker->leaveParallelMarkingMode();
}

bool ParallelMarkTask::hasWork() const {
  return marker->hasEntries(marker->markColor());
}

void ParallelMarkTask::recordDuration() {
  gc->stats().recordParallelPhase(gcstats::PhaseKind::PARALLEL_MARK,
                                  duration());
  gc->stats().recordParallelPhase(gcstats::PhaseKind::PARALLEL_MARK_MARK,
                                  markTime.ref());
  gc->stats().recordParallelPhase(gcstats::PhaseKind::PARALLEL_MARK_WAIT,
                                  waitTime.ref());
}

void ParallelMarkTask::run(AutoLockHelperThreadState& lock) {
  AutoUnlockHelperThreadState unlock(lock);

  {
    AutoLockGC gcLock(pm->gc);

    markOrRequestWork(gcLock);

    MOZ_ASSERT(!isWaiting);
    if (hasWork()) {
      pm->decActiveTasks(this, gcLock);
    }
  }
}

void ParallelMarkTask::markOrRequestWork(AutoLockGC& lock) {
  for (;;) {
    if (hasWork() && !tryMarking(lock)) {
      return;
    }

    while (!hasWork()) {
      if (!requestWork(lock)) {
        return;
      }
    }
  }
}

bool ParallelMarkTask::tryMarking(AutoLockGC& lock) {
  MOZ_ASSERT(hasWork());
  MOZ_ASSERT(marker->isParallelMarking());

  // Mark until budget exceeded or we run out of work.
  {
    AutoUnlockGC unlock(lock);

    AutoAddTimeDuration time(markTime.ref());
    marker->markCurrentColorInParallel(budget);
  }

  if (!hasWork()) {
    pm->decActiveTasks(this, lock);
  }

  return !budget.isOverBudget();
}

bool ParallelMarkTask::requestWork(AutoLockGC& lock) {
  MOZ_ASSERT(!hasWork());

  if (!pm->hasActiveTasks(lock)) {
    return false;  // All other tasks are empty. We're finished.
  }

  budget.stepAndForceCheck();
  if (budget.isOverBudget()) {
    return false;  // Over budget or interrupted.
  }

  // Add ourselves to the waiting list and wait for another task to give us
  // work. The task with work calls ParallelMarker::donateWorkFrom.
  waitUntilResumed(lock);

  if (hasWork()) {
    pm->incActiveTasks(this, lock);
  }

  return true;
}

void ParallelMarkTask::waitUntilResumed(AutoLockGC& lock) {
  GeckoProfilerRuntime& profiler = gc->rt->geckoProfiler();
  if (profiler.enabled()) {
    profiler.markEvent("Parallel marking wait start", "");
  }

  pm->addTaskToWaitingList(this, lock);

  // Set isWaiting flag and wait for another thread to clear it and resume us.
  MOZ_ASSERT(!isWaiting);
  isWaiting = true;

  AutoAddTimeDuration time(waitTime.ref());

  do {
    MOZ_ASSERT(pm->hasActiveTasks(lock));
    resumed.wait(lock.guard());
  } while (isWaiting);

  MOZ_ASSERT(!pm->isTaskInWaitingList(this, lock));

  if (profiler.enabled()) {
    profiler.markEvent("Parallel marking wait end", "");
  }
}

void ParallelMarkTask::resume(const AutoLockGC& lock) {
  MOZ_ASSERT(isWaiting);

  isWaiting = false;
  resumed.notify_all();
}

void ParallelMarker::addTaskToWaitingList(ParallelMarkTask* task,
                                          const AutoLockGC& lock) {
  MOZ_ASSERT(!task->hasWork());
  MOZ_ASSERT(hasActiveTasks(lock));
  MOZ_ASSERT(!isTaskInWaitingList(task, lock));
  MOZ_ASSERT(waitingTaskCount < workerCount() - 1);

  waitingTasks.ref().pushBack(task);
  waitingTaskCount++;
}

#ifdef DEBUG
bool ParallelMarker::isTaskInWaitingList(const ParallelMarkTask* task,
                                         const AutoLockGC& lock) const {
  // The const cast is because ElementProbablyInList is not const.
  return const_cast<ParallelMarkTaskList&>(waitingTasks.ref())
      .ElementProbablyInList(const_cast<ParallelMarkTask*>(task));
}
#endif

void ParallelMarker::incActiveTasks(ParallelMarkTask* task,
                                    const AutoLockGC& lock) {
  MOZ_ASSERT(task->hasWork());
  MOZ_ASSERT(activeTasks < workerCount());

  activeTasks++;
}

void ParallelMarker::decActiveTasks(ParallelMarkTask* task,
                                    const AutoLockGC& lock) {
  MOZ_ASSERT(activeTasks != 0);

  activeTasks--;

  if (activeTasks == 0) {
    // We're finished. Wake up any tasks waiting for work.
    activeTasksAvailable.ref().notify_all();

    while (!waitingTasks.ref().isEmpty()) {
      ParallelMarkTask* task = waitingTasks.ref().popFront();
      MOZ_ASSERT(waitingTaskCount != 0);
      waitingTaskCount--;
      task->resume(lock);
    }
  }
}

void ParallelMarker::donateWorkFrom(GCMarker* src) {
  AutoLockGC lock(gc);

  // Check there are tasks waiting for work while holding the lock.
  if (waitingTaskCount == 0) {
    return;
  }

  // Take the first waiting task off the list.
  ParallelMarkTask* waitingTask = waitingTasks.ref().popFront();
  waitingTaskCount--;

  // |task| is not running so it's safe to move work to it.
  MOZ_ASSERT(waitingTask->isWaiting);

  // TODO: When using more than two marking threads it may be better to
  // release the lock here.

  // Move some work from this thread's mark stack to the waiting task.
  GCMarker::moveWork(waitingTask->marker, src);

  // Resume waiting task.
  waitingTask->resume(lock);

  gc->stats().count(gcstats::COUNT_PARALLEL_MARK_INTERRUPTIONS);

  GeckoProfilerRuntime& profiler = gc->rt->geckoProfiler();
  if (profiler.enabled()) {
    profiler.markEvent("Parallel marking donated work", "");
  }
}
