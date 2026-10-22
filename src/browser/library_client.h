#pragma once

#include <string>

#include "include/cef_client.h"

namespace orbit {

class BrowserWindow;

// The client of a window's panel browsers: the library (Zen's <zen-library>,
// orbit://library/) and the site data panel (ZenSiteDataPanel,
// orbit://sitedata/). A panel never navigates away from its page and opens
// nothing itself; its actions go through the page's API to the window.
class LibraryClient : public CefClient,
                      public CefLifeSpanHandler,
                      public CefRequestHandler,
                      public CefKeyboardHandler,
                      public CefDisplayHandler {
 public:
  enum Panel { kLibrary, kSiteData, kTheme, kBoost };
  LibraryClient(BrowserWindow* window, Panel panel) : window_(window), panel_(panel) {}
  void Detach() { window_ = nullptr; }  // the window is going away

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

  // CefDisplayHandler: the page's title (diagnostics and tests).
  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override {
    title_ = title.ToWString();
  }
  const std::wstring& title() const { return title_; }

  // CefLifeSpanHandler
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id,
                     const CefString& target_url, const CefString& target_frame_name,
                     cef_window_open_disposition_t target_disposition, bool user_gesture,
                     const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override;
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  // Destroys the browser's own window instead of letting CEF send WM_CLOSE
  // to the top-level window (which would close the whole window).
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefRequestHandler: the panel shows orbit://library/ only.
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool user_gesture,
                      bool is_redirect) override;

  // CefKeyboardHandler: window shortcuts work in the panel; an Escape the
  // page did not handle closes it.
  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event,
                     CefEventHandle os_event, bool* is_keyboard_shortcut) override;
  bool OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event,
                  CefEventHandle os_event) override;

 private:
  BrowserWindow* window_;
  Panel panel_;
  std::wstring title_;
  IMPLEMENT_REFCOUNTING(LibraryClient);
};

}  // namespace orbit
