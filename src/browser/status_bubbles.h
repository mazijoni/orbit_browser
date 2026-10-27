// Chromium's link-status bubble (the URL shown at the bottom left while a
// link is hovered) is a small popup window of each tab's embedded browser.
// When Orbit hides a tab's page (tab switch, close), Chromium is not told, so
// a bubble that was showing stayed on screen for good, under the next ones.
// Orbit notes which tab's page each bubble appeared over and hides it with
// that page.
#pragma once

#include <windows.h>

namespace orbit::status_bubbles {

// Once, on the UI thread: starts watching for bubbles of this process.
void Install();
// The page in |tab_host| is hidden or goes away: so do its bubbles.
void HideFor(HWND tab_host);

}  // namespace orbit::status_bubbles
