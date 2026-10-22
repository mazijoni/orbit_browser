#include "browser/library_client.h"

#include <windows.h>

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"
#include "ui/browser_window.h"

namespace orbit {

bool LibraryClient::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
                                  const CefString&, const CefString&,
                                  cef_window_open_disposition_t, bool, const CefPopupFeatures&,
                                  CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&,
                                  CefRefPtr<CefDictionaryValue>&, bool*) {
  return true;  // no popups from the panel
}

void LibraryClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  if (!window_) return;
  if (panel_ == kLibrary) {
    window_->OnLibraryCreated(browser);
  } else if (panel_ == kTheme) {
    window_->OnThemePickerCreated(browser);
  } else if (panel_ == kBoost) {
    window_->OnBoostEditorCreated(browser);
  } else {
    window_->OnSiteDataCreated(browser);
  }
}

bool LibraryClient::DoClose(CefRefPtr<CefBrowser> browser) {
  // Asynchronously, as Tab::OnDoClose does: destroying it here would run
  // OnBeforeClose re-entrantly.
  if (HWND window = browser->GetHost()->GetWindowHandle()) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](HWND h) {
                              if (::IsWindow(h)) ::DestroyWindow(h);
                            },
                            window));
  }
  return true;
}

void LibraryClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  if (!window_) return;
  if (panel_ == kLibrary) {
    window_->OnLibraryClosed(browser);
  } else if (panel_ == kTheme) {
    window_->OnThemePickerClosed(browser);
  } else if (panel_ == kBoost) {
    window_->OnBoostEditorClosed(browser);
  } else {
    window_->OnSiteDataClosed(browser);
  }
}

bool LibraryClient::OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                   CefRefPtr<CefRequest> request, bool, bool) {
  const std::wstring url = request->GetURL().ToWString();
  const wchar_t* page = panel_ == kLibrary ? L"orbit://library/"
                       : panel_ == kTheme ? L"orbit://theme/"
                       : panel_ == kBoost ? L"orbit://boost/"
                                          : L"orbit://sitedata/";
  return url.rfind(page, 0) != 0;  // true cancels
}

bool LibraryClient::OnPreKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent& event,
                                  CefEventHandle, bool*) {
  if (event.type != KEYEVENT_RAWKEYDOWN || !window_) return false;
  const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
  const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
  const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
  return window_->HandleAccelerator(event.windows_key_code, ctrl, shift, alt);
}

bool LibraryClient::OnKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent& event, CefEventHandle) {
  if (event.type != KEYEVENT_RAWKEYDOWN || event.windows_key_code != VK_ESCAPE || !window_ ||
      (event.modifiers & (EVENTFLAG_SHIFT_DOWN | EVENTFLAG_CONTROL_DOWN | EVENTFLAG_ALT_DOWN))) {
    return false;
  }
  if (panel_ == kLibrary) {
    window_->CloseLibrary();
  } else if (panel_ == kTheme) {
    window_->CloseThemePicker();
  } else if (panel_ == kBoost) {
    window_->CloseBoostEditor();
  } else {
    window_->CloseSiteData();
  }
  return true;
}

}  // namespace orbit
