// What Chrome's toolbar does for an extension, done without Chrome's toolbar
// (Orbit's tabs are embedded browsers, which have none):
//
// - Click(): an extension without a popup (Picture-in-Picture, dark-mode
//   togglers, ...) gets chrome.action.onClicked with the tab, in its service
//   worker (woken if asleep), over Orbit's private DevTools pipe.
// - PreparePopup(): an extension's popup page sees the tab it was opened for
//   as the current tab (chrome.tabs.query({active, currentWindow})).
//
// In both, the extension's chrome.scripting.executeScript / insertCSS on that
// tab first run as usual; if Chrome refuses them because the access a real
// toolbar click grants (activeTab) is missing, Orbit runs the same script /
// CSS in the page itself, in an isolated world of its main frame, with a user
// gesture (as the click was one). That access ends when the page changes
// origin or after a while, as activeTab does.
#pragma once

#include <functional>
#include <string>

#include "include/cef_browser.h"

namespace orbit {

class Tab;

namespace extension_actions {

// Installs the pipe's event handler (once, browser start).
void Init();

using Done = std::function<void(bool ok, const std::string& error)>;
void Click(Tab* tab, const std::string& extension_id, Done done);

// The popup browser exists and shows about:blank; |ready| runs when its page
// can be loaded (the context is in place, or could not be: it loads anyway).
void PreparePopup(CefRefPtr<CefBrowser> popup, Tab* tab, const std::string& extension_id,
                  std::function<void()> ready);

// The JavaScript put into the extension's worker / popup (self-tests read it).
std::string ShimFor(long long window_id);

}  // namespace extension_actions
}  // namespace orbit
