#include "browser/browser_client.h"

#include "browser/boost_apply.h"

#include <cmath>

#include "include/cef_parser.h"

#include "app/orbit_app.h"

#include "app/application.h"
#include "browser/downloads.h"
#include "browser/external_protocol.h"
#include "browser/permissions.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/log.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/base/cef_callback.h"
#include "ui/browser_window.h"

namespace orbit {
namespace {

BrowserWindow* WindowFor(Tab* tab) {
  return (tab && tab->workspace()) ? tab->workspace()->window() : nullptr;
}

constexpr int kMenuSplitLink = MENU_ID_USER_FIRST + 1;
constexpr int kMenuGlanceLink = MENU_ID_USER_FIRST + 2;

bool IsWebLink(const std::wstring& url) {
  return url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0;
}

}  // namespace

void BrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                                        CefRefPtr<CefContextMenuParams> params,
                                        CefRefPtr<CefMenuModel> model) {
  // canOpenLinkInSplitView: web links of tabs in a space (not Essentials).
  Tab* tab = FindTab(browser->GetIdentifier());
  if (!tab || !WindowFor(tab) || !(params->GetTypeFlags() & CM_TYPEFLAG_LINK)) return;
  if (!IsWebLink(params->GetLinkUrl().ToWString())) return;
  model->InsertItemAt(0, kMenuGlanceLink, "Open Link in Glance");
  model->InsertItemAt(1, kMenuSplitLink, "Split Link to New Tab");
  model->InsertSeparatorAt(2);
}

bool BrowserClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                                         CefRefPtr<CefContextMenuParams> params, int command_id,
                                         EventFlags) {
  if (command_id != kMenuSplitLink && command_id != kMenuGlanceLink) return false;
  Tab* tab = FindTab(browser->GetIdentifier());
  const std::wstring url = params->GetLinkUrl().ToWString();
  if (BrowserWindow* w = WindowFor(tab); w && IsWebLink(url)) {
    if (command_id == kMenuGlanceLink) {
      w->OpenGlance(tab, url);
    } else {
      w->SplitLinkInNewTab(tab, url);
    }
  }
  return true;
}

bool BrowserClient::OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event,
                               CefEventHandle) {
  if (event.type != KEYEVENT_RAWKEYDOWN || event.windows_key_code != VK_ESCAPE ||
      event.modifiers & (EVENTFLAG_SHIFT_DOWN | EVENTFLAG_CONTROL_DOWN | EVENTFLAG_ALT_DOWN)) {
    return false;
  }
  Tab* tab = FindTab(browser->GetIdentifier());
  BrowserWindow* w = WindowFor(tab);
  if (!w || !w->IsGlanceTab(tab)) return false;
  w->CloseGlanceAnimated();
  return true;
}

Tab* BrowserClient::FindTab(int browser_id) const {
  auto it = tabs_.find(browser_id);
  return it == tabs_.end() ? nullptr : it->second;
}

bool BrowserClient::OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                  int popup_id, const CefString& target_url,
                                  const CefString& target_frame_name,
                                  cef_window_open_disposition_t target_disposition, bool user_gesture,
                                  const CefPopupFeatures& popupFeatures,
                                  CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client,
                                  CefBrowserSettings& settings,
                                  CefRefPtr<CefDictionaryValue>& extra_info,
                                  bool* no_javascript_access) {
  extra_info = Application::Get()->RendererInfo();
  Tab* opener = FindTab(browser->GetIdentifier());
  BrowserWindow* window = WindowFor(opener);
  if (!window) return true;  // cancel

  // Popup blocking: script-initiated windows without a user gesture are
  // refused. Tab dispositions (ctrl/middle click) always carry a gesture.
  const bool is_window = target_disposition == CEF_WOD_NEW_POPUP ||
                         target_disposition == CEF_WOD_NEW_WINDOW;
  if (is_window && !user_gesture) {
    ORBIT_LOG_INFO("blocked popup without user gesture");
    return true;
  }

  // Every popup becomes a sidebar tab next to its opener. Letting Chromium
  // create the browser (instead of cancelling and opening the URL ourselves)
  // preserves window.opener, which sign-in flows rely on, and keeps the popup
  // in the opener's request context (same profile).
  const bool activate = target_disposition != CEF_WOD_NEW_BACKGROUND_TAB;
  Tab* tab = window->CreatePopupTab(opener, target_url.ToWString(), activate, windowInfo);
  if (!tab) return true;
  pending_popups_[popup_id] = tab;
  return false;
}

void BrowserClient::OnBeforePopupAborted(CefRefPtr<CefBrowser> browser, int popup_id) {
  auto it = pending_popups_.find(popup_id);
  if (it == pending_popups_.end()) return;
  Tab* tab = it->second;
  pending_popups_.erase(it);
  Application::Get()->RemoveTab(tab, /*remember=*/false);
}

void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  HWND browser_hwnd = browser->GetHost()->GetWindowHandle();
  Tab* tab = Tab::FromHost(::GetParent(browser_hwnd));
  if (!tab) {
    // Not created by us (e.g. DevTools); close it to keep invariants simple.
    ORBIT_LOG_WARNING("unexpected browser created; closing");
    browser->GetHost()->CloseBrowser(true);
    return;
  }
  for (auto it = pending_popups_.begin(); it != pending_popups_.end(); ++it) {
    if (it->second == tab) {
      pending_popups_.erase(it);
      break;
    }
  }
  tabs_[browser->GetIdentifier()] = tab;
  ORBIT_LOG_VERBOSE("browser %d created", browser->GetIdentifier());
  tab->OnAfterCreated(browser);
}

bool BrowserClient::DoClose(CefRefPtr<CefBrowser> browser) {
  Tab* tab = FindTab(browser->GetIdentifier());
  ORBIT_LOG_VERBOSE("browser %d DoClose (known=%d)", browser->GetIdentifier(), tab ? 1 : 0);
  if (!tab) return false;
  if (tab->OnDoClose()) Application::Get()->DetachClosingTab(tab);
  // We tore down the host window ourselves; never let CEF send WM_CLOSE to
  // the top-level window (that would close every tab in it).
  return true;
}

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  auto it = tabs_.find(browser->GetIdentifier());
  ORBIT_LOG_VERBOSE("browser %d OnBeforeClose (known=%d)", browser->GetIdentifier(),
                     it != tabs_.end() ? 1 : 0);
  if (it == tabs_.end()) return;
  Tab* tab = it->second;
  tabs_.erase(it);
  tab->OnBeforeClose();
  Application::Get()->OnBrowserClosed(tab);  // may delete |tab|
}

void BrowserClient::OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                    const CefString& url) {
  if (!frame->IsMain()) return;
  if (Tab* tab = FindTab(browser->GetIdentifier())) tab->OnAddressChange(url.ToWString());
}

void BrowserClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) {
  if (Tab* tab = FindTab(browser->GetIdentifier())) tab->OnTitleChange(title.ToWString());
}

void BrowserClient::OnFaviconURLChange(CefRefPtr<CefBrowser> browser,
                                       const std::vector<CefString>& icon_urls) {
  Tab* tab = FindTab(browser->GetIdentifier());
  if (!tab) return;
  tab->OnFaviconUrl(icon_urls.empty() ? std::wstring() : icon_urls.front().ToWString());
}

void BrowserClient::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) {
  Tab* tab = FindTab(browser->GetIdentifier());
  if (BrowserWindow* w = WindowFor(tab)) w->SetContentFullscreen(fullscreen);
}

void BrowserClient::OnLoadingProgressChange(CefRefPtr<CefBrowser> browser, double progress) {
  if (Tab* tab = FindTab(browser->GetIdentifier())) tab->OnLoadingProgress(progress);
}

void BrowserClient::OnMediaAccessChange(CefRefPtr<CefBrowser> browser, bool has_video_access,
                                        bool has_audio_access) {
  if (Tab* tab = FindTab(browser->GetIdentifier())) {
    tab->OnMediaAccessChange(has_video_access, has_audio_access);
  }
}

void BrowserClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading,
                                         bool canGoBack, bool canGoForward) {
  if (Tab* tab = FindTab(browser->GetIdentifier())) {
    tab->OnLoadingStateChange(isLoading, canGoBack, canGoForward);
  }
}

bool BrowserClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                             CefRefPtr<CefFrame> frame,
                                             CefProcessId source_process,
                                             CefRefPtr<CefProcessMessage> message) {
  // Renderer messages are untrusted input: each reads one typed argument.
  if (message->GetName() == kGlanceOpenMessage) {
    Tab* tab = FindTab(browser->GetIdentifier());
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (tab && (args->GetSize() == 1 || args->GetSize() == 5) && args->GetType(0) == VTYPE_STRING) {
      const std::wstring url = args->GetString(0).ToWString();
      // Optional link rect (CSS px in the page's viewport), sanity-checked.
      std::optional<BrowserWindow::GlanceOrigin> origin;
      if (args->GetSize() == 5) {
        double v[4] = {};
        bool ok = true;
        for (size_t i = 0; i < 4 && ok; ++i) {
          ok = args->GetType(i + 1) == VTYPE_DOUBLE;
          if (ok) v[i] = args->GetDouble(i + 1);
          ok = ok && std::isfinite(v[i]) && std::abs(v[i]) < 1e6;
        }
        if (ok && v[2] >= 0.0 && v[3] >= 0.0) {
          origin = BrowserWindow::GlanceOrigin{v[0], v[1], v[2], v[3],
                                               std::pow(1.2, browser->GetHost()->GetZoomLevel())};
        }
      }
      if (url.size() <= 8192 && IsWebLink(url) && !Application::Get()->GlanceTrigger().empty()) {
        if (BrowserWindow* w = WindowFor(tab)) w->OpenGlance(tab, url, origin);
      }
    }
    return true;
  }
  if (message->GetName() == kMediaStateMessage) {
    Tab* tab = FindTab(browser->GetIdentifier());
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (!tab || !frame || args->GetSize() != 1 || args->GetType(0) != VTYPE_STRING) return true;
    const std::string json = args->GetString(0).ToString();
    if (json.size() > 2048) return true;
    CefRefPtr<CefValue> value = CefParseJSON(json, JSON_PARSER_RFC);
    CefRefPtr<CefDictionaryValue> d = value ? value->GetDictionary() : nullptr;
    if (!d) return true;
    Tab::MediaState s;
    const std::string state = d->GetString("state").ToString();
    s.state = state == "playing" ? Tab::MediaState::State::kPlaying
            : state == "paused"  ? Tab::MediaState::State::kPaused
                                 : Tab::MediaState::State::kNone;
    s.title = d->GetString("title").ToWString().substr(0, 200);
    s.artist = d->GetString("artist").ToWString().substr(0, 200);
    auto number = [&](const char* key) {
      const double v = d->GetType(key) == VTYPE_DOUBLE ? d->GetDouble(key)
                     : d->GetType(key) == VTYPE_INT  ? d->GetInt(key) : 0.0;
      return std::isfinite(v) && v >= 0.0 && v < 1e7 ? v : 0.0;
    };
    s.time = number("time");
    s.duration = number("duration");
    s.updated_at = ::GetTickCount64();
    tab->OnMediaState(frame->GetIdentifier().ToString(), s);
    return true;
  }
  if (message->GetName() == kBoostPickMessage) {
    // A trusted click in a page in pick mode (the window checks it is the
    // page its editor is open for, and the mode).
    Tab* tab = FindTab(browser->GetIdentifier());
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (tab && frame && frame->IsMain() && args->GetSize() == 2 && args->GetType(0) == VTYPE_STRING &&
        args->GetType(1) == VTYPE_STRING) {
      if (BrowserWindow* w = WindowFor(tab)) {
        w->OnBoostPicked(tab, args->GetString(0).ToWString().substr(0, 8), args->GetString(1).ToWString());
      }
    }
    return true;
  }
  if (message->GetName() == kWindowDragMessage) {
    Tab* tab = FindTab(browser->GetIdentifier());
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    // Only while the left button is physically down: a page cannot fake that.
    if (tab && args->GetSize() == 1 && args->GetType(0) == VTYPE_DOUBLE &&
        ::GetAsyncKeyState(VK_LBUTTON) < 0 &&
        Application::Get()->store().GetPref(Store::kGlobalScope, L"drag_window_from_content")
                .value_or(L"1") == L"1") {
      if (BrowserWindow* w = WindowFor(tab)) w->StartWindowDrag(args->GetDouble(0));
    }
    return true;
  }
  if (message->GetName() != "orbit.media-reply") return false;
  Tab* tab = FindTab(browser->GetIdentifier());
  CefRefPtr<CefListValue> args = message->GetArgumentList();
  if (tab && args->GetSize() == 1 && args->GetType(0) == VTYPE_BOOL) {
    tab->OnMediaReply(args->GetBool(0));
  }
  return true;
}

void BrowserClient::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                TransitionType) {
  // Boosts: the site's look applies as the page starts (and again below).
  if (frame->IsMain()) ApplyBoost(FindTab(browser->GetIdentifier()));
}

void BrowserClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                              int httpStatusCode) {
  if (!frame->IsMain()) return;
  ApplyBoost(FindTab(browser->GetIdentifier()));
  // Error pages (status 0) and failed loads are not recorded as history.
  if (httpStatusCode <= 0 || httpStatusCode >= 400) return;
  if (Tab* tab = FindTab(browser->GetIdentifier())) Application::Get()->RecordVisit(tab);
}

void BrowserClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                ErrorCode errorCode, const CefString& errorText,
                                const CefString& failedUrl) {
  // ERR_ABORTED (-3) is a cancelled navigation (e.g. a download or a new
  // navigation replacing this one), not a failure worth surfacing.
  if (!frame->IsMain() || errorCode == ERR_ABORTED) return;
  ORBIT_LOG_INFO("main frame load error %d", static_cast<int>(errorCode));
  if (Tab* tab = FindTab(browser->GetIdentifier())) tab->OnLoadError(errorCode);
}

bool BrowserClient::CanDownload(CefRefPtr<CefBrowser> browser, const CefString& url,
                                const CefString& request_method) {
  return FindTab(browser->GetIdentifier()) != nullptr;
}

bool BrowserClient::OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefDownloadItem> download_item,
                                     const CefString& suggested_name,
                                     CefRefPtr<CefBeforeDownloadCallback> callback) {
  Tab* tab = FindTab(browser->GetIdentifier());
  Application::Get()->downloads().OnBeforeDownload(tab ? tab->profile() : nullptr, download_item,
                                                   suggested_name, callback);
  if (BrowserWindow* window = tab ? tab->window() : nullptr) window->AnimateDownload();
  return true;
}

void BrowserClient::OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefDownloadItem> download_item,
                                      CefRefPtr<CefDownloadItemCallback> callback) {
  Application::Get()->downloads().OnDownloadUpdated(download_item, callback);
}

bool BrowserClient::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                                   CefRefPtr<CefFrame> frame,
                                                   const CefString& requesting_origin,
                                                   uint32_t requested_permissions,
                                                   CefRefPtr<CefMediaAccessCallback> callback) {
  Tab* tab = FindTab(browser->GetIdentifier());
  BrowserWindow* window = WindowFor(tab);
  return Application::Get()->permissions().OnRequestMediaAccess(
      tab ? tab->profile() : nullptr, window ? window->hwnd() : nullptr,
      requesting_origin.ToWString(), requested_permissions, callback);
}

bool BrowserClient::OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser, uint64_t prompt_id,
                                           const CefString& requesting_origin,
                                           uint32_t requested_permissions,
                                           CefRefPtr<CefPermissionPromptCallback> callback) {
  Tab* tab = FindTab(browser->GetIdentifier());
  BrowserWindow* window = WindowFor(tab);
  return Application::Get()->permissions().OnShowPrompt(
      tab ? tab->profile() : nullptr, window ? window->hwnd() : nullptr,
      requesting_origin.ToWString(), requested_permissions, callback);
}

bool BrowserClient::OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString& origin_url,
                               JSDialogType dialog_type, const CefString& message_text,
                               const CefString& default_prompt_text,
                               CefRefPtr<CefJSDialogCallback> callback, bool& suppress_message) {
  // Background tabs may not raise alert()/confirm()/prompt() over the page the
  // user is looking at (a common nuisance/phishing pattern): suppress them.
  Tab* tab = FindTab(browser->GetIdentifier());
  BrowserWindow* window = WindowFor(tab);
  if (!window || window->active_tab() != tab) {
    suppress_message = true;
    return false;
  }
  return false;  // default CEF dialog, owned by the browser window
}

bool BrowserClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event,
                                  CefEventHandle os_event, bool* is_keyboard_shortcut) {
  if (event.type != KEYEVENT_RAWKEYDOWN) return false;
  Tab* tab = FindTab(browser->GetIdentifier());
  BrowserWindow* window = WindowFor(tab);
  if (!window) return false;
  // The settings page is recording a shortcut: let it see the keys.
  if (Application::Get()->ShortcutRecording() && tab->url().rfind(L"orbit://settings/", 0) == 0) {
    return false;
  }
  const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
  const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
  const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
  return window->HandleAccelerator(event.windows_key_code, ctrl, shift, alt);
}

bool BrowserClient::OnOpenURLFromTab(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                     const CefString& target_url,
                                     cef_window_open_disposition_t target_disposition,
                                     bool user_gesture) {
  if (target_disposition != CEF_WOD_NEW_FOREGROUND_TAB &&
      target_disposition != CEF_WOD_NEW_BACKGROUND_TAB &&
      target_disposition != CEF_WOD_NEW_WINDOW) {
    return false;
  }
  Tab* opener = FindTab(browser->GetIdentifier());
  BrowserWindow* window = WindowFor(opener);
  if (!window) return false;
  window->OpenRoutedTab(target_url.ToWString(), target_disposition != CEF_WOD_NEW_BACKGROUND_TAB,
                        /*external=*/false, opener);
  return true;
}

CefRefPtr<CefResourceRequestHandler> BrowserClient::GetResourceRequestHandler(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
    bool is_navigation, bool is_download, const CefString& request_initiator,
    bool& disable_default_handling) {
  // Only navigations can trigger external protocol handling.
  return is_navigation ? this : nullptr;
}

void BrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                              TerminationStatus status, int error_code,
                                              const CefString& error_string) {
  ORBIT_LOG_WARNING("render process terminated (status %d, code %d)",
                     static_cast<int>(status), error_code);
  if (Tab* tab = FindTab(browser->GetIdentifier())) tab->OnRenderProcessTerminated();
}

void BrowserClient::OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefRequest> request,
                                        bool& allow_os_execution) {
  // IO thread. Never let Chromium launch handlers directly; ask the user on
  // the UI thread and launch through our own validated path.
  allow_os_execution = false;
  if (!browser) return;  // service worker / background requests
  const int browser_id = browser->GetIdentifier();
  const std::wstring url = request->GetURL().ToWString();
  CefPostTask(TID_UI, base::BindOnce(&ConfirmExternalProtocol, browser_id, url));
}

bool BrowserClient::OnSetFocus(CefRefPtr<CefBrowser> browser, FocusSource source) {
  // A page that starts loading asks for focus; while the user is typing in
  // the address bar (inline or floating) that request is refused.
  if (source != FOCUS_SOURCE_NAVIGATION) return false;
  Tab* tab = FindTab(browser->GetIdentifier());
  BrowserWindow* w = WindowFor(tab);
  return w && w->AddressBarHasFocus();
}

void BrowserClient::OnGotFocus(CefRefPtr<CefBrowser> browser) {
  Tab* tab = FindTab(browser->GetIdentifier());
  if (BrowserWindow* w = WindowFor(tab)) w->OnContentFocused(tab);
}

}  // namespace orbit
