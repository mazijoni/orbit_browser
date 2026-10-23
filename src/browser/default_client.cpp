#include "browser/default_client.h"

#include <windows.h>

#include "app/application.h"
#include "browser/profile.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/cef_browser.h"
#include "include/cef_request_context.h"
#include "ui/browser_window.h"

namespace orbit {
namespace {

bool IsAdoptableUrl(const std::string& url) {
  return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 ||
         url.rfind("chrome-extension://", 0) == 0;
}

HWND TopLevelOf(CefRefPtr<CefBrowser> browser) {
  HWND child = browser->GetHost()->GetWindowHandle();
  return child ? ::GetAncestor(child, GA_ROOT) : nullptr;
}

}  // namespace

// Off the screen and out of the taskbar at once: Chrome shows the window
// itself a moment later, and nothing of it should ever be seen.
static void Banish(HWND top) {
  if (!top) return;
  ::SetWindowLongPtrW(top, GWL_EXSTYLE, (::GetWindowLongPtrW(top, GWL_EXSTYLE) | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
  ::SetWindowPos(top, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  ::ShowWindow(top, SW_HIDE);
}

void DefaultClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  Banish(TopLevelOf(browser));
}

void DefaultClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  adopted_.erase(browser->GetIdentifier());
  closing_.erase(browser->GetIdentifier());
  if (Application* app = Application::Get()) app->OnWindowBrowserClosed();
}

bool DefaultClient::Adopt(CefRefPtr<CefBrowser> browser, const std::string& url) {
  if (adopted_.count(browser->GetIdentifier())) return true;
  if (!IsAdoptableUrl(url)) return false;
  Application* app = Application::Get();
  CefRefPtr<CefRequestContext> context = browser->GetHost()->GetRequestContext();
  // The window of the profile the browser belongs to; failing that (Chromium's
  // own default profile), any window.
  BrowserWindow* target = nullptr;
  for (const auto& window : app->windows()) {
    if (window->closing() || window->profile()->is_private()) continue;
    Profile* profile = window->profile();
    const bool same = context && profile->has_request_context() && profile->request_context()->IsSame(context);
    if (same) {
      target = window.get();
      break;
    }
    if (!target) target = window.get();
  }
  if (!target) return false;
  adopted_.insert(browser->GetIdentifier());
  target->OpenTab(FromUtf8(url), true);
  browser->GetHost()->CloseBrowser(true);
  ORBIT_LOG_INFO("a page Chromium opened on its own became a tab");
  return true;
}

bool DefaultClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request, bool, bool) {
  if (!frame->IsMain()) return false;
  return Adopt(browser, request->GetURL().ToString());  // true cancels the load here
}

void DefaultClient::OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                    const CefString& url) {
  if (!frame->IsMain() || adopted_.count(browser->GetIdentifier())) return;
  Banish(TopLevelOf(browser));
  if (Adopt(browser, url.ToString())) return;
  // Anything else is a window Chrome opened for its own UI (after an
  // extension install it opens a New Tab window only to show its "added"
  // bubble). Orbit says that itself, so the window just goes.
  if (url.ToString().rfind("about:blank", 0) == 0 && closing_.count(browser->GetIdentifier()) == 0) return;
  closing_.insert(browser->GetIdentifier());
  browser->GetHost()->CloseBrowser(true);
  ORBIT_LOG_INFO("closed a window Chromium opened for its own UI");
}

}  // namespace orbit
