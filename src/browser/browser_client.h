#pragma once

#include <unordered_map>

#include "include/cef_client.h"

namespace orbit {

class Tab;

// The single CefClient shared by every browser. Using one client (instead of
// one per window) lets a tab move between windows without its callbacks
// going to a stale owner. Every callback handled here runs on the UI thread
// and is routed to the owning Tab by browser identifier.
class BrowserClient : public CefClient,
                      public CefLifeSpanHandler,
                      public CefDisplayHandler,
                      public CefLoadHandler,
                      public CefKeyboardHandler,
                      public CefRequestHandler,
                      public CefResourceRequestHandler,
                      public CefFocusHandler,
                      public CefDownloadHandler,
                      public CefPermissionHandler,
                      public CefJSDialogHandler {
 public:
  BrowserClient() = default;

  Tab* FindTab(int browser_id) const;
  size_t browser_count() const { return tabs_.size(); }

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefFocusHandler> GetFocusHandler() override { return this; }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
  CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  // CefLifeSpanHandler
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popup_id,
                     const CefString& target_url, const CefString& target_frame_name,
                     cef_window_open_disposition_t target_disposition, bool user_gesture,
                     const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override;
  void OnBeforePopupAborted(CefRefPtr<CefBrowser> browser, int popup_id) override;
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefDisplayHandler
  void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                       const CefString& url) override;
  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
  void OnFaviconURLChange(CefRefPtr<CefBrowser> browser,
                          const std::vector<CefString>& icon_urls) override;
  void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override;
  void OnLoadingProgressChange(CefRefPtr<CefBrowser> browser, double progress) override;
  void OnMediaAccessChange(CefRefPtr<CefBrowser> browser, bool has_video_access,
                           bool has_audio_access) override;

  // CefLoadHandler
  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack,
                            bool canGoForward) override;

  void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override;
  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode, const CefString& errorText,
                   const CefString& failedUrl) override;

  // CefDownloadHandler
  bool CanDownload(CefRefPtr<CefBrowser> browser, const CefString& url,
                   const CefString& request_method) override;
  bool OnBeforeDownload(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> download_item,
                        const CefString& suggested_name,
                        CefRefPtr<CefBeforeDownloadCallback> callback) override;
  void OnDownloadUpdated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> download_item,
                         CefRefPtr<CefDownloadItemCallback> callback) override;

  // CefPermissionHandler
  bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                      const CefString& requesting_origin,
                                      uint32_t requested_permissions,
                                      CefRefPtr<CefMediaAccessCallback> callback) override;
  bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser, uint64_t prompt_id,
                              const CefString& requesting_origin, uint32_t requested_permissions,
                              CefRefPtr<CefPermissionPromptCallback> callback) override;

  // CefJSDialogHandler
  bool OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString& origin_url,
                  JSDialogType dialog_type, const CefString& message_text,
                  const CefString& default_prompt_text, CefRefPtr<CefJSDialogCallback> callback,
                  bool& suppress_message) override;

  // CefKeyboardHandler
  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event,
                     CefEventHandle os_event, bool* is_keyboard_shortcut) override;

  // CefRequestHandler
  bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        const CefString& target_url, cef_window_open_disposition_t target_disposition,
                        bool user_gesture) override;
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
      bool is_navigation, bool is_download, const CefString& request_initiator,
      bool& disable_default_handling) override;
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status,
                                 int error_code, const CefString& error_string) override;

  // CefResourceRequestHandler
  void OnProtocolExecution(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefRequest> request, bool& allow_os_execution) override;

  // CefFocusHandler
  bool OnSetFocus(CefRefPtr<CefBrowser> browser, FocusSource source) override;
  void OnGotFocus(CefRefPtr<CefBrowser> browser) override;

 private:
  std::unordered_map<int, Tab*> tabs_;           // browser id -> tab
  std::unordered_map<int, Tab*> pending_popups_;  // popup id -> tab

  IMPLEMENT_REFCOUNTING(BrowserClient);
};

}  // namespace orbit
