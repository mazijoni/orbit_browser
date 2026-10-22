#include "browser/live_folders.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>

#include "app/application.h"
#include "browser/profile.h"
#include "browser/text_fetch.h"
#include "browser/tab.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/base/cef_callback.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "ui/browser_window.h"
#include "ui/theme.h"

namespace orbit {
namespace {

constexpr int64_t kTickMs = 30 * 1000;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// NetworkHelper.isTextMimeType, roughly: text, XML and JSON types.

std::wstring HostOf(const std::wstring& url) {
  const size_t scheme = url.find(L"://");
  if (scheme == std::wstring::npos) return url;
  const size_t end = url.find_first_of(L"/?#:", scheme + 3);
  std::wstring host = url.substr(scheme + 3, end == std::wstring::npos ? std::wstring::npos : end - scheme - 3);
  if (host.rfind(L"www.", 0) == 0) host = host.substr(4);
  return host;
}

std::wstring JoinLines(const std::set<std::wstring>& set) {
  std::wstring out;
  for (const auto& s : set) out += s + L"\n";
  return out;
}

std::set<std::wstring> SplitLines(const std::wstring& text) {
  std::set<std::wstring> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find(L'\n', start);
    if (nl == std::wstring::npos) nl = text.size();
    if (nl > start) out.insert(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

// The item ids of a feed or page may hold anything; keep them on one line.
std::wstring CleanId(std::wstring id) {
  for (wchar_t& c : id) {
    if (c < 0x20) c = L' ';
  }
  return id.substr(0, 2000);
}

// pullsDashboardSurfaceContentRoute (github.com/pulls with Accept: JSON).
bool ParsePullsJson(const std::string& text, GithubPage& page) {
  CefRefPtr<CefValue> root = CefParseJSON(text, JSON_PARSER_RFC);
  if (!root || root->GetType() != VTYPE_DICTIONARY) return false;
  CefRefPtr<CefDictionaryValue> payload = root->GetDictionary()->GetDictionary("payload");
  CefRefPtr<CefDictionaryValue> route =
      payload ? payload->GetDictionary("pullsDashboardSurfaceContentRoute") : nullptr;
  CefRefPtr<CefListValue> results = route ? route->GetList("results") : nullptr;
  if (!results) return true;  // JSON, but nothing we know: no items
  for (size_t i = 0; i < results->GetSize(); ++i) {
    CefRefPtr<CefDictionaryValue> pr = results->GetDictionary(i);
    if (!pr) continue;
    const std::wstring repo = pr->GetString("repoNameWithOwner").ToWString();
    const CefRefPtr<CefValue> number = pr->GetValue("number");
    const int64_t n = !number ? 0
                      : number->GetType() == VTYPE_INT ? number->GetInt()
                                                       : static_cast<int64_t>(number->GetDouble());
    const std::wstring url = pr->GetString("permalink").ToWString();
    if (repo.empty() || !n || !StartsWithNoCase(url, L"https://")) continue;
    LiveItem item;
    item.id = repo + L"#" + std::to_wstring(n);
    item.title = pr->GetString("title").ToWString();
    if (CefRefPtr<CefDictionaryValue> author = pr->GetDictionary("author")) {
      item.subtitle = author->GetString("displayLogin").ToWString();
    }
    item.url = url;
    page.repos.insert(repo);
    page.items.push_back(std::move(item));
  }
  return true;
}

}  // namespace

void LiveFolders::Load() {
  if (loaded_) return;
  loaded_ = true;
  for (const LiveFolderRecord& r : Application::Get()->store().LoadLiveFolders()) {
    auto state = ParseLiveState(r.state);
    if (!state || !WorkspaceOf(r.group_id)) continue;  // no point without its folder
    Entry e;
    e.group_id = r.group_id;
    e.state = std::move(*state);
    e.dismissed = SplitLines(r.dismissed);
    entries_[r.group_id] = std::move(e);
  }
  ScheduleTick();
}

void LiveFolders::Shutdown() {
  ++generation_;  // stops the clock; fetches in flight find nothing to apply
  entries_.clear();
}

void LiveFolders::ScheduleTick() {
  const int generation = generation_;
  CefPostDelayedTask(TID_UI, base::BindOnce([](int g) {
    LiveFolders& self = Application::Get()->live_folders();
    if (g != self.generation_) return;
    self.Tick();
    self.ScheduleTick();
  }, generation), kTickMs);
}

void LiveFolders::Tick() {
  // start(): fetch when interval has passed since lastFetched (this also
  // covers waking from sleep).
  const int64_t now = NowMs();
  for (auto& [id, e] : entries_) {
    if (!e.fetching && now - e.state.last_fetched >= e.state.interval_ms) Fetch(e);
  }
}

Workspace* LiveFolders::WorkspaceOf(int64_t group_id) const {
  for (const auto& p : Application::Get()->profiles()) {
    if (p->is_private()) continue;
    for (const auto& ws : p->workspaces()) {
      if (ws->FindGroup(group_id)) return ws.get();
    }
  }
  return nullptr;
}

const LiveFolders::Entry* LiveFolders::Find(int64_t group_id) const {
  const auto it = entries_.find(group_id);
  return it == entries_.end() ? nullptr : &it->second;
}

void LiveFolders::Save(const Entry& e) {
  Application::Get()->store().SaveLiveFolder({e.group_id, SerializeLiveState(e.state), JoinLines(e.dismissed)});
}

void LiveFolders::Create(Workspace* ws, LiveFolderState::Kind kind, const std::wstring& feed_url,
                         std::function<void(int64_t)> done) {
  if (!ws || ws->profile()->is_private()) {
    if (done) done(0);
    return;
  }
  const int64_t ws_id = ws->id();
  Profile* profile = ws->profile();
  auto make = [this, ws_id, profile, kind](const std::wstring& label, const std::wstring& url) -> int64_t {
    Workspace* target = profile->FindWorkspace(ws_id);
    if (!target) return 0;
    const uint32_t color = ui::kAccentChoices[(target->groups().size() + 1) % std::size(ui::kAccentChoices)];
    const int64_t id = target->CreateSubfolder(0, label, nullptr, color);
    if (!id) return 0;
    TabGroupRecord g = *target->FindGroup(id);
    g.collapsed = true;  // createFolder(..., {collapsed: true})
    target->UpdateGroup(g);
    Entry e;
    e.group_id = id;
    e.state.kind = kind;
    e.state.url = url;
    entries_[id] = e;
    Save(entries_[id]);
    for (BrowserWindow* w : Application::Get()->WindowsShowing(target)) w->OnTabsChanged();
    return id;
  };
  if (kind != LiveFolderState::Kind::kRss) {
    const int64_t id = make(kind == LiveFolderState::Kind::kGithubPulls ? L"Pull Requests" : L"Issues",
                            feed_url.empty() ? DefaultGithubUrl(kind) : feed_url);
    if (id) Fetch(entries_[id]);
    if (done) done(id);
    return;
  }
  // The folder is named after the feed (getMetadata), so ask it first; the
  // same answer fills the folder.
  const int generation = generation_;
  FetchText(profile, feed_url, L"", /*cookies=*/true, [this, make, feed_url, done, generation](int status,
                                                                                const std::string& body) {
    if (generation != generation_) return;
    const std::optional<Feed> feed = status == 200 ? ParseFeed(body) : std::nullopt;
    std::wstring label = feed ? feed->title : std::wstring();
    if (label.empty()) label = HostOf(feed_url);
    const int64_t id = make(label.substr(0, 200), feed_url);
    if (id) {
      Fetched result;
      if (!feed) {
        result.error = kLiveFailedFetch;
      } else {
        const Entry& e = entries_[id];
        result.items = FilterFeedItems(feed->items, NowMs(), e.state.time_range_ms, e.state.max_items);
      }
      entries_[id].state.last_fetched = NowMs();
      Apply(id, result);
    }
    if (done) done(id);
  });
}

void LiveFolders::Refresh(int64_t group_id) {
  const auto it = entries_.find(group_id);
  if (it != entries_.end() && !it->second.fetching) Fetch(it->second);
}

void LiveFolders::Update(int64_t group_id, const std::function<void(LiveFolderState&)>& change) {
  const auto it = entries_.find(group_id);
  if (it == entries_.end()) return;
  change(it->second.state);
  Save(it->second);
  if (!it->second.fetching) Fetch(it->second);
}

void LiveFolders::Fetch(Entry& entry) {
  Workspace* ws = WorkspaceOf(entry.group_id);
  if (!ws) {  // the folder is gone
    Application::Get()->store().DeleteLiveFolder(entry.group_id);
    const int64_t id = entry.group_id;
    entries_.erase(id);
    return;
  }
  entry.fetching = true;
  const int64_t id = entry.group_id;
  const int generation = generation_;
  auto finish = [this, id, generation](Fetched result) {
    if (generation != generation_) return;
    const auto it = entries_.find(id);
    if (it == entries_.end()) return;
    it->second.fetching = false;
    it->second.state.last_fetched = NowMs();
    if (result.json_api >= 0) it->second.state.json_api = result.json_api;
    if (result.error.empty() && it->second.state.kind != LiveFolderState::Kind::kRss) {
      it->second.state.repos = result.repos;
    }
    Apply(id, result);
  };
  if (entry.state.kind == LiveFolderState::Kind::kRss) {
    const LiveFolderState state = entry.state;
    FetchText(ws->profile(), state.url, L"", /*cookies=*/true, [state, finish](int status, const std::string& body) {
      Fetched r;
      const std::optional<Feed> feed = status == 200 ? ParseFeed(body) : std::nullopt;
      if (!feed) {
        r.error = kLiveFailedFetch;
      } else {
        r.items = FilterFeedItems(feed->items, NowMs(), state.time_range_ms, state.max_items);
      }
      finish(std::move(r));
    });
    return;
  }
  FetchGithub(ws->profile(), entry.state, finish);
}

void LiveFolders::FetchGithub(Profile* profile, LiveFolderState state, std::function<void(Fetched)> done) {
  const bool pulls = state.kind == LiveFolderState::Kind::kGithubPulls;
  if (!state.author_me && !state.assigned_me && !(pulls && state.review_requested)) {
    done({kLiveGithubNoFilter, {}, {}, -1});
    return;
  }
  constexpr wchar_t kAccept[] = L"application/json,text/html";
  if (pulls && state.json_api < 0) {
    // Which /pulls page this account gets (new JSON-capable UI or classic).
    FetchText(profile, state.url, kAccept, /*cookies=*/true, [this, profile, state, done](int status, const std::string& body) mutable {
      if (status == 404) return done({kLiveGithubNoAuth, {}, {}, -1});
      if (status != 200) return done({kLiveFailedFetch, {}, {}, -1});
      CefRefPtr<CefValue> json = CefParseJSON(body, JSON_PARSER_RFC);
      state.json_api = json && json->GetType() == VTYPE_DICTIONARY ? 1 : 0;
      FetchGithub(profile, state, [done, api = state.json_api](Fetched r) {
        if (r.json_api < 0) r.json_api = api;
        done(std::move(r));
      });
    });
    return;
  }
  const std::vector<std::wstring> queries = GithubQueries(state);
  if (queries.empty()) {
    done({kLiveGithubNoFilter, {}, {}, -1});
    return;
  }
  // The queries run one after another; results merge by id.
  struct Run {
    Profile* profile;
    LiveFolderState state;
    std::vector<std::wstring> queries;
    size_t next = 0;
    Fetched result;
    std::set<std::wstring> seen;
    std::function<void(Fetched)> done;
  };
  auto run = std::make_shared<Run>(Run{profile, state, queries, 0, {}, {}, std::move(done)});
  auto step = std::make_shared<std::function<void()>>();
  *step = [run, step] {
    if (run->next >= run->queries.size()) {
      Fetched out = std::move(run->result);
      auto finish = std::move(run->done);
      *step = nullptr;  // breaks the cycle
      finish(std::move(out));
      return;
    }
    const std::wstring url = run->state.url + L"?q=" + EscapeQueryComponent(run->queries[run->next++]);
    const bool pulls = run->state.kind == LiveFolderState::Kind::kGithubPulls;
    FetchText(run->profile, url, pulls ? L"application/json,text/html" : L"", /*cookies=*/true,
              [run, step, url, pulls](int status, const std::string& body) {
      auto fail = [&](const wchar_t* error) {
        Fetched out;
        out.error = error;
        out.json_api = run->result.json_api;
        auto finish = std::move(run->done);
        *step = nullptr;
        finish(std::move(out));
      };
      // Assume no auth on 404; any other error is a failed fetch, not an
      // empty result (which would close every tab in the folder).
      if (status == 404) return fail(kLiveGithubNoAuth);
      if (status != 200) return fail(kLiveFailedFetch);
      GithubPage page;
      if (pulls) {
        if (ParsePullsJson(body, page)) {
          run->result.json_api = 1;
        } else if (run->state.json_api == 1) {
          run->result.json_api = 0;  // the page changed; the next fetch asks the classic way
          return fail(kLiveFailedFetch);
        } else {
          page = ParseGithubPullsHtml(body, url);
        }
      } else {
        page = ParseGithubIssuesHtml(body, url);
      }
      for (LiveItem& item : page.items) {
        if (run->seen.insert(item.id).second) run->result.items.push_back(std::move(item));
      }
      run->result.repos.insert(page.repos.begin(), page.repos.end());
      (*step)();
    });
  };
  (*step)();
}

void LiveFolders::Apply(int64_t group_id, const Fetched& result) {
  // onLiveFolderFetch.
  auto it = entries_.find(group_id);
  if (it == entries_.end()) return;
  Entry& e = it->second;
  Workspace* ws = WorkspaceOf(group_id);
  if (!ws) {
    Application::Get()->store().DeleteLiveFolder(group_id);
    entries_.erase(it);
    return;
  }
  e.state.last_error = result.error;
  const std::vector<BrowserWindow*> windows = Application::Get()->WindowsShowing(ws);
  if (!result.error.empty()) {
    Save(e);
    for (BrowserWindow* w : windows) w->OnTabsChanged();
    return;
  }
  std::vector<LiveItem> items = result.items;
  for (LiveItem& item : items) item.id = CleanId(item.id);
  std::vector<std::wstring> existing;
  const std::wstring prefix = std::to_wstring(group_id) + L":";
  for (Tab* t : ws->GroupTabs(group_id)) {
    if (t->record().live_item.rfind(prefix, 0) == 0) existing.push_back(t->record().live_item);
  }
  const LiveDiff diff = DiffLiveItems(group_id, existing, items, e.dismissed);
  // Outdated items go (skipSessionStore: not into recently closed).
  removing_ = true;
  for (const std::wstring& gone : diff.remove) {
    for (Tab* t : ws->GroupTabs(group_id)) {
      if (t->record().live_item != gone) continue;
      if (t->load_state() == Tab::LoadState::kUnloaded) {
        Application::Get()->RemoveTab(t, /*remember=*/false);
      } else {
        t->mutable_record().live_item.clear();  // its close is ours, not a dismissal
        t->RequestClose();
      }
      break;
    }
  }
  removing_ = false;
  // New items join the end of the folder, unloaded (createLazyBrowser).
  for (const LiveItem& item : diff.add) {
    const auto members = ws->GroupTabs(group_id);
    const int index = members.empty() ? -1 : ws->IndexOf(members.back()) + 1;
    TabRecord rec;
    rec.url = item.url;
    rec.title = item.title.empty() ? item.url : item.title;
    rec.group_id = group_id;
    rec.live_item = LiveCompositeId(group_id, item.id);
    rec.sublabel = item.subtitle;
    ws->InsertTab(std::make_unique<Tab>(ws, rec), index);
  }
  Save(e);
  for (BrowserWindow* w : windows) w->OnTabsChanged();
}

void LiveFolders::OnTabDismissed(Tab* tab) {
  std::wstring& live = tab->mutable_record().live_item;
  if (live.empty()) return;
  const size_t colon = live.find(L':');
  const int64_t folder = colon == std::wstring::npos ? 0 : _wtoi64(live.substr(0, colon).c_str());
  if (!removing_) {
    if (auto it = entries_.find(folder); it != entries_.end()) {
      it->second.dismissed.insert(live);
      Save(it->second);
    }
  }
  live.clear();
  tab->mutable_record().sublabel.clear();
}

void LiveFolders::ForgetTab(Tab* tab) {
  if (tab->record().live_item.empty()) return;
  tab->mutable_record().live_item.clear();
}

void LiveFolders::OnFolderRemoved(int64_t group_id) {
  const auto it = entries_.find(group_id);
  if (it == entries_.end()) return;
  // Its item tabs (unpacked) become ordinary tabs.
  if (Workspace* ws = WorkspaceOf(group_id)) {
    const std::wstring prefix = std::to_wstring(group_id) + L":";
    for (const auto& t : ws->tabs()) {
      if (t->record().live_item.rfind(prefix, 0) == 0) {
        t->mutable_record().live_item.clear();
        t->mutable_record().sublabel.clear();
      }
    }
  }
  entries_.erase(it);
  Application::Get()->store().DeleteLiveFolder(group_id);
}

}  // namespace orbit
