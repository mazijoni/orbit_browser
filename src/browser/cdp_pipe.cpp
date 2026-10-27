#include "browser/cdp_pipe.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core/json.h"
#include "core/log.h"
#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"

namespace orbit::cdp_pipe {
namespace {

HANDLE g_to_browser = nullptr;    // we write commands here
HANDLE g_from_browser = nullptr;  // we read answers here
std::mutex g_mutex;               // guards the writes and the pending map
std::map<int, Done> g_pending;
std::atomic<int> g_next_id{1};
EventHandler g_event_handler;  // UI thread

void Deliver(const std::string& message) {
  auto json = ParseJson(message);
  if (!json || !json->is_object()) return;
  const JsonValue* id = json->Get("id");
  if (!id || id->type != JsonValue::Type::kNumber) {
    const std::string method = json->Str("method");
    if (method.empty()) return;
    CefPostTask(TID_UI, base::BindOnce(
                            [](std::string m, std::string session, std::string text) {
                              if (g_event_handler) g_event_handler(m, session, text);
                            },
                            method, json->Str("sessionId"), message));
    return;
  }
  Done done;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_pending.find(static_cast<int>(id->number));
    if (it == g_pending.end()) return;
    done = std::move(it->second);
    g_pending.erase(it);
  }
  const JsonValue* error = json->Get("error");
  const bool ok = !error;
  const std::string text = ok ? message : (error->is_object() ? error->Str("message") : std::string("error"));
  CefPostTask(TID_UI, base::BindOnce([](Done d, bool ok, std::string t) { d(ok, t); }, std::move(done), ok, text));
}

void ReadLoop() {
  std::string buffer;
  char chunk[16 * 1024];
  for (;;) {
    DWORD read = 0;
    if (!::ReadFile(g_from_browser, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
    buffer.append(chunk, read);
    size_t end;
    while ((end = buffer.find('\0')) != std::string::npos) {
      Deliver(buffer.substr(0, end));
      buffer.erase(0, end + 1);
    }
    if (buffer.size() > 64 * 1024 * 1024) buffer.clear();  // never: answers are small
  }
  ORBIT_LOG_INFO("cdp pipe closed");
}

}  // namespace

void Setup(CefRefPtr<CefCommandLine> command_line) {
  if (g_to_browser) return;
  // Troubleshooting: run without it (extension clicks then do nothing).
  if (command_line->HasSwitch("orbit-no-devtools-pipe")) return;
  // Not inheritable: no child process gets them.
  HANDLE browser_reads = nullptr, browser_writes = nullptr;
  if (!::CreatePipe(&browser_reads, &g_to_browser, nullptr, 0) ||
      !::CreatePipe(&g_from_browser, &browser_writes, nullptr, 0)) {
    ORBIT_LOG_WARNING("cdp pipe: CreatePipe failed (%lu)", ::GetLastError());
    g_to_browser = g_from_browser = nullptr;
    return;
  }
  command_line->AppendSwitch("remote-debugging-pipe");
  command_line->AppendSwitchWithValue(
      "remote-debugging-io-pipes",
      std::to_string(reinterpret_cast<uintptr_t>(browser_reads)) + "," +
          std::to_string(reinterpret_cast<uintptr_t>(browser_writes)));
  // Lets pipe clients (only) use the Extensions domain.
  command_line->AppendSwitch("enable-unsafe-extension-debugging");
  std::thread(&ReadLoop).detach();
}

bool available() { return g_to_browser != nullptr; }

void SetEventHandler(EventHandler handler) { g_event_handler = std::move(handler); }

void Send(const std::string& method, const std::string& params_json, Done done, const std::string& session_id) {
  if (!g_to_browser) {
    if (done) done(false, "no pipe");
    return;
  }
  const int id = g_next_id++;
  const std::string message = "{\"id\":" + std::to_string(id) +
                              (session_id.empty() ? std::string() : ",\"sessionId\":" + JsonQuote(session_id)) +
                              ",\"method\":" + JsonQuote(method) +
                              ",\"params\":" + (params_json.empty() ? std::string("{}") : params_json) + "}";
  std::lock_guard<std::mutex> lock(g_mutex);
  if (done) g_pending[id] = std::move(done);
  DWORD written = 0;
  const std::string framed = message + '\0';
  if (!::WriteFile(g_to_browser, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr)) {
    auto it = g_pending.find(id);
    if (it != g_pending.end()) {
      Done failed = std::move(it->second);
      g_pending.erase(it);
      CefPostTask(TID_UI, base::BindOnce([](Done d) { d(false, "pipe write failed"); }, std::move(failed)));
    }
  }
}

}  // namespace orbit::cdp_pipe
