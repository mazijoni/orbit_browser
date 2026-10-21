#pragma once

#include <string>
#include <vector>

#include "include/cef_app.h"

namespace orbit {

// CefApp shared by all processes. Browser-process behaviour is delegated to
// Application; sub-processes (renderer, GPU, utility) only need the scheme
// registration, which must be identical in every process.
inline constexpr char kMediaQueryMessage[] = "orbit.media-query";
inline constexpr char kMediaReplyMessage[] = "orbit.media-reply";

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

  // CefBrowserProcessHandler
  void OnContextInitialized() override;
  bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                   const CefString& current_directory) override;

 private:
  IMPLEMENT_REFCOUNTING(OrbitApp);
};

}  // namespace orbit
