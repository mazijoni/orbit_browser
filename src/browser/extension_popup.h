#pragma once

// An extension's action popup (UI thread only). Chrome would draw it in its
// own toolbar; Orbit has none of Chrome's views, so the popup page
// (chrome-extension://<id>/<popup>) runs in a Chrome-style browser inside a
// small popup window under Orbit's extensions button. One popup at a time.
//
// Known limit: Chrome answers chrome.tabs "the current tab" for the popup's
// own window, so an extension that reads the page it was opened on (rather
// than using its content scripts or host permissions) sees the popup itself.

#include <string>

#include <windows.h>

#include "include/cef_client.h"

namespace orbit {

class BrowserWindow;
struct ExtensionInfo;

class ExtensionPopup : public CefClient,
                       public CefLifeSpanHandler,
                       public CefLoadHandler,
                       public CefDisplayHandler,
                       public CefKeyboardHandler,
                       public CefRequestHandler {
 public:
  // Opens |extension|'s popup for |window|'s active tab at |screen_at|
  // (screen pixels, the popup's top-left). Replaces any popup already open.
  static void Open(BrowserWindow* window, const ExtensionInfo& extension, POINT screen_at);
  // Starts closing the popup, if any.
  static void CloseCurrent();
  // True from Open() until CEF has reported the browser closed.
  static bool Alive();
  static bool IsPopupPath(const std::string& path);
  // The open popup's page title and window (tests).
  static std::wstring CurrentTitle();
  static HWND CurrentWindow();

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  // CefLifeSpanHandler
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id,
                     const CefString& target_url, const CefString& target_frame_name,
                     cef_window_open_disposition_t target_disposition, bool user_gesture,
                     const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info, bool* no_javascript_access) override;
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override { return false; }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefLoadHandler
  void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;

  // CefDisplayHandler: the page's title, and its size reports (ORBIT-POP:<w>,<h>).
  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override { title_ = title.ToWString(); }
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message,
                        const CefString& source, int line) override;

  // CefKeyboardHandler: Escape closes the popup.
  bool OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event, CefEventHandle os_event) override;

  // CefRequestHandler: links in a popup open as tabs of the window.
  bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& target_url,
                        cef_window_open_disposition_t target_disposition, bool user_gesture) override;

 private:
  ExtensionPopup(BrowserWindow* window, std::string extension_id);
  static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void Resize(int width_css, int height_css);

  BrowserWindow* window_;
  std::string extension_id_;
  HWND popup_ = nullptr;
  CefRefPtr<CefBrowser> browser_;
  bool closing_ = false;
  std::wstring title_;

  IMPLEMENT_REFCOUNTING(ExtensionPopup);
};

}  // namespace orbit
