/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// tests the translation infobar, using a fake 'Translation' implementation.

const { PermissionTestUtils } = ChromeUtils.import(
  "resource://testing-common/PermissionTestUtils.jsm"
);

const kLanguagesPref = "browser.translation.neverForLanguages";
const kShowUIPref = "browser.translation.ui.show";
const kEnableTranslationPref = "browser.translation.detectLanguage";

function test() {
  waitForExplicitFinish();

  Services.prefs.setBoolPref(kShowUIPref, true);
  Services.prefs.setBoolPref(kEnableTranslationPref, true);
  let tab = BrowserTestUtils.addTab(gBrowser);
  gBrowser.selectedTab = tab;
  registerCleanupFunction(function() {
    gBrowser.removeTab(tab);
    Services.prefs.clearUserPref(kShowUIPref);
    Services.prefs.clearUserPref(kEnableTranslationPref);
  });
  BrowserTestUtils.browserLoaded(tab.linkedBrowser).then(() => {
    (async function() {
      for (let testCase of gTests) {
        info(testCase.desc);
        await testCase.run();
      }
    })().then(finish, ex => {
      ok(false, "Unexpected Exception: " + ex);
      finish();
    });
  });

  BrowserTestUtils.loadURIString(
    gBrowser.selectedBrowser,
    "http://example.com/"
  );
}

function getLanguageExceptions() {
  let langs = Services.prefs.getCharPref(kLanguagesPref);
  return langs ? langs.split(",") : [];
}

function getDomainExceptions() {
  let results = [];
  for (let perm of Services.perms.all) {
    if (
      perm.type == "translate" &&
      perm.capability == Services.perms.DENY_ACTION
    ) {
      results.push(perm.principal);
    }
  }

  return results;
}

function openPopup(aPopup) {
  return new Promise(resolve => {
    aPopup.addEventListener(
      "popupshown",
      function() {
        TestUtils.executeSoon(resolve);
      },
      { once: true }
    );

    aPopup.focus();
    // One down event to open the popup.
    EventUtils.synthesizeKey("VK_DOWN", {
      altKey: !navigator.platform.includes("Mac"),
    });
  });
}

function waitForWindowLoad(aWin) {
  return new Promise(resolve => {
    aWin.addEventListener(
      "load",
      function() {
        TestUtils.executeSoon(resolve);
      },
      { capture: true, once: true }
    );
  });
}

var gTests = [
  {
    desc: "clean exception lists at startup",
    run: function checkNeverForLanguage() {
      is(
        getLanguageExceptions().length,
        0,
        "we start with an empty list of languages to never translate"
      );
      is(
        getDomainExceptions().length,
        0,
        "we start with an empty list of sites to never translate"
      );
    },
  },

  {
    desc: "language exception list",
    run: async function checkLanguageExceptions() {
      // Put 2 languages in the pref before opening the window to check
      // the list is displayed on load.
      Services.prefs.setCharPref(kLanguagesPref, "fr,de");

      // Open the translation exceptions dialog.
      let win = openDialog(
        "chrome://browser/content/preferences/dialogs/translation.xhtml",
        "Browser:TranslationExceptions",
        "",
        null
      );
      await waitForWindowLoad(win);

      // Check that the list of language exceptions is loaded.
      let getById = win.document.getElementById.bind(win.document);
      let tree = getById("languagesTree");
      let remove = getById("removeLanguage");
      let removeAll = getById("removeAllLanguages");
      is(tree.view.rowCount, 2, "The language exceptions list has 2 items");
      ok(remove.disabled, "The 'Remove Language' button is disabled");
      ok(!removeAll.disabled, "The 'Remove All Languages' button is enabled");

      // Select the first item.
      tree.view.selection.select(0);
      ok(!remove.disabled, "The 'Remove Language' button is enabled");

      // Click the 'Remove' button.
      remove.click();
      is(tree.view.rowCount, 1, "The language exceptions now contains 1 item");
      is(getLanguageExceptions().length, 1, "One exception in the pref");

      // Clear the pref, and check the last item is removed from the display.
      Services.prefs.setCharPref(kLanguagesPref, "");
      is(tree.view.rowCount, 0, "The language exceptions list is empty");
      ok(remove.disabled, "The 'Remove Language' button is disabled");
      ok(removeAll.disabled, "The 'Remove All Languages' button is disabled");

      // Add an item and check it appears.
      Services.prefs.setCharPref(kLanguagesPref, "fr");
      is(tree.view.rowCount, 1, "The language exceptions list has 1 item");
      ok(remove.disabled, "The 'Remove Language' button is disabled");
      ok(!removeAll.disabled, "The 'Remove All Languages' button is enabled");

      // Click the 'Remove All' button.
      removeAll.click();
      is(tree.view.rowCount, 0, "The language exceptions list is empty");
      ok(remove.disabled, "The 'Remove Language' button is disabled");
      ok(removeAll.disabled, "The 'Remove All Languages' button is disabled");
      is(Services.prefs.getCharPref(kLanguagesPref), "", "The pref is empty");

      win.close();
    },
  },

  {
    desc: "domains exception list",
    run: async function checkDomainExceptions() {
      // Put 2 exceptions before opening the window to check the list is
      // displayed on load.
      PermissionTestUtils.add(
        "http://example.org",
        "translate",
        Services.perms.DENY_ACTION
      );
      PermissionTestUtils.add(
        "http://example.com",
        "translate",
        Services.perms.DENY_ACTION
      );

      // Open the translation exceptions dialog.
      let win = openDialog(
        "chrome://browser/content/preferences/dialogs/translation.xhtml",
        "Browser:TranslationExceptions",
        "",
        null
      );
      await waitForWindowLoad(win);

      // Check that the list of language exceptions is loaded.
      let getById = win.document.getElementById.bind(win.document);
      let tree = getById("sitesTree");
      let remove = getById("removeSite");
      let removeAll = getById("removeAllSites");
      is(tree.view.rowCount, 2, "The sites exceptions list has 2 items");
      ok(remove.disabled, "The 'Remove Site' button is disabled");
      ok(!removeAll.disabled, "The 'Remove All Sites' button is enabled");

      // Select the first item.
      tree.view.selection.select(0);
      ok(!remove.disabled, "The 'Remove Site' button is enabled");

      // Click the 'Remove' button.
      remove.click();
      is(tree.view.rowCount, 1, "The site exceptions now contains 1 item");
      is(getDomainExceptions().length, 1, "One exception in the permissions");

      // Clear the permissions, and check the last item is removed from the display.
      PermissionTestUtils.remove("http://example.org", "translate");
      PermissionTestUtils.remove("http://example.com", "translate");
      is(tree.view.rowCount, 0, "The site exceptions list is empty");
      ok(remove.disabled, "The 'Remove Site' button is disabled");
      ok(removeAll.disabled, "The 'Remove All Site' button is disabled");

      // Add an item and check it appears.
      PermissionTestUtils.add(
        "http://example.com",
        "translate",
        Services.perms.DENY_ACTION
      );
      is(tree.view.rowCount, 1, "The site exceptions list has 1 item");
      ok(remove.disabled, "The 'Remove Site' button is disabled");
      ok(!removeAll.disabled, "The 'Remove All Sites' button is enabled");

      // Click the 'Remove All' button.
      removeAll.click();
      is(tree.view.rowCount, 0, "The site exceptions list is empty");
      ok(remove.disabled, "The 'Remove Site' button is disabled");
      ok(removeAll.disabled, "The 'Remove All Sites' button is disabled");
      is(getDomainExceptions().length, 0, "No exceptions in the permissions");

      win.close();
    },
  },
];
