#pragma once

#include <map>
#include <string>
#include <vector>

#include "include/cef_app.h"

namespace orbit {

// CefApp shared by all processes. Browser-process behaviour is delegated to
// Application; sub-processes (renderer, GPU, utility) only need the scheme
// registration, which must be identical in every process.
inline constexpr char kMediaQueryMessage[] = "orbit.media-query";
inline constexpr char kMediaReplyMessage[] = "orbit.media-reply";
// Renderer -> browser: a trusted Alt+click on a web link (Zen's glance).
inline constexpr char kGlanceOpenMessage[] = "orbit.glance-open";
// Renderer -> browser: a trusted drag from the page's top strip (Zen's
// zen.view.drag-window-from-content); one number, the distance moved.
inline constexpr char kWindowDragMessage[] = "orbit.window-drag";
// Boosts' zap / focus picker: the browser turns it on in a page's main frame
// (mode "zap" / "focus" / "off"); the page reports picked selectors.
inline constexpr char kBoostPickModeMessage[] = "orbit.boost-pick-mode";
inline constexpr char kBoostPickMessage[] = "orbit.boost-pick";
// Renderer -> browser: this frame's media state as JSON (Zen's media card).
inline constexpr char kMediaStateMessage[] = "orbit.media-state";

class OrbitApp : public CefApp,
                  public CefBrowserProcessHandler,
                  public CefRenderProcessHandler {
 public:
  OrbitApp() = default;

  // Extracts navigable URLs from command-line arguments (as passed by the
  // shell when Orbit is the default browser). Anything that does not fix up
  // to an http(s)/file URL is ignored.
  static std::vector<std::wstring> UrlsFromCommandLine(CefRefPtr<CefCommandLine> command_line);

  // CefApp
  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override;
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

  // CefRenderProcessHandler (runs in sandboxed renderer processes)
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;
  void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue> extra_info) override;
  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override;

  // CefBrowserProcessHandler
  void OnContextInitialized() override;
  bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                   const CefString& current_directory) override;

 private:
  // Renderer process: each browser's glance trigger key ("" = off).
  std::map<int, std::string> glance_trigger_;
  std::map<int, bool> window_drag_;
  // The picker control installed in each main frame's context (by browser id).
  std::map<int, std::pair<CefRefPtr<CefV8Context>, CefRefPtr<CefV8Value>>> pickers_;
  IMPLEMENT_REFCOUNTING(OrbitApp);
};

}  // namespace orbit
