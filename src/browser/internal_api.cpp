// JSON endpoints for orbit:// pages. Built on CEF's JSON parser/writer so no
// hand-written escaping is involved. Every endpoint is scoped to the profile
// whose request context served the request.

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>

#include "app/application.h"
#include "browser/browser_client.h"
#include "browser/downloads.h"
#include "browser/internal_page_parts.h"
#include "browser/permissions.h"
#include "browser/profile.h"
#include "core/bookmark_html.h"
#include "core/process_metrics.h"
#include "core/strings.h"
#include "browser/tab.h"
#include "include/base/cef_callback.h"
#include "include/cef_cookie.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/cef_parser.h"
#include "include/cef_values.h"
#include "include/cef_version.h"
#include "ui/browser_window.h"

namespace orbit {
namespace {

using Dict = CefRefPtr<CefDictionaryValue>;

InternalResponse Json(Dict dict, int status = 200) {
  CefRefPtr<CefValue> value = CefValue::Create();
  value->SetDictionary(dict);
  return {status, "application/json", CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString()};
}

InternalResponse Error(int status, const char* message) {
  Dict d = CefDictionaryValue::Create();
  d->SetString("error", message);
  return Json(d, status);
}

InternalResponse Ok() {
  Dict d = CefDictionaryValue::Create();
  d->SetBool("ok", true);
  return Json(d);
}

Dict ParseBody(const InternalRequest& req) {
  CefRefPtr<CefValue> v = CefParseJSON(req.body, JSON_PARSER_RFC);
  if (v && v->GetType() == VTYPE_DICTIONARY) return v->GetDictionary();
  return CefDictionaryValue::Create();
}

// JSON numbers arrive as int or double; ids/timestamps exceed int32.
int64_t Number(const Dict& d, const char* key) {
  if (!d->HasKey(key)) return 0;
  switch (d->GetType(key)) {
    case VTYPE_INT:
      return d->GetInt(key);
    case VTYPE_DOUBLE:
      return static_cast<int64_t>(d->GetDouble(key));
    default:
      return 0;
  }
}

std::wstring Str(const Dict& d, const char* key) {
  return d->HasKey(key) && d->GetType(key) == VTYPE_STRING ? d->GetString(key).ToWString()
                                                            : std::wstring();
}

std::map<std::wstring, std::wstring> ParseQuery(const std::wstring& query) {
  std::map<std::wstring, std::wstring> out;
  size_t start = 0;
  while (start < query.size()) {
    size_t end = query.find(L'&', start);
    if (end == std::wstring::npos) end = query.size();
    const std::wstring pair = query.substr(start, end - start);
    const size_t eq = pair.find(L'=');
    std::wstring key = pair.substr(0, eq);
    std::wstring value = eq == std::wstring::npos ? L"" : pair.substr(eq + 1);
    std::replace(value.begin(), value.end(), L'+', L' ');
    if (value.empty()) {  // CefURIDecode rejects empty input
      out[key] = L"";
      start = end + 1;
      continue;
    }
    out[key] = CefURIDecode(value, true,
                            static_cast<cef_uri_unescape_rule_t>(
                                UU_SPACES | UU_PATH_SEPARATORS |
                                UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS))
                   .ToWString();
    start = end + 1;
  }
  return out;
}

std::wstring ApiName(const InternalRequest& req) {
  constexpr std::wstring_view kPrefix = L"/api/";
  return req.path.starts_with(kPrefix) ? req.path.substr(kPrefix.size()) : std::wstring();
}

void NotifyBookmarksChanged(const Profile* profile) {
  for (const auto& w : Application::Get()->windows()) {
    if (w->profile() == profile) w->OnBookmarksChanged();
  }
}

BrowserWindow* WindowFor(const Profile* profile) {
  for (const auto& w : Application::Get()->windows()) {
    if (w->profile() == profile && !w->closing()) return w.get();
  }
  return nullptr;
}

// ---------------------------------------------------------------------------

void AddBookmarkTree(Store& store, int64_t profile_id, int64_t parent,
                     const std::vector<BookmarkNode>& nodes, int& count) {
  for (const auto& n : nodes) {
    BookmarkRecord b;
    b.profile_id = profile_id;
    b.parent_id = parent;
    b.is_folder = n.is_folder;
    b.title = n.title.substr(0, 500);
    b.url = n.url.substr(0, 4096);
    const int64_t id = store.AddBookmark(b);
    if (!id) continue;
    ++count;
    if (n.is_folder) AddBookmarkTree(store, profile_id, id, n.children, count);
  }
}

std::vector<BookmarkNode> BuildBookmarkTree(const std::vector<BookmarkRecord>& all,
                                            int64_t parent, int depth) {
  std::vector<BookmarkNode> out;
  if (depth > 64) return out;
  for (const auto& b : all) {
    if (b.parent_id != parent) continue;
    BookmarkNode n{b.title, b.url, b.is_folder, {}};
    if (b.is_folder) n.children = BuildBookmarkTree(all, b.id, depth + 1);
    out.push_back(std::move(n));
  }
  return out;
}

bool ChooseFolder(HWND owner, std::wstring& path) {
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
    return false;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dialog->SetTitle(L"Choose download location");
  if (FAILED(dialog->Show(owner))) return false;  // cancelled
  Microsoft::WRL::ComPtr<IShellItem> item;
  PWSTR result = nullptr;
  if (FAILED(dialog->GetResult(&item)) ||
      FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &result))) {
    return false;
  }
  path = result;
  ::CoTaskMemFree(result);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// History

InternalResponse HistoryApi(const InternalRequest& req, Profile* profile) {
  if (profile->is_private()) return Error(403, "history is not kept in private windows");
  Store& store = Application::Get()->store();
  const std::wstring api = ApiName(req);
  if (api == L"list") {
    auto q = ParseQuery(req.query);
    const int64_t before = q.count(L"before") ? _wtoi64(q[L"before"].c_str()) : 0;
    Dict d = CefDictionaryValue::Create();
    CefRefPtr<CefListValue> items = CefListValue::Create();
    size_t i = 0;
    for (const auto& h : store.QueryHistory(profile->id(), q[L"q"], before, 150)) {
      Dict e = CefDictionaryValue::Create();
      e->SetDouble("id", static_cast<double>(h.id));
      e->SetString("url", h.url);
      e->SetString("title", h.title);
      e->SetDouble("time", static_cast<double>(h.visit_time));
      items->SetDictionary(i++, e);
    }
    d->SetList("items", items);
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  Dict body = ParseBody(req);
  if (api == L"delete") {
    if (body->HasKey("ids") && body->GetType("ids") == VTYPE_LIST) {
      CefRefPtr<CefListValue> ids = body->GetList("ids");
      for (size_t i = 0; i < ids->GetSize() && i < 1000; ++i) {
        store.DeleteVisit(profile->id(), static_cast<int64_t>(ids->GetDouble(i)));
      }
    }
    return Ok();
  }
  if (api == L"clear") {
    const std::wstring range = Str(body, "range");
    const int64_t now = NowUnixMillis();
    int64_t from = 0;
    if (range == L"hour") from = now - 3600LL * 1000;
    else if (range == L"day") from = now - 24LL * 3600 * 1000;
    else if (range == L"week") from = now - 7LL * 24 * 3600 * 1000;
    else if (range == L"month") from = now - 30LL * 24 * 3600 * 1000;
    else if (range != L"all") return Error(400, "bad range");
    const int n = store.DeleteHistoryRange(profile->id(), from, 0);
    Dict d = CefDictionaryValue::Create();
    d->SetInt("deleted", n);
    return Json(d);
  }
  return Error(404, "unknown endpoint");
}

// ---------------------------------------------------------------------------
// Bookmarks

InternalResponse BookmarksApi(const InternalRequest& req, Profile* profile) {
  if (profile->is_private()) return Error(403, "bookmarks are not available in private windows");
  Store& store = Application::Get()->store();
  const std::wstring api = ApiName(req);
  if (api == L"list") {
    CefRefPtr<CefListValue> items = CefListValue::Create();
    size_t i = 0;
    for (const auto& b : store.LoadBookmarks(profile->id())) {
      Dict e = CefDictionaryValue::Create();
      e->SetDouble("id", static_cast<double>(b.id));
      e->SetDouble("parent", static_cast<double>(b.parent_id));
      e->SetBool("folder", b.is_folder);
      e->SetString("title", b.title);
      e->SetString("url", b.url);
      items->SetDictionary(i++, e);
    }
    Dict d = CefDictionaryValue::Create();
    d->SetList("items", items);
    return Json(d);
  }
  if (api == L"export") {
    return {200, "text/html",
            ExportBookmarksHtml(BuildBookmarkTree(store.LoadBookmarks(profile->id()), 0, 0))};
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  Dict body = ParseBody(req);
  const int64_t id = Number(body, "id");
  bool ok = false;
  if (api == L"add") {
    BookmarkRecord b;
    b.profile_id = profile->id();
    b.parent_id = Number(body, "parent");
    b.is_folder = body->HasKey("folder") && body->GetBool("folder");
    b.title = Str(body, "title").substr(0, 500);
    b.url = Str(body, "url").substr(0, 4096);
    if (b.title.empty()) return Error(400, "title required");
    ok = store.AddBookmark(b) != 0;
  } else if (api == L"update") {
    ok = store.UpdateBookmark(profile->id(), id, Str(body, "title").substr(0, 500),
                              Str(body, "url").substr(0, 4096));
  } else if (api == L"move") {
    ok = store.MoveBookmark(profile->id(), id, Number(body, "parent"));
  } else if (api == L"delete") {
    ok = store.DeleteBookmark(profile->id(), id);
  } else if (api == L"import") {
    if (req.body.size() > 16u * 1024 * 1024) return Error(413, "file too large");
    const std::string html = ToUtf8(Str(body, "html"));
    int count = 0;
    Database::Transaction txn(store.db());
    BookmarkRecord folder;
    folder.profile_id = profile->id();
    folder.is_folder = true;
    folder.title = L"Imported";
    const int64_t root = store.AddBookmark(folder);
    AddBookmarkTree(store, profile->id(), root, ImportBookmarksHtml(html), count);
    if (!txn.Commit()) return Error(500, "import failed");
    NotifyBookmarksChanged(profile);
    Dict d = CefDictionaryValue::Create();
    d->SetInt("imported", count);
    return Json(d);
  } else {
    return Error(404, "unknown endpoint");
  }
  if (!ok) return Error(400, "operation failed");
  NotifyBookmarksChanged(profile);
  return Ok();
}

// ---------------------------------------------------------------------------
// Downloads

InternalResponse DownloadsApi(const InternalRequest& req, Profile* profile) {
  Application* app = Application::Get();
  DownloadManager& dm = app->downloads();
  const std::wstring api = ApiName(req);
  if (api == L"list") {
    CefRefPtr<CefListValue> items = CefListValue::Create();
    size_t i = 0;
    for (const auto& it : dm.List(profile)) {
      Dict e = CefDictionaryValue::Create();
      const DownloadRecord& r = it.record;
      e->SetDouble("id", static_cast<double>(r.id));
      e->SetString("url", r.url);
      e->SetString("path", r.path);
      e->SetString("name", std::filesystem::path(r.path).filename().wstring());
      e->SetDouble("total", static_cast<double>(r.total_bytes));
      e->SetDouble("received", static_cast<double>(r.received_bytes));
      e->SetInt("state", static_cast<int>(r.state));
      e->SetDouble("started", static_cast<double>(r.started_at));
      e->SetDouble("speed", static_cast<double>(it.speed));
      std::error_code ec;
      e->SetBool("exists", r.state == DownloadState::kComplete &&
                               std::filesystem::exists(r.path, ec));
      items->SetDictionary(i++, e);
    }
    Dict d = CefDictionaryValue::Create();
    d->SetList("items", items);
    d->SetString("dir", dm.Directory().wstring());
    d->SetBool("ask", app->store().GetPref(Store::kGlobalScope, L"download_ask") == L"1");
    d->SetBool("private", profile->is_private());
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  Dict body = ParseBody(req);
  const int64_t id = Number(body, "id");
  BrowserWindow* window = WindowFor(profile);
  HWND owner = window ? window->hwnd() : nullptr;
  bool ok = false;
  if (api == L"pause") ok = dm.Pause(profile, id);
  else if (api == L"resume") ok = dm.Resume(profile, id);
  else if (api == L"cancel") ok = dm.Cancel(profile, id);
  else if (api == L"open") ok = dm.Open(profile, id, owner);
  else if (api == L"show") ok = dm.ShowInFolder(profile, id);
  else if (api == L"remove") ok = dm.Remove(profile, id);
  else if (api == L"ask") {
    app->store().SetPref(Store::kGlobalScope, L"download_ask",
                         Str(body, "value") == L"1" ? L"1" : L"0");
    ok = true;
  }
  else if (api == L"choose-dir") {
    std::wstring dir;
    if (!ChooseFolder(owner, dir)) return Ok();  // cancelled
    app->store().SetPref(Store::kGlobalScope, L"download_dir", dir);
    ok = true;
  } else {
    return Error(404, "unknown endpoint");
  }
  return ok ? Ok() : Error(400, "operation failed");
}

// ---------------------------------------------------------------------------
// Settings

namespace {

struct SearchPreset {
  const wchar_t* name;
  const wchar_t* url;
};
constexpr SearchPreset kSearchPresets[] = {
    {L"DuckDuckGo", L"https://duckduckgo.com/?q={query}"},
    {L"Google", L"https://www.google.com/search?q={query}"},
    {L"Bing", L"https://www.bing.com/search?q={query}"},
    {L"Brave Search", L"https://search.brave.com/search?q={query}"},
    {L"Startpage", L"https://www.startpage.com/do/search?q={query}"},
    {L"Ecosia", L"https://www.ecosia.org/search?q={query}"},
};

bool ValidSearchTemplate(const std::wstring& t) {
  return t.size() < 512 && StartsWithNoCase(t, L"https://") &&
         t.find(L"{query}") != std::wstring::npos &&
         t.find_first_of(L" \t\r\n\"<>") == std::wstring::npos;
}

class DeleteCookiesDone : public CefDeleteCookiesCallback {
 public:
  void OnComplete(int num_deleted) override {}
  IMPLEMENT_REFCOUNTING(DeleteCookiesDone);
};

}  // namespace

InternalResponse SettingsApi(const InternalRequest& req, Profile* profile) {
  Application* app = Application::Get();
  Store& store = app->store();
  const std::wstring api = ApiName(req);
  if (api == L"get") {
    Dict d = CefDictionaryValue::Create();
    d->SetString("theme", store.GetPref(Store::kGlobalScope, L"theme").value_or(L"system"));
    wchar_t accent[8];
    swprintf_s(accent, L"%06X", app->accent_color());
    d->SetString("accent", accent);
    CefRefPtr<CefListValue> accents = CefListValue::Create();
    for (size_t i = 0; i < std::size(ui::kAccentChoices); ++i) {
      swprintf_s(accent, L"%06X", ui::kAccentChoices[i]);
      accents->SetString(i, accent);
    }
    d->SetList("accents", accents);
    d->SetString("search", app->search_template());
    CefRefPtr<CefListValue> presets = CefListValue::Create();
    for (size_t i = 0; i < std::size(kSearchPresets); ++i) {
      Dict p = CefDictionaryValue::Create();
      p->SetString("name", kSearchPresets[i].name);
      p->SetString("url", kSearchPresets[i].url);
      presets->SetDictionary(i, p);
    }
    d->SetList("presets", presets);
    d->SetBool("compact", store.GetPref(Store::kGlobalScope, L"sidebar_compact") == L"1");
    d->SetBool("hidden", store.GetPref(Store::kGlobalScope, L"sidebar_hidden") == L"1");
    d->SetString("themeStrength",
                 store.GetPref(Store::kGlobalScope, L"theme_strength").value_or(L"0.45"));
    d->SetString("sleep", store.GetPref(Store::kGlobalScope, L"sleep_minutes").value_or(L"0"));
    d->SetString("profile", profile->record().name);
    d->SetBool("private", profile->is_private());
    d->SetString("version", ORBIT_VERSION);
    d->SetString("engine", CEF_VERSION);
    d->SetString("dataDir", profile->is_private() ? L"(in memory)" : profile->data_dir().wstring());
    return Json(d);
  }
  if (api == L"permissions") {
    CefRefPtr<CefListValue> items = CefListValue::Create();
    size_t i = 0;
    for (const auto& e : app->permissions().List(profile)) {
      Dict d = CefDictionaryValue::Create();
      d->SetString("origin", e.origin);
      d->SetInt("bit", static_cast<int>(e.permission));
      d->SetString("label", PermissionManager::Describe(e.permission));
      d->SetBool("allowed", e.allowed);
      items->SetDictionary(i++, d);
    }
    Dict d = CefDictionaryValue::Create();
    d->SetList("items", items);
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  Dict body = ParseBody(req);
  if (api == L"set") {
    const std::wstring key = Str(body, "key");
    const std::wstring value = Str(body, "value");
    if (key == L"theme") {
      if (value != L"system" && value != L"dark" && value != L"light") return Error(400, "bad");
      store.SetPref(Store::kGlobalScope, L"theme", value);
      app->ApplyThemeToAllWindows();
    } else if (key == L"accent") {
      if (value.size() != 6 || value.find_first_not_of(L"0123456789abcdefABCDEF") !=
                                   std::wstring::npos) {
        return Error(400, "bad");
      }
      store.SetPref(Store::kGlobalScope, L"accent", value);
      app->ApplyThemeToAllWindows();
    } else if (key == L"search") {
      if (!ValidSearchTemplate(value)) {
        return Error(400, "Search URL must start with https:// and contain {query}");
      }
      store.SetPref(Store::kGlobalScope, L"search_template", value);
    } else if (key == L"compact") {
      store.SetPref(Store::kGlobalScope, L"sidebar_compact", value == L"1" ? L"1" : L"0");
      app->ApplySidebarModeToAllWindows(value == L"1");
    } else if (key == L"hidden") {
      store.SetPref(Store::kGlobalScope, L"sidebar_hidden", value == L"1" ? L"1" : L"0");
      app->ApplyCompactModeToAllWindows(value == L"1");
    } else if (key == L"theme_strength") {
      static constexpr const wchar_t* kAllowed[] = {L"0", L"0.25", L"0.45", L"0.7"};
      if (std::none_of(std::begin(kAllowed), std::end(kAllowed),
                       [&](const wchar_t* a) { return value == a; })) {
        return Error(400, "bad");
      }
      store.SetPref(Store::kGlobalScope, L"theme_strength", value);
      app->ApplyThemeToAllWindows();
    } else if (key == L"sleep") {
      static constexpr const wchar_t* kAllowed[] = {L"0", L"15", L"30", L"60", L"120", L"240"};
      if (std::none_of(std::begin(kAllowed), std::end(kAllowed),
                       [&](const wchar_t* a) { return value == a; })) {
        return Error(400, "bad");
      }
      store.SetPref(Store::kGlobalScope, L"sleep_minutes", value);
      app->UpdateSleepPolicy();
    } else if (key == L"download_ask") {
      store.SetPref(Store::kGlobalScope, L"download_ask", value == L"1" ? L"1" : L"0");
    } else {
      return Error(400, "unknown setting");
    }
    return Ok();
  }
  if (api == L"permission-forget") {
    app->permissions().Forget(profile, Str(body, "origin"),
                              static_cast<uint32_t>(Number(body, "bit")));
    return Ok();
  }
  if (api == L"clear-data") {
    if (body->HasKey("history") && body->GetBool("history") && !profile->is_private()) {
      store.DeleteHistoryRange(profile->id(), 0, 0);
    }
    if (body->HasKey("cookies") && body->GetBool("cookies")) {
      if (auto cm = profile->request_context()->GetCookieManager(nullptr)) {
        cm->DeleteCookies("", "", new DeleteCookiesDone());
      }
    }
    if (body->HasKey("closed") && body->GetBool("closed") && !profile->is_private()) {
      while (store.PopClosedTab(profile->id())) {
      }
    }
    return Ok();
  }
  return Error(404, "unknown endpoint");
}

// ---------------------------------------------------------------------------
// Diagnostics

InternalResponse DiagnosticsApi(const InternalRequest& req, Profile* profile) {
  Application* app = Application::Get();
  const std::wstring api = ApiName(req);
  if (api == L"metrics") {
    CefRefPtr<CefListValue> procs = CefListValue::Create();
    size_t i = 0;
    for (const auto& m : CollectProcessTree(::GetCurrentProcessId())) {
      Dict e = CefDictionaryValue::Create();
      e->SetInt("pid", static_cast<int>(m.pid));
      e->SetString("type", m.type);
      e->SetDouble("privateWs", static_cast<double>(m.private_working_set));
      e->SetDouble("ws", static_cast<double>(m.working_set));
      e->SetDouble("commit", static_cast<double>(m.commit));
      e->SetDouble("cpuMs", static_cast<double>(m.cpu_time_100ns) / 10000.0);
      procs->SetDictionary(i++, e);
    }
    int live = 0, sleeping = 0, total = 0;
    auto count = [&](const Profile& p) {
      for (const auto& ws : p.workspaces()) {
        for (const auto& t : ws->tabs()) {
          ++total;
          if (t->has_browser()) ++live;
          else ++sleeping;
        }
      }
    };
    for (const auto& p : app->profiles()) count(*p);
    if (profile->is_private()) count(*profile);
    Dict d = CefDictionaryValue::Create();
    d->SetList("processes", procs);
    d->SetInt("tabs", total);
    d->SetInt("liveTabs", live);
    d->SetInt("sleepingTabs", sleeping);
    d->SetInt("windows", static_cast<int>(app->windows().size()));
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  if (api == L"sleep-all") {
    // Two passes (query media, then decide) a second apart, idle threshold 0.
    app->ApplySleepPolicy(0);
    CefPostDelayedTask(TID_UI, base::BindOnce([] {
                         if (Application* a = Application::Get()) a->ApplySleepPolicy(0);
                       }),
                       1100);
    return Ok();
  }
  return Error(404, "unknown endpoint");
}

// ---------------------------------------------------------------------------
// New tab

InternalResponse NewTabApi(const InternalRequest& req, Profile* profile) {
  if (ApiName(req) != L"top") return Error(404, "unknown endpoint");
  CefRefPtr<CefListValue> items = CefListValue::Create();
  if (!profile->is_private()) {
    size_t i = 0;
    for (const auto& t : Application::Get()->store().TopSites(profile->id(), 8)) {
      Dict e = CefDictionaryValue::Create();
      e->SetString("url", t.url);
      e->SetString("title", t.title);
      items->SetDictionary(i++, e);
    }
  }
  Dict d = CefDictionaryValue::Create();
  d->SetList("items", items);
  return Json(d);
}

}  // namespace orbit
