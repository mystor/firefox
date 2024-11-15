#filter dumbComments emptyLines substitution

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Non-static prefs that are specific to Android GeckoView belong in this file.
//
// Please indent all prefs defined within #ifdef/#ifndef conditions. This
// improves readability, particular for conditional blocks that exceed a single
// screen.

// Absolute path to the devtools unix domain socket file used
// to communicate with a usb cable via adb forward.
pref("devtools.debugger.unix-domain-socket", "@ANDROID_PACKAGE_NAME@/firefox-debugger-socket");

// AccessibleCaret css for Android L style assets (bug 1097398)
pref("layout.accessiblecaret.height", "22.0");
pref("layout.accessiblecaret.margin-left", "-11.5");
pref("layout.accessiblecaret.width", "22.0");
