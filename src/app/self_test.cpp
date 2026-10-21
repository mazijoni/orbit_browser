#include "app/self_test.h"

#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/application.h"
#include "app/test_http_server.h"
#include "browser/downloads.h"
#include "browser/permissions.h"
#include "core/process_metrics.h"
#include "browser/browser_client.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/base/cef_callback.h"
#include "include/cef_cookie.h"
#include "include/wrapper/cef_closure_task.h"
#include "ui/browser_window.h"

namespace orbit::self_test {
namespace {

constexpr int kPollMs = 25;
constexpr int kDefaultTimeoutMs = 20000;

std::shared_ptr<TestHttpServer> g_server;  // services suite only

struct Step {
  std::string name;
  std::function<void()> action;
  std::function<bool()> done;  // polled until true (or timeout)
  int timeout_ms = kDefaultTimeoutMs;
};

class Runner {
 public:
  explicit Runner(std::wstring suite) : suite_(std::move(suite)) {}

  void Add(std::string name, std::function<void()> action, std::function<bool()> done = nullptr,
           int timeout_ms = kDefaultTimeoutMs) {
    steps_.push_back({std::move(name), std::move(action), std::move(done), timeout_ms});
  }
  void Check(bool ok, const std::string& what) {
    if (!ok) ++failures_;
    Log(std::string(ok ? "  ok   " : "  FAIL ") + what);
  }
  void Log(const std::string& line) {
    lines_.push_back(line);
    ORBIT_LOG_INFO("selftest: %s", line.c_str());
  }
  void Begin() { Schedule(0); }

 private:
  void Schedule(int delay_ms) {
    CefPostDelayedTask(TID_UI, base::BindOnce(&Runner::TickStatic, this), delay_ms);
  }
  static void TickStatic(Runner* self) { self->Tick(); }

  void Tick() {
    if (index_ >= steps_.size()) {
      Finish();
      return;
    }
    Step& step = steps_[index_];
    if (!started_) {
      started_ = true;
      start_ = ::GetTickCount64();
      Log("step: " + step.name);
      if (step.action) step.action();
    }
    if (!step.done || step.done()) {
      ++index_;
      started_ = false;
      Schedule(0);
      return;
    }
    if (::GetTickCount64() - start_ > static_cast<ULONGLONG>(step.timeout_ms)) {
      Check(false, "timed out waiting: " + step.name);
      index_ = steps_.size();  // abort: later steps depend on this one
      Schedule(0);
      return;
    }
    Schedule(kPollMs);
  }

  void Finish() {
    Application* app = Application::Get();
    Log(failures_ == 0 ? "RESULT: PASS" : "RESULT: FAIL (" + std::to_string(failures_) + ")");
    const auto path = app->user_data_dir() / (L"selftest-" + suite_ + L".txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f) {
      for (const auto& line : lines_) std::fprintf(f, "%s\n", line.c_str());
      std::fclose(f);
    }
    app->set_exit_code(failures_ == 0 ? 0 : 3);
    if (g_server) g_server->Stop();
    app->CloseAllWindows();  // exercises the normal shutdown path
  }

  std::wstring suite_;
  std::vector<Step> steps_;
  std::vector<std::string> lines_;
  size_t index_ = 0;
  bool started_ = false;
  ULONGLONG start_ = 0;
  int failures_ = 0;
};

Runner* g_runner = nullptr;
bool g_active = false;

std::wstring TestUrl(const std::wstring& query) { return L"orbit://selftest/?" + query; }

BrowserWindow* FirstWindowOf(Profile* profile) {
  for (const auto& w : Application::Get()->windows()) {
    if (w->profile() == profile && !w->closing()) return w.get();
  }
  return nullptr;
}

std::string N(const std::wstring& s) { return ToUtf8(s); }

// --- Cookie helpers (async CEF APIs -> shared result slots) ---------------

struct CookieProbe {
  bool done = false;
  std::wstring value;  // "name=value;..." of visited cookies
};

class CookieVisitor : public CefCookieVisitor {
 public:
  explicit CookieVisitor(std::shared_ptr<CookieProbe> probe) : probe_(std::move(probe)) {}
  ~CookieVisitor() override {
    // Destroyed after the last Visit (or immediately if there are none).
    CefPostTask(TID_UI, base::BindOnce([](std::shared_ptr<CookieProbe> p) { p->done = true; },
                                       probe_));
  }
  bool Visit(const CefCookie& cookie, int count, int total, bool& delete_cookie) override {
    std::wstring entry = CefString(&cookie.name).ToWString() + L"=" +
                         CefString(&cookie.value).ToWString() + L";";
    CefPostTask(TID_UI, base::BindOnce([](std::shared_ptr<CookieProbe> p,
                                          std::wstring e) { p->value += e; },
                                       probe_, entry));
    return true;
  }

 private:
  std::shared_ptr<CookieProbe> probe_;
  IMPLEMENT_REFCOUNTING(CookieVisitor);
};

class SetCookieDone : public CefSetCookieCallback {
 public:
  explicit SetCookieDone(std::shared_ptr<CookieProbe> probe) : probe_(std::move(probe)) {}
  void OnComplete(bool success) override {
    CefPostTask(TID_UI, base::BindOnce(
                            [](std::shared_ptr<CookieProbe> p, bool ok) {
                              p->value = ok ? L"set" : L"failed";
                              p->done = true;
                            },
                            probe_, success));
  }

 private:
  std::shared_ptr<CookieProbe> probe_;
  IMPLEMENT_REFCOUNTING(SetCookieDone);
};

// --- Suites ----------------------------------------------------------------

void AddBasicSuite(Runner& r) {
  Application* app = Application::Get();
  struct State {
    BrowserWindow* window = nullptr;
    Tab* tab = nullptr;
    size_t tabs_before = 0;
    int first_browser_id = 0;
  };
  auto s = std::make_shared<State>();

  r.Add("window exists", [s, app] {
    s->window = FirstWindowOf(app->profiles().front().get());
  }, [s] { return s->window != nullptr && s->window->active_tab() != nullptr; });

  r.Add("navigate to page A", [s] {
    s->tab = s->window->active_tab();
    s->tab->Navigate(TestUrl(L"t=Alpha"));
  }, [s] { return s->tab->record().title == L"Alpha" && !s->tab->loading(); });

  r.Add("navigate to page B", [s] { s->tab->Navigate(TestUrl(L"t=Bravo")); },
        [s] { return s->tab->record().title == L"Bravo" && !s->tab->loading(); });

  r.Add("back (Alt+Left shortcut)", [s, &r] {
    r.Check(s->tab->can_go_back(), "can_go_back after two navigations");
    r.Check(s->window->HandleAccelerator(VK_LEFT, false, false, true), "Alt+Left handled");
  }, [s] { return s->tab->record().title == L"Alpha" && !s->tab->loading(); });

  r.Add("forward", [s, &r] {
    r.Check(s->tab->can_go_forward(), "can_go_forward after back");
    s->tab->GoForward();
  }, [s] { return s->tab->record().title == L"Bravo" && !s->tab->loading(); });

  r.Add("address bar state follows URL", [s, &r] {
    r.Check(s->tab->url().find(L"t=Bravo") != std::wstring::npos, "tab URL is page B");
    r.Check(s->tab->security() == SecurityLevel::kInternal, "orbit:// shows internal identity");
  });

  r.Add("open 4 tabs (Ctrl+T + navigate)", [s] {
    s->tabs_before = s->window->workspace()->tabs().size();
    for (int i = 0; i < 4; ++i) {
      s->window->HandleAccelerator('T', true, false, false);
      s->window->active_tab()->Navigate(TestUrl(L"t=Tab" + std::to_wstring(i)));
    }
  }, [s] {
    const auto& tabs = s->window->workspace()->tabs();
    if (tabs.size() != s->tabs_before + 4) return false;
    for (const auto& t : tabs) {
      if (!t->has_browser()) return false;
    }
    return true;
  });

  r.Add("every tab maps to a distinct Chromium browser", [s, &r, app] {
    std::vector<int> ids;
    for (const auto& t : s->window->workspace()->tabs()) {
      const int id = t->browser()->GetIdentifier();
      r.Check(app->client()->FindTab(id) == t.get(), "browser " + std::to_string(id) +
                                                         " routes to its own tab");
      for (int other : ids) r.Check(other != id, "browser ids are unique");
      ids.push_back(id);
    }
  });

  r.Add("Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+1 switch tabs", [s, &r] {
    auto& tabs = s->window->workspace()->tabs();
    s->window->HandleAccelerator('1', true, false, false);
    r.Check(s->window->active_tab() == tabs[0].get(), "Ctrl+1 selects first tab");
    s->window->HandleAccelerator(VK_TAB, true, false, false);
    r.Check(s->window->active_tab() == tabs[1].get(), "Ctrl+Tab selects next tab");
    s->window->HandleAccelerator(VK_TAB, true, true, false);
    r.Check(s->window->active_tab() == tabs[0].get(), "Ctrl+Shift+Tab selects previous tab");
    s->window->HandleAccelerator(VK_TAB, true, true, false);
    r.Check(s->window->active_tab() == tabs.back().get(), "Ctrl+Shift+Tab wraps to last");
  });

  r.Add("reorder tab (drag equivalent) persists", [s, &r, app] {
    Workspace* ws = s->window->workspace();
    Tab* moving = ws->tabs().back().get();
    ws->MoveTab(moving, 0);
    r.Check(ws->tabs().front().get() == moving, "moved tab is first in sidebar");
    auto stored = app->store().LoadTabs(ws->id());
    r.Check(!stored.empty() && stored.front().id == moving->id(), "database order matches");
  });

  r.Add("tab groups keep members together and persist", [s, &r, app] {
    Workspace* ws = s->window->workspace();
    const auto& tabs = ws->tabs();
    Tab* a = tabs[0].get();
    Tab* b = tabs[2].get();
    const int64_t gid = ws->CreateGroup(L"Research", 0x4FB3A9, {a, b});
    r.Check(gid != 0, "group created");
    r.Check(ws->IndexOf(b) == ws->IndexOf(a) + 1, "members are contiguous after grouping");
    auto stored = app->store().LoadTabs(ws->id());
    int grouped = 0;
    for (const auto& t : stored) grouped += t.group_id == gid;
    r.Check(grouped == 2, "membership stored (" + std::to_string(grouped) + ")");
    r.Check(app->store().LoadTabGroups(ws->id()).size() == 1, "group row stored");
    // A tab opened from a grouped tab joins the group.
    Tab* child = s->window->OpenTabNear(a, TestUrl(L"t=Child"), false);
    r.Check(child->record().group_id == gid, "tab opened from a member joins the group");
    r.Check(ws->GroupSize(gid) == 3, "group has three members");
    // Leaving the group one by one prunes the empty group.
    for (Tab* t : {a, b, child}) ws->SetTabGroup(t, 0);
    r.Check(ws->groups().empty(), "empty group removed");
    r.Check(app->store().LoadTabGroups(ws->id()).empty(), "group row removed from the store");
    child->RequestClose();
  }, [s, app] { return s->window->workspace()->tabs().size() == 5; });

  r.Add("Ctrl+W closes active tab", [s] {
    s->tabs_before = s->window->workspace()->tabs().size();
    s->first_browser_id = s->window->active_tab()->browser()->GetIdentifier();
    s->window->HandleAccelerator('W', true, false, false);
  }, [s, app] {
    return s->window->workspace()->tabs().size() == s->tabs_before - 1 &&
           app->client()->FindTab(s->first_browser_id) == nullptr;
  });

  r.Add("Ctrl+Shift+T reopens it", [s] {
    s->tabs_before = s->window->workspace()->tabs().size();
    s->window->HandleAccelerator('T', true, true, false);
  }, [s] {
    return s->window->workspace()->tabs().size() == s->tabs_before + 1 &&
           s->window->active_tab()->record().title.starts_with(L"Tab");
  });

  r.Add("put background tab to sleep", [s, &r] {
    Workspace* ws = s->window->workspace();
    s->tab = ws->tabs().front().get() == s->window->active_tab() ? ws->tabs()[1].get()
                                                                  : ws->tabs().front().get();
    r.Check(s->tab->has_browser(), "background tab is live before discard");
    s->tab->Unload(/*mark_discarded=*/true);
  }, [s] { return !s->tab->has_browser() && s->tab->load_state() == Tab::LoadState::kUnloaded; });

  r.Add("sleeping tab keeps its URL and reloads on selection", [s, &r] {
    r.Check(s->tab->is_discarded(), "tab is marked discarded");
    r.Check(s->tab->url().starts_with(L"orbit://selftest/"), "URL preserved while sleeping");
    s->window->ActivateTab(s->tab);
  }, [s] { return s->tab->has_browser() && !s->tab->loading() && !s->tab->is_discarded(); });

  r.Add("popup without user gesture is blocked", [s] {
    s->tabs_before = s->window->workspace()->tabs().size();
    s->window->active_tab()->Navigate(TestUrl(L"t=Popup&op=popup"));
  }, [s] { return s->window->active_tab()->record().title == L"POPUP-TRIED"; });

  r.Add("no tab was added by the blocked popup", [s, &r] {
    r.Check(s->window->workspace()->tabs().size() == s->tabs_before,
            "tab count unchanged after window.open() without gesture");
  });
}

void AddIsolationSuite(Runner& r) {
  Application* app = Application::Get();
  struct State {
    Profile* a = nullptr;
    Profile* b = nullptr;
    BrowserWindow* wa = nullptr;
    BrowserWindow* wb = nullptr;
    std::shared_ptr<CookieProbe> probe;
  };
  auto s = std::make_shared<State>();
  const CefString cookie_url("https://isolation.orbit.test/");

  r.Add("create second profile", [s, app] {
    s->a = app->profiles().front().get();
    s->b = app->CreateProfile(L"Isolated", 0x4FB3A9);
    s->wa = FirstWindowOf(s->a);
    s->wb = app->OpenWindow(s->b, app->FreeWorkspaceFor(s->b), nullptr);
  }, [s] { return s->wa && s->wb && s->wb->active_tab(); });

  r.Add("profiles use different storage directories", [s, &r] {
    r.Check(s->a->data_dir() != s->b->data_dir(), "distinct cache_path per profile");
    // CEF silently falls back to an in-memory profile for paths it rejects.
    r.Check(s->a->request_context()->GetCachePath().ToWString() == s->a->data_dir().wstring() &&
                s->b->request_context()->GetCachePath().ToWString() ==
                    s->b->data_dir().wstring(),
            "Chromium accepted both on-disk profile paths");
    r.Check(!s->a->request_context()->IsSame(s->b->request_context()),
            "distinct CefRequestContext objects");
    r.Check(!s->a->request_context()->IsSharingWith(s->b->request_context()),
            "request contexts do not share storage");
  });

  r.Add("profile A writes localStorage + IndexedDB", [s] {
    s->wa->active_tab()->Navigate(TestUrl(L"op=write&v=secretA"));
  }, [s] { return s->wa->active_tab()->record().title == L"DONE"; });

  r.Add("profile A sets a cookie", [s, cookie_url] {
    s->probe = std::make_shared<CookieProbe>();
    CefCookie cookie;
    CefString(&cookie.name) = "k";
    CefString(&cookie.value) = "cookieA";
    CefString(&cookie.domain) = "isolation.orbit.test";
    CefString(&cookie.path) = "/";
    cookie.secure = true;
    s->a->request_context()->GetCookieManager(nullptr)->SetCookie(
        cookie_url, cookie, new SetCookieDone(s->probe));
  }, [s] { return s->probe->done; });

  r.Add("profile B cannot see A's localStorage/IndexedDB", [s] {
    s->wb->active_tab()->Navigate(TestUrl(L"op=read"));
  }, [s, &r] {
    const std::wstring& t = s->wb->active_tab()->record().title;
    if (!t.starts_with(L"R:")) return false;
    r.Check(t == L"R:null|none", "profile B read '" + N(t) + "', expected 'R:null|none'");
    return true;
  });

  r.Add("profile A still sees its own data", [s] {
    s->wa->active_tab()->Navigate(TestUrl(L"op=read&n=2"));
  }, [s, &r] {
    const std::wstring& t = s->wa->active_tab()->record().title;
    if (!t.starts_with(L"R:")) return false;
    r.Check(t == L"R:secretA|secretA", "profile A read '" + N(t) + "'");
    return true;
  });

  r.Add("profile B has no cookie for the site", [s, cookie_url] {
    s->probe = std::make_shared<CookieProbe>();
    s->b->request_context()->GetCookieManager(nullptr)->VisitUrlCookies(
        cookie_url, true, new CookieVisitor(s->probe));
  }, [s, &r] {
    if (!s->probe->done) return false;
    r.Check(s->probe->value.empty(), "profile B cookies: '" + N(s->probe->value) + "'");
    return true;
  });

  r.Add("profile A has its cookie", [s, cookie_url] {
    s->probe = std::make_shared<CookieProbe>();
    s->a->request_context()->GetCookieManager(nullptr)->VisitUrlCookies(
        cookie_url, true, new CookieVisitor(s->probe));
  }, [s, &r] {
    if (!s->probe->done) return false;
    r.Check(s->probe->value == L"k=cookieA;", "profile A cookies: '" + N(s->probe->value) + "'");
    return true;
  });

  r.Add("private window is isolated from profile A", [s, app] {
    BrowserWindow* w = app->OpenPrivateWindow();
    w->active_tab()->Navigate(TestUrl(L"op=read"));
    s->wb = w;  // reuse slot
  }, [s, &r] {
    const std::wstring& t = s->wb->active_tab()->record().title;
    if (!t.starts_with(L"R:")) return false;
    r.Check(t == L"R:null|none", "private window read '" + N(t) + "'");
    return true;
  });
}

void AddServicesSuite(Runner& r) {
  Application* app = Application::Get();
  struct State {
    BrowserWindow* window = nullptr;
    BrowserWindow* private_window = nullptr;
    std::filesystem::path dl_dir;
    size_t downloads_before = 0;
  };
  auto s = std::make_shared<State>();
  auto server = std::make_shared<TestHttpServer>();
  g_server = server;

  r.Add("start loopback server", [s, server, app, &r] {
    r.Check(server->Start(), "test HTTP server listening on 127.0.0.1");
    s->window = FirstWindowOf(app->profiles().front().get());
    s->dl_dir = app->user_data_dir() / L"downloads-test";
    std::filesystem::create_directories(s->dl_dir);
    app->store().SetPref(Store::kGlobalScope, L"download_dir", s->dl_dir.wstring());
  }, [s] { return s->window && s->window->active_tab(); });

  r.Add("visit is recorded in history", [s, server] {
    s->window->active_tab()->Navigate(server->Url(L"/page?t=HistoryVisit"));
  }, [s, app, &r] {
    Tab* t = s->window->active_tab();
    if (t->record().title != L"HistoryVisit" || t->loading()) return false;
    auto rows = app->store().QueryHistory(app->profiles().front()->id(), L"HistoryVisit", 0, 10);
    r.Check(rows.size() == 1, "one history row with the page title (" + std::to_string(rows.size()) + ")");
    return true;
  });

  r.Add("Ctrl+D bookmarks the page, again removes it", [s, app, &r] {
    const int64_t pid = app->profiles().front()->id();
    const std::wstring url = s->window->active_tab()->url();
    s->window->HandleAccelerator('D', true, false, false);
    r.Check(app->store().FindBookmarkByUrl(pid, url).has_value(), "bookmark created");
    s->window->HandleAccelerator('D', true, false, false);
    r.Check(!app->store().FindBookmarkByUrl(pid, url).has_value(), "bookmark removed");
  });

  r.Add("private window records no history", [s, server, app] {
    s->private_window = app->OpenPrivateWindow();
    s->private_window->active_tab()->Navigate(server->Url(L"/page?t=PrivateSecret"));
  }, [s, app, &r] {
    Tab* t = s->private_window->active_tab();
    if (t->record().title != L"PrivateSecret" || t->loading()) return false;
    bool leaked = false;
    for (const auto& p : app->profiles()) {
      leaked = leaked || !app->store().QueryHistory(p->id(), L"PrivateSecret", 0, 10).empty();
    }
    r.Check(!leaked, "no history row for the private visit");
    return true;
  });

  r.Add("download completes into the configured folder", [s, server, app] {
    s->downloads_before = app->store().LoadDownloads(app->profiles().front()->id(), 100).size();
    s->window->active_tab()->Navigate(server->Url(L"/file.bin"));
  }, [s, app] {
    auto list = app->store().LoadDownloads(app->profiles().front()->id(), 100);
    return list.size() == s->downloads_before + 1 && list.front().state == DownloadState::kComplete;
  });

  r.Add("downloaded file is intact and marked as from the internet", [s, app, &r] {
    auto d = app->store().LoadDownloads(app->profiles().front()->id(), 1).front();
    std::error_code ec;
    r.Check(std::filesystem::path(d.path).parent_path() == s->dl_dir, "saved in configured folder");
    r.Check(std::filesystem::file_size(d.path, ec) == TestHttpServer::kFileSize, "size matches");
    const std::wstring zone = d.path + L":Zone.Identifier";
    r.Check(::GetFileAttributesW(zone.c_str()) != INVALID_FILE_ATTRIBUTES,
            "Zone.Identifier (Mark-of-the-Web) stream present");
  });

  r.Add("second download gets a unique name", [s, server] {
    s->window->active_tab()->Navigate(server->Url(L"/file.bin"));
  }, [s, app, &r] {
    auto list = app->store().LoadDownloads(app->profiles().front()->id(), 100);
    if (list.size() != s->downloads_before + 2 || list.front().state != DownloadState::kComplete) {
      return false;
    }
    const std::wstring name = std::filesystem::path(list.front().path).filename().wstring();
    r.Check(name == L"file (1).bin", "collision renamed to '" + N(name) + "'");
    return true;
  });

  r.Add("web page cannot read or call orbit:// APIs", [s, server] {
    s->window->active_tab()->Navigate(server->Url(L"/attack"));
  }, [s, app, &r] {
    const std::wstring& t = s->window->active_tab()->record().title;
    if (!t.starts_with(L"ATTACK:")) return false;
    r.Check(t.find(L"READ:200") == std::wstring::npos, "cross-origin read blocked (" + N(t) + ")");
    r.Check(t.find(L"+header-blocked") != std::wstring::npos,
            "cross-origin request carrying the gate header was blocked");
    auto rows = app->store().QueryHistory(app->profiles().front()->id(), L"HistoryVisit", 0, 10);
    r.Check(!rows.empty(), "history survived the forged clear request");
    return true;
  });

  for (const wchar_t* page : {L"history", L"bookmarks", L"downloads", L"settings", L"newtab"}) {
    const std::wstring url = std::wstring(L"orbit://") + page + L"/";
    const std::wstring title = page == std::wstring(L"newtab") ? L"New Tab"
                               : std::wstring(1, static_cast<wchar_t>(towupper(page[0]))) + (page + 1);
    r.Add("internal page renders: " + N(url), [s, url] {
      s->window->active_tab()->Navigate(url);
    }, [s, title] {
      Tab* t = s->window->active_tab();
      return !t->loading() && t->record().title == title;
    });
  }
}

void AddMemorySuite(Runner& r) {
  Application* app = Application::Get();
  struct State {
    BrowserWindow* window = nullptr;
    Tab* audio = nullptr;
    Tab* silent = nullptr;
    Tab* pinned = nullptr;
  };
  auto s = std::make_shared<State>();
  auto server = std::make_shared<TestHttpServer>();
  g_server = server;

  r.Add("open audio, silent and pinned background tabs", [s, server, app, &r] {
    r.Check(server->Start(), "test HTTP server listening");
    s->window = FirstWindowOf(app->profiles().front().get());
    s->audio = s->window->OpenTab(server->Url(L"/audio"), false);
    s->silent = s->window->OpenTab(server->Url(L"/page?t=Silent"), false);
    s->pinned = s->window->OpenTab(server->Url(L"/page?t=Pinned"), false);
    s->pinned->mutable_record().pinned = true;
    s->window->workspace()->MoveTab(s->pinned, 0);
  }, [s] {
    return s->audio->record().title == L"PLAYING" && s->silent->record().title == L"Silent" &&
           s->pinned->record().title == L"Pinned";
  });

  r.Add("policy pass 1: renderers are asked about media", [s, app] {
    for (Tab* t : {s->audio, s->silent, s->pinned}) t->mutable_record().last_active = 0;
    app->ApplySleepPolicy(0);
  }, [s] { return ::GetTickCount64() - s->silent->media_query_sent_at() > 1100; });

  r.Add("policy pass 2: only the silent tab sleeps", [s, app] { app->ApplySleepPolicy(0); },
        [s, &r] {
          if (s->silent->load_state() != Tab::LoadState::kUnloaded) return false;
          r.Check(s->silent->is_discarded(), "silent background tab is asleep");
          r.Check(s->audio->has_browser(), "tab playing audio stays awake");
          r.Check(s->pinned->has_browser(), "pinned tab stays awake");
          r.Check(s->window->active_tab()->has_browser(), "active tab stays awake");
          return true;
        });

  r.Add("sleeping tab wakes on selection with its URL", [s] { s->window->ActivateTab(s->silent); },
        [s] { return s->silent->has_browser() && s->silent->record().title == L"Silent"; });

  r.Add("background tabs are hidden to Chromium (Page Visibility)", [s, server] {
    s->audio = s->window->OpenTab(server->Url(L"/vis?bg"), false);  // stays in background
    s->silent = s->window->OpenTab(server->Url(L"/vis?fg"), true);  // becomes active
  }, [s, &r] {
    const std::wstring bg = s->audio->record().title, fg = s->silent->record().title;
    if (bg == L"VIS:?" || bg.empty() || fg == L"VIS:?" || fg.empty()) return false;
    r.Check(fg == L"VIS:visible", "active tab reports visible (" + N(fg) + ")");
    // Known CEF Alloy limitation (docs/architecture.md section 6): windowed
    // browsers whose host window is hidden still report "visible", so
    // Chromium does not throttle them. Recorded, not asserted, so a future CEF
    // that fixes it shows up here.
    r.Log("  info background tab visibility: " + N(bg) +
          (bg == L"VIS:hidden" ? " (throttling active)" : " (known limitation: not throttled)"));
    return true;
  });

  r.Add("process metrics cover the browser tree", nullptr, [&r] {
    const auto tree = CollectProcessTree(::GetCurrentProcessId());
    int renderers = 0;
    uint64_t total = 0;
    for (const auto& m : tree) {
      renderers += m.type == L"renderer";
      total += m.private_working_set;
    }
    r.Check(tree.size() >= 3, "browser + children found (" + std::to_string(tree.size()) + ")");
    r.Check(renderers >= 1, "renderer processes identified (" + std::to_string(renderers) + ")");
    r.Log("  info total private working set: " + std::to_string(total / (1024 * 1024)) + " MB");
    return true;
  });
}

void AddSecuritySuite(Runner& r) {
  Application* app = Application::Get();
  struct State {
    BrowserWindow* window = nullptr;
  };
  auto s = std::make_shared<State>();
  auto server = std::make_shared<TestHttpServer>();
  g_server = server;

  r.Add("setup", [s, server, app, &r] {
    r.Check(server->Start(), "test HTTP server listening");
    app->permissions().SetTestAutoAnswer(false);  // the "user" clicks Block
    s->window = FirstWindowOf(app->profiles().front().get());
  }, [s] { return s->window && s->window->active_tab(); });

  r.Add("geolocation requires a prompt; Block denies it", [s, server] {
    s->window->active_tab()->Navigate(server->Url(L"/geo"));
  }, [s, app, &r] {
    const std::wstring& t = s->window->active_tab()->record().title;
    if (!t.starts_with(L"GEO:")) return false;
    r.Check(t == L"GEO:1", "PERMISSION_DENIED reported to the page (" + N(t) + ")");
    r.Check(app->permissions().test_prompt_count() == 1, "exactly one prompt was shown");
    return true;
  });

  r.Add("the decision is remembered (no second prompt)", [s] {
    s->window->active_tab()->Reload(false);
  }, [s, app, &r] {
    const std::wstring& t = s->window->active_tab()->record().title;
    if (!t.starts_with(L"GEO:") || s->window->active_tab()->loading()) return false;
    r.Check(t == L"GEO:1", "still denied after reload");
    r.Check(app->permissions().test_prompt_count() == 1, "no new prompt");
    const auto entries = app->permissions().List(app->profiles().front().get());
    r.Check(entries.size() == 1 && !entries[0].allowed, "one stored 'blocked' decision");
    return true;
  });

  r.Add("notifications are always denied without prompting", [s, server] {
    s->window->active_tab()->Navigate(server->Url(L"/notify"));
  }, [s, app, &r] {
    const std::wstring& t = s->window->active_tab()->record().title;
    if (!t.starts_with(L"NOTIFY:")) return false;
    r.Check(t == L"NOTIFY:denied", "Notification.requestPermission() -> " + N(t));
    r.Check(app->permissions().test_prompt_count() == 1, "no prompt for notifications");
    return true;
  });

  r.Add("expired certificate is refused (needs internet)", [s] {
    s->window->active_tab()->Navigate(L"https://expired.badssl.com/");
  }, [s, &r] {
    Tab* t = s->window->active_tab();
    if (t->loading() || t->last_error() == 0) return false;
    const int err = t->last_error();
    if (err == -105 || err == -106 || err == -109 || err == -118 || err == -137) {
      r.Log("  skip offline (net error " + std::to_string(err) + ")");
      return true;
    }
    r.Check(err == -201, "ERR_CERT_DATE_INVALID (-201), got " + std::to_string(err));
    r.Check(t->security() == SecurityLevel::kDangerous, "address bar shows the danger state");
    return true;
  }, 30000);
}

// Two-phase session restore test: phase 1 builds state and exits normally;
// phase 2 (a separate launch on the same --user-data-dir) verifies it.
void AddRestorePhase1(Runner& r) {
  Application* app = Application::Get();
  auto window = std::make_shared<BrowserWindow*>(nullptr);
  r.Add("build session", [window, app] {
    *window = FirstWindowOf(app->profiles().front().get());
    BrowserWindow* w = *window;
    w->active_tab()->Navigate(TestUrl(L"op=write&v=persisted"));
    for (const wchar_t* name : {L"One", L"Two", L"Three"}) {
      w->OpenTab(TestUrl(std::wstring(L"t=") + name), false);
    }
  }, [window] {
    // Every page reports completion through its title (never the placeholder).
    for (const auto& t : (*window)->workspace()->tabs()) {
      if (t->loading() || t->record().title.empty() || t->record().title == L"Loading") {
        return false;
      }
    }
    return true;
  });
  r.Add("pin 'Two', activate 'Three'", [window] {
    Workspace* ws = (*window)->workspace();
    Tab* two = ws->tabs()[2].get();
    two->mutable_record().pinned = true;
    ws->MoveTab(two, 0);
    (*window)->ActivateTab(ws->tabs().back().get());
    const int64_t gid = ws->CreateGroup(L"Saved group", 0xE0905A, {ws->tabs()[2].get()});
    TabGroupRecord g = *ws->FindGroup(gid);
    g.collapsed = true;
    ws->UpdateGroup(g);
    Application::Get()->SaveNow();
  });
}

void AddRestorePhase2(Runner& r) {
  Application* app = Application::Get();
  r.Add("session restored", nullptr, [app] {
    BrowserWindow* w = FirstWindowOf(app->profiles().front().get());
    return w && w->active_tab() && w->active_tab()->has_browser();
  });
  r.Add("tab order, pin and active tab", [app, &r] {
    BrowserWindow* w = FirstWindowOf(app->profiles().front().get());
    const auto& tabs = w->workspace()->tabs();
    std::wstring titles;
    for (const auto& t : tabs) titles += t->record().title + L",";
    r.Check(tabs.size() == 4, "4 tabs restored (" + N(titles) + ")");
    if (tabs.size() != 4) return;
    r.Check(tabs[0]->record().title == L"Two" && tabs[0]->pinned(), "pinned 'Two' is first");
    r.Check(tabs[1]->record().title == L"DONE", "write tab second");
    r.Check(tabs[2]->record().title == L"One" && tabs[3]->record().title == L"Three",
            "remaining order preserved");
    r.Check(w->active_tab() == tabs[3].get(), "previously active tab is active");
    const auto& groups = w->workspace()->groups();
    r.Check(groups.size() == 1 && groups[0].name == L"Saved group" && groups[0].collapsed,
            "collapsed tab group restored");
    r.Check(tabs[2]->record().group_id != 0 && groups.size() == 1 &&
                tabs[2]->record().group_id == groups[0].id,
            "'One' is still in the group");
    int live = 0;
    for (const auto& t : tabs) live += t->load_state() != Tab::LoadState::kUnloaded;
    r.Check(live == 1, "only the active tab was loaded at startup (lazy restore), live=" +
                           std::to_string(live));
  });
  r.Add("site storage persisted across restart", [app] {
    FirstWindowOf(app->profiles().front().get())->active_tab()->Navigate(TestUrl(L"op=read"));
  }, [app, &r] {
    const std::wstring& t =
        FirstWindowOf(app->profiles().front().get())->active_tab()->record().title;
    if (!t.starts_with(L"R:")) return false;
    r.Check(t == L"R:persisted|persisted", "read after restart: '" + N(t) + "'");
    return true;
  });
}

}  // namespace

bool IsActive() { return g_active; }

void Start(const std::wstring& suite) {
  g_active = true;
  g_runner = new Runner(suite);  // lives until process exit
  if (suite == L"basic") {
    AddBasicSuite(*g_runner);
  } else if (suite == L"isolation") {
    AddIsolationSuite(*g_runner);
  } else if (suite == L"services") {
    AddServicesSuite(*g_runner);
  } else if (suite == L"memory") {
    AddMemorySuite(*g_runner);
  } else if (suite == L"security") {
    AddSecuritySuite(*g_runner);
  } else if (suite == L"restore-1") {
    AddRestorePhase1(*g_runner);
  } else if (suite == L"restore-2") {
    AddRestorePhase2(*g_runner);
  } else {
    g_runner->Check(false, "unknown suite " + N(suite));
  }
  g_runner->Begin();
}


}  // namespace orbit::self_test
