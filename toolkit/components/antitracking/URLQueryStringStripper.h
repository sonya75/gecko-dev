/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_URLQueryStringStripper_h
#define mozilla_URLQueryStringStripper_h

#include "nsIURLQueryStrippingListService.h"

#include "nsStringFwd.h"
#include "nsTHashSet.h"

class nsIURI;

namespace mozilla {

// URLQueryStringStripper is responsible for stripping certain part of the query
// string of the given URI to address the bounce(redirect) tracking issues. It
// will strip every query parameter which matches the strip list defined in
// the pref 'privacy.query_stripping.strip_list'. Note that It's different from
// URLDecorationStripper which strips the entire query string from the referrer
// if there is a tracking query parameter present in the URI.
//
// TODO: Given that URLQueryStringStripper and URLDecorationStripper are doing
//       similar things. We could somehow combine these two modules into one.
//       We will improve this in the future.

class URLQueryStringStripper final : public nsIURLQueryStrippingListObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIURLQUERYSTRIPPINGLISTOBSERVER

  // Strip the query parameters that are in the strip list. Return the amount of
  // query parameters that have been stripped. Returns 0 if no query parameters
  // have been stripped or the feature is disabled.
  static uint32_t Strip(nsIURI* aURI, bool aIsPBM, nsCOMPtr<nsIURI>& aOutput);

  // Test-only method to get the current strip list.
  static void TestGetStripList(nsACString& aStripList);

 private:
  URLQueryStringStripper();
  ~URLQueryStringStripper() = default;

  static URLQueryStringStripper* GetOrCreate();

  static void OnPrefChange(const char* aPref, void* aData);

  nsresult Init();
  nsresult Shutdown();

  uint32_t StripQueryString(nsIURI* aURI, nsCOMPtr<nsIURI>& aOutput);

  bool CheckAllowList(nsIURI* aURI);

  void PopulateStripList(const nsAString& aList);
  void PopulateAllowList(const nsACString& aList);

  nsTHashSet<nsString> mList;
  nsTHashSet<nsCString> mAllowList;
  nsCOMPtr<nsIURLQueryStrippingListService> mListService;
  bool mIsInitialized;
};

}  // namespace mozilla

#endif  // mozilla_URLQueryStringStripper_h
