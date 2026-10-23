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

void DefaultClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  // Out of sight at once, so no Chrome window flashes up; shown again if the
  // page turns out not to be one Orbit takes over.
  if (HWND top = TopLevelOf(browser)) ::ShowWindow(top, SW_HIDE);
}

void DefaultClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  adopted_.erase(browser->GetIdentifier());
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
  if (!Adopt(browser, url.ToString())) {
    if (HWND top = TopLevelOf(browser)) ::ShowWindow(top, SW_SHOWNA);  // not ours to take: leave it visible
  }
}

}  // namespace orbit
