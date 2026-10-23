#pragma once

// The client for browsers Chromium creates on its own, with no Orbit tab
// behind them: an extension calling chrome.tabs.create (a welcome page after
// install), chrome.windows.create, and the like. Left alone, each would be a
// separate Chrome window. This client hides that window and moves the page
// into the profile's Orbit window as an ordinary tab.

#include <set>
#include <string>

#include "include/cef_client.h"

namespace orbit {

class DefaultClient : public CefClient,
                      public CefLifeSpanHandler,
                      public CefRequestHandler,
                      public CefDisplayHandler {
 public:
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser>) override { return false; }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
                      bool user_gesture, bool is_redirect) override;
  void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& url) override;

 private:
  // Opens |url| as a tab where |browser|'s profile has a window and closes
  // the browser. False when the page is left alone (not a web / extension
  // page, or no window to put it in).
  bool Adopt(CefRefPtr<CefBrowser> browser, const std::string& url);
  std::set<int> adopted_;
  std::set<int> closing_;

  IMPLEMENT_REFCOUNTING(DefaultClient);
};

}  // namespace orbit
