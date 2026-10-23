#pragma once

// Orbit's window onto Chrome's extension system (UI thread only).
//
// Browsers run in CEF's Chrome style, so Chromium's own extension system
// (Manifest V3, content scripts, service workers, the Web Store install flow)
// is present, but CEF exposes no API for it. The one door is the privileged
// chrome://extensions page, whose script can call chrome.developerPrivate and
// chrome.management. Each profile gets a hidden browser on that page; Orbit
// sends it small commands as JavaScript and reads the answers from console
// messages. Orbit's own orbit://extensions page and menus sit on top of this,
// so no Chrome UI is needed for managing extensions (only Chrome's install
// confirmation, shown when the Web Store asks to add one).

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

#include "browser/tab.h"  // FaviconPixels
#include "include/cef_client.h"

namespace orbit {

class Profile;

struct ExtensionInfo {
  std::string id;
  std::string name;
  std::string version;
  std::string description;
  std::string location;      // FROM_STORE, UNPACKED, ...
  std::string icon;          // data: URL (may be empty)
  FaviconPixels icon_pixels; // the icon decoded (empty if none)
  std::string options_url;   // chrome-extension://... (may be empty)
  std::string home_url;      // the Web Store page or the extension's home page
  std::string popup;         // action popup path from the manifest ("" = none)
  std::string action_title;
  std::vector<std::string> permissions;  // user-readable lines
  bool enabled = false;
  bool may_disable = true;
};

class ExtensionBridge : public CefClient,
                        public CefLifeSpanHandler,
                        public CefLoadHandler,
                        public CefDisplayHandler,
                        public CefRequestHandler {
 public:
  explicit ExtensionBridge(Profile* profile);

  // Creates the hidden browser (idempotent). Commands sent before it is ready
  // are queued.
  void Start();
  // Closes it; alive() stays true until CEF reports the close.
  void Close();
  bool alive() const { return state_ != State::kNone; }
  bool ready() const { return state_ == State::kReady; }

  // The last list the page reported, user-installed extensions only.
  const std::vector<ExtensionInfo>& extensions() const { return extensions_; }
  // Incremented every time the list changes; pages poll it.
  uint64_t revision() const { return revision_; }
  // The last error text a command reported (cleared by the next command).
  const std::string& last_error() const { return last_error_; }

  void Refresh();
  void SetEnabled(const std::string& id, bool enabled);
  void Uninstall(const std::string& id);
  // Chrome only loads unpacked extensions while its developer mode is on.
  bool developer_mode() const { return developer_mode_; }
  void SetDeveloperMode(bool on);
  // Chrome's folder picker, then loads the chosen unpacked extension.
  void LoadUnpacked();

  // True when |id| looks like an extension id (32 letters a-p).
  static bool IsExtensionId(const std::string& id);

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  // CefLifeSpanHandler
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id,
                     const CefString& target_url, const CefString& target_frame_name,
                     cef_window_open_disposition_t target_disposition, bool user_gesture,
                     const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override {
    return true;  // the page never opens anything
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override { return false; }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefLoadHandler
  void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override;

  // CefDisplayHandler
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                        const CefString& message, const CefString& source, int line) override;

  // CefRequestHandler: the browser stays on chrome://extensions.
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool user_gesture,
                      bool is_redirect) override;

 private:
  enum class State { kNone, kCreating, kLoading, kReady, kClosing };

  struct Command {
    std::string script;
    bool needs_gesture;
  };
  // Now if the page is ready, else queued until it is.
  void Run(const std::string& script, bool needs_gesture);
  void AnchorToWindow();
  void HandleMessage(const std::string& kind, const std::string& json);

  Profile* profile_;
  State state_ = State::kNone;
  CefRefPtr<CefBrowser> browser_;
  HWND host_ = nullptr;
  std::vector<Command> queued_;
  std::vector<ExtensionInfo> extensions_;
  uint64_t revision_ = 0;
  bool developer_mode_ = false;
  bool listed_once_ = false;  // the first list is what was already installed
  std::string last_error_;

  IMPLEMENT_REFCOUNTING(ExtensionBridge);
};

}  // namespace orbit
