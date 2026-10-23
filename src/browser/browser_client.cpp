#include "browser/browser_client.h"

#include "browser/boost_apply.h"
#include "browser/password_vault.h"

#include <cmath>

#include "include/cef_parser.h"

#include "app/orbit_app.h"

#include "app/application.h"
#include "browser/downloads.h"
#include "browser/external_protocol.h"
#include "browser/find_bar.h"
#include "browser/permissions.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/log.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/base/cef_callback.h"
#include "ui/browser_window.h"
#include "ui/menu.h"
#include "core/strings.h"

namespace orbit {
namespace {

BrowserWindow* WindowFor(Tab* tab) {
  return tab ? tab->HostWindow() : nullptr;
}

constexpr int kMenuSplitLink = MENU_ID_USER_FIRST + 1;
constexpr int kMenuGlanceLink = MENU_ID_USER_FIRST + 2;
constexpr int kMenuInspect = MENU_ID_USER_FIRST + 3;
constexpr int kMenuNewTabLink = MENU_ID_USER_FIRST + 4;
constexpr int kMenuCopyLink = MENU_ID_USER_FIRST + 5;
constexpr int kMenuSearchSelection = MENU_ID_USER_FIRST + 6;
constexpr int kMenuOpenImage = MENU_ID_USER_FIRST + 7;
constexpr int kMenuCopyImageLink = MENU_ID_USER_FIRST + 8;

bool CopyText(HWND owner, const std::wstring& text) {
  bool open = false;
  for (int attempt = 0; attempt < 20 && !(open = ::OpenClipboard(owner)); ++attempt) ::Sleep(10);
  if (!open) return false;
  bool ok = false;
  ::EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* dst = ::GlobalLock(mem)) {
      memcpy(dst, text.c_str(), bytes);
      ::GlobalUnlock(mem);
      ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    }
    if (!ok) ::GlobalFree(mem);
  }
  ::CloseClipboard();
  return ok;
}

std::wstring AcceleratorText(int key, bool shift, bool ctrl, bool alt) {
  std::wstring out;
  if (ctrl) out += L"Ctrl+";
  if (alt) out += L"Alt+";
  if (shift) out += L"Shift+";
  if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
    out += static_cast<wchar_t>(key);
  } else if (key >= VK_F1 && key <= VK_F24) {
    out += L"F" + std::to_wstring(key - VK_F1 + 1);
  } else {
    return L"";
  }
  return out;
}

// CEF's menu model as an HMENU for ui::TrackMenu (ids are CEF's).
HMENU BuildMenu(CefRefPtr<CefMenuModel> model) {
  HMENU menu = ::CreatePopupMenu();
  for (size_t i = 0; i < model->GetCount(); ++i) {
    const cef_menu_item_type_t type = model->GetTypeAt(i);
    const UINT grayed = model->IsEnabledAt(i) ? 0 : MF_GRAYED;
    std::wstring label = model->GetLabelAt(i).ToWString();
    if (type == MENUITEMTYPE_SEPARATOR) {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else if (type == MENUITEMTYPE_SUBMENU) {
      ::AppendMenuW(menu, MF_POPUP | grayed, reinterpret_cast<UINT_PTR>(BuildMenu(model->GetSubMenuAt(i))),
                    label.c_str());
    } else if (type == MENUITEMTYPE_COMMAND || type == MENUITEMTYPE_CHECK || type == MENUITEMTYPE_RADIO) {
      int key = 0;
      bool shift = false, ctrl = false, alt = false;
      if (model->GetAcceleratorAt(i, key, shift, ctrl, alt)) {
        const std::wstring accel = AcceleratorText(key, shift, ctrl, alt);
        if (!accel.empty()) label += L"\t" + accel;
      }
      const UINT checked = model->IsCheckedAt(i) ? MF_CHECKED : 0;
      ::AppendMenuW(menu, MF_STRING | grayed | checked, static_cast<UINT_PTR>(model->GetCommandIdAt(i)),
                    label.c_str());
    }
  }
  return menu;
}

bool IsWebLink(const std::wstring& url) {
  return url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0;
}

}  // namespace

void BrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                                        CefRefPtr<CefContextMenuParams> params,
                                        CefRefPtr<CefMenuModel> model) {
  Tab* tab = FindTab(browser->GetIdentifier());
  if (!tab || !WindowFor(tab)) return;
  const int flags = params->GetTypeFlags();
  size_t at = 0;
  if ((flags & CM_TYPEFLAG_LINK) && IsWebLink(params->GetLinkUrl().ToWString())) {
    // Firefox's link items, and canOpenLinkInSplitView / Glance (Zen).
    model->InsertItemAt(at++, kMenuNewTabLink, "Open Link in New &Tab");
    model->InsertItemAt(at++, kMenuGlanceLink, "Open Link in &Glance");
    model->InsertItemAt(at++, kMenuSplitLink, "&Split Link to New Tab");
    model->InsertItemAt(at++, kMenuCopyLink, "Copy &Link Address");
    model->InsertSeparatorAt(at++);
  }
  if ((flags & CM_TYPEFLAG_MEDIA) && params->GetMediaType() == CM_MEDIATYPE_IMAGE &&
      IsWebLink(params->GetSourceUrl().ToWString())) {
    model->InsertItemAt(at++, kMenuOpenImage, "Open &Image in New Tab");
    model->InsertItemAt(at++, kMenuCopyImageLink, "Copy Image Addr&ess");
    model->InsertSeparatorAt(at++);
  }
  if (flags & CM_TYPEFLAG_SELECTION) {
    std::wstring text = params->GetSelectionText().ToWString();
    if (text.size() > 30) text = text.substr(0, 29) + L"\u2026";
    for (wchar_t& c : text) {
      if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
    }
    if (!text.empty()) {
      std::wstring label = L"Search for \u201C" + text + L"\u201D";
      std::wstring escaped;
      for (wchar_t c : label) {
        if (c == L'&') escaped += L'&';
        escaped += c;
      }
      model->AddSeparator();
      model->AddItem(kMenuSearchSelection, escaped);
    }
  }
  // Every page can be inspected (DevTools, docked).
  model->AddSeparator();
  model->AddItem(kMenuInspect, "I&nspect");
}

bool BrowserClient::RunContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                                   CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel> model,
                                   CefRefPtr<CefRunContextMenuCallback> callback) {
  Tab* tab = FindTab(browser->GetIdentifier());
  if (!WindowFor(tab) || model->GetCount() == 0) return false;
  HMENU menu = BuildMenu(model);
  POINT pt;
  ::GetCursorPos(&pt);
  // Shown after this callback returns (the menu runs its own loop).
  CefPostTask(TID_UI, base::BindOnce(
                          [](HMENU menu, POINT pt, int browser_id, CefRefPtr<CefRunContextMenuCallback> callback) {
                            Application* app = Application::Get();
                            Tab* t = app ? app->client()->FindTab(browser_id) : nullptr;
                            BrowserWindow* w = WindowFor(t);
                            const UINT cmd = w ? ui::TrackMenu(menu, 0, pt.x, pt.y, w->hwnd(), w->palette()) : 0;
                            ::DestroyMenu(menu);
                            if (cmd) {
                              callback->Continue(static_cast<int>(cmd), EVENTFLAG_NONE);
                            } else {
                              callback->Cancel();
                            }
                          },
                          menu, pt, browser->GetIdentifier(), callback));
  return true;
}

bool BrowserClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                                         CefRefPtr<CefContextMenuParams> params, int command_id,
                                         EventFlags) {
  if (command_id < kMenuSplitLink || command_id > kMenuCopyImageLink) return false;
  Tab* tab = FindTab(browser->GetIdentifier());
  BrowserWindow* w = WindowFor(tab);
  if (!w) return true;
  const std::wstring link = params->GetLinkUrl().ToWString();
  const std::wstring image = params->GetSourceUrl().ToWString();
  switch (command_id) {
    case kMenuGlanceLink:
      if (IsWebLink(link)) w->OpenGlance(tab, link);
      break;
    case kMenuSplitLink:
      if (IsWebLink(link)) w->SplitLinkInNewTab(tab, link);
      break;
    case kMenuNewTabLink:
      if (IsWebLink(link)) w->OpenRoutedTab(link, /*activate=*/false, /*external=*/false, tab);
      break;
    case kMenuCopyLink:
      if (IsWebLink(link)) CopyText(w->hwnd(), link);
      break;
    case kMenuOpenImage:
      if (IsWebLink(image)) w->OpenTabNear(tab, image, /*activate=*/false);
      break;
    case kMenuCopyImageLink:
      if (IsWebLink(image)) CopyText(w->hwnd(), image);
      break;
    case kMenuSearchSelection: {
      const std::wstring text = params->GetSelectionText().ToWString().substr(0, 500);
      std::wstring url = Application::Get()->search_template();
      const size_t q = url.find(L"{query}");
      if (!text.empty() && q != std::wstring::npos) {
        url.replace(q, 7, EscapeQueryComponent(text));
        w->OpenTabNear(tab, url, true);
      }
      break;
    }
    case kMenuInspect:
      w->InspectElement(tab, params->GetXCoord(), params->GetYCoord());
      break;
    default:
      break;
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

  // window.open(url, name, "width=...,height=...") — a sign-in popup (OAuth):
  // shown as a glance over the page that opened it. It is a real popup of the
  // opener (window.opener, postMessage and window.close() keep working), but
  // lives inside Orbit's window as a tab, never as a window of its own (a
  // native Alloy popup crashed in Chrome style).
  if (target_disposition == CEF_WOD_NEW_POPUP) {
    Tab* tab = window->CreatePopupTab(opener, target_url.ToWString(), true, windowInfo, /*as_glance=*/true);
    if (!tab) return true;
    pending_popups_[popup_id] = tab;
    return false;
  }

  // Letting Chromium create the browser (instead of cancelling and opening the
  // URL ourselves) preserves window.opener, which sign-in flows rely on, and
  // keeps the popup in the opener's request context (same profile).
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

void BrowserClient::OnFindResult(CefRefPtr<CefBrowser> browser, int identifier, int count, const CefRect&,
                                 int active_match_ordinal, bool) {
  FindBar::OnResult(browser, identifier, count, active_match_ordinal);
}

void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  HWND browser_hwnd = browser->GetHost()->GetWindowHandle();
  Tab* tab = Tab::FromHost(::GetParent(browser_hwnd));
  if (!tab) {
    if (browser->IsPopup()) {
      // A native popup window (OAuth / sign-in). It runs on its own; we only
      // track it so shutdown can wait for it.
      native_popups_.insert(browser->GetIdentifier());
      ORBIT_LOG_INFO("native popup window created (browser %d)", browser->GetIdentifier());
      return;
    }
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
  // Chrome style: CEF closes the browser's own widget inside the tab's host
  // (it never touches our top-level window); the tab destroys the host later.
  return false;
}

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  if (native_popups_.erase(browser->GetIdentifier())) {
    Application::Get()->OnWindowBrowserClosed();  // in case a window close waits
    return;
  }
  auto it = tabs_.find(browser->GetIdentifier());
  ORBIT_LOG_VERBOSE("browser %d OnBeforeClose (known=%d)", browser->GetIdentifier(),
                     it != tabs_.end() ? 1 : 0);
  if (it == tabs_.end()) return;
  Tab* tab = it->second;
  tabs_.erase(it);
  tab->NoteClosedByPage();
  // Chrome style closes a child browser without calling DoClose; run what
  // DoClose does (detach a tab that is being closed) here instead.
  if (tab->load_state() != Tab::LoadState::kClosing && tab->OnDoClose()) {
    Application::Get()->DetachClosingTab(tab);
  }
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
  if (message->GetName() == kPasswordSubmitMessage) {
    Tab* tab = FindTab(browser->GetIdentifier());
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (tab && frame && frame->IsMain() && args->GetSize() == 2 && args->GetType(0) == VTYPE_STRING &&
        args->GetType(1) == VTYPE_STRING) {
      // The origin is the sending frame's own (never the page's claim).
      passwords::OfferToSave(tab, frame->GetURL().ToWString(), args->GetString(0).ToWString(),
                             args->GetString(1).ToWString());
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
  Tab* tab = FindTab(browser->GetIdentifier());
  ApplyBoost(tab);
  passwords::FillSavedLogin(tab);  // autofill a saved login for this origin
  // Error pages (status 0) and failed loads are not recorded as history.
  if (httpStatusCode <= 0 || httpStatusCode >= 400) return;
  if (tab) Application::Get()->RecordVisit(tab);
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
