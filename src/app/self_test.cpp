#include "app/self_test.h"
#include "core/live_folders.h"
#include "core/space_theme.h"
#include "core/boosts.h"

#include <windows.h>

#include <crtdbg.h>
#include <csignal>
#include <dbghelp.h>

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
#include "core/shortcuts.h"
#include "core/strings.h"
#include "ui/graphics.h"
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
      // Ctrl+T opens the floating URL bar; Enter creates the tab (Zen).
      s->window->HandleAccelerator('T', true, false, false);
      s->window->TypeInAddressBar(TestUrl(L"t=Tab" + std::to_wstring(i)));
      s->window->SubmitAddressBar();
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
    // A pinned page different from the current one: must survive the restart.
    two->SetPinnedUrl(TestUrl(L"t=TwoHome"));
    ws->tabs().back()->SetCustomTitle(L"Labelled Three");  // 'Three', persisted label
    // An Essential must come back after the restart, outside every space.
    Tab* ess = (*window)->OpenTab(TestUrl(L"t=EssentialRestored"), false);
    (*window)->AddToEssentials(ess);
    // zen.pinned-tab-manager.restore-pinned-tabs-to-pinned-url, checked after restart.
    Application::Get()->store().SetPref(Store::kGlobalScope, L"restore_pinned_to_pinned_url", L"1");
    // Phase 2 checks the last selected tab comes back (Zen's default, off, is
    // phase 3's empty tab).
    Application::Get()->store().SetPref(Store::kGlobalScope, L"continue_where_left_off", L"1");
    ws->MoveTab(two, 0);
    (*window)->ActivateTab(ws->tabs().back().get());
    const int64_t gid = ws->CreateGroup(L"Saved group", 0xE0905A, {});
    TabGroupRecord g = *ws->FindGroup(gid);
    g.collapsed = true;
    ws->UpdateGroup(g);
    // 'One' lives in a subfolder with an emoji icon (ZenFolders nesting).
    const int64_t sub = ws->CreateGroup(L"Saved subfolder", 0x4FB3A9, {ws->tabs()[2].get()}, gid);
    TabGroupRecord sg = *ws->FindGroup(sub);
    sg.icon = L"📚";
    ws->UpdateGroup(sg);
    // A visited empty space gets a hidden placeholder, which must stay hidden.
    Application::Get()->CreateWorkspace((*window)->profile(), L"EmptyRestored", 0x4FB3A9);
    (*window)->HandleAccelerator(VK_RIGHT, true, false, true);
    (*window)->HandleAccelerator(VK_LEFT, true, false, true);
    // A glance left open at exit is not restored (ZenGlanceManager.onUnload).
    (*window)->OpenGlance((*window)->active_tab(), TestUrl(L"t=GlanceAtExit"));
    // A split with custom sizes in a space that is not on screen must come back.
    {
      Workspace* other = Application::Get()->CreateWorkspace((*window)->profile(), L"SplitRestored",
                                                            0xE0905A);
      std::vector<Tab*> pair;
      for (const wchar_t* name : {L"Left", L"Right"}) {
        TabRecord rec;
        rec.url = TestUrl(std::wstring(L"t=") + name);
        rec.title = name;
        pair.push_back(other->InsertTab(std::make_unique<Tab>(other, rec), -1));
      }
      const int64_t id = other->CreateSplit(pair, SplitGrid::kVertical);
      if (Workspace::SplitView* split = other->FindSplit(id)) {
        split->root.children[0].size = 30.f;
        split->root.children[1].size = 70.f;
        other->SaveSplit(id);
      }
    }
    Application::Get()->SaveNow();
  });
  // A live folder (in a space that is not on screen) with a dismissed item.
  auto server = std::make_shared<TestHttpServer>();
  auto live = std::make_shared<int64_t>(0);
  r.Add("live folder", [window, server, live, &r] {
    r.Check(server->Start(), "test HTTP server listening");
    Workspace* ws = Application::Get()->CreateWorkspace((*window)->profile(), L"LiveRestored", 0x7C8CFF);
    Application::Get()->live_folders().Create(ws, LiveFolderState::Kind::kRss,
                                              server->Url(L"/feed?items=x,y"),
                                              [live](int64_t id) { *live = id; });
  }, [live] { return *live != 0; });
  r.Add("dismiss one live item", [window, server, live, &r] {
    Workspace* ws = nullptr;
    for (const auto& w : (*window)->profile()->workspaces()) {
      if (w->record().name == L"LiveRestored") ws = w.get();
    }
    Tab* y = nullptr;
    for (const auto& t : ws->tabs()) {
      if (t->record().live_item.ends_with(L":item-y")) y = t.get();
    }
    r.Check(y != nullptr, "'y' was added");
    if (y) y->RequestClose();
    server->Stop();
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
    r.Check(tabs[0]->pinned() && tabs[0]->pinned_url() == TestUrl(L"t=TwoHome"),
            "pinned 'Two' is first, its pinned URL restored ('" + N(tabs[0]->pinned_url()) + "')");
    r.Check(tabs[0]->url() == TestUrl(L"t=TwoHome") && !tabs[0]->pin_changed(),
            "restore-to-pinned-URL setting reset it to its pinned page ('" + N(tabs[0]->url()) + "')");
    r.Check(tabs[1]->record().title == L"DONE", "write tab second");
    r.Check(tabs[2]->record().title == L"One" && tabs[3]->record().title == L"Three",
            "remaining order preserved");
    r.Check(tabs[3]->DisplayTitle() == L"Labelled Three", "custom tab label restored");
    const Workspace* ess = w->profile()->essentials();
    r.Check(ess && ess->tabs().size() == 1 &&
                ess->tabs()[0]->url() == TestUrl(L"t=EssentialRestored") && ess->tabs()[0]->pinned(),
            "the Essential was restored outside the spaces");
    r.Check(w->active_tab() == tabs[3].get(), "previously active tab is active");
    bool glance_restored = false;
    for (const auto& t : tabs) glance_restored |= t->url() == TestUrl(L"t=GlanceAtExit");
    r.Check(!glance_restored, "a glance open at exit was not restored");
    {
      const Workspace* other = nullptr;
      for (const auto& ws : w->profile()->workspaces()) {
        if (ws->record().name == L"SplitRestored") other = ws.get();
      }
      bool ok = other && other->splits().size() == 1 &&
                other->splits()[0].root.children.size() == 2;
      if (ok) {
        const auto members = other->SplitMembers(other->splits()[0]);
        const float left = other->splits()[0].root.children[0].size;
        ok = members.size() == 2 && members[0]->record().title == L"Left" &&
             members[1]->record().title == L"Right" && left > 29.f && left < 31.f;
      }
      r.Check(ok, "the split view came back with its sizes");
    }
    const Workspace* empty = nullptr;
    for (const auto& ws : w->profile()->workspaces()) {
      if (ws->record().name == L"EmptyRestored") empty = ws.get();
    }
    r.Check(empty && empty->tabs().size() == 1 && empty->tabs()[0]->empty_placeholder(),
            "an empty space's placeholder comes back hidden");
    const Workspace* home = w->workspace();
    const TabGroupRecord* outer = nullptr;
    const TabGroupRecord* inner = nullptr;
    for (const auto& g : home->groups()) {
      if (g.name == L"Saved group") outer = &g;
      if (g.name == L"Saved subfolder") inner = &g;
    }
    r.Check(home->groups().size() == 2 && outer && outer->collapsed && outer->parent_id == 0,
            "collapsed tab group restored");
    r.Check(inner && outer && inner->parent_id == outer->id && inner->icon == L"📚",
            "its subfolder came back inside it, with its icon");
    r.Check(inner && tabs[2]->record().group_id == inner->id, "'One' is still in the subfolder");
    // The live folder: its settings, its item tab and the dismissal.
    const Workspace* lws = nullptr;
    for (const auto& ws : w->profile()->workspaces()) {
      if (ws->record().name == L"LiveRestored") lws = ws.get();
    }
    const TabGroupRecord* lg = lws && !lws->groups().empty() ? &lws->groups()[0] : nullptr;
    const auto* le = lg ? app->live_folders().Find(lg->id) : nullptr;
    r.Check(le && le->state.kind == LiveFolderState::Kind::kRss && le->state.url.find(L"/feed?items=x,y") != std::wstring::npos &&
                le->state.last_fetched > 0,
            "the live folder came back with its feed");
    r.Check(le && le->dismissed.count(LiveCompositeId(lg->id, L"item-y")), "with its dismissed item");
    int items = 0;
    if (lws) {
      for (const auto& t : lws->tabs()) items += t->record().live_item == LiveCompositeId(lg->id, L"item-x");
    }
    r.Check(items == 1, "and its item tab");
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
  r.Add("turn 'Continue where you left off' off for phase 3", [app] {
    app->store().SetPref(Store::kGlobalScope, L"continue_where_left_off", L"0");
    app->SaveNow();
  });
}

// Phase 3: zen.workspaces.continue-where-left-off off restores the tabs but
// opens on an empty tab with the URL bar, loading none of them.
void AddRestorePhase3(Runner& r) {
  Application* app = Application::Get();
  r.Add("session restored onto an empty tab", nullptr, [app] {
    BrowserWindow* w = FirstWindowOf(app->profiles().front().get());
    return w && w->active_tab();
  });
  r.Add("continue where left off: off", [app, &r] {
    BrowserWindow* w = FirstWindowOf(app->profiles().front().get());
    Tab* active = w->active_tab();
    r.Check(active->empty_placeholder() && active->url() == Application::kNewTabUrl,
            "the active tab is the empty tab ('" + N(active->url()) + "')");
    r.Check(w->floating_url_bar_open(), "the URL bar is open on it");
    int restored = 0, live = 0;
    for (const auto& t : w->workspace()->tabs()) {
      if (t.get() == active) continue;
      ++restored;
      live += t->load_state() != Tab::LoadState::kUnloaded;
    }
    r.Check(restored == 4, "the 4 tabs were restored (" + std::to_string(restored) + ")");
    r.Check(live == 0, "none of them was loaded, live=" + std::to_string(live));
  });
  // The empty tab stays hidden from the strip ([zen-empty-tab]), as Zen's.
  r.Add("selecting a restored tab loads it", [app] {
    BrowserWindow* w = FirstWindowOf(app->profiles().front().get());
    w->CancelAddressBar();
    w->ActivateTab(w->workspace()->tabs()[1].get());
  }, [app, &r] {
    BrowserWindow* w = FirstWindowOf(app->profiles().front().get());
    Tab* t = w->workspace()->tabs()[1].get();
    if (w->active_tab() != t || !t->has_browser() || t->loading()) return false;
    r.Check(!w->floating_url_bar_open(), "the URL bar closed");
    return true;
  });
}


// Zen parity features (docs/zen-parity/). Each step drives the same entry
// points as the UI (shortcuts, commands) and checks the real outcome.
std::wstring ClipboardText() {
  std::wstring text;
  if (!::OpenClipboard(nullptr)) return text;
  if (HANDLE data = ::GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* chars = static_cast<const wchar_t*>(::GlobalLock(data))) {
      text = chars;
      ::GlobalUnlock(data);
    }
  }
  ::CloseClipboard();
  return text;
}

void AddZenSuite(Runner& r) {
  Application* app = Application::Get();
  struct State {
    BrowserWindow* window = nullptr;
    Tab* tab = nullptr;
    size_t tabs_before = 0;
  };
  auto s = std::make_shared<State>();

  r.Add("window exists", [s, app] {
    s->window = FirstWindowOf(app->profiles().front().get());
  }, [s] { return s->window != nullptr && s->window->active_tab() != nullptr; });

  r.Add("load a page", [s] {
    s->tab = s->window->active_tab();
    s->tab->Navigate(TestUrl(L"t=Copy"));
  }, [s] { return s->tab->record().title == L"Copy" && !s->tab->loading(); });

  r.Add("Ctrl+Shift+C copies the current URL and shows a toast", [s, &r] {
    r.Check(s->window->HandleAccelerator('C', true, true, false), "Ctrl+Shift+C handled");
    r.Check(ClipboardText() == s->tab->url(),
            "clipboard holds the tab URL ('" + N(ClipboardText()) + "')");
    r.Check(s->window->visible_toast_id() == L"copy-url", "\"Copied current URL!\" toast shown");
  });

  r.Add("the copy toast hides after its 3 s timeout", nullptr,
        [s] { return s->window->visible_toast_id().empty(); }, 6000);

  r.Add("set up: one pinned tab and three unpinned tabs", [s] {
    Workspace* ws = s->window->workspace();
    ws->tabs().front()->mutable_record().pinned = true;  // the first tab
    for (int i = 0; i < 3; ++i) s->window->OpenTab(TestUrl(L"t=Unpinned" + std::to_wstring(i)), false);
    s->tab = ws->tabs().back().get();
    s->window->ActivateTab(s->tab);
  }, [s] {
    for (const auto& t : s->window->workspace()->tabs()) {
      if (t->loading() || !t->has_browser()) return false;
    }
    return s->window->workspace()->tabs().size() == 4;
  });

  r.Add("an inactive window greys out (zen.view.grey-out-inactive-windows)", [s, &r] {
    const HWND h = s->window->hwnd();
    ::SendMessageW(h, WM_ACTIVATEAPP, FALSE, 0);
    r.Check(s->window->greyed_out(), "greyed out when another application is active");
    ::SendMessageW(h, WM_ACTIVATE, WA_ACTIVE, 0);
    r.Check(!s->window->greyed_out(), "back to colour when active");
  });
  auto markdown = std::make_shared<std::wstring>();
  r.Add("pinning a tab moves the rows with the tween", [s, &r] {
    Tab* t = s->window->active_tab();
    r.Check(s->window->HandleAccelerator('D', true, true, false) && t->pinned(), "Ctrl+Shift+D pinned it");
    r.Check(s->window->rows_animating() == ui::AnimationsEnabled(), "the rows tween");
    s->window->HandleAccelerator('D', true, true, false);
    r.Check(!t->pinned(), "unpinned again");
  });
  r.Add("Ctrl+Shift+Alt+C copies the URL as Markdown", [s, markdown, &r] {
    Tab* t = s->window->active_tab();
    *markdown = L"[" + t->DisplayTitle() + L"](" + t->url() + L")";
    r.Check(s->window->HandleAccelerator('C', true, true, true), "handled");
  }, [markdown] { return ClipboardText() == *markdown; });  // the clipboard may be busy for a moment
  r.Add("Ctrl+Shift+K closes unpinned tabs except the selected one", [s, &r] {
    r.Check(s->window->HandleAccelerator('K', true, true, false), "Ctrl+Shift+K handled");
    r.Check(s->window->visible_toast_id() == L"tabs-closed", "\"Tabs Closed!\" toast shown");
  }, [s] { return s->window->workspace()->tabs().size() == 2; });

  r.Add("pinned and selected tabs survive", [s, &r] {
    const auto& tabs = s->window->workspace()->tabs();
    r.Check(tabs.front()->pinned(), "pinned tab kept");
    r.Check(s->window->active_tab() == s->tab, "selected tab kept and still active");
  });

  r.Add("Ctrl+Shift+T reopens the whole batch", [s, &r] {
    r.Check(s->window->HandleAccelerator('T', true, true, false), "Ctrl+Shift+T handled");
  }, [s] { return s->window->workspace()->tabs().size() == 4; });

  // Selection on close: owner first, then most recently used.
  struct Pick {
    Tab* a = nullptr;
    Tab* b = nullptr;
    Tab* c = nullptr;
    Tab* child = nullptr;
  };
  auto p = std::make_shared<Pick>();
  r.Add("activate tabs C, then A, then B", [s, p] {
    const auto& tabs = s->window->workspace()->tabs();
    p->a = tabs[1].get();
    p->b = tabs[2].get();
    p->c = tabs[3].get();
    s->window->ActivateTab(p->c);
  }, [p] { return true; });
  r.Add("activate A", [s, p] { s->window->ActivateTab(p->a); }, nullptr);
  r.Add("activate B (after a moment, so timestamps differ)", nullptr, [s, p] {
    static ULONGLONG start = ::GetTickCount64();
    if (::GetTickCount64() - start < 50) return false;
    s->window->ActivateTab(p->b);
    return true;
  });
  r.Add("closing B selects the most recently used tab (A), not its neighbour (C)", [s] {
    s->window->HandleAccelerator('W', true, false, false);
  }, [s, p, &r] {
    if (s->window->workspace()->IndexOf(p->b) >= 0) return false;
    r.Check(s->window->active_tab() == p->a, "A selected after closing B");
    return true;
  });

  r.Add("a foreground tab opened from A is owned by A", [s, p, &r] {
    s->window->ActivateTab(p->a);
    p->child = s->window->OpenTabNear(p->a, TestUrl(L"t=Child"), true);
    r.Check(p->child->owner() == p->a, "owner set");
  }, [p] { return p->child->has_browser() && !p->child->loading() &&
                  p->child->record().title == L"Child"; });

  r.Add("Back with no history closes the owned tab and returns to its owner", [s, p, &r] {
    r.Check(!p->child->can_go_back(), "child has no history");
    r.Check(s->window->HandleAccelerator(VK_LEFT, false, false, true), "Alt+Left handled");
  }, [s, p] {
    return s->window->workspace()->IndexOf(p->child) < 0 && s->window->active_tab() == p->a;
  });

  r.Add("switching away clears the owner", [s, p, &r] {
    Tab* child = s->window->OpenTabNear(p->a, TestUrl(L"t=Child2"), true);
    s->window->ActivateTab(p->c);
    r.Check(child->owner() == nullptr, "owner cleared after switching away");
  });

  r.Add("Ctrl+Shift+D pins and unpins the active tab", [s, p, &r] {
    s->window->ActivateTab(p->c);
    r.Check(!p->c->pinned(), "C starts unpinned");
    s->window->HandleAccelerator('D', true, true, false);
    r.Check(p->c->pinned(), "Ctrl+Shift+D pinned C");
    r.Check(s->window->workspace()->IndexOf(p->c) < s->window->workspace()->PinnedCount(),
            "C moved into the pinned section");
    s->window->HandleAccelerator('D', true, true, false);
    r.Check(!p->c->pinned(), "Ctrl+Shift+D unpinned C");
  });

  r.Add("Ctrl+T opens the floating URL bar without creating a tab", [s, &r] {
    s->tab = s->window->active_tab();
    const size_t before = s->window->workspace()->tabs().size();
    r.Check(s->window->HandleAccelerator('T', true, false, false), "Ctrl+T handled");
    r.Check(s->window->new_tab_mode(), "floating bar open in new-tab mode");
    r.Check(s->window->workspace()->tabs().size() == before, "no tab created yet");
    r.Check(s->window->active_tab() == s->tab, "the current tab stays active underneath");
  });

  r.Add("Escape closes it without a tab, and the typed text comes back", [s, &r] {
    const size_t before = s->window->workspace()->tabs().size();
    s->window->TypeInAddressBar(L"half-typed search");
    s->window->CancelAddressBar();
    r.Check(!s->window->floating_url_bar_open(), "bar closed");
    r.Check(s->window->workspace()->tabs().size() == before, "still no new tab");
    s->window->HandleAccelerator('T', true, false, false);
    wchar_t text[64] = {};
    ::GetWindowTextW(::GetFocus(), text, 64);
    r.Check(std::wstring(text) == L"half-typed search",
            "text restored within 45 s ('" + N(text) + "')");
  });

  r.Add("Ctrl+T again closes the bar", [s, &r] {
    s->window->HandleAccelerator('T', true, false, false);
    r.Check(!s->window->floating_url_bar_open(), "second Ctrl+T closed the bar");
  });

  r.Add("Enter in the new-tab bar opens the URL in a new tab", [s] {
    s->tabs_before = s->window->workspace()->tabs().size();
    s->window->HandleAccelerator('T', true, false, false);
    s->window->TypeInAddressBar(TestUrl(L"t=FromBar"));
    s->window->SubmitAddressBar();
  }, [s, &r] {
    if (s->window->workspace()->tabs().size() != s->tabs_before + 1) return false;
    Tab* t = s->window->active_tab();
    if (t->record().title != L"FromBar") return false;
    r.Check(t != s->tab, "a new tab, not the previous one");
    r.Check(!s->window->floating_url_bar_open(), "bar closed after Enter");
    return true;
  });

  r.Add("Ctrl+L opens the floating bar for the current tab", [s, &r] {
    Tab* current = s->window->active_tab();
    s->window->HandleAccelerator('L', true, false, false);
    r.Check(s->window->floating_url_bar_open() && !s->window->new_tab_mode(),
            "floating bar open in current-tab mode");
    s->window->TypeInAddressBar(TestUrl(L"t=SameTab"));
    s->window->SubmitAddressBar();
    s->tab = current;
  }, [s] { return s->tab->record().title == L"SameTab"; });

  // Zen pinned tab manager.
  r.Add("pinning remembers the pinned page", [s] {
    s->tab = s->window->OpenTab(TestUrl(L"t=PinHome"), true);
  }, [s, &r] {
    if (s->tab->record().title != L"PinHome" || s->tab->loading()) return false;
    s->window->HandleAccelerator('D', true, true, false);
    r.Check(s->tab->pinned() && s->tab->pinned_url() == s->tab->url(), "pinned URL recorded");
    r.Check(!s->tab->pin_changed(), "not changed at its own page");
    return true;
  });
  r.Add("navigating away marks the pinned tab as changed", [s] {
    s->tab->Navigate(TestUrl(L"t=PinAway"));
  }, [s, &r] {
    if (s->tab->record().title != L"PinAway" || s->tab->loading()) return false;
    r.Check(s->tab->pin_changed(), "pin_changed after navigating away");
    r.Check(s->tab->can_go_back(), "has history before the reset");
    return true;
  });
  r.Add("Reset Pinned Tab returns to the pinned page with fresh history", [s] {
    s->window->ResetPinnedTab(s->tab, false);
  }, [s, &r] {
    if (!s->tab->has_browser() || s->tab->loading() || s->tab->record().title != L"PinHome") {
      return false;
    }
    r.Check(!s->tab->pin_changed(), "back at the pinned page");
    r.Check(!s->tab->can_go_back(), "history reset to one entry");
    r.Check(s->window->active_tab() == s->tab, "still the selected tab");
    return true;
  });
  r.Add("Replace with Current URL adopts the current page", [s] {
    s->tab->Navigate(TestUrl(L"t=PinNew"));
  }, [s, &r] {
    if (s->tab->record().title != L"PinNew" || s->tab->loading()) return false;
    s->window->ReplacePinnedUrlWithCurrent(s->tab);
    r.Check(!s->tab->pin_changed() && s->tab->pinned_url() == s->tab->url(), "pinned URL replaced");
    r.Check(s->window->visible_toast_id() == L"pinned-replaced", "confirmation toast");
    return true;
  });
  r.Add("Edit Pinned URL validates input", [s, &r] {
    r.Check(!s->window->SetPinnedUrlFromText(s->tab, L"just some words"), "a search is rejected");
    r.Check(s->window->visible_toast_id() == L"pinned-url-invalid", "invalid-URL toast");
    r.Check(s->window->SetPinnedUrlFromText(s->tab, L"example.com/start"), "an address is accepted");
    r.Check(s->tab->pinned_url() == L"https://example.com/start",
            "stored fixed-up URL ('" + N(s->tab->pinned_url()) + "')");
    r.Check(s->tab->pin_changed(), "now away from the new pinned page");
    s->tab->SetPinnedUrl(s->tab->url());  // undo for the next step
  });
  r.Add("Ctrl+W on a pinned tab resets, unloads and switches (not closes)", [s] {
    s->tab->Navigate(TestUrl(L"t=PinW"));
  }, [s, &r] {
    if (s->tab->record().title != L"PinW" || s->tab->loading()) return false;
    const size_t before = s->window->workspace()->tabs().size();
    s->window->HandleAccelerator('W', true, false, false);
    r.Check(s->window->workspace()->tabs().size() == before, "the pinned tab was not closed");
    r.Check(s->window->active_tab() != s->tab, "switched to another tab");
    r.Check(!s->tab->pin_changed(), "URL reset to the pinned page");
    return true;
  });
  r.Add("... and the pinned tab ends up unloaded", nullptr, [s] {
    return !s->tab->has_browser() && s->tab->load_state() == Tab::LoadState::kUnloaded;
  });
  r.Add("unpinning forgets the pinned page", [s, &r] {
    s->window->ActivateTab(s->tab);
    s->window->HandleAccelerator('D', true, true, false);
    r.Check(!s->tab->pinned() && s->tab->pinned_url().empty(), "pinned URL cleared");
  });

  // URL-bar results (urlbar view).
  r.Add("open a tab to find later", [s] {
    s->tab = s->window->OpenTab(TestUrl(L"t=Omnibox Target Page"), false);
  }, [s] { return s->tab->record().title == L"Omnibox Target Page" && !s->tab->loading(); });
  r.Add("typing in the new-tab bar lists a Switch to Tab result", [s, &r] {
    s->tabs_before = s->window->workspace()->tabs().size();
    s->window->HandleAccelerator('T', true, false, false);
    s->window->TypeInAddressBar(L"omnibox target");
    const auto& rows = s->window->url_results();
    r.Check(!rows.empty() && rows[0].kind == OmniboxRow::Kind::kSearch, "search heuristic first");
    int index = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].kind == OmniboxRow::Kind::kSwitchTab &&
          rows[i].tab_key == reinterpret_cast<intptr_t>(s->tab)) {
        index = static_cast<int>(i);
      }
    }
    r.Check(index > 0, "the open tab is offered (row " + std::to_string(index) + ")");
    // Down arrow moves the selection; Enter switches.
    for (int i = 0; i < index; ++i) ::SendMessageW(::GetFocus(), WM_KEYDOWN, VK_DOWN, 0);
    r.Check(s->window->selected_url_result() == index, "arrow keys select it");
    ::SendMessageW(::GetFocus(), WM_KEYDOWN, VK_RETURN, 0);
    r.Check(s->window->active_tab() == s->tab, "Enter switched to the tab");
    r.Check(s->window->workspace()->tabs().size() == s->tabs_before, "no tab was created");
    r.Check(!s->window->floating_url_bar_open(), "the bar closed");
  });
  r.Add("bookmarks are listed and open in the current tab", [s, app, &r] {
    BookmarkRecord b;
    b.profile_id = s->window->profile()->id();
    b.title = L"Zen parity bookmark";
    b.url = TestUrl(L"t=Bookmarked");
    app->store().AddBookmark(b);
    s->window->HandleAccelerator('L', true, false, false);
    s->window->TypeInAddressBar(L"parity bookmark");
    const auto& rows = s->window->url_results();
    const bool listed = rows.size() >= 2 && rows[1].kind == OmniboxRow::Kind::kBookmark &&
                        rows[1].url == b.url;
    r.Check(listed, "bookmark row after the heuristic");
    if (listed) ::SendMessageW(::GetFocus(), WM_KEYDOWN, VK_DOWN, 0);
    ::SendMessageW(::GetFocus(), WM_KEYDOWN, VK_RETURN, 0);
  }, [s] { return s->tab->record().title == L"Bookmarked" && !s->tab->loading(); });
  r.Add("typing in the sidebar field grows it in place (breakout)", [s, &r] {
    s->window->ActivateTab(s->tab);
    ::SetFocus(nullptr);
    s->window->FocusAddressBarForTest();
    r.Check(!s->window->floating_url_bar_open(), "focusing by click does not float");
    s->window->TypeInAddressBar(L"zen");
    r.Check(s->window->url_bar_breakout(), "typing opened the anchored breakout bar");
    r.Check(!s->window->url_results().empty(), "with results");
    s->window->CancelAddressBar();
    r.Check(!s->window->floating_url_bar_open(), "Escape closes it");
  });

  // Renaming (ZenTabRenamer) and space actions.
  r.Add("Change Label renames a tab inline; Enter commits and persists", [s, app, &r] {
    s->window->ActivateTab(s->tab);
    s->window->StartRenameTab(s->tab);
    r.Check(s->window->renaming(), "inline editor shown");
    HWND edit = ::GetFocus();
    ::SetWindowTextW(edit, L"  My   renamed  tab ");
    ::SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
    r.Check(!s->window->renaming(), "editor closed");
    r.Check(s->tab->DisplayTitle() == L"My renamed tab", "whitespace collapsed and trimmed ('" +
                                                          N(s->tab->DisplayTitle()) + "')");
    r.Check(s->window->visible_toast_id() == L"tab-renamed", "\"renamed\" toast");
    app->SaveNow();
    bool stored = false;
    for (const auto& rec : app->store().LoadTabs(s->window->workspace()->id())) {
      stored = stored || rec.custom_title == L"My renamed tab";
    }
    r.Check(stored, "label stored");
  });
  r.Add("Escape cancels a rename; an empty name restores the page title", [s, &r] {
    s->window->StartRenameTab(s->tab);
    HWND edit = ::GetFocus();
    ::SetWindowTextW(edit, L"Discarded");
    ::SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
    r.Check(s->tab->DisplayTitle() == L"My renamed tab", "Escape kept the old label");
    s->window->StartRenameTab(s->tab);
    edit = ::GetFocus();
    ::SetWindowTextW(edit, L"   ");
    ::SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
    r.Check(s->tab->custom_title().empty() && s->tab->DisplayTitle() == s->tab->record().title,
            "empty name went back to the page title");
  });
  r.Add("the space name renames inline too", [s, &r] {
    Workspace* ws = s->window->workspace();
    const std::wstring old = ws->record().name;
    s->window->StartRenameWorkspace(ws);
    HWND edit = ::GetFocus();
    ::SetWindowTextW(edit, L"Renamed Space");
    ::SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
    r.Check(ws->record().name == L"Renamed Space", "space renamed");
    r.Check(s->window->visible_toast_id() == L"workspace-renamed", "toast");
    ws->mutable_record().name = old;
  });
  r.Add("Change Icon keeps one symbol (an emoji with its modifiers)", [s, app, &r] {
    Workspace* ws = s->window->workspace();
    s->window->SetWorkspaceIcon(ws, L" \U0001F680\U0001F44D rest");  // rocket, thumbs up
    r.Check(ws->record().icon == L"\U0001F680", "first emoji kept");
    s->window->SetWorkspaceIcon(ws, L"\U0001F469\u200D\U0001F4BB");    // woman technologist (ZWJ)
    r.Check(ws->record().icon == L"\U0001F469\u200D\U0001F4BB", "ZWJ sequence kept whole");
    bool stored = false;
    for (const auto& w : app->store().LoadWorkspaces(s->window->profile()->id())) {
      stored = stored || (w.id == ws->id() && w.icon == ws->record().icon);
    }
    r.Check(stored, "icon stored");
    s->window->SetWorkspaceIcon(ws, L"");
    r.Check(ws->record().icon.empty(), "empty input removes the icon");
  });
  r.Add("Unload Space unloads every loaded tab except the one on screen", [s, &r] {
    Workspace* ws = s->window->workspace();
    int live_before = 0;
    for (const auto& t : ws->tabs()) live_before += t->has_browser();
    const int unloaded = s->window->UnloadWorkspaceTabs(ws);
    r.Check(unloaded == live_before - 1, "unloaded " + std::to_string(unloaded) + " of " +
                                             std::to_string(live_before));
  }, [s] {
    for (const auto& t : s->window->workspace()->tabs()) {
      if (t.get() != s->window->active_tab() && t->has_browser()) return false;
    }
    return s->window->active_tab()->has_browser();
  });

  // Essentials.
  struct Ess {
    Tab* tab = nullptr;
    Workspace* home = nullptr;
    Workspace* second = nullptr;
  };
  auto e = std::make_shared<Ess>();
  r.Add("load a tab for Essentials", [s, e] {
    e->home = s->window->workspace();
    e->tab = s->window->OpenTab(TestUrl(L"t=Essential"), true);
  }, [e] { return e->tab->record().title == L"Essential" && !e->tab->loading(); });
  r.Add("Add to Essentials moves the tab out of the space and keeps it on screen", [s, e, app, &r] {
    r.Check(s->window->AddToEssentials(e->tab), "added");
    Profile* prof = s->window->profile();
    r.Check(prof->IsEssential(e->tab) && e->home->IndexOf(e->tab) < 0, "now an Essential");
    r.Check(e->tab->pinned() && !e->tab->pinned_url().empty(), "pinned, with a pinned URL");
    r.Check(s->window->active_tab() == e->tab && s->window->active_essential() == e->tab,
            "still the selected tab");
    app->SaveNow();
    bool stored = false;
    for (const auto& w : app->store().LoadWorkspaces(prof->id())) {
      if (w.kind != WorkspaceRecord::kKindEssentials) continue;
      for (const auto& rec : app->store().LoadTabs(w.id)) stored = stored || rec.id == e->tab->id();
    }
    r.Check(stored, "stored in the Essentials container");
  });
  r.Add("Essentials are shared by every space", [s, e, app, &r] {
    e->second = app->CreateWorkspace(s->window->profile(), L"Second", 0x4FB3A9);
    s->window->HandleAccelerator(VK_RIGHT, true, false, true);  // Ctrl+Alt+Right
    r.Check(s->window->workspace() == e->second, "switched to the second space");
    r.Check(s->window->active_tab() != e->tab, "the second space shows its own tab");
    s->window->ActivateTab(e->tab);
    r.Check(s->window->active_tab() == e->tab, "the Essential opens from the second space");
  }, [e] { return e->tab->has_browser() && !e->tab->loading(); });
  r.Add("a space left on an Essential returns to it", [s, e, &r] {
    s->window->HandleAccelerator(VK_LEFT, true, false, true);  // back to Home (wraps)
    r.Check(s->window->workspace() == e->home, "back in Home");
    s->window->ActivateTab(e->home->tabs().back().get());
    s->window->HandleAccelerator(VK_RIGHT, true, false, true);
    r.Check(s->window->active_tab() == e->tab, "Second restored its Essential");
    s->window->HandleAccelerator(VK_LEFT, true, false, true);
    r.Check(s->window->active_tab() != e->tab, "Home restored its own tab");
  });
  r.Add("Remove from Essentials puts it back into the current space, unpinned", [s, e, &r] {
    s->window->ActivateTab(e->tab);
    s->window->RemoveFromEssentials(e->tab);
    r.Check(!s->window->profile()->IsEssential(e->tab) && e->home->IndexOf(e->tab) >= 0,
            "in Home again");
    r.Check(!e->tab->pinned() && e->tab->pinned_url().empty(), "unpinned");
    r.Check(s->window->active_tab() == e->tab, "still selected");
  });
  r.Add("at most 12 Essentials", [s, &r] {
    std::vector<Tab*> added;
    for (int i = 0; i < BrowserWindow::kMaxEssentials; ++i) {
      Tab* t = s->window->OpenTab(L"about:blank", false);
      if (s->window->AddToEssentials(t)) added.push_back(t);
    }
    r.Check(static_cast<int>(added.size()) == BrowserWindow::kMaxEssentials, "12 added");
    Tab* extra = s->window->OpenTab(L"about:blank", false);
    r.Check(!s->window->CanAddToEssentials(extra) && !s->window->AddToEssentials(extra),
            "the 13th is refused");
    for (Tab* t : added) t->RequestClose();
    extra->RequestClose();
  }, [s] {
    const Workspace* ess = s->window->profile()->essentials();
    return ess && ess->tabs().empty();
  });

  r.Add("an empty space holds a hidden placeholder tab", [s, e, &r] {
    s->window->HandleAccelerator(VK_RIGHT, true, false, true);  // to Second
    r.Check(s->window->workspace() == e->second, "in Second");
    const auto& tabs = e->second->tabs();
    r.Check(tabs.size() == 1 && tabs[0]->empty_placeholder(), "only the placeholder");
    bool listed = false;
    for (const auto& item : s->window->AccessibleItems()) {
      listed = listed || item.key == reinterpret_cast<intptr_t>(tabs[0].get());
    }
    r.Check(!listed, "not listed in the sidebar");
    s->window->HandleAccelerator('T', true, false, false);
    s->window->TypeInAddressBar(TestUrl(L"t=RealTab"));
    s->window->SubmitAddressBar();
  }, [e] {
    const auto& tabs = e->second->tabs();
    return tabs.size() == 1 && !tabs[0]->empty_placeholder() &&
           tabs[0]->record().title == L"RealTab";
  });
  r.Add("back to Home", [s, e] { s->window->HandleAccelerator(VK_LEFT, true, false, true); },
        [s, e] { return s->window->workspace() == e->home; });

  r.Add("the space name folds the pinned section, keeping the selected pinned tab", [s, e, &r] {
    Workspace* ws = s->window->workspace();
    r.Check(ws->PinnedCount() >= 1, "Home has pinned tabs");
    auto listed = [&](const Tab* t) {
      for (const auto& item : s->window->AccessibleItems()) {
        if (item.key == reinterpret_cast<intptr_t>(t)) return true;
      }
      return false;
    };
    Tab* pinned = ws->tabs().front().get();
    Tab* regular = ws->tabs().back().get();
    s->window->ActivateTab(regular);
    r.Check(!s->window->PinnedSectionCollapsed(), "starts expanded");
    s->window->TogglePinnedSection();
    r.Check(s->window->PinnedSectionCollapsed(), "folded");
    r.Check(s->window->rows_animating() == ui::AnimationsEnabled(),
            "the rows tween (0.18s) unless Windows animations are off");
    r.Check(!listed(pinned), "pinned tab hidden while folded");
    s->window->ActivateTab(pinned);
    r.Check(listed(pinned), "the selected pinned tab stays visible");
    s->window->TogglePinnedSection();
    r.Check(!s->window->PinnedSectionCollapsed() && listed(pinned), "expanded again");
  });

  // G1: Ctrl+Alt+Right slides the spaces (250ms spring) and crossfades the
  // backdrop; with Windows animations off it switches at once.
  r.Add("a space switch slides, then settles", [s, e, &r] {
    s->window->HandleAccelerator(VK_RIGHT, true, false, true);
    r.Check(s->window->workspace() == e->second, "switched to Second");
    if (ui::AnimationsEnabled()) {
      r.Check(s->window->space_switch_animating(), "the slide is running");
    } else {
      r.Check(!s->window->space_switch_animating(), "no slide with animations off");
    }
  }, [s] { return !s->window->space_switch_animating(); }, 2000);
  r.Add("and back to Home", [s] { s->window->HandleAccelerator(VK_LEFT, true, false, true); },
        [s, e] { return s->window->workspace() == e->home && !s->window->space_switch_animating(); },
        2000);

  r.Add("with the setting off, closing selects the neighbour instead", [s, p, app] {
    app->store().SetPref(Store::kGlobalScope, L"select_recent_on_close", L"0");
    p->b = s->window->OpenTab(TestUrl(L"t=X"), false);  // second to last
    p->child = s->window->OpenTab(TestUrl(L"t=Y"), false);  // last
    s->window->ActivateTab(p->a);  // most recently used
    s->window->ActivateTab(p->child);
    s->window->HandleAccelerator('W', true, false, false);
  }, [s, p, app, &r] {
    if (s->window->workspace()->IndexOf(p->child) >= 0) return false;
    r.Check(s->window->active_tab() == p->b, "previous neighbour X selected, not A");
    app->store().SetPref(Store::kGlobalScope, L"select_recent_on_close", L"1");
    return true;
  });

  // Split view (ZenViewSplitter): Ctrl+Alt+V splits the current tab with
  // the next one; both hosts show side by side; the divider resizes and
  // persists; Ctrl+Alt+H relayouts; leaving/closing/unsplitting.
  struct Split {
    Tab* a = nullptr;
    Tab* b = nullptr;
    Tab* c = nullptr;
    std::wstring layout_before;
  };
  auto sp = std::make_shared<Split>();
  r.Add("open three tabs for a split", [s, sp] {
    sp->a = s->window->OpenTab(TestUrl(L"t=SplitA"), true);
    sp->b = s->window->OpenTab(TestUrl(L"t=SplitB"), false);
    sp->c = s->window->OpenTab(TestUrl(L"t=SplitC"), false);
    s->window->ActivateTab(sp->a);
  }, [sp] { return sp->a->record().title == L"SplitA" && !sp->a->loading(); });
  r.Add("Ctrl+Alt+V splits the current tab with the next one", [s, sp, &r] {
    BrowserWindow* w = s->window;
    r.Check(w->HandleAccelerator('V', true, false, true), "Ctrl+Alt+V handled");
    const auto panes = w->SplitPanes();
    r.Check(panes.size() == 2 && panes[0].tab == sp->a && panes[1].tab == sp->b,
            "A and B are the panes, in order");
    if (panes.size() == 2) {
      r.Check(panes[0].rect.right < panes[1].rect.left &&
                  panes[0].rect.top == panes[1].rect.top,
              "side by side with a gap");
    }
    r.Check(sp->b->has_browser(), "the second pane loads with the split");
    int split_rows = 0;
    for (const auto& item : w->AccessibleItems()) {
      if (item.name.find(L", in split view") != std::wstring::npos) ++split_rows;
    }
    r.Check(split_rows == 2, "both members are listed (one sidebar row)");
  }, [sp, &r] {
    if (!(sp->b->has_browser() && sp->b->host() && ::IsWindowVisible(sp->b->host()) &&
          ::IsWindowVisible(sp->a->host()))) {
      return false;
    }
    // The pane created while the split appeared fills its (narrower) host.
    RECT host{}, page{};
    ::GetClientRect(sp->b->host(), &host);
    ::GetClientRect(sp->b->browser()->GetHost()->GetWindowHandle(), &page);
    r.Check(page.right == host.right && page.bottom == host.bottom,
            "B's page window fits its pane (" + std::to_string(page.right) + " vs " +
                std::to_string(host.right) + ")");
    return true;
  });
  r.Add("dragging the divider resizes the panes and is saved", [s, sp, app, &r] {
    BrowserWindow* w = s->window;
    const Workspace::SplitView* split = w->workspace()->SplitOf(sp->a);
    r.Check(split != nullptr, "in a split");
    if (!split) return;
    const auto stored = app->store().LoadSplitViews(w->workspace()->id());
    sp->layout_before = stored.empty() ? L"" : stored[0].layout;
    std::vector<SplitHandle> handles;
    const auto before = w->SplitPanes(&handles);
    r.Check(handles.size() == 1 && handles[0].vertical, "one vertical divider");
    if (handles.size() != 1 || before.size() != 2) return;
    const float scale = static_cast<float>(::GetDpiForWindow(w->hwnd())) / 96.f;
    const int x = static_cast<int>((handles[0].rect.left + handles[0].rect.right) / 2.f * scale);
    const int y = static_cast<int>((handles[0].rect.top + handles[0].rect.bottom) / 2.f * scale);
    ::SendMessageW(w->hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    ::SendMessageW(w->hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + static_cast<int>(80 * scale), y));
    ::SendMessageW(w->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x + static_cast<int>(80 * scale), y));
    const auto after = w->SplitPanes();
    r.Check(after.size() == 2 && after[0].rect.right > before[0].rect.right + 60.f,
            "the left pane grew by the drag");
    RECT host{};
    ::GetWindowRect(sp->a->host(), &host);
    r.Check(host.right - host.left > static_cast<LONG>((before[0].rect.right - before[0].rect.left) * scale) + 40,
            "its page window grew too");
    const auto saved = app->store().LoadSplitViews(w->workspace()->id());
    r.Check(saved.size() == 1 && saved[0].layout != sp->layout_before,
            "the new sizes are stored (" + N(saved.empty() ? L"" : saved[0].layout) + ")");
  });
  r.Add("focus moving into the other pane selects its tab", [s, sp, &r] {
    BrowserWindow* w = s->window;
    w->OnContentFocused(sp->b);  // what a click into B's page reports
    r.Check(w->active_tab() == sp->b && w->SplitPanes().size() == 2,
            "B is current and the split stays on screen");
    w->ActivateTab(sp->a);
  });
  r.Add("Ctrl+Alt+H stacks the same split", [s, sp, &r] {
    BrowserWindow* w = s->window;
    w->HandleAccelerator('H', true, false, true);
    const auto panes = w->SplitPanes();
    r.Check(panes.size() == 2 && panes[0].rect.bottom < panes[1].rect.top, "one above the other");
  });
  r.Add("selecting a tab outside the split hides its panes", [s, sp, &r] {
    BrowserWindow* w = s->window;
    w->ActivateTab(sp->c);
    r.Check(w->SplitPanes().empty(), "no panes for C");
    r.Check(!::IsWindowVisible(sp->a->host()) && !::IsWindowVisible(sp->b->host()),
            "A and B hidden");
    w->ActivateTab(sp->b);
    r.Check(w->SplitPanes().size() == 2 && ::IsWindowVisible(sp->a->host()),
            "selecting B shows the split again");
  });
  r.Add("closing a member dissolves a two-tab split", [s, sp, &r] {
    BrowserWindow* w = s->window;
    w->CloseTabShortcut(sp->b);
    r.Check(w->workspace()->splits().empty() && w->SplitPanes().empty(), "no split left");
    r.Check(w->workspace()->IndexOf(sp->a) >= 0, "A stays");
  });
  r.Add("Split Link to New Tab joins the current split", [s, sp, &r] {
    BrowserWindow* w = s->window;
    w->ActivateTab(sp->a);
    w->HandleAccelerator('V', true, false, true);  // A + C
    const size_t before = w->workspace()->tabs().size();
    w->SplitLinkInNewTab(sp->a, TestUrl(L"t=SplitLink"));
    r.Check(w->workspace()->tabs().size() == before + 1, "a new tab");
    r.Check(w->SplitPanes().size() == 3 && w->active_tab()->url() == TestUrl(L"t=SplitLink"),
            "it is a third pane and current");
    w->HandleAccelerator('U', true, false, true);
    w->CloseTabShortcut(w->active_tab());
  });
  r.Add("Ctrl+Alt+U unsplits", [s, sp, &r] {
    BrowserWindow* w = s->window;
    w->ActivateTab(sp->a);
    w->HandleAccelerator('G', true, false, true);
    r.Check(w->SplitPanes().size() == 2, "split again (grid of two)");
    w->HandleAccelerator('U', true, false, true);
    r.Check(w->SplitPanes().empty() && w->workspace()->splits().empty(), "unsplit");
    w->CloseTabShortcut(sp->c);
  });

  // Glance (ZenGlanceManager): only a real Alt+click on a web link opens
  // one; it shows over its parent, Escape closes it, expand keeps it as a
  // tab, split puts it beside its parent.
  auto gserver = std::make_shared<TestHttpServer>();
  struct GlanceT {
    Tab* page = nullptr;
    Tab* glance = nullptr;
    bool saw_arc = false;  // the opening animation was seen running
  };
  auto gt = std::make_shared<GlanceT>();
  auto listed = [s](const Tab* t) {
    for (const auto& item : s->window->AccessibleItems()) {
      if (item.key == reinterpret_cast<intptr_t>(t)) return true;
    }
    return false;
  };
  r.Add("glance: a page with a link", [s, gt, gserver, &r] {
    g_server = gserver;
    r.Check(gserver->Start(), "test HTTP server listening");
    gt->page = s->window->OpenTab(gserver->Url(L"/links"), true);
  }, [gt] { return gt->page->record().title == L"links" && !gt->page->loading(); });
  r.Add("a synthetic Alt+click never opens a glance", [gt] {
    gt->page->browser()->GetMainFrame()->ExecuteJavaScript(
        "l.dispatchEvent(new MouseEvent('click', {altKey: true, bubbles: true, cancelable: true}))",
        "", 0);
  }, [s, gt, &r] {
    if (gt->page->record().title != L"SYNTH") return false;
    r.Check(!s->window->glance_tab(), "no glance from an untrusted click");
    return true;
  });
  r.Add("a real Alt+click on the link opens a glance over the page", [gt] {
    CefMouseEvent e;
    e.x = 60;
    e.y = 60;
    e.modifiers = EVENTFLAG_ALT_DOWN;
    CefRefPtr<CefBrowserHost> host = gt->page->browser()->GetHost();
    host->SendMouseMoveEvent(e, false);
    host->SendMouseClickEvent(e, MBT_LEFT, false, 1);
    host->SendMouseClickEvent(e, MBT_LEFT, true, 1);
  }, [s, gt, listed, &r] {
    gt->saw_arc |= s->window->glance_animating();
    Tab* g = s->window->glance_tab();
    if (!g || g->record().title != L"Glanced" || g->loading() || s->window->glance_animating()) return false;
    gt->glance = g;
    r.Check(gt->saw_arc, "it grew out of the link (the opening arc ran)");
    r.Check(s->window->active_tab() == g, "the glance is the current tab");
    r.Check(gt->page->url().find(L"/links") != std::wstring::npos, "the parent did not navigate");
    r.Check(::IsWindowVisible(gt->page->host()) && ::IsWindowVisible(g->host()),
            "the parent stays on screen behind it");
    RECT a{}, b{};
    ::GetWindowRect(gt->page->host(), &a);
    ::GetWindowRect(g->host(), &b);
    const double ratio = static_cast<double>(b.right - b.left) / (a.right - a.left);
    r.Check(ratio > 0.75 && ratio < 0.85, "80% of the page's width");
    const HWND dim = ::FindWindowExW(s->window->hwnd(), nullptr, L"OrbitGlanceBackground", nullptr);
    r.Check(dim && ::IsWindowVisible(dim), "the parent is faded");
    r.Check(!listed(g) && listed(gt->page), "no sidebar row of its own (it sits in the parent's)");
    return true;
  }, 10000);
  r.Add("Escape in the glance closes it and returns to the parent", [gt] {
    CefKeyEvent k;
    k.type = KEYEVENT_RAWKEYDOWN;
    k.windows_key_code = VK_ESCAPE;
    k.native_key_code = static_cast<int>(::MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC) << 16);
    gt->glance->browser()->GetHost()->SendKeyEvent(k);
    k.type = KEYEVENT_KEYUP;
    gt->glance->browser()->GetHost()->SendKeyEvent(k);
    gt->saw_arc = false;
  }, [s, gt] {
    gt->saw_arc |= s->window->glance_closing();
    return gt->saw_arc && !s->window->glance_tab() && s->window->active_tab() == gt->page &&
           s->window->workspace()->IndexOf(gt->glance) < 0;
  }, 8000);
  r.Add("expand keeps the glance as an ordinary tab", [s, gt, gserver, listed, &r] {
    s->window->OpenGlance(gt->page, gserver->Url(L"/page?t=Expanded"));
    gt->glance = s->window->glance_tab();
    r.Check(gt->glance != nullptr, "glance open");
    r.Check(s->window->HandleAccelerator('O', true, false, false), "Ctrl+O expands it");
    r.Check(!s->window->glance_tab() && s->window->active_tab() == gt->glance, "now a tab");
    r.Check(!s->window->HandleAccelerator('O', true, false, false), "Ctrl+O does nothing without a glance");
    r.Check(listed(gt->glance), "with its own row");
    s->window->CloseTabShortcut(gt->glance);
  }, [s, gt] { return s->window->workspace()->IndexOf(gt->glance) < 0; });
  r.Add("split puts the glance beside its parent", [s, gt, gserver, &r] {
    s->window->OpenGlance(gt->page, gserver->Url(L"/page?t=SplitGlance"));
    gt->glance = s->window->glance_tab();
    s->window->SplitGlance();
    const auto panes = s->window->SplitPanes();
    r.Check(!s->window->glance_tab() && panes.size() == 2 && panes[0].tab == gt->page &&
                panes[1].tab == gt->glance,
            "a split of the parent and the glance");
    s->window->UnsplitCurrent();
    s->window->CloseTabShortcut(gt->glance);
  }, [s, gt] { return s->window->workspace()->IndexOf(gt->glance) < 0; });
  // Settings (Look and Feel / Tab Management), applied live.
  auto set_pref = [app](const wchar_t* key, const wchar_t* value) {
    app->store().SetPref(Store::kGlobalScope, key, value);
    app->ApplyThemeToAllWindows();
  };
  auto item_rect = [s](const wchar_t* name) {
    for (const auto& item : s->window->AccessibleItems()) {
      if (item.name == name) return item.rect_dip;
    }
    return D2D1_RECT_F{};
  };
  r.Add("settings: the New Tab button can move below the tabs or hide", [s, set_pref, item_rect, &r] {
    const D2D1_RECT_F top = item_rect(L"New tab");
    set_pref(L"newtab_button_top", L"0");
    const D2D1_RECT_F bottom = item_rect(L"New tab");
    float last_tab = 0.f;
    for (const auto& item : s->window->AccessibleItems()) {
      if (item.role == AccessibleItem::Role::kTab) last_tab = std::max(last_tab, item.rect_dip.top);
    }
    r.Check(top.bottom > top.top && bottom.top > top.top && bottom.top > last_tab,
            "zen.view.show-newtab-button-top off: after the last tab");
    set_pref(L"newtab_button", L"0");
    r.Check(item_rect(L"New tab").bottom <= item_rect(L"New tab").top, "zen.tabs.show-newtab-vertical off: gone");
    set_pref(L"newtab_button", L"1");
    set_pref(L"newtab_button_top", L"1");
    r.Check(item_rect(L"New tab").top == top.top, "back on top");
  });
  r.Add("quick actions: typing a command offers it and Enter runs it", [s, &r] {
    BrowserWindow* w = s->window;
    const bool compact = w->compact_mode();
    w->HandleAccelerator('L', true, false, false);
    w->TypeInAddressBar(L"toggle compact");
    r.Check(w->url_bar_open(), "bar open");
    int row = -1;
    const auto& rows = w->url_results();
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].kind == OmniboxRow::Kind::kAction && rows[i].action_id == L"compact-mode") row = static_cast<int>(i);
    }
    r.Check(row == 1, "\"Toggle Compact Mode\" is offered right after the search row");
    w->ChooseUrlResult(row);
    r.Check(w->compact_mode() != compact && !w->url_bar_open(), "it ran and the bar closed");
    w->HandleAccelerator('S', true, false, false);  // back
    r.Check(w->compact_mode() == compact, "and back");
  });
  r.Add("layout: multiple toolbars put navigation and the URL field above the page", [s, set_pref, &r] {
    BrowserWindow* w = s->window;
    set_pref(L"single_toolbar", L"0");
    const D2D1_RECT_F bar = w->toolbar_rect();
    const D2D1_RECT_F field = w->address_rect();
    const RECT page = w->ContentRectPx();
    const float scale = static_cast<float>(::GetDpiForWindow(w->hwnd())) / 96.f;
    r.Check(w->multi_toolbar() && bar.bottom > bar.top && bar.top == 0.f, "a toolbar along the top");
    r.Check(field.top >= bar.top && field.bottom <= bar.bottom && field.left >= bar.left, "the URL field is in it");
    r.Check(static_cast<float>(page.top) >= bar.bottom * scale - 1.f, "the page starts below it");
    POINT close{static_cast<LONG>((bar.right - 10.f) * scale), static_cast<LONG>(10.f * scale)};
    ::ClientToScreen(w->hwnd(), &close);
    r.Check(::SendMessageW(w->hwnd(), WM_NCHITTEST, 0, MAKELPARAM(close.x, close.y)) == HTCLOSE,
            "the window controls sit at its end");
    // Compact mode with multiple toolbars: hide the toolbar (the sidebar stays).
    w->SetCompactHides(false, true);
    r.Check(w->compact_mode() && !w->CompactHidesSidebar() && w->CompactHidesToolbar(), "compact: toolbar only");
    r.Check(!w->toolbar_shown() && w->ContentRectPx().top < static_cast<LONG>(20.f * scale), "the page takes the top");
    r.Check(w->sidebar_left() == 0.f && w->ContentRectPx().left > static_cast<LONG>(100.f * scale), "the sidebar stays");
    w->SetCompactHides(true, true);
    r.Check(w->CompactHidesSidebar() && w->CompactHidesToolbar() && w->ContentRectPx().left < static_cast<LONG>(20.f * scale),
            "hide both: the page takes the whole window");
    w->SetCompactHides(true, false);
    w->HandleAccelerator('S', true, false, false);  // compact mode off
    r.Check(!w->compact_mode() && w->toolbar_shown(), "compact off: the toolbar is back");
    set_pref(L"single_toolbar", L"1");
    r.Check(!w->multi_toolbar() && w->address_rect().right <= w->sidebar_left() + 400.f, "back to the single toolbar");
  });
  r.Add("tabs on the right: the sidebar and its rows move to the right edge", [s, set_pref, item_rect, &r] {
    BrowserWindow* w = s->window;
    set_pref(L"tabs_right", L"1");
    r.Check(w->tabs_on_right() && w->sidebar_left() > 0.f, "the sidebar starts at the right");
    const RECT page = w->ContentRectPx();
    const float scale = static_cast<float>(::GetDpiForWindow(w->hwnd())) / 96.f;
    float tab_left = 1e9f;
    for (const auto& item : w->AccessibleItems()) {
      if (item.role == AccessibleItem::Role::kTab) tab_left = std::min(tab_left, item.rect_dip.left);
    }
    r.Check(tab_left * scale >= page.right, "tab rows are right of the page");
    r.Check(page.left < 20, "the page starts at the left edge");
    set_pref(L"tabs_right", L"0");
    r.Check(!w->tabs_on_right() && w->sidebar_left() == 0.f, "back on the left");
  });
  r.Add("settings: URL bar behavior", [s, set_pref, &r] {
    BrowserWindow* w = s->window;
    w->HandleAccelerator('L', true, false, false);
    r.Check(w->url_bar_floating(), "floating-on-type: Ctrl+L floats");
    w->CancelAddressBar();
    set_pref(L"urlbar_behavior", L"normal");
    w->HandleAccelerator('L', true, false, false);
    r.Check(w->url_bar_open() && !w->url_bar_floating(), "normal: Ctrl+L opens it in the sidebar");
    w->CancelAddressBar();
    set_pref(L"urlbar_behavior", L"floating-on-type");
  });
  // zen.tabs.ctrl-tab.ignore-essential-tabs: by default Ctrl+Tab passes
  // through the Essentials too; with the setting it stays in its group.
  auto ess_tab = std::make_shared<Tab*>(nullptr);
  r.Add("settings: Ctrl+Tab and Essentials", [s, gserver, ess_tab] {
    *ess_tab = s->window->OpenTab(gserver->Url(L"/page?t=CtrlTabEssential"), true);
  }, [s, set_pref, ess_tab, &r] {
    Tab* e = *ess_tab;
    if (e->record().title != L"CtrlTabEssential" || e->loading()) return false;
    BrowserWindow* w = s->window;
    w->AddToEssentials(e);
    const Workspace* ws = w->workspace();
    w->ActivateTab(ws->tabs().back().get());  // the last space tab
    w->HandleAccelerator(VK_TAB, true, false, false);
    const Workspace* ess = w->profile()->essentials();
    r.Check(ess && !ess->tabs().empty() && w->active_tab() == ess->tabs().front().get(),
            "from the last space tab, Ctrl+Tab wraps into the Essentials");
    set_pref(L"ctrl_tab_ignore_essentials", L"1");
    w->HandleAccelerator(VK_TAB, true, false, false);
    r.Check(ess->tabs().size() > 1 ? w->active_tab() != nullptr && ess->IndexOf(w->active_tab()) >= 0
                                   : w->active_tab() == ess->tabs().front().get(),
            "with the setting, it stays among the Essentials");
    set_pref(L"ctrl_tab_ignore_essentials", L"0");
    w->RemoveFromEssentials(e);
    w->CloseTabShortcut(e);
    return true;
  }, 10000);
  auto pending = std::make_shared<Tab*>(nullptr);
  r.Add("settings: a tab put to sleep for Ctrl+Tab", [s, gserver, pending] {
    *pending = s->window->OpenTabNear(s->window->active_tab(), gserver->Url(L"/page?t=Pending"), false);
  }, [pending] {
    if (!(*pending)->has_browser() || (*pending)->loading()) return false;
    (*pending)->Unload(/*discarded=*/true);
    return true;
  });
  r.Add("settings: Ctrl+Tab can skip tabs that are not loaded", nullptr, [s, pending, set_pref, &r] {
    Tab* t = *pending;
    if (!t->is_discarded() && t->load_state() != Tab::LoadState::kUnloaded) return false;
    BrowserWindow* w = s->window;
    Tab* a = w->active_tab();
    set_pref(L"ctrl_tab_skip_pending", L"1");
    w->HandleAccelerator(VK_TAB, true, false, false);
    r.Check(w->active_tab() != t, "the sleeping tab was skipped");
    set_pref(L"ctrl_tab_skip_pending", L"0");
    w->ActivateTab(a);
    w->HandleAccelerator(VK_TAB, true, false, false);
    r.Check(w->active_tab() == t, "with the setting off it is selected");
    w->ActivateTab(a);
    w->CloseTabShortcut(t);
    return true;
  });
  r.Add("settings: the glance trigger can be Ctrl (new pages)", [s, gt, gserver, set_pref] {
    set_pref(L"glance_trigger", L"ctrl");
    s->window->ActivateTab(gt->page);
    gt->page->Navigate(gserver->Url(L"/links?ctrl"));  // same browser: its trigger stays Alt
    gt->glance = s->window->OpenTab(gserver->Url(L"/links?new"), true);  // a new browser: Ctrl
  }, [gt] { return gt->glance->record().title == L"links" && !gt->glance->loading(); });
  r.Add("a real Ctrl+click opens the glance", [gt] {
    CefMouseEvent e;
    e.x = 60;
    e.y = 60;
    e.modifiers = EVENTFLAG_CONTROL_DOWN;
    CefRefPtr<CefBrowserHost> host = gt->glance->browser()->GetHost();
    host->SendMouseMoveEvent(e, false);
    host->SendMouseClickEvent(e, MBT_LEFT, false, 1);
    host->SendMouseClickEvent(e, MBT_LEFT, true, 1);
  }, [s, gt, set_pref] {
    Tab* g = s->window->glance_tab();
    if (!g || g->record().title != L"Glanced") return false;
    s->window->CloseGlance();
    s->window->CloseTabShortcut(gt->glance);
    set_pref(L"glance_trigger", L"alt");
    return true;
  }, 10000);
  // Keyboard shortcuts (Zen's Keyboard Shortcuts pane).
  r.Add("shortcuts: a rebound action runs from its new chord only", [s, app, &r] {
    BrowserWindow* w = s->window;
    app->SetShortcut(L"new-tab", {'B', true, false, false});
    r.Check(w->HandleAccelerator('B', true, false, false) && w->url_bar_open(), "Ctrl+B opens a new tab");
    w->CancelAddressBar();
    r.Check(!w->HandleAccelerator('T', true, false, false) && !w->url_bar_open(), "Ctrl+T does nothing now");
    app->ResetShortcut(L"new-tab");
    r.Check(w->HandleAccelerator('T', true, false, false) && w->url_bar_open(), "reset: Ctrl+T again");
    w->CancelAddressBar();
  });
  r.Add("shortcuts: an unbound action (switch to space 2) can be bound", [s, app, &r] {
    BrowserWindow* w = s->window;
    Workspace* home = w->workspace();
    const auto& spaces = w->profile()->workspaces();
    Workspace* second = spaces.size() > 1 ? spaces[1].get() : nullptr;
    r.Check(second != nullptr && !second->window(), "a second space exists");
    if (!second || second->window()) return;
    app->SetShortcut(L"space-2", {'2', true, false, true});
    r.Check(w->HandleAccelerator('2', true, false, true) && w->workspace() == second, "Ctrl+Alt+2 switched");
    app->ResetShortcut(L"space-2");
    r.Check(!w->HandleAccelerator('2', true, false, true), "unbound again");
    for (size_t i = 0; i < spaces.size(); ++i) {
      if (spaces[i].get() == home) w->HandleAccelerator(VK_LEFT, true, false, true);
    }
    while (w->workspace() != home) w->HandleAccelerator(VK_LEFT, true, false, true);
  });
  auto kbs = std::make_shared<Tab*>(nullptr);
  r.Add("shortcuts: the settings page lists them", [s, kbs] {
    *kbs = s->window->OpenTab(L"orbit://settings/", true);
  }, [kbs] { return (*kbs)->record().title == L"Settings" && !(*kbs)->loading(); }, 8000);
  r.Add("shortcuts: a conflict is reported, then replaced", [kbs] {
    (*kbs)->browser()->GetMainFrame()->ExecuteJavaScript(R"js((async () => {
      const body = {id: 'reload', vk: 87, ctrl: true, shift: false, alt: false};
      const first = await orbitAction('/api/shortcut-set', body);
      const again = first.conflicts ? await orbitAction('/api/shortcut-set', Object.assign({replace: true}, body)) : null;
      document.title = (first.conflicts ? 'CONFLICT:' + first.conflicts.join() : 'NONE') + (again && again.ok ? '|REPLACED' : '');
    })())js", "", 0);
  }, [s, kbs, app, &r] {
    const std::wstring t = (*kbs)->record().title;
    if (t.rfind(L"CONFLICT", 0) != 0 && t != L"NONE") return false;
    r.Check(t == L"CONFLICT:Close tab|REPLACED", "Ctrl+W conflicts with Close tab (" + N(t) + ")");
    r.Check(FormatChord(app->shortcuts().Effective(L"reload")) == L"Ctrl+W" &&
                app->shortcuts().Effective(L"close-tab").empty(),
            "reload is Ctrl+W, Close tab is unassigned");
    return true;
  });
  r.Add("shortcuts: while recording, the settings page gets the keys", [s, kbs, app, &r] {
    app->SetShortcutRecording(true);
    CefKeyEvent k;
    k.type = KEYEVENT_RAWKEYDOWN;
    k.windows_key_code = 'T';
    k.modifiers = EVENTFLAG_CONTROL_DOWN;
    bool shortcut = false;
    const bool consumed = app->client()->OnPreKeyEvent((*kbs)->browser(), k, nullptr, &shortcut);
    r.Check(!consumed && !s->window->url_bar_open(), "Ctrl+T reached the page instead of opening a tab");
    app->SetShortcutRecording(false);
    app->ResetShortcut(L"");
    r.Check(app->shortcuts().overrides().empty(), "reset all");
    s->window->CloseTabShortcut(*kbs);
  });
  // Site data panel (ZenSiteDataPanel).
  auto site = std::make_shared<Tab*>(nullptr);
  r.Add("site data: a page with a stored permission", [s, gserver, site, app] {
    *site = s->window->OpenTab(gserver->Url(L"/page?t=SiteData"), true);
    app->permissions().Set(s->window->profile(), gserver->Url(L"/").substr(0, gserver->Url(L"/").size()),
                           CEF_PERMISSION_TYPE_GEOLOCATION, true);
  }, [site] { return (*site)->record().title == L"SiteData" && !(*site)->loading(); });
  r.Add("the site-info button opens the panel", [s] { s->window->OpenSiteData(); }, [s] {
    auto b = s->window->site_data_browser();
    return s->window->site_data_open() && b && !b->IsLoading();
  }, 8000);
  r.Add("it shows the connection and the site's permission", [s] {
    s->window->site_data_browser()->GetMainFrame()->ExecuteJavaScript(
        "fetch('/api/info').then(r => r.json()).then(i => document.title = 'I:' + i.security + ':' +"
        " i.permissions.length + ':' + (i.permissions[0] && i.permissions[0].allowed))", "", 0);
  }, [s, &r] {
    const std::wstring t = s->window->site_data_title();
    if (t.rfind(L"I:", 0) != 0) return false;
    r.Check(t == L"I:not-secure:1:true", "plain http, one allowed permission (" + N(t) + ")");
    return true;
  });
  r.Add("blocking it from the panel is stored for the page's origin", [s, gserver, app] {
    s->window->site_data_browser()->GetMainFrame()->ExecuteJavaScript(
        "orbitAction('/api/permission', {bit: " + std::to_string(CEF_PERMISSION_TYPE_GEOLOCATION) +
            ", allow: false})", "", 0);
  }, [s, gserver, app, &r] {
    const auto d = app->permissions().Get(s->window->profile(), gserver->Url(L"/"), CEF_PERMISSION_TYPE_GEOLOCATION);
    if (d != PermissionManager::Decision::kBlock) return false;
    r.Check(true, "blocked");
    return true;
  });
  r.Add("orbit://sitedata in an ordinary tab is refused", [s, site] {
    Tab* probe = s->window->OpenTab(L"orbit://sitedata/", true);
    *site = probe;
  }, [site] { return (*site)->has_browser() && !(*site)->loading(); }, 8000);
  r.Add("its API answers 403", [site] {
    (*site)->browser()->GetMainFrame()->ExecuteJavaScript(
        "fetch('/api/info', {headers: {'X-Orbit-Request': '1'}}).then(r => document.title = 'S' + r.status)", "", 0);
  }, [s, site, &r] {
    if ((*site)->record().title.rfind(L"S", 0) != 0) return false;
    r.Check((*site)->record().title == L"S403", "403 (" + N((*site)->record().title) + ")");
    s->window->CloseTabShortcut(*site);
    s->window->CloseSiteData();
    return true;
  });
  r.Add("the closed panel releases its page", nullptr, [s] {
    return !s->window->site_data_open() && s->window->site_data_browser_id() == 0;
  }, 5000);
  // Welcome (ZenWelcome): the page's choices act on its window.
  auto welcome = std::make_shared<Tab*>(nullptr);
  r.Add("welcome: the onboarding page", [s, welcome] {
    *welcome = s->window->OpenTab(L"orbit://welcome/", true);
  }, [welcome] { return (*welcome)->record().title == L"Welcome to Orbit" && !(*welcome)->loading(); }, 8000);
  r.Add("welcome: picking a space colour and an Essential, then done", [s, welcome, app] {
    const uint32_t before = s->window->workspace()->record().color;
    const int pick = ui::kAccentChoices[0] == before ? 1 : 0;
    (*welcome)->browser()->GetMainFrame()->ExecuteJavaScript(
        "(async () => { await orbitAction('/api/space-color', {index: " + std::to_string(pick) + "});"
        " await orbitAction('/api/essentials', {apps: [4]}); await orbitAction('/api/done'); })()", "", 0);
  }, [s, welcome, app, &r] {
    if (s->window->workspace()->IndexOf(*welcome) >= 0) return false;  // done closes the tab
    const Workspace* ess = s->window->profile()->essentials();
    bool github = false;
    if (ess) {
      for (const auto& t : ess->tabs()) github |= t->url().rfind(L"https://github.com", 0) == 0;
    }
    r.Check(github, "GitHub became an Essential");
    r.Check(app->store().GetPref(Store::kGlobalScope, L"welcome_done") == L"1", "marked done");
    if (ess) {
      for (const auto& t : ess->tabs()) {
        if (t->url().rfind(L"https://github.com", 0) == 0) {
          s->window->RemoveFromEssentials(t.get());
          break;
        }
      }
    }
    return true;
  }, 8000);
  // Space routing (ZenSpaceRoutingManager).
  r.Add("space routing: a link opened in a new tab goes to its space", [s, app, &r] {
    BrowserWindow* w = s->window;
    const auto& spaces = w->profile()->workspaces();
    Workspace* home = w->workspace();
    Workspace* target = nullptr;
    for (const auto& ws : spaces) {
      if (ws.get() != home && !ws->window()) target = ws.get();
    }
    r.Check(target != nullptr, "another space");
    if (!target) return;
    app->store().SetPref(Store::kGlobalScope, L"space_routes",
                         L"contains\t" + std::to_wstring(target->id()) + L"\troute-me\n");
    const size_t before = target->tabs().size();
    Tab* opener = w->active_tab();
    app->client()->OnOpenURLFromTab(opener->browser(), opener->browser()->GetMainFrame(),
                                    TestUrl(L"t=route-me"), CEF_WOD_NEW_FOREGROUND_TAB, true);
    r.Check(target->tabs().size() == before + 1 && target->tabs().back()->url() == TestUrl(L"t=route-me"),
            "the tab is in the routed space");
    r.Check(w->workspace() == home && w->active_tab() == opener, "the current space stays");
    const size_t home_before = home->tabs().size();
    app->client()->OnOpenURLFromTab(opener->browser(), opener->browser()->GetMainFrame(),
                                    TestUrl(L"t=not-routed"), CEF_WOD_NEW_BACKGROUND_TAB, true);
    r.Check(home->tabs().size() == home_before + 1, "other links stay in this space");
    app->store().SetPref(Store::kGlobalScope, L"space_routes", L"");
    target->tabs().back()->RequestClose();
    w->CloseTabShortcut(home->tabs().back().get());
  });
  // Media controls (ZenMediaController).
  auto player = std::make_shared<Tab*>(nullptr);
  r.Add("media: a tab plays with MediaSession metadata", [s, gserver, player] {
    *player = s->window->OpenTab(gserver->Url(L"/media"), true);
  }, [player] { return (*player)->record().title == L"PLAYING"; }, 10000);
  r.Add("no card while the playing tab is on screen", nullptr, [s, player, &r] {
    if ((*player)->media().state != Tab::MediaState::State::kPlaying) return false;
    r.Check(s->window->media_card_tab() == nullptr, "hidden for its own tab");
    r.Check((*player)->media().title == L"Orbit Tone" && (*player)->media().artist == L"Self Test",
            "title and artist from MediaSession");
    return true;
  }, 5000);
  r.Add("leaving it shows the card after 500 ms", [s, player] {
    s->window->ActivateTab(s->window->workspace()->tabs().front().get());
  }, [s, player] { return s->window->media_card_tab() == *player; }, 3000);
  r.Add("the card pauses and resumes it", [s] { s->window->MediaCommand("toggle"); }, [player] {
    return (*player)->media().state == Tab::MediaState::State::kPaused;
  }, 5000);
  r.Add("and plays again", [s] { s->window->MediaCommand("toggle"); }, [player] {
    return (*player)->media().state == Tab::MediaState::State::kPlaying;
  }, 5000);
  r.Add("focus shows the playing tab", [s, player, &r] {
    s->window->MediaCommand("focus");
    r.Check(s->window->active_tab() == *player && s->window->media_card_tab() == nullptr,
            "its tab is selected and the card hides");
    s->window->CloseTabShortcut(*player);
  }, [s, player] { return s->window->workspace()->IndexOf(*player) < 0; });
  r.Add("glance cleanup", [s, gt, gserver] {
    s->window->CloseTabShortcut(gt->page);
    gserver->Stop();
  }, [s, gt] { return s->window->workspace()->IndexOf(gt->page) < 0; });

  // Library (ZenLibrary): Alt+Shift+L slides the panel in and pushes the
  // page; its API answers only the panel of a window; a space card switches
  // spaces; Escape closes it and the page is released.
  struct Lib {
    RECT page_before{};
    Tab* probe = nullptr;
    int64_t other_space = 0;
  };
  auto lib = std::make_shared<Lib>();
  r.Add("Alt+Shift+L opens the library", [s, lib, &r] {
    BrowserWindow* w = s->window;
    ::GetWindowRect(w->active_tab()->host(), &lib->page_before);
    r.Check(w->HandleAccelerator('L', false, true, true) && w->library_open(), "open");
  }, [s] {
    auto b = s->window->library_browser();
    return b && !b->IsLoading() && b->GetMainFrame()->GetURL().ToString().rfind("orbit://library/", 0) == 0;
  }, 8000);
  r.Add("the panel covers the sidebar and pushes the page right", nullptr, [s, lib, &r] {
    BrowserWindow* w = s->window;
    if (w->space_switch_animating()) return false;
    const HWND host = ::FindWindowExW(w->hwnd(), nullptr, L"OrbitLibrary", nullptr);
    RECT page{}, panel{};
    ::GetWindowRect(w->active_tab()->host(), &page);
    if (host) ::GetWindowRect(host, &panel);
    if (page.left <= lib->page_before.left + 100) return false;  // still sliding
    r.Check(host && ::IsWindowVisible(host) && panel.right > panel.left + 300, "the panel is shown");
    r.Check(page.right - page.left == lib->page_before.right - lib->page_before.left,
            "the page moved without being resized");
    return true;
  }, 3000);
  r.Add("orbit://library in an ordinary tab gets no window actions", [s, lib] {
    lib->probe = s->window->OpenTab(L"orbit://library/", true);
  }, [lib] { return lib->probe->has_browser() && !lib->probe->loading() &&
                    lib->probe->record().title == L"Library"; }, 8000);
  r.Add("its API refuses it", [lib] {
    lib->probe->browser()->GetMainFrame()->ExecuteJavaScript(
        "fetch('/api/spaces', {headers: {'X-Orbit-Request': '1'}}).then(r => document.title = 'S' + r.status)",
        "", 0);
  }, [s, lib, &r] {
    if (lib->probe->record().title.rfind(L"S", 0) != 0) return false;
    r.Check(lib->probe->record().title == L"S403", "403 outside the panel (" + N(lib->probe->record().title) + ")");
    s->window->CloseTabShortcut(lib->probe);
    return true;
  });
  r.Add("a space card switches spaces and closes the panel", [s, lib, app, &r] {
    BrowserWindow* w = s->window;
    if (!w->library_open()) w->OpenLibrary();
    for (const auto& ws : w->profile()->workspaces()) {
      if (ws.get() != w->workspace() && !ws->window()) lib->other_space = ws->id();
    }
    if (!lib->other_space) {
      lib->other_space = app->CreateWorkspace(w->profile(), L"LibrarySpace", 0x4FB3A9)->id();
    }
    const std::string js = "orbitAction('/api/spaces/show', {space: " + std::to_string(lib->other_space) + ", tab: -1})";
    w->library_browser()->GetMainFrame()->ExecuteJavaScript(js, "", 0);
  }, [s, lib] {
    return s->window->workspace() && s->window->workspace()->id() == lib->other_space &&
           !s->window->library_open();
  }, 8000);
  r.Add("back, and Escape in the panel closes it", [s, lib, &r] {
    BrowserWindow* w = s->window;
    w->HandleAccelerator(VK_LEFT, true, false, true);
    w->OpenLibrary();
    r.Check(w->library_open(), "open again");
  }, [s] {
    auto b = s->window->library_browser();
    if (!b || b->IsLoading()) return false;
    CefKeyEvent k;
    k.type = KEYEVENT_RAWKEYDOWN;
    k.windows_key_code = VK_ESCAPE;
    b->GetHost()->SendKeyEvent(k);
    return true;
  }, 8000);
  r.Add("the closed panel releases its page", nullptr, [s] {
    return !s->window->library_open() && s->window->library_browser_id() == 0;
  }, 5000);

  // Fix 6: in compact mode the revealed sidebar floats over the page in a
  // child window above the tab host; the page does not move.
  r.Add("compact mode: the revealed sidebar overlays the page", [s, &r] {
    BrowserWindow* w = s->window;
    r.Check(w->HandleAccelerator('S', true, false, false) && w->compact_mode(), "Ctrl+S: compact");
    const HWND host = w->active_tab()->host();
    RECT before{};
    ::GetWindowRect(host, &before);
    ::SendMessageW(w->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(1, 300));  // the left edge
    const HWND overlay = ::FindWindowExW(w->hwnd(), nullptr, L"OrbitCompactSidebar", nullptr);
    r.Check(overlay && ::IsWindowVisible(overlay), "the sidebar card is shown");
    RECT after{};
    ::GetWindowRect(host, &after);
    r.Check(::EqualRect(&before, &after) != FALSE, "the page was not pushed aside");
    bool host_below = false;
    for (HWND h = overlay ? ::GetWindow(overlay, GW_HWNDNEXT) : nullptr; h;
         h = ::GetWindow(h, GW_HWNDNEXT)) {
      if (h == host) host_below = true;
    }
    r.Check(host_below, "the card is stacked above the page");
    r.Check(overlay && ::FindWindowExW(overlay, nullptr, L"EDIT", nullptr) != nullptr,
            "the address field rides on the card");
  });
  r.Add("leaving compact mode removes the card", [s, &r] {
    BrowserWindow* w = s->window;
    w->HandleAccelerator('S', true, false, false);
    const HWND overlay = ::FindWindowExW(w->hwnd(), nullptr, L"OrbitCompactSidebar", nullptr);
    r.Check(!w->compact_mode() && overlay && !::IsWindowVisible(overlay), "card hidden");
    r.Check(::FindWindowExW(overlay, nullptr, L"EDIT", nullptr) == nullptr &&
                ::FindWindowExW(w->hwnd(), nullptr, L"EDIT", nullptr) != nullptr,
            "the address field is back in the window");
  });

  // ZenFolders: subfolders (nested headers, indentation, collapse, unpack,
  // convert to a space).
  r.Add("folders: subfolders nest, collapse, unpack and become a space", [s, gserver, app, &r] {
    BrowserWindow* w = s->window;
    Workspace* ws = w->workspace();
    Tab* home = w->active_tab();
    Tab* a = w->OpenTabNear(home, gserver->Url(L"/page?t=FolderA"), false);
    Tab* b = w->OpenTabNear(a, gserver->Url(L"/page?t=FolderB"), false);
    const int64_t outer = ws->CreateGroup(L"Outer", 0x4FB3A9, {a});
    Tab* empty = nullptr;
    const int64_t inner = ws->CreateSubfolder(outer, L"Inner", &empty);
    r.Check(inner && ws->FindGroup(inner)->parent_id == outer && ws->GroupDepth(inner) == 1,
            "the subfolder sits in its parent");
    r.Check(empty && empty->empty_placeholder() && empty->record().group_id == inner,
            "a new subfolder holds a hidden empty tab");
    auto find = [w](AccessibleItem::Role role, intptr_t key) {
      for (const auto& item : w->AccessibleItems()) {
        if (item.role == role && item.key == key) return item.rect_dip;
      }
      return D2D1_RECT_F{};
    };
    const D2D1_RECT_F outer_row = find(AccessibleItem::Role::kGroup, outer);
    const D2D1_RECT_F inner_row = find(AccessibleItem::Role::kGroup, inner);
    r.Check(inner_row.right > 0.f && inner_row.left > outer_row.left,
            "the empty subfolder shows, indented under its parent");
    ws->SetTabGroup(b, inner);
    w->DropFolderPlaceholder(inner);
    bool placeholder_left = false;
    for (const auto& t : ws->tabs()) placeholder_left |= t.get() == empty;
    r.Check(!placeholder_left, "a real tab replaces the subfolder's empty tab");
    const D2D1_RECT_F b_row = find(AccessibleItem::Role::kTab, reinterpret_cast<intptr_t>(b));
    r.Check(b_row.left > find(AccessibleItem::Role::kGroup, inner).left, "its tab is indented deeper");
    auto stored = app->store().LoadTabGroups(ws->id());
    bool saved = false;
    for (const auto& g : stored) saved |= g.id == inner && g.parent_id == outer;
    r.Check(saved, "the nesting is stored");
    TabGroupRecord fold = *ws->FindGroup(outer);
    fold.collapsed = true;
    ws->UpdateGroup(fold);
    r.Check(find(AccessibleItem::Role::kGroup, inner).right == 0.f &&
                find(AccessibleItem::Role::kTab, reinterpret_cast<intptr_t>(b)).right == 0.f,
            "collapsing the parent hides the subfolder and its tabs");
    w->ActivateTab(b);
    r.Check(!ws->FindGroup(outer)->collapsed, "selecting a tab inside opens its folders");
    w->UnpackFolder(outer);
    r.Check(!ws->FindGroup(outer) && ws->FindGroup(inner) && ws->FindGroup(inner)->parent_id == 0 &&
                a->record().group_id == 0,
            "unpacking moves its tabs and subfolder up a level");
    const size_t spaces = w->profile()->workspaces().size();
    w->FolderToSpace(inner);
    Workspace* made = w->workspace();
    r.Check(w->profile()->workspaces().size() == spaces + 1 && made != ws &&
                made->record().name == L"Inner",
            "convert to space: a space named after the folder is shown");
    r.Check(b->workspace() == made && b->pinned() && b->record().group_id == 0 && !ws->FindGroup(inner),
            "the folder's tab is pinned there and the folder is gone");
    w->LibraryShowSpace(ws->id(), ws->IndexOf(home));
    w->CloseTabShortcut(a);
  });

  // Live folders (ZenLiveFoldersManager): an RSS folder fills itself;
  // dismissed items stay away, outdated ones go; errors keep the tabs; the
  // GitHub providers read the pull request JSON / issue list.
  auto lserver = std::make_shared<TestHttpServer>();
  struct LiveT {
    int64_t rss = 0;
    int64_t pulls = 0;
    int64_t issues = 0;
  };
  auto lt = std::make_shared<LiveT>();
  auto live_items = [s](int64_t folder) {
    std::vector<Tab*> out;
    for (Tab* t : s->window->workspace()->GroupTabs(folder)) {
      if (!t->record().live_item.empty()) out.push_back(t);
    }
    return out;
  };
  auto live_idle = [app](int64_t id) {
    const auto* e = app->live_folders().Find(id);
    return e && !e->fetching;
  };
  r.Add("live folders: an RSS folder fills from its feed", [s, lserver, app, lt, &r] {
    r.Check(lserver->Start(), "test HTTP server listening");
    app->live_folders().Create(s->window->workspace(), LiveFolderState::Kind::kRss,
                               lserver->Url(L"/feed?items=a,b,c"), [lt](int64_t id) { lt->rss = id; });
  }, [lt, live_items] { return lt->rss && live_items(lt->rss).size() == 3; });
  r.Add("live folders: its tabs, state and storage", [s, app, lt, live_items, &r] {
    Workspace* ws = s->window->workspace();
    const TabGroupRecord* g = ws->FindGroup(lt->rss);
    r.Check(g && g->name == L"Orbit Test Feed" && g->collapsed && g->parent_id == 0,
            "named after the feed, collapsed, top level");
    const auto items = live_items(lt->rss);
    r.Check(items[0]->record().title == L"Item a" && items[2]->record().title == L"Item c" &&
                items[0]->load_state() == Tab::LoadState::kUnloaded,
            "one unloaded tab per item, in feed order");
    bool has_placeholder = false;
    for (Tab* t : ws->GroupTabs(lt->rss)) has_placeholder |= t->empty_placeholder();
    r.Check(has_placeholder, "the folder keeps its hidden empty tab");
    bool stored = false;
    for (const auto& rec : app->store().LoadLiveFolders()) stored |= rec.group_id == lt->rss;
    r.Check(stored, "its state is stored");
    r.Check(app->live_folders().IsLive(lt->rss), "it is a live folder");
    const auto* e = app->live_folders().Find(lt->rss);
    r.Check(e && e->state.last_error.empty() && e->state.last_fetched > 0, "fetched without error");
    // Closing an item dismisses it.
    items[1]->RequestClose();
    r.Check(e && e->dismissed.count(LiveCompositeId(lt->rss, L"item-b")), "closing 'b' dismissed it");
    app->live_folders().Update(lt->rss, [](LiveFolderState& st) {
      st.url = st.url.substr(0, st.url.find(L"items=")) + L"items=a,b,d";
    });
  }, [lt, live_idle] { return live_idle(lt->rss); });
  r.Add("live folders: dismissed items stay away, outdated ones go", [lt, live_items, app, &r] {
    std::wstring titles;
    for (Tab* t : live_items(lt->rss)) titles += t->record().title + L",";
    r.Check(titles == L"Item a,Item d,", "a kept, b stays dismissed, c gone, d added (" + N(titles) + ")");
    bool saved = false;
    for (const auto& rec : app->store().LoadLiveFolders()) {
      saved |= rec.group_id == lt->rss && rec.dismissed.find(L":item-b") != std::wstring::npos;
    }
    r.Check(saved, "the dismissal is stored");
    app->live_folders().Update(lt->rss, [](LiveFolderState& st) {
      st.url = st.url.substr(0, st.url.find(L"/feed")) + L"/page?t=NotAFeed";
    });
  }, [lt, live_idle] { return live_idle(lt->rss); });
  r.Add("live folders: a failed fetch keeps the tabs and shows the error", [s, lt, live_items, app, &r] {
    const auto* e = app->live_folders().Find(lt->rss);
    r.Check(e && e->state.last_error == kLiveFailedFetch, "the error is 'Failed to update'");
    r.Check(live_items(lt->rss).size() == 2, "the tabs are kept");
    bool button = false;
    for (const auto& item : s->window->AccessibleItems()) {
      button |= item.role == AccessibleItem::Role::kButton && item.name == L"Failed to update. Try again.";
    }
    r.Check(button, "the folder shows its error button");
    app->live_folders().Update(lt->rss, [](LiveFolderState& st) {
      st.url = st.url.substr(0, st.url.find(L"/page")) + L"/feed?items=a,d";
    });
  }, [lt, live_idle] { return live_idle(lt->rss); });
  r.Add("live folders: GitHub pull requests from the JSON page", [s, lserver, app, lt, &r] {
    const auto* e = app->live_folders().Find(lt->rss);
    r.Check(e && e->state.last_error.empty(), "the RSS folder recovered");
    app->live_folders().Create(s->window->workspace(), LiveFolderState::Kind::kGithubPulls,
                               lserver->Url(L"/gh/pulls"), [lt](int64_t id) { lt->pulls = id; });
  }, [lt, live_items, live_idle] { return lt->pulls && live_idle(lt->pulls) && live_items(lt->pulls).size() == 2; });
  r.Add("live folders: pull request items, authors and repositories", [s, app, lt, live_items, &r] {
    const auto items = live_items(lt->pulls);
    r.Check(items[0]->record().title == L"Add live folders" && items[0]->record().sublabel == L"alice" &&
                items[0]->url() == L"https://github.com/orbit/browser/pull/7",
            "title, author sublabel and link");
    const auto* e = app->live_folders().Find(lt->pulls);
    r.Check(e && e->state.json_api == 1 && e->state.repos.size() == 2, "JSON page detected, repositories seen");
    r.Check(s->window->workspace()->FindGroup(lt->pulls)->name == L"Pull Requests", "named Pull Requests");
    // Excluding a repository refetches without it (the fake page ignores
    // the query, so both stay; the state must carry the exclusion).
    app->live_folders().Update(lt->pulls, [](LiveFolderState& st) { st.repo_excludes.insert(L"orbit/site"); });
    r.Check(e && e->state.repo_excludes.count(L"orbit/site") &&
                GithubQueries(e->state)[0].find(L"-repo:orbit/site") != std::wstring::npos,
            "the exclusion goes into the search");
  }, [lt, live_idle] { return live_idle(lt->pulls); });
  r.Add("live folders: GitHub issues, and a signed-out account", [s, lserver, app, lt] {
    app->live_folders().Update(lt->pulls, [&](LiveFolderState& st) { st.url = lserver->Url(L"/gh/missing"); });
    app->live_folders().Create(s->window->workspace(), LiveFolderState::Kind::kGithubIssues,
                               lserver->Url(L"/gh/issues"), [lt](int64_t id) { lt->issues = id; });
  }, [lt, live_items, live_idle] {
    return lt->issues && live_idle(lt->issues) && live_idle(lt->pulls) && live_items(lt->issues).size() == 1;
  });
  r.Add("live folders: issue items; no-auth error; deleting forgets the folder", [s, app, lt, live_items, &r] {
    const auto items = live_items(lt->issues);
    r.Check(items[0]->record().title == L"Crash on start" && items[0]->record().sublabel == L"carol",
            "the issue row and its author");
    const auto* pulls = app->live_folders().Find(lt->pulls);
    r.Check(pulls && pulls->state.last_error == kLiveGithubNoAuth && live_items(lt->pulls).size() == 2,
            "a 404 says 'Not signed in to GitHub' and keeps the tabs");
    // Unpacking a live folder leaves ordinary tabs.
    s->window->UnpackFolder(lt->issues);
    r.Check(!app->live_folders().IsLive(lt->issues) && items[0]->record().live_item.empty() &&
                items[0]->record().sublabel.empty(),
            "unpacked: the folder is no longer live, its tab is ordinary");
    s->window->CloseTabShortcut(items[0]);
    s->window->DeleteFolder(lt->pulls);
    s->window->DeleteFolder(lt->rss);
  }, [s, app, lt] {
    return !app->live_folders().IsLive(lt->pulls) && !app->live_folders().IsLive(lt->rss) &&
           !s->window->workspace()->FindGroup(lt->rss);
  });
  r.Add("live folders: deleted folders leave no state", [app, lserver, &r] {
    lserver->Stop();
    r.Check(app->store().LoadLiveFolders().empty(), "no live folder rows left");
  });

  // Transitions: a tab moving into a folder slides to its indent (0.1 s);
  // the pinned separator collapses when the last pinned tab goes (0.08 s).
  auto moved = std::make_shared<Tab*>(nullptr);
  auto pinned_before = std::make_shared<std::vector<Tab*>>();
  r.Add("transitions: the pinned separator opens", [s, moved, pinned_before] {
    BrowserWindow* w = s->window;
    *moved = w->OpenTabNear(w->active_tab(), L"about:blank", false);
    for (const auto& t : w->workspace()->tabs()) {
      if (t->pinned()) pinned_before->push_back(t.get());
    }
    for (Tab* t : *pinned_before) w->SetTabPinned(t, false);  // start without pinned tabs
    w->SetTabPinned(*moved, true);
  }, [s] { return s->window->pinned_separator_shown() >= 1.f; }, 3000);
  auto saw_mid = std::make_shared<bool>(false);
  r.Add("transitions: the pinned separator collapses with the last pinned tab", [s, moved, saw_mid, &r] {
    s->window->SetTabPinned(*moved, false);
    r.Check(s->window->workspace()->PinnedCount() == 0 && s->window->pinned_separator_shown() > 0.f,
            "not gone at once");
  }, [s, saw_mid] {
    const float shown = s->window->pinned_separator_shown();
    *saw_mid |= shown > 0.f && shown < 1.f;
    return shown <= 0.f;
  }, 3000);
  r.Add("transitions: it passed through in between", [saw_mid, &r] {
    r.Check(*saw_mid, "an intermediate height was drawn");
  });
  r.Add("transitions: a tab joining a folder slides in", [s, moved, &r] {
    BrowserWindow* w = s->window;
    Workspace* ws = w->workspace();
    Tab* empty = nullptr;
    const int64_t folder = ws->CreateSubfolder(0, L"Indent", &empty, 0x4FB3A9);
    w->MoveTabToFolderAnimated(*moved, folder);
    r.Check(w->indent_animating(), "the indent animates");
  }, [s] { return !s->window->indent_animating(); }, 3000);
  r.Add("transitions: done", [s, moved, pinned_before] {
    for (Tab* t : *pinned_before) s->window->SetTabPinned(t, true);
    const int64_t folder = (*moved)->record().group_id;
    s->window->DeleteFolder(folder);
  }, [s, moved] { return s->window->workspace()->IndexOf(*moved) < 0; });

  // zen.view.compact.toolbar-flash-popup (off by default): in compact mode,
  // selecting a tab shows the sidebar for 800 ms.
  auto flash_tabs = std::make_shared<std::pair<Tab*, Tab*>>();
  r.Add("compact flash: off by default, on shows the sidebar briefly", [s, set_pref, flash_tabs, &r] {
    BrowserWindow* w = s->window;
    flash_tabs->first = w->active_tab();
    flash_tabs->second = w->OpenTabNear(w->active_tab(), L"about:blank", false);
    w->HandleAccelerator('S', true, false, false);  // compact mode
    w->ActivateTab(flash_tabs->second);
    r.Check(!w->sidebar_flashing(), "no flash while the setting is off");
    set_pref(L"compact_flash", L"1");
    w->ActivateTab(flash_tabs->first);
    r.Check(w->sidebar_flashing(), "selecting a tab flashes the sidebar");
  }, [s] {
    // It hides after 800 ms unless the real pointer rests on the window
    // (then it stays, as a hover would keep it).
    POINT pt;
    RECT wr;
    const bool pointer_on_window = ::GetCursorPos(&pt) && ::GetWindowRect(s->window->hwnd(), &wr) &&
                                   ::PtInRect(&wr, pt);
    return !s->window->sidebar_flashing() || pointer_on_window;
  }, 4000);
  r.Add("compact flash: done", [s, set_pref, flash_tabs, &r] {
    set_pref(L"compact_flash", L"0");
    s->window->HandleAccelerator('S', true, false, false);
    r.Check(!s->window->compact_mode(), "compact mode off again");
    s->window->CloseTabShortcut(flash_tabs->second);
  });

  // ZenDownloadAnimation: the circle flies to the library button; with the
  // sidebar hidden a library box slides in at the corner instead.
  auto flight_start = [s] {
    RECT page = s->window->ContentRectPx();
    POINT p{(page.left + page.right) / 2, (page.top + page.bottom) / 2};
    ::ClientToScreen(s->window->hwnd(), &p);
    return p;
  };
  r.Add("download animation: an arc to the library button", [s, flight_start, &r] {
    s->window->AnimateDownloadFrom(flight_start());
    r.Check(s->window->download_animating() && !s->window->download_box_visible(),
            "the circle flies; no box while the button is visible");
    HWND arc = ::FindWindowExW(nullptr, nullptr, L"OrbitDownloadFlight", nullptr);
    while (arc && ::GetWindow(arc, GW_OWNER) != s->window->hwnd()) {
      arc = ::FindWindowExW(nullptr, arc, L"OrbitDownloadFlight", nullptr);
    }
    const LONG_PTR ex = arc ? ::GetWindowLongPtrW(arc, GWL_EXSTYLE) : 0;
    r.Check((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_NOACTIVATE), "it lets clicks through and never activates");
  }, [s] { return !s->window->download_animating(); }, 3000);
  r.Add("download animation: the box stands in when the sidebar is hidden", [s, flight_start, &r] {
    s->window->HandleAccelerator('S', true, false, false);  // compact mode
    s->window->AnimateDownloadFrom(flight_start());
    r.Check(s->window->download_animating() && s->window->download_box_visible(), "circle and box");
  }, [s] { return !s->window->download_animating() && !s->window->download_box_visible(); }, 5000);
  r.Add("download animation: done", [s, &r] {
    s->window->HandleAccelerator('S', true, false, false);
    r.Check(!s->window->compact_mode(), "compact mode off again");
  });

  // Zen's window keys: Ctrl+Shift+N a new (unsynced) window, Ctrl+Shift+P a
  // private one. Last steps, since the new windows take the foreground.
  r.Add("Ctrl+Shift+N opens a normal window, Ctrl+Shift+P a private one", [s, app, &r] {
    const size_t before = app->windows().size();
    r.Check(s->window->HandleAccelerator('N', true, true, false), "Ctrl+Shift+N handled");
    r.Check(app->windows().size() == before + 1 && !app->windows().back()->profile()->is_private(),
            "a normal window opened");
    r.Check(s->window->HandleAccelerator('P', true, true, false), "Ctrl+Shift+P handled");
    r.Check(app->windows().size() == before + 2 && app->windows().back()->profile()->is_private(),
            "a private window opened");
  });
  // nsZenWorkspaceIcons: dragging a space icon in the footer reorders.
  r.Add("space icons: dragging one past its neighbour reorders the spaces", [s, app, &r] {
    BrowserWindow* w = s->window;
    if (w->profile()->workspaces().size() < 2) app->CreateWorkspace(w->profile(), L"ReorderMe", 0x5AA0E0);
    const auto icons = w->space_icon_rects();
    r.Check(icons.size() >= 2, "two space icons in the footer");
    if (icons.size() < 2) return;
    const auto& spaces = w->profile()->workspaces();
    Workspace* first = spaces[0].get();
    Workspace* second = spaces[1].get();
    const Workspace* shown = w->workspace();
    const float dpi = static_cast<float>(::GetDpiForWindow(w->hwnd())) / 96.f;
    auto at = [dpi](float x, float y) {
      return MAKELPARAM(static_cast<int>(x * dpi), static_cast<int>(y * dpi));
    };
    const float y = (icons[0].top + icons[0].bottom) / 2.f;
    ::SendMessageW(w->hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, at((icons[0].left + icons[0].right) / 2.f, y));
    ::SendMessageW(w->hwnd(), WM_MOUSEMOVE, MK_LBUTTON, at(icons[1].right - 2.f, y));
    r.Check(w->reordering_spaces(), "the drag started");
    ::SendMessageW(w->hwnd(), WM_LBUTTONUP, 0, at(icons[1].right - 2.f, y));
    r.Check(spaces[0].get() == second && spaces[1].get() == first, "the first space is now second");
    r.Check(w->workspace() == shown, "no switch on release");
    bool stored = false;
    const auto records = app->store().LoadWorkspaces(w->profile()->id());
    for (size_t i = 0; i + 1 < records.size(); ++i) {
      if (records[i].id == second->id() && records[i + 1].id == first->id()) stored = true;
    }
    r.Check(stored, "the order is stored");
    w->profile()->MoveWorkspace(first, 0);  // back
  });

  // URL bar results: engine suggestions follow the search row (setting, off
  // by default), arrowing through rows fills the input, Escape restores
  // what was typed, and more than five rows scroll.
  r.Add("url bar: search suggestions, selection fills the input, Escape restores", [s, set_pref, &r] {
    BrowserWindow* w = s->window;
    set_pref(L"search_suggestions", L"1");
    const std::wstring typed = L"orbit suggestion probe";
    w->HandleAccelerator('L', true, false, false);  // focus the address field
    w->TypeInAddressBar(typed);
    w->SetSearchSuggestionsForTesting(typed, {L"orbit suggestion probe alpha", L"orbit suggestion probe beta"});
    const auto& rows = w->url_results();
    r.Check(rows.size() >= 3 && rows[1].title == L"orbit suggestion probe alpha" &&
                rows[1].kind == OmniboxRow::Kind::kSearch,
            "the suggestions follow the search row");
    w->MoveUrlSelectionForTesting(1);
    r.Check(w->address_text() == L"orbit suggestion probe alpha", "the selected suggestion fills the input");
    w->MoveUrlSelectionForTesting(-1);
    r.Check(w->address_text() == typed, "back on the first row: what was typed");
    w->CancelAddressBar();
    set_pref(L"search_suggestions", L"0");
  });
  r.Add("url bar: more than five rows scroll", [s, app, &r] {
    BrowserWindow* w = s->window;
    for (int i = 0; i < 12; ++i) {
      app->store().AddVisit(w->profile()->id(), L"https://orbitscroll" + std::to_wstring(i) + L".example/",
                            L"Orbitscroll " + std::to_wstring(i), 1000 + i);
    }
    w->HandleAccelerator('L', true, false, false);  // focus the address field
    w->TypeInAddressBar(L"orbitscroll");
    r.Check(w->url_results().size() == 10, "ten rows (" + std::to_string(w->url_results().size()) + ")");
    for (int i = 0; i < 7; ++i) w->MoveUrlSelectionForTesting(1);
    r.Check(w->selected_url_result() == 7 && w->first_visible_url_result() == 3, "the selection stays in view");
    r.Check(w->address_text().rfind(L"https://orbitscroll", 0) == 0, "a history row fills its URL");
    w->CancelAddressBar();
  });

  // ZenSpacesSwipe: horizontal scrolling over the sidebar drags the space;
  // a short pull springs back, a longer one switches.
  auto swipe_from = std::make_shared<Workspace*>(nullptr);
  auto hwheel = [s](int delta) {
    POINT pt{40, 300};  // over the tab list
    const float dpi = static_cast<float>(::GetDpiForWindow(s->window->hwnd())) / 96.f;
    pt.x = static_cast<LONG>(pt.x * dpi);
    pt.y = static_cast<LONG>(pt.y * dpi);
    ::ClientToScreen(s->window->hwnd(), &pt);
    ::SendMessageW(s->window->hwnd(), WM_MOUSEHWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), MAKELPARAM(pt.x, pt.y));
  };
  r.Add("swipe: a short pull drags the strip and springs back", [s, app, swipe_from, hwheel, &r] {
    if (s->window->profile()->workspaces().size() < 2) {
      app->CreateWorkspace(s->window->profile(), L"SwipeTarget", 0x5AA0E0);
    }
    *swipe_from = s->window->workspace();
    hwheel(20);
    r.Check(s->window->swiping() && s->window->swipe_offset() < 0.f, "the strip follows (to the left)");
  }, [s, swipe_from] {
    return !s->window->swiping() && s->window->swipe_offset() == 0.f && s->window->workspace() == *swipe_from;
  }, 3000);
  r.Add("swipe: a longer pull switches to the next space", [s, hwheel] {
    for (int i = 0; i < 6; ++i) hwheel(40);
  }, [s, swipe_from, &r] {
    if (s->window->swiping() || s->window->workspace() == *swipe_from) return false;
    r.Check(s->window->space_switch_animating() || !ui::AnimationsEnabled(), "and slides on from where the pull left it");
    return true;
  }, 3000);
  r.Add("swipe: back", [s, swipe_from] {
    s->window->LibraryShowSpace((*swipe_from)->id(), -1);
  }, [s, swipe_from] { return s->window->workspace() == *swipe_from && !s->window->space_switch_animating(); });

  // Boosts: the editor for the page's site; a setting reaches the page as a
  // constructed style sheet; Zap picks an element with a real click and
  // hides it; deleting removes it all.
  auto boost_server = std::make_shared<TestHttpServer>();
  auto boost_tab = std::make_shared<Tab*>(nullptr);
  auto page_js = [boost_tab](const std::string& code) {
    if (CefRefPtr<CefBrowser> b = (*boost_tab)->browser()) b->GetMainFrame()->ExecuteJavaScript(code, "", 0);
  };
  auto editor_js = [s](const std::string& code) {
    if (CefRefPtr<CefBrowser> b = s->window->boost_editor_browser()) b->GetMainFrame()->ExecuteJavaScript(code, "", 0);
  };
  r.Add("boosts: a page and its boost editor", [s, boost_server, boost_tab, &r] {
    r.Check(boost_server->Start(), "test HTTP server listening");
    *boost_tab = s->window->OpenTab(boost_server->Url(L"/links"), true);
  }, [s, boost_tab] {
    if ((*boost_tab)->record().title != L"links" || (*boost_tab)->loading()) return false;
    if (!s->window->boost_editor_open()) s->window->OpenBoostEditor();
    return s->window->boost_editor_title() == L"Boost: 127.0.0.1";
  }, 10000);
  r.Add("boosts: Size reaches the page", [editor_js] {
    editor_js("document.getElementById('size').click();");  // 100% -> 110%
  }, [s, app, page_js, boost_tab] {
    const auto stored = ParseBoost(app->store().LoadBoost(s->window->profile()->id(), L"127.0.0.1"));
    if (!stored || std::fabs(stored->size - 1.1f) > 0.001f) return false;
    page_js("document.title = 'Z:' + getComputedStyle(document.documentElement).zoom;");
    return (*boost_tab)->record().title == L"Z:1.1";
  }, 6000);
  r.Add("boosts: Zap hides the element a real click picks", [s, boost_tab, page_js, &r] {
    s->window->SetBoostPickMode(L"zap");
    r.Check(s->window->boost_pick_mode() == L"zap", "zap mode on");
    // The page itself cannot zap: a synthetic click is ignored.
    page_js("const f = document.createElement('p'); f.id = 'fake'; f.textContent = 'x'; document.body.append(f);"
            "f.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true}));");
    CefMouseEvent e;
    e.x = 60;
    e.y = 60;
    CefRefPtr<CefBrowserHost> host = (*boost_tab)->browser()->GetHost();
    host->SendMouseMoveEvent(e, false);
    host->SendMouseClickEvent(e, MBT_LEFT, false, 1);
    host->SendMouseClickEvent(e, MBT_LEFT, true, 1);
  }, [s, app, page_js, boost_tab] {
    const auto stored = ParseBoost(app->store().LoadBoost(s->window->profile()->id(), L"127.0.0.1"));
    if (!stored || stored->zap.size() != 1 || stored->zap[0] != L"#l") return false;  // not #fake
    page_js("document.title = 'D:' + getComputedStyle(document.getElementById('l')).display;");
    return (*boost_tab)->record().title == L"D:none";
  }, 8000);
  r.Add("boosts: deleting restores the page", [s, editor_js] {
    s->window->SetBoostPickMode(L"off");
    editor_js("orbitAction('/api/delete');");
  }, [s, app, page_js, boost_tab] {
    if (!app->store().LoadBoost(s->window->profile()->id(), L"127.0.0.1").empty()) return false;
    page_js("document.title = 'R:' + getComputedStyle(document.getElementById('l')).display;");
    return (*boost_tab)->record().title == L"R:block";
  }, 6000);
  r.Add("boosts: done", [s, boost_server, boost_tab] {
    s->window->CloseBoostEditor();
    s->window->CloseTabShortcut(*boost_tab);
    boost_server->Stop();
  });

  // Copy URL strips tracking parameters (zenStrippedURI) and the toast
  // offers Share for web links.
  auto share_server = std::make_shared<TestHttpServer>();
  auto share_tab = std::make_shared<Tab*>(nullptr);
  r.Add("copy URL: a web page with tracking parameters", [s, share_server, share_tab, &r] {
    r.Check(share_server->Start(), "test HTTP server listening");
    *share_tab = s->window->OpenTab(share_server->Url(L"/page?t=Share&utm_source=news&id=7&fbclid=x"), true);
  }, [share_tab] { return (*share_tab)->record().title == L"Share" && !(*share_tab)->loading(); });
  r.Add("copy URL: the copy is clean and the toast offers Share", [s, share_server, &r] {
    r.Check(s->window->HandleAccelerator('C', true, true, false), "Ctrl+Shift+C handled");
    const std::wstring clean = share_server->Url(L"/page?t=Share&id=7");
    r.Check(ClipboardText() == clean, "tracking parameters stripped ('" + N(ClipboardText()) + "')");
    const POINT button = s->window->toast_button_center();
    HWND toast = button.x >= 0 ? s->window->toast_hwnd() : nullptr;
    r.Check(toast != nullptr, "the toast has a share button");
    if (toast) {
      POINT client = button;
      ::ScreenToClient(toast, &client);
      const LPARAM at = MAKELPARAM(client.x, client.y);
      ::SendMessageW(toast, WM_MOUSEMOVE, 0, at);
      ::SendMessageW(toast, WM_LBUTTONUP, 0, at);
    }
    r.Check(s->window->last_share_url() == clean, "Share hands the clean URL to the share sheet");
  });
  r.Add("copy URL: done", [s, share_server, share_tab] {
    s->window->CloseTabShortcut(*share_tab);
    share_server->Stop();
  });

  // Theme picker ("Edit Theme…", nsZenThemePicker): picking colours on
  // the pad themes the space live; the result is stored when it closes.
  auto js = [s](const std::string& code) {
    if (CefRefPtr<CefBrowser> b = s->window->theme_picker_browser()) b->GetMainFrame()->ExecuteJavaScript(code, "", 0);
  };
  auto stored_theme = [app, s] {
    for (const auto& w : app->store().LoadWorkspaces(s->window->profile()->id())) {
      if (w.id == s->window->workspace()->id()) return w.theme;
    }
    return std::wstring();
  };
  r.Add("theme picker: Edit Theme opens the picker for the space", [s] {
    s->window->OpenThemePicker(s->window->workspace());
  }, [s] {
    return s->window->theme_picker_open() &&
           s->window->theme_picker_title() == L"Edit Theme: " + s->window->workspace()->record().name;
  }, 10000);
  r.Add("theme picker: a click on the pad adds the primary colour", [js] {
    js("const r = pad.getBoundingClientRect();"
       "pad.dispatchEvent(new MouseEvent('click', {bubbles: true, clientX: r.left + 280, clientY: r.top + 120, button: 0}));");
  }, [s, &r] {
    const auto t = ParseSpaceTheme(s->window->workspace()->record().theme);
    if (!t || t->dots.size() != 1) return false;
    r.Check(t->dots[0].primary && s->window->space_themed(), "one primary colour; the window is themed");
    return true;
  }, 5000);
  r.Add("theme picker: Add colour brings a harmony", [js] {
    js("document.getElementById('add').click();");
  }, [s] {
    const auto t = ParseSpaceTheme(s->window->workspace()->record().theme);
    return t && t->dots.size() == 2 && t->algorithm == L"complementary";
  }, 5000);
  r.Add("theme picker: a preset sets three analogous colours, opacity and grain", [js] {
    js("document.querySelectorAll('#pages .swatch')[8].click();"
       "slider.value = 0.7; slider.dispatchEvent(new Event('input')); slider.dispatchEvent(new Event('change'));"
       "texture = 0.25; send(true);");
  }, [s, &r] {
    const auto t = ParseSpaceTheme(s->window->workspace()->record().theme);
    if (!t || t->dots.size() != 3 || t->texture != 0.25f) return false;
    r.Check(t->algorithm == L"analogous" && t->opacity > 0.69f && t->opacity < 0.71f, "analogous, opacity 0.7");
    const D2D1_COLOR_F top_left = s->window->backdrop_at(0.f, 0.f);
    RECT client;
    ::GetClientRect(s->window->hwnd(), &client);
    const float dpi = static_cast<float>(::GetDpiForWindow(s->window->hwnd())) / 96.f;
    const D2D1_COLOR_F bottom = s->window->backdrop_at(client.right / dpi / 2.f, client.bottom / dpi);
    r.Check(std::abs(top_left.r - bottom.r) + std::abs(top_left.g - bottom.g) + std::abs(top_left.b - bottom.b) > 0.05f,
            "the backdrop is a gradient (corners differ)");
    return true;
  }, 5000);
  r.Add("theme picker: closing stores the theme", [s, stored_theme, &r] {
    r.Check(s->window->library_alive(), "the picker's browser counts for the window's close");
    // As Alt+F4 on the panel would: it closes the picker, the popup survives.
    HWND popup = ::FindWindowW(L"OrbitSiteData", L"Edit Theme");
    r.Check(popup != nullptr, "the picker popup exists");
    if (popup) ::SendMessageW(popup, WM_CLOSE, 0, 0);
    r.Check(!s->window->theme_picker_open() && popup && ::IsWindow(popup), "WM_CLOSE hides it without destroying it");
    const auto t = ParseSpaceTheme(stored_theme());
    r.Check(t && t->dots.size() == 3, "stored with three colours");
    s->window->SetSpaceTheme(s->window->workspace(), std::nullopt, true);  // back to the plain colour
    r.Check(!s->window->space_themed() && stored_theme().empty(), "cleared again");
  });

  // Space menu "Manage Spaces…" (cmd_zenManageWorkspaces): the library on
  // its Spaces section; again, it closes.
  r.Add("Manage Spaces opens the library on Spaces", [s] {
    s->window->ToggleLibrarySection(L"spaces");
  }, [s, app, &r] {
    if (!s->window->library_open() || !s->window->library_browser_id()) return false;
    r.Check(app->store().GetPref(Store::kGlobalScope, L"library_last_tab").value_or(L"") == L"spaces",
            "the Spaces section is remembered");
    s->window->ToggleLibrarySection(L"spaces");
    r.Check(!s->window->library_open(), "asking again closes it");
    return true;
  }, 8000);
  // The suite ends with the library open: the window close must wait for
  // (and not hang on) its browser.
  r.Add("left open for the shutdown", [s] {
    s->window->OpenLibrary();
    s->window->OpenThemePicker(s->window->workspace());  // and the theme picker
  }, [s] { return s->window->library_browser_id() != 0 && s->window->theme_picker_browser_id() != 0; }, 8000);
}
}  // namespace

bool IsActive() { return g_active; }

// Self-test runs only: a crash inside a window procedure ends the process
// with STATUS_FATAL_USER_CALLBACK_EXCEPTION and no report; log the faulting
// frames (resolved through the PDB) so the cause is visible in orbit.log.
void LogStack() {
  static bool once = false;
  if (once) return;
  once = true;
  HANDLE process = ::GetCurrentProcess();
  ::SymSetOptions(::SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  ::SymInitialize(process, nullptr, TRUE);
  void* frames[24];
  const USHORT n = ::RtlCaptureStackBackTrace(0, 24, frames, nullptr);
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
  for (USHORT i = 0; i < n; ++i) {
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD line_displacement = 0;
    const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
    const bool named = ::SymFromAddr(process, address, &displacement, symbol);
    const bool lined = ::SymGetLineFromAddr64(process, address, &line_displacement, &line);
    ORBIT_LOG_ERROR("selftest:   #%u %s %s:%lu", i, named ? symbol->Name : "?",
                    lined ? line.FileName : "", lined ? line.LineNumber : 0ul);
  }
}

LONG CALLBACK LogFatalException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_ILLEGAL_INSTRUCTION && code != 0xC0000409 /* fail fast */) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  ORBIT_LOG_ERROR("selftest: exception 0x%08lx at %p", code, info->ExceptionRecord->ExceptionAddress);
  LogStack();
  return EXCEPTION_CONTINUE_SEARCH;
}

// Debug builds: a failed CRT/STL assertion (e.g. a bad iterator) calls
// abort(); log its message and where it happened.
int LogCrtReport(int type, char* message, int* result) {
  if (type == _CRT_WARN) return FALSE;
  ORBIT_LOG_ERROR("selftest: CRT report: %s", message ? message : "");
  LogStack();
  *result = 0;
  return FALSE;
}

void LogAbort(int) {
  ORBIT_LOG_ERROR("selftest: abort()");
  LogStack();
}

void Start(const std::wstring& suite) {
  g_active = true;
  ::AddVectoredExceptionHandler(1, &LogFatalException);
  _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &LogCrtReport);
  std::signal(SIGABRT, &LogAbort);
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
  } else if (suite == L"restore-3") {
    AddRestorePhase3(*g_runner);
  } else if (suite == L"zen") {
    AddZenSuite(*g_runner);
  } else {
    g_runner->Check(false, "unknown suite " + N(suite));
  }
  g_runner->Begin();
}


}  // namespace orbit::self_test
