#include "browser/extension_bridge.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app/application.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/crypto.h"
#include "core/json.h"
#include "include/cef_image.h"
#include "core/log.h"
#include "core/strings.h"
#include "ui/browser_window.h"
#include "include/cef_browser.h"
#include "include/wrapper/cef_closure_task.h"

namespace orbit {
namespace {

constexpr char kUrl[] = "chrome://extensions/";
constexpr char kPrefix[] = "ORBIT-EXT:";
constexpr wchar_t kHostClass[] = L"OrbitExtensionBridgeHost";

// Installed once per page load. Answers go out as console messages
// (ORBIT-EXT:<kind>:<json>); Orbit reads them in OnConsoleMessage.
constexpr char kBridgeScript[] = R"js((function () {
  if (window.__orbit) return;
  const dp = chrome.developerPrivate;
  const mg = chrome.management;
  const out = (kind, data) => console.log('ORBIT-EXT:' + kind + ':' + JSON.stringify(data));
  const fail = (e) => out('error', String((e && e.message) || e));
  const list = async () => {
    try {
      const all = await dp.getExtensionsInfo({includeDisabled: true, includeTerminated: false});
      const cfg = await dp.getProfileConfiguration();
      out('list', {dev: !!cfg.inDeveloperMode, items: all
        .filter((e) => e.type === 'EXTENSION' && (e.location === 'FROM_STORE' || e.location === 'UNPACKED'))
        .map((e) => ({
          id: e.id, name: e.name, version: e.version, description: e.description || '',
          state: e.state, location: e.location, icon: e.iconUrl || '', path: e.path || '',
          mayDisable: !!e.userMayModify && !e.mustRemainInstalled,
          options: (e.optionsPage && e.optionsPage.specified) ? e.optionsPage.url : '',
          home: e.webStoreUrl || (e.homePage && e.homePage.specified ? e.homePage.url : ''),
          perms: ((e.permissions && e.permissions.simplePermissions) || []).map((p) => p.message),
        }))});
    } catch (e) { fail(e); }
  };
  window.__orbit = {
    list,
    enable: async (id, on) => { try { await mg.setEnabled(id, on); } catch (e) { fail(e); } await list(); },
    uninstall: async (id) => { try { await mg.uninstall(id, {showConfirmDialog: true}); } catch (e) { fail(e); } await list(); },
    setDeveloperMode: async (on) => {
      try { await dp.updateProfileConfiguration({inDeveloperMode: on}); } catch (e) { fail(e); }
      await list();
    },
    loadUnpacked: async () => {
      try { await dp.loadUnpacked({failQuietly: false, populateError: true}); } catch (e) { fail(e); }
      await list();
    },
  };
  dp.onItemStateChanged.addListener(() => list());
  list();
})())js";

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterHostClass() {
  static bool registered = false;
  if (registered) return;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = &HostProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = kHostClass;
  ::RegisterClassExW(&wc);
  registered = true;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A "data:image/png;base64,..." icon as premultiplied BGRA (the toolbar's).
FaviconPixels DecodeIcon(const std::string& data_url) {
  static uint32_t generation = 0;
  FaviconPixels out;
  constexpr char kPng[] = "data:image/png;base64,";
  if (data_url.rfind(kPng, 0) != 0) return out;
  const auto png = crypto::Base64Decode(std::string_view(data_url).substr(sizeof(kPng) - 1));
  if (!png || png->empty()) return out;
  CefRefPtr<CefImage> image = CefImage::CreateImage();
  if (!image->AddPNG(1.0f, png->data(), png->size())) return out;
  int w = 0, h = 0;
  CefRefPtr<CefBinaryValue> bgra = image->GetAsBitmap(1.0f, CEF_COLOR_TYPE_BGRA_8888, CEF_ALPHA_TYPE_PREMULTIPLIED, w, h);
  if (!bgra || w <= 0 || h <= 0 || bgra->GetSize() < static_cast<size_t>(w) * h * 4) return out;
  out.width = w;
  out.height = h;
  out.bgra.resize(static_cast<size_t>(w) * h * 4);
  bgra->GetData(out.bgra.data(), out.bgra.size(), 0);
  out.generation = ++generation;
  return out;
}

// The extension's installed directory: <profile>/Extensions/<id>/<version>_<n>.
std::filesystem::path InstalledDir(const Profile& profile, const ExtensionInfo& info) {
  std::error_code ec;
  const auto base = profile.data_dir() / L"Extensions" / std::filesystem::path(info.id);
  std::filesystem::path best;
  for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(info.version + "_", 0) == 0 || name == info.version) best = entry.path();
  }
  return best;
}

// Fills the action's popup and title from the manifest. For UNPACKED
// extensions the files live where the user loaded them, which is not known
// here; they simply have no toolbar action.
void ReadManifest(const Profile& profile, ExtensionInfo& info, const std::string& unpacked_path) {
  // An unpacked extension reports where it was loaded from; others are in the profile.
  const auto dir = unpacked_path.empty() ? InstalledDir(profile, info)
                                         : std::filesystem::path(FromUtf8(unpacked_path));
  if (dir.empty()) return;
  const auto manifest = ParseJson(ReadFile(dir / L"manifest.json"));
  if (!manifest || !manifest->is_object()) return;
  const JsonValue* action = manifest->Get("action");
  if (!action) action = manifest->Get("browser_action");
  if (!action || !action->is_object()) return;
  info.popup = action->Str("default_popup");
  info.action_title = action->Str("default_title");
}

}  // namespace

ExtensionBridge::ExtensionBridge(Profile* profile) : profile_(profile) {}

bool ExtensionBridge::IsExtensionId(const std::string& id) {
  return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'p'; });
}

void ExtensionBridge::Start() {
  if (state_ != State::kNone || profile_->is_private()) return;
  RegisterHostClass();
  host_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHostClass, L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0,
                            800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!host_) return;
  CefWindowInfo info;
  info.SetAsChild(host_, CefRect(0, 0, 800, 600));
  info.runtime_style = CEF_RUNTIME_STYLE_CHROME;
  CefBrowserSettings settings;
  if (!CefBrowserHost::CreateBrowser(info, this, kUrl, settings, nullptr, profile_->request_context())) {
    ORBIT_LOG_ERROR("extension bridge: CreateBrowser failed");
    ::DestroyWindow(host_);
    host_ = nullptr;
    return;
  }
  state_ = State::kCreating;
}

void ExtensionBridge::Close() {
  if (state_ == State::kNone || state_ == State::kClosing) return;
  const bool created = browser_ != nullptr;
  state_ = State::kClosing;  // a browser still being created is closed in OnAfterCreated
  if (created) browser_->GetHost()->CloseBrowser(true);
}

void ExtensionBridge::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  browser_ = browser;
  if (state_ == State::kClosing) {
    browser->GetHost()->CloseBrowser(true);
    return;
  }
  state_ = State::kLoading;
}

void ExtensionBridge::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  browser_ = nullptr;
  state_ = State::kNone;
  queued_.clear();
  if (host_) {
    ::ShowWindow(host_, SW_HIDE);
    Tab::DestroyHostWhenGone(host_);
    host_ = nullptr;
  }
  if (Application* app = Application::Get()) app->OnWindowBrowserClosed();
}

bool ExtensionBridge::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefRequest> request, bool user_gesture, bool is_redirect) {
  // Only the extensions page itself (and its own sub-navigations) may load.
  const std::string url = request->GetURL().ToString();
  return url.rfind("chrome://extensions", 0) != 0 && url != "about:blank";
}

void ExtensionBridge::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int) {
  if (!frame->IsMain() || state_ == State::kClosing) return;
  if (frame->GetURL().ToString().rfind("chrome://extensions", 0) != 0) return;
  state_ = State::kReady;
  frame->ExecuteJavaScript(kBridgeScript, "", 0);
  std::vector<Command> queued = std::move(queued_);
  queued_.clear();
  for (const Command& c : queued) Run(c.script, c.needs_gesture);
}

// Chrome shows its confirmations (removing an extension, the folder picker)
// over the hidden page's window; put that window over one of the profile's
// Orbit windows, and make it owned by it, so they appear there.
void ExtensionBridge::AnchorToWindow() {
  Application* app = Application::Get();
  if (!app || !host_) return;
  for (const auto& w : app->windows()) {
    if (w->profile() != profile_ || w->closing()) continue;
    RECT rc;
    if (!::GetWindowRect(w->hwnd(), &rc)) continue;
    ::SetWindowLongPtrW(host_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(w->hwnd()));
    ::SetWindowPos(host_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
    return;
  }
}

void ExtensionBridge::Run(const std::string& script, bool needs_gesture) {
  if (state_ == State::kNone) Start();
  last_error_.clear();
  if (state_ == State::kReady && browser_) {
    if (needs_gesture) {
      AnchorToWindow();
      // chrome.management.uninstall and the folder picker demand a user
      // gesture: a click in the hidden page gives its script transient
      // activation. The click lands on empty space.
      CefMouseEvent at;
      at.x = at.y = 4;
      at.modifiers = 0;
      browser_->GetHost()->SendMouseClickEvent(at, MBT_LEFT, false, 1);
      browser_->GetHost()->SendMouseClickEvent(at, MBT_LEFT, true, 1);
    }
    browser_->GetMainFrame()->ExecuteJavaScript(script, "", 0);
  } else {
    queued_.push_back({script, needs_gesture});
  }
}

void ExtensionBridge::Refresh() { Run("window.__orbit && window.__orbit.list();", false); }

void ExtensionBridge::SetEnabled(const std::string& id, bool enabled) {
  if (!IsExtensionId(id)) return;
  Run("window.__orbit.enable('" + id + "', " + (enabled ? "true" : "false") + ");", false);
}

void ExtensionBridge::Uninstall(const std::string& id) {
  if (!IsExtensionId(id)) return;
  Run("window.__orbit.uninstall('" + id + "');", true);
}

void ExtensionBridge::SetDeveloperMode(bool on) {
  Run(std::string("window.__orbit.setDeveloperMode(") + (on ? "true" : "false") + ");", false);
}

void ExtensionBridge::LoadUnpacked() { Run("window.__orbit.loadUnpacked();", true); }

bool ExtensionBridge::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t, const CefString& message,
                                       const CefString&, int) {
  if (!browser_ || !browser->IsSame(browser_)) return false;
  const std::string text = message.ToString();
  if (text.rfind(kPrefix, 0) != 0) return false;
  const size_t start = sizeof(kPrefix) - 1;
  const size_t colon = text.find(':', start);
  if (colon == std::string::npos) return true;
  HandleMessage(text.substr(start, colon - start), text.substr(colon + 1));
  return true;
}

void ExtensionBridge::HandleMessage(const std::string& kind, const std::string& json) {
  const auto value = ParseJson(json);
  if (!value) return;
  if (kind == "error") {
    last_error_ = value->is_string() ? value->string : std::string();
    ORBIT_LOG_WARNING("extension bridge: %s", last_error_.c_str());
    ++revision_;
    return;
  }
  if (kind != "list" || !value->is_object()) return;
  const JsonValue* items = value->Get("items");
  if (!items || !items->is_array()) return;
  developer_mode_ = value->Get("dev") && value->Get("dev")->boolean;
  std::vector<ExtensionInfo> list;
  for (const JsonValue& e : items->array) {
    if (!e.is_object()) continue;
    ExtensionInfo info;
    info.id = e.Str("id");
    if (!IsExtensionId(info.id)) continue;
    info.name = e.Str("name");
    info.version = e.Str("version");
    info.description = e.Str("description");
    info.location = e.Str("location");
    info.icon = e.Str("icon");
    // The previous decode is kept when the icon did not change.
    for (const ExtensionInfo& old : extensions_) {
      if (old.id == info.id && old.icon == info.icon) info.icon_pixels = old.icon_pixels;
    }
    if (info.icon_pixels.bgra.empty()) info.icon_pixels = DecodeIcon(info.icon);
    info.options_url = e.Str("options");
    info.home_url = e.Str("home");
    info.enabled = e.Str("state") == "ENABLED";
    if (const JsonValue* may = e.Get("mayDisable")) info.may_disable = may->boolean;
    if (const JsonValue* perms = e.Get("perms"); perms && perms->is_array()) {
      for (const JsonValue& p : perms->array) {
        if (p.is_string()) info.permissions.push_back(p.string);
      }
    }
    ReadManifest(*profile_, info, e.Str("path"));
    list.push_back(std::move(info));
  }
  std::sort(list.begin(), list.end(), [](const ExtensionInfo& a, const ExtensionInfo& b) { return a.name < b.name; });
  // A new extension (installed from the Web Store just now): Orbit says so,
  // instead of Chrome's own bubble.
  std::vector<std::string> added;
  if (listed_once_) {
    for (const ExtensionInfo& e : list) {
      const bool known = std::any_of(extensions_.begin(), extensions_.end(), [&](const ExtensionInfo& x) { return x.id == e.id; });
      if (!known) added.push_back(e.name);
    }
  }
  listed_once_ = true;
  extensions_ = std::move(list);
  ++revision_;
  for (const std::string& name : added) {
    if (Application* app = Application::Get()) app->OnExtensionAdded(profile_, name);
  }
  if (Application* app = Application::Get()) app->OnExtensionsChanged(profile_);
}

}  // namespace orbit
