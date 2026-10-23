#pragma once

// Find in page (Ctrl+F). Chrome style has no find bar for a child browser, so
// Orbit draws its own: a small panel (orbit://find/, an Alloy-style browser in
// a popup window) at the top right of the page area, driving
// CefBrowserHost::Find on the window's active tab. One bar at a time.

#include <string>

#include <windows.h>

#include "include/cef_client.h"

namespace orbit {

class BrowserWindow;
class Tab;

class FindBar : public CefClient,
                public CefLifeSpanHandler,
                public CefKeyboardHandler,
                public CefRequestHandler {
 public:
  // Opens the bar for |window| (or focuses it when already open there).
  static void Open(BrowserWindow* window);
  // Closes the bar and clears the highlights.
  static void Close();
  // True from Open() until CEF has reported the browser closed.
  static bool Alive();
  static bool IsOpenFor(const BrowserWindow* window);
  // F3 / Shift+F3 in the window.
  static void FindNext(BrowserWindow* window, bool forward);
  // The window moved or resized, or its active tab changed.
  static void Reposition(BrowserWindow* window);
  static void OnActiveTabChanged(BrowserWindow* window);

  // CefFindHandler results arrive here (see BrowserClient).
  static void OnResult(CefRefPtr<CefBrowser> browser, int identifier, int count, int active);
  // What the page's API drives (UI thread).
  static void Search(const std::wstring& text, bool forward, bool match_case, bool next);
  struct State {
    int count = 0;
    int active = 0;
    bool open = false;
  };
  static State CurrentState();
  static const std::wstring& LastText();
  static CefRefPtr<CefBrowser> BrowserForTest();

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  // CefLifeSpanHandler
  bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString&, const CefString&,
                     cef_window_open_disposition_t, bool, const CefPopupFeatures&, CefWindowInfo&,
                     CefRefPtr<CefClient>&, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&,
                     bool*) override {
    return true;
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  // Alloy style: the browser's window is destroyed by us, asynchronously, so
  // OnBeforeClose does not run inside this callback.
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefKeyboardHandler: Escape closes the bar; window shortcuts keep working.
  bool OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event, CefEventHandle os_event) override;

  // CefRequestHandler: the bar stays on orbit://find/.
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
                      bool user_gesture, bool is_redirect) override;

 private:
  explicit FindBar(BrowserWindow* window) : window_(window) {}
  static LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void Place();

  BrowserWindow* window_;
  HWND popup_ = nullptr;
  CefRefPtr<CefBrowser> browser_;
  bool closing_ = false;

  IMPLEMENT_REFCOUNTING(FindBar);
};

}  // namespace orbit
