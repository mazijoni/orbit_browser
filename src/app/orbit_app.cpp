#include "app/orbit_app.h"

#include "app/application.h"
#include "app/self_test.h"
#include "browser/internal_pages.h"
#include "core/strings.h"
#include "core/url_fixup.h"
#include "include/cef_command_line.h"
#include "include/base/cef_callback.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_closure_task.h"

#include <algorithm>

namespace orbit {

void OrbitApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterInternalScheme(registrar);
}

void OrbitApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                              CefRefPtr<CefCommandLine> command_line) {
  // Self-test runs only: let fixture pages start audio without a user gesture
  // so media detection can be tested unattended. Never set for normal use.
  if (process_type.empty() && command_line->HasSwitch("orbit-self-test")) {
    command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
  }
}

bool OrbitApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame, CefProcessId source_process,
                                         CefRefPtr<CefProcessMessage> message) {
  // Renderer process: report whether any audible <audio>/<video> element is
  // playing in this frame. Only the browser process may ask. Web Audio API
  // output is not visible to this check (documented limitation).
  if (source_process != PID_BROWSER || message->GetName() != kMediaQueryMessage) return false;
  bool playing = false;
  CefRefPtr<CefV8Context> context = frame->GetV8Context();
  if (context && context->Enter()) {
    CefRefPtr<CefV8Value> result;
    CefRefPtr<CefV8Exception> exception;
    if (context->Eval("Array.prototype.some.call(document.querySelectorAll('audio,video'),"
                      "m => !m.paused && !m.ended && !m.muted && m.volume > 0)",
                      CefString(), 0, result, exception) &&
        result && result->IsBool()) {
      playing = result->GetBoolValue();
    }
    context->Exit();
  }
  CefRefPtr<CefProcessMessage> reply = CefProcessMessage::Create(kMediaReplyMessage);
  reply->GetArgumentList()->SetBool(0, playing);
  frame->SendProcessMessage(PID_BROWSER, reply);
  return true;
}

std::vector<std::wstring> OrbitApp::UrlsFromCommandLine(
    CefRefPtr<CefCommandLine> command_line) {
  std::vector<std::wstring> urls;
  CefCommandLine::ArgumentList args;
  command_line->GetArguments(args);
  for (const auto& arg : args) {
    const OmniboxResult r = FixupOmniboxInput(arg.ToWString(), L"");
    if (r.kind != OmniboxResult::Kind::kUrl) continue;
    const std::wstring lower = ToLowerAscii(r.url);
    if (lower.starts_with(L"http://") || lower.starts_with(L"https://") ||
        lower.starts_with(L"file:")) {
      urls.push_back(r.url);
    }
    if (urls.size() >= 20) break;  // bound work from a single launch
  }
  return urls;
}

void OrbitApp::OnContextInitialized() {
  CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
  Application::Get()->Start(UrlsFromCommandLine(command_line));
  // Self-tests only run against an explicitly chosen data directory.
  if (command_line->HasSwitch("orbit-self-test") && command_line->HasSwitch("user-data-dir")) {
    self_test::Start(command_line->GetSwitchValue("orbit-self-test").ToWString());
  }
  // Benchmark hook (scripts/benchmark.ps1): after N seconds, run the sleep
  // policy on every background tab regardless of idle time.
  if (command_line->HasSwitch("orbit-bench-sleep-after")) {
    const int seconds = std::clamp(
        _wtoi(command_line->GetSwitchValue("orbit-bench-sleep-after").ToWString().c_str()), 1,
        600);
    for (int pass = 0; pass < 2; ++pass) {
      CefPostDelayedTask(TID_UI, base::BindOnce([] {
                           if (Application* app = Application::Get()) app->ApplySleepPolicy(0);
                         }),
                         seconds * 1000 + pass * 1500);
    }
  }
}

bool OrbitApp::OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                            const CefString& current_directory) {
  Application::Get()->OnRelaunch(UrlsFromCommandLine(command_line));
  return true;
}

}  // namespace orbit
