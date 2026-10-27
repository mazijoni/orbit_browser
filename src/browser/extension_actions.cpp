#include "browser/extension_actions.h"

#include <windows.h>

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "app/application.h"
#include "browser/browser_client.h"
#include "browser/cdp_pipe.h"
#include "browser/extension_bridge.h"
#include "browser/tab.h"
#include "core/json.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/base/cef_callback.h"
#include "include/cef_devtools_message_observer.h"
#include "include/wrapper/cef_closure_task.h"

namespace orbit::extension_actions {
namespace {

constexpr char kBinding[] = "__orbitRun";
constexpr ULONGLONG kGrantMs = 5 * 60 * 1000;  // after the click, as long as the page keeps its origin
constexpr int kDetachIdleMs = 60 * 1000;      // a woken worker may sleep again

// The extension's worker or popup page, attached over the pipe.
struct Context {
  std::string extension_id;
  int tab_browser_id = 0;       // the tab the click / popup was for
  std::string page_target;      // its DevTools target
  std::string origin;           // its origin at the click
  ULONGLONG granted_until = 0;  // worker: the activeTab-like window; popup: 0 = while open
  ULONGLONG last_used = 0;
  bool popup = false;
};
std::map<std::string, Context>& Sessions() {  // by session id
  static std::map<std::string, Context> sessions;
  return sessions;
}
std::map<std::string, std::string>& WorkerSessions() {  // "<extension id>/<context>" -> session id
  static std::map<std::string, std::string> sessions;
  return sessions;
}

std::string OriginOf(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return {};
  const size_t end = url.find_first_of("/?#", scheme + 3);
  return url.substr(0, end == std::string::npos ? url.size() : end);
}

bool IsWebUrl(const std::string& url) { return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0; }

// One DevTools call on a browser's own session (CEF), for its target id.
class OneResult : public CefDevToolsMessageObserver {
 public:
  using Callback = std::function<void(bool ok, const std::string& json)>;
  static void Call(CefRefPtr<CefBrowser> browser, const std::string& method, Callback callback) {
    CefRefPtr<OneResult> self = new OneResult(std::move(callback));
    self->registration_ = browser->GetHost()->AddDevToolsMessageObserver(self);
    self->calling_ = true;
    self->id_ = browser->GetHost()->ExecuteDevToolsMethod(0, method, nullptr);
    self->calling_ = false;
    if (self->id_ == 0) {
      self->Finish(false, "");
      return;
    }
    // The answer may have come during the call, before its id was known.
    for (auto& [id, answer] : self->early_) {
      if (id == self->id_ && self->callback_) self->Finish(answer.first, answer.second);
    }
    self->early_.clear();
  }
  void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int message_id, bool success, const void* result,
                              size_t result_size) override {
    if (!callback_) return;
    std::string text(static_cast<const char*>(result), result_size);
    if (calling_) {
      early_.emplace_back(message_id, std::make_pair(success, std::move(text)));
      return;
    }
    if (message_id == id_) Finish(success, text);
  }

 private:
  explicit OneResult(Callback callback) : callback_(std::move(callback)) {}
  void Finish(bool ok, const std::string& json) {
    Callback callback = std::move(callback_);
    callback_ = nullptr;
    // Unregistered outside the observer callback.
    CefPostTask(TID_UI, base::BindOnce([](CefRefPtr<OneResult> self) { self->registration_ = nullptr; },
                                       CefRefPtr<OneResult>(this)));
    if (callback) callback(ok, json);
  }
  Callback callback_;
  int id_ = 0;
  bool calling_ = false;
  std::vector<std::pair<int, std::pair<bool, std::string>>> early_;
  CefRefPtr<CefRegistration> registration_;
  IMPLEMENT_REFCOUNTING(OneResult);
};

// A browser's DevTools target and its browser context (the profile).
struct TargetInfo {
  std::string id;
  std::string context;
};
void TargetOf(CefRefPtr<CefBrowser> browser, std::function<void(const TargetInfo&)> done) {
  OneResult::Call(browser, "Target.getTargetInfo", [done](bool ok, const std::string& json) {
    auto v = ok ? ParseJson(json) : std::nullopt;
    const JsonValue* info = v ? v->Get("targetInfo") : nullptr;
    done(info ? TargetInfo{info->Str("targetId"), info->Str("browserContextId")} : TargetInfo{});
  });
}

// A pipe call whose answer's "result" object is handed on (null on error).
using ResultFn = std::function<void(const JsonValue* result, const std::string& error)>;
void Pipe(const std::string& method, const std::string& params, ResultFn done, const std::string& session = {}) {
  cdp_pipe::Send(
      method, params,
      [done](bool ok, const std::string& json) {
        if (!done) return;
        if (!ok) {
          done(nullptr, json);
          return;
        }
        auto v = ParseJson(json);
        const JsonValue* result = v ? v->Get("result") : nullptr;
        done(result, result ? std::string() : "bad answer");
      },
      session);
}

// Runtime.evaluate: the value (by value), or the exception's text.
using ValueFn = std::function<void(bool ok, const std::string& value_json, const std::string& error)>;
void Evaluate(const std::string& session, const std::string& expression, bool user_gesture, int context_id,
              ValueFn done) {
  std::string params = "{\"expression\":" + JsonQuote(expression) +
                       ",\"awaitPromise\":true,\"returnByValue\":true,\"userGesture\":" +
                       (user_gesture ? "true" : "false");
  if (context_id) params += ",\"contextId\":" + std::to_string(context_id);
  params += "}";
  Pipe("Runtime.evaluate", params,
       [done](const JsonValue* result, const std::string& error) {
         if (!done) return;
         if (!result) {
           done(false, {}, error);
           return;
         }
         if (const JsonValue* ex = result->Get("exceptionDetails")) {
           const JsonValue* exception = ex->Get("exception");
           std::string text = exception ? exception->Str("description") : std::string();
           if (text.empty()) text = ex->Str("text");
           done(false, {}, text.empty() ? "script error" : text);
           return;
         }
         const JsonValue* remote = result->Get("result");
         const JsonValue* value = remote ? remote->Get("value") : nullptr;
         done(true, value ? ToJson(*value) : "null", {});
       },
       session);
}

void Attach(const std::string& target, std::function<void(const std::string& session)> done) {
  Pipe("Target.attachToTarget", "{\"targetId\":" + JsonQuote(target) + ",\"flatten\":true}",
       [done](const JsonValue* result, const std::string&) { done(result ? result->Str("sessionId") : ""); });
}

void Detach(const std::string& session) {
  Sessions().erase(session);
  for (auto it = WorkerSessions().begin(); it != WorkerSessions().end(); ++it) {
    if (it->second == session) {
      WorkerSessions().erase(it);
      break;
    }
  }
  Pipe("Target.detachFromTarget", "{\"sessionId\":" + JsonQuote(session) + "}", nullptr);
}

void ScheduleIdleDetach(const std::string& session) {
  CefPostDelayedTask(TID_UI, base::BindOnce([](std::string s) {
                       auto it = Sessions().find(s);
                       if (it == Sessions().end()) return;
                       if (::GetTickCount64() - it->second.last_used < static_cast<ULONGLONG>(kDetachIdleMs)) {
                         ScheduleIdleDetach(s);
                         return;
                       }
                       Detach(s);
                     }, session),
                     kDetachIdleMs);
}

// The extension's service worker target, started if it is asleep.
// (The extension has a worker in each browser context it runs in: the one of
// the tab's profile.)
void FindWorker(const std::string& extension_id, const TargetInfo& page, int tries,
                std::function<void(const std::string& worker)> done) {
  const std::string& page_target = page.id;
  Pipe("Target.getTargets", "{}", [=](const JsonValue* result, const std::string&) {
    const JsonValue* infos = result ? result->Get("targetInfos") : nullptr;
    const std::string prefix = "chrome-extension://" + extension_id + "/";
    if (infos && infos->is_array()) {
      for (const JsonValue& t : infos->array) {
        if (t.Str("type") == "service_worker" && t.Str("url").rfind(prefix, 0) == 0 &&
            (page.context.empty() || t.Str("browserContextId") == page.context)) {
          done(t.Str("targetId"));
          return;
        }
      }
    }
    if (tries <= 0) {
      done({});
      return;
    }
    if (tries == 8) {
      // Asleep: start it from the tab's session (same profile storage).
      Attach(page_target, [=](const std::string& ps) {
        if (ps.empty()) return done({});
        Pipe("ServiceWorker.enable", "{}", nullptr, ps);
        Pipe("ServiceWorker.startWorker", "{\"scopeURL\":" + JsonQuote(prefix) + "}",
             [=](const JsonValue*, const std::string&) {
               Pipe("Target.detachFromTarget", "{\"sessionId\":" + JsonQuote(ps) + "}", nullptr);
               FindWorker(extension_id, page, tries - 1, done);
             }, ps);
      });
      return;
    }
    CefPostDelayedTask(TID_UI, base::BindOnce([](std::string e, TargetInfo p, int t,
                                                 std::function<void(const std::string&)> d) { FindWorker(e, p, t, d); },
                                              extension_id, page, tries - 1, done),
                       150);
  });
}

// Runs a script or CSS from the extension in the tab's page (main frame).
void RunInPage(const Context& ctx, const JsonValue& request, ValueFn done) {
  const std::string kind = request.Str("kind");
  const bool main_world = request.Str("world") == "MAIN";
  std::string code = request.Str("code");
  if (kind == "css") {
    code = "(() => { const s = document.createElement('style'); s.textContent = " + JsonQuote(request.Str("css")) +
           "; (document.head || document.documentElement).append(s); })()";
  } else if (kind != "script") {
    done(false, {}, "unknown request");
    return;
  }
  const std::string world_name = "Orbit extension " + ctx.extension_id;
  Attach(ctx.page_target, [=](const std::string& ps) {
    if (ps.empty()) return done(false, {}, "Cannot access contents of the page.");
    auto finish = [ps, done](bool ok, const std::string& value, const std::string& error) {
      Pipe("Target.detachFromTarget", "{\"sessionId\":" + JsonQuote(ps) + "}", nullptr);
      done(ok, value, error);
    };
    if (main_world) {
      Evaluate(ps, code, true, 0, finish);
      return;
    }
    Pipe("Page.getFrameTree", "{}", [=](const JsonValue* result, const std::string& error) {
      const JsonValue* tree = result ? result->Get("frameTree") : nullptr;
      const JsonValue* frame = tree ? tree->Get("frame") : nullptr;
      if (!frame) return finish(false, {}, error.empty() ? "no frame" : error);
      Pipe("Page.createIsolatedWorld",
           "{\"frameId\":" + JsonQuote(frame->Str("id")) + ",\"worldName\":" + JsonQuote(world_name) + "}",
           [=](const JsonValue* world, const std::string& err) {
             const JsonValue* id = world ? world->Get("executionContextId") : nullptr;
             if (!id) return finish(false, {}, err.empty() ? "no world" : err);
             Evaluate(ps, code, true, static_cast<int>(id->number), finish);
           }, ps);
    }, ps);
  });
}

void Answer(const std::string& session, long long call_id, bool ok, const std::string& value_json,
            const std::string& error) {
  const std::string expr = "globalThis.__orbitRunDone && globalThis.__orbitRunDone(" + std::to_string(call_id) + ", " +
                           (ok ? "true, " + value_json : "false, " + JsonQuote(error)) + ")";
  Evaluate(session, expr, false, 0, nullptr);
}

void OnBindingCalled(const std::string& session, const std::string& message) {
  auto it = Sessions().find(session);
  if (it == Sessions().end()) return;
  Context& ctx = it->second;
  auto v = ParseJson(message);
  const JsonValue* params = v ? v->Get("params") : nullptr;
  if (!params || params->Str("name") != kBinding) return;
  auto request = ParseJson(params->Str("payload"));
  if (!request || !request->is_object()) return;
  const JsonValue* id = request->Get("id");
  const long long call_id = id && id->type == JsonValue::Type::kNumber ? static_cast<long long>(id->number) : 0;
  ctx.last_used = ::GetTickCount64();
  // The access a click grants: that tab, while it shows the same origin.
  Application* app = Application::Get();
  Tab* tab = app ? app->client()->FindTab(ctx.tab_browser_id) : nullptr;
  const bool expired = !ctx.popup && ::GetTickCount64() > ctx.granted_until;
  if (!tab || expired || OriginOf(ToUtf8(tab->url())) != ctx.origin) {
    Answer(session, call_id, false, {}, "Cannot access contents of the page.");
    return;
  }
  ORBIT_LOG_INFO("extension %s: %s in its tab, as after a toolbar click", ctx.extension_id.c_str(),
                 request->Str("kind").c_str());
  RunInPage(ctx, *request, [session, call_id](bool ok, const std::string& value, const std::string& error) {
    Answer(session, call_id, ok, value, error);
  });
}

}  // namespace

std::string ShimFor(long long window_id) {
  // Runs in the extension's worker or popup page.
  std::string shim = R"js((() => {
  const g = globalThis;
  g.__orbitWindow = __WINDOW__;
  if (g.__orbitShim) return;
  g.__orbitShim = true;
  const pending = new Map();
  let next = 1;
  g.__orbitRunDone = (id, ok, value) => {
    const p = pending.get(id);
    if (!p) return;
    pending.delete(id);
    if (ok) p.resolve(value); else p.reject(new Error(value));
  };
  const viaOrbit = (request) => new Promise((resolve, reject) => {
    if (typeof g.__orbitRun !== 'function') { reject(new Error('Cannot access contents of the page.')); return; }
    const id = next++;
    pending.set(id, {resolve, reject});
    setTimeout(() => { if (pending.delete(id)) reject(new Error('Cannot access contents of the page.')); }, 20000);
    g.__orbitRun(JSON.stringify(Object.assign({id}, request)));
  });
  const replace = (api, name, wrap) => {
    if (!api || typeof api[name] !== 'function') return;
    const original = api[name].bind(api);
    const fn = function (...args) {
      const callback = typeof args[args.length - 1] === 'function' ? args.pop() : null;
      const promise = wrap(original, ...args);
      if (!callback) return promise;
      promise.then((r) => callback(r), (e) => { console.error(e); callback(undefined); });
    };
    try { api[name] = fn; } catch (e) {}
    if (api[name] !== fn) {
      try { Object.defineProperty(api, name, {value: fn, configurable: true, writable: true}); } catch (e) {}
    }
  };
  const orbitTab = async () => {
    const [t] = await chrome.tabs.query({windowId: g.__orbitWindow});
    return t;
  };
  const fileText = async (f) => (await fetch(chrome.runtime.getURL(f))).text();
  const isOrbitTab = async (target) => {
    const t = await orbitTab();
    return !!(t && target && target.tabId === t.id);
  };
  // The current tab is the Orbit tab the click or popup was for.
  replace(chrome.tabs, 'query', async (query, q) => {
    q = q || {};
    if (q.active && (q.currentWindow || q.lastFocusedWindow) && q.windowId === undefined) {
      const r = Object.assign({}, q);
      delete r.currentWindow;
      delete r.lastFocusedWindow;
      r.windowId = g.__orbitWindow;
      return query(r);
    }
    return query(q);
  });
  // activeTab: what Chrome refuses without the toolbar's grant, Orbit runs.
  replace(chrome.scripting, 'executeScript', async (execute, injection) => {
    try { return await execute(injection); } catch (e) {
      if (!injection || !(await isOrbitTab(injection.target))) throw e;
      let code;
      if (injection.files && injection.files.length) {
        code = (await Promise.all(injection.files.map(fileText))).join('\n;\n');
      } else if (typeof injection.func === 'function') {
        code = '(' + injection.func.toString() + ')(...' + JSON.stringify(injection.args || []) + ')';
      } else {
        throw e;
      }
      const result = await viaOrbit({kind: 'script', code, world: injection.world === 'MAIN' ? 'MAIN' : 'ISOLATED'});
      return [{frameId: 0, result: result === null ? undefined : result}];
    }
  });
  replace(chrome.scripting, 'insertCSS', async (insert, injection) => {
    try { return await insert(injection); } catch (e) {
      if (!injection || !(await isOrbitTab(injection.target))) throw e;
      let css = injection.css || '';
      if (injection.files && injection.files.length) css += (await Promise.all(injection.files.map(fileText))).join('\n');
      await viaOrbit({kind: 'css', css});
    }
  });
})())js";
  const std::string token = "__WINDOW__";
  shim.replace(shim.find(token), token.size(), std::to_string(window_id));
  return shim;
}

void Init() {
  cdp_pipe::SetEventHandler([](const std::string& method, const std::string& session, const std::string& message) {
    if (method == "Runtime.bindingCalled") {
      OnBindingCalled(session, message);
    } else if (method == "Target.detachedFromTarget") {
      // The worker slept or the popup closed.
      auto v = ParseJson(message);
      const JsonValue* params = v ? v->Get("params") : nullptr;
      const std::string gone = params ? params->Str("sessionId") : "";
      Sessions().erase(gone);
      for (auto it = WorkerSessions().begin(); it != WorkerSessions().end(); ++it) {
        if (it->second == gone) {
          WorkerSessions().erase(it);
          break;
        }
      }
    }
  });
}

void Click(Tab* tab, const std::string& extension_id, Done done) {
  auto fail = [done](const std::string& error) {
    ORBIT_LOG_WARNING("extension action click failed: %s", error.c_str());
    if (done) done(false, error);
  };
  if (!cdp_pipe::available()) return fail("no DevTools pipe");
  if (!tab || !tab->browser()) return fail("the page is not loaded");
  if (!ExtensionBridge::IsExtensionId(extension_id)) return fail("bad id");
  const int browser_id = tab->browser()->GetIdentifier();
  const std::string origin = OriginOf(ToUtf8(tab->url()));
  TargetOf(tab->browser(), [=](const TargetInfo& page) {
    const std::string page_target = page.id;
    if (page_target.empty()) return fail("no target for the tab");
    Pipe("Browser.getWindowForTarget", "{\"targetId\":" + JsonQuote(page_target) + "}",
         [=](const JsonValue* window, const std::string& error) {
      const JsonValue* wid = window ? window->Get("windowId") : nullptr;
      if (!wid) return fail("no window: " + error);
      const long long window_id = static_cast<long long>(wid->number);
      auto with_session = [=](const std::string& session) {
        if (session.empty()) return fail("could not reach the extension");
        Context& ctx = Sessions()[session];
        ctx.extension_id = extension_id;
        ctx.tab_browser_id = browser_id;
        ctx.page_target = page_target;
        ctx.origin = IsWebUrl(origin + "/") ? origin : std::string("-");  // no grant on other schemes
        ctx.granted_until = ::GetTickCount64() + kGrantMs;
        ctx.last_used = ::GetTickCount64();
        Evaluate(session, ShimFor(window_id), false, 0, [=](bool, const std::string&, const std::string&) {
          const std::string dispatch =
              "(async () => { const [t] = await chrome.tabs.query({windowId: " + std::to_string(window_id) +
              "}); if (!t) throw new Error('no tab'); chrome.action.onClicked.dispatch(t); return t.id; })()";
          Evaluate(session, dispatch, true, 0, [=](bool ok, const std::string&, const std::string& error) {
            if (!ok) return fail(error);
            ORBIT_LOG_INFO("extension %s: toolbar click delivered", extension_id.c_str());
            if (done) done(true, {});
          });
        });
      };
      auto known = WorkerSessions().find(extension_id + "/" + page.context);
      if (known != WorkerSessions().end() && Sessions().count(known->second)) {
        with_session(known->second);
        return;
      }
      FindWorker(extension_id, page, 8, [=](const std::string& worker) {
        if (worker.empty()) return fail("the extension has no background worker");
        Attach(worker, [=](const std::string& session) {
          if (session.empty()) return with_session(session);
          WorkerSessions()[extension_id + "/" + page.context] = session;
          Pipe("Runtime.addBinding", std::string("{\"name\":\"") + kBinding + "\"}", nullptr, session);
          ScheduleIdleDetach(session);
          with_session(session);
        });
      });
    });
  });
}

void PreparePopup(CefRefPtr<CefBrowser> popup, Tab* tab, const std::string& extension_id,
                  std::function<void()> ready) {
  auto once = std::make_shared<std::function<void()>>(std::move(ready));
  auto go = [once] {
    if (*once) {
      auto r = std::move(*once);
      *once = nullptr;
      r();
    }
  };
  if (!cdp_pipe::available() || !popup || !tab || !tab->browser() || !ExtensionBridge::IsExtensionId(extension_id)) {
    go();
    return;
  }
  // Never wait long: the popup loads anyway.
  CefPostDelayedTask(TID_UI, base::BindOnce([](std::function<void()> g) { g(); }, std::function<void()>(go)), 1500);
  const int browser_id = tab->browser()->GetIdentifier();
  const std::string origin = OriginOf(ToUtf8(tab->url()));
  TargetOf(tab->browser(), [=](const TargetInfo& page) {
    const std::string page_target = page.id;
    if (page_target.empty()) return go();
    Pipe("Browser.getWindowForTarget", "{\"targetId\":" + JsonQuote(page_target) + "}",
         [=](const JsonValue* window, const std::string&) {
      const JsonValue* wid = window ? window->Get("windowId") : nullptr;
      if (!wid) return go();
      const long long window_id = static_cast<long long>(wid->number);
      TargetOf(popup, [=](const TargetInfo& popup_info) {
        const std::string popup_target = popup_info.id;
        if (popup_target.empty()) return go();
        Attach(popup_target, [=](const std::string& session) {
          if (session.empty()) return go();
          Context& ctx = Sessions()[session];
          ctx.extension_id = extension_id;
          ctx.tab_browser_id = browser_id;
          ctx.page_target = page_target;
          ctx.origin = IsWebUrl(origin + "/") ? origin : std::string("-");
          ctx.popup = true;
          ctx.last_used = ::GetTickCount64();
          // Enabled domains (and so the binding) carry over to the extension
          // page's process when the popup leaves about:blank.
          Pipe("Runtime.enable", "{}", nullptr, session);
          Pipe("Runtime.addBinding", std::string("{\"name\":\"") + kBinding + "\"}", nullptr, session);
          Pipe("Page.enable", "{}", nullptr, session);
          Pipe("Page.addScriptToEvaluateOnNewDocument", "{\"source\":" + JsonQuote(ShimFor(window_id)) + "}",
               [go](const JsonValue*, const std::string&) { go(); }, session);
        });
      });
    });
  });
}

}  // namespace orbit::extension_actions
