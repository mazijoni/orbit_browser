// JSON endpoints for orbit:// pages. Built on CEF's JSON parser/writer so no
// hand-written escaping is involved. Every endpoint is scoped to the profile
// whose request context served the request.

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <functional>
#include <map>

#include "app/application.h"
#include "app/installer.h"
#include "app/updater.h"
#include "browser/browser_client.h"
#include "browser/downloads.h"
#include "browser/extension_bridge.h"
#include "browser/find_bar.h"
#include "browser/internal_page_parts.h"
#include "browser/permissions.h"
#include "browser/profile.h"
#include "core/bookmark_html.h"
#include "browser/importer.h"
#include "browser/password_vault.h"
#include "core/log.h"
#include "core/process_metrics.h"
#include "core/shortcuts.h"
#include "core/space_routing.h"
#include "core/space_theme.h"
#include "core/boosts.h"
#include "browser/boost_apply.h"
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
    d->SetBool("singleToolbar", store.GetPref(Store::kGlobalScope, L"single_toolbar").value_or(L"1") != L"0");
    d->SetBool("hidden", store.GetPref(Store::kGlobalScope, L"sidebar_hidden") == L"1");
    d->SetBool("selectRecent",
               store.GetPref(Store::kGlobalScope, L"select_recent_on_close").value_or(L"1") == L"1");
    d->SetString("pinnedClose", store.GetPref(Store::kGlobalScope, L"pinned_close_behavior")
                                    .value_or(L"reset-unload-switch"));
    d->SetBool("restorePinned",
               store.GetPref(Store::kGlobalScope, L"restore_pinned_to_pinned_url") == L"1");
    d->SetBool("closeOnBack",
               store.GetPref(Store::kGlobalScope, L"close_on_back_no_history").value_or(L"1") == L"1");
    d->SetString("themeStrength",
                 store.GetPref(Store::kGlobalScope, L"theme_strength").value_or(L"0.45"));
    d->SetString("sleep", store.GetPref(Store::kGlobalScope, L"sleep_minutes").value_or(L"0"));
    auto pref_on = [&](const wchar_t* key, bool fallback) {
      const auto v = store.GetPref(Store::kGlobalScope, key);
      return v ? *v == L"1" : fallback;
    };
    d->SetBool("newtabButton", pref_on(L"newtab_button", true));
    d->SetBool("newtabTop", pref_on(L"newtab_button_top", true));
    d->SetBool("ctrlTabSkipPending", pref_on(L"ctrl_tab_skip_pending", false));
    d->SetBool("greyOut", pref_on(L"grey_out_inactive", true));
    d->SetBool("urlbarWide", pref_on(L"urlbar_wide", false));
    d->SetBool("dragFromContent", pref_on(L"drag_window_from_content", true));
    d->SetBool("quickActions", pref_on(L"quick_actions", true));
    d->SetBool("tabsRight", pref_on(L"tabs_right", false));
    d->SetBool("compactFlash", pref_on(L"compact_flash", false));  // zen.view.compact.toolbar-flash-popup
    d->SetBool("searchSuggestions", pref_on(L"search_suggestions", false));  // browser.search.suggest.enabled
    d->SetBool("ctrlTabWithin", pref_on(L"ctrl_tab_ignore_essentials", false));  // zen.tabs.ctrl-tab.ignore-essential-tabs
    d->SetBool("continueLeftOff", pref_on(L"continue_where_left_off", false));
    d->SetBool("autofillPasswords", pref_on(L"autofill_passwords", true));  // zen.workspaces.continue-where-left-off
    d->SetBool("glance", pref_on(L"glance_enabled", true));
    d->SetString("glanceTrigger", store.GetPref(Store::kGlobalScope, L"glance_trigger").value_or(L"alt"));
    d->SetString("urlbar", store.GetPref(Store::kGlobalScope, L"urlbar_behavior").value_or(L"floating-on-type"));
    d->SetString("profile", profile->record().name);
    d->SetBool("private", profile->is_private());
    d->SetString("version", ORBIT_VERSION);
    d->SetBool("defaultBrowser", installer::IsDefaultBrowser());
    d->SetBool("updateAutoCheck", pref_on(L"update_auto_check", true));
    d->SetString("engine", CEF_VERSION);
    d->SetString("dataDir", profile->is_private() ? L"(in memory)" : profile->data_dir().wstring());
    return Json(d);
  }
  if (api == L"update") {
    // Read-only state; the actions below need a trusted request.
    const updater::Status u = updater::Current();
    Dict d = CefDictionaryValue::Create();
    d->SetString("state", updater::StateName(u.state));
    d->SetString("current", ORBIT_VERSION);
    d->SetString("latest", u.latest);
    d->SetString("notes", u.notes_url);
    d->SetInt("progress", u.progress);
    d->SetString("error", u.error);
    return Json(d);
  }
  if ((api == L"update-check" || api == L"update-download" || api == L"update-install") && !profile->is_private()) {
    if (!req.trusted_action) return Error(403, "forbidden");
    if (api == L"update-check") updater::Check(false);
    if (api == L"update-download") updater::Download();
    if (api == L"update-install") updater::InstallAndRestart();
    return Ok();
  }
  if (api == L"default-browser" && req.trusted_action) {
    installer::MakeDefaultBrowser();
    return Ok();
  }
  if (api == L"routes") {
    CefRefPtr<CefListValue> routes = CefListValue::Create();
    size_t i = 0;
    for (const SpaceRoute& r : ParseRoutes(store.GetPref(Store::kGlobalScope, L"space_routes").value_or(L""))) {
      Dict e = CefDictionaryValue::Create();
      e->SetString("match", r.match == SpaceRoute::Match::kEqualTo ? L"equal-to" : L"contains");
      e->SetString("reference", r.reference);
      e->SetDouble("space", static_cast<double>(r.space_id));
      routes->SetDictionary(i++, e);
    }
    // Routes may name a space of any profile (the tab opens in its profile).
    CefRefPtr<CefListValue> spaces = CefListValue::Create();
    i = 0;
    for (Workspace* ws : app->Spaces()) {
      Dict e = CefDictionaryValue::Create();
      e->SetDouble("id", static_cast<double>(ws->id()));
      e->SetString("name", ws->profile() == app->profiles().front().get()
                               ? ws->record().name
                               : ws->record().name + L" (" + ws->profile()->record().name + L")");
      spaces->SetDictionary(i++, e);
    }
    Dict d = CefDictionaryValue::Create();
    d->SetList("routes", routes);
    d->SetList("spaces", spaces);
    d->SetDouble("external", static_cast<double>(
        _wtoi64(store.GetPref(Store::kGlobalScope, L"space_route_external").value_or(L"0").c_str())));
    d->SetBool("private", profile->is_private());
    return Json(d);
  }
  if (api == L"shortcuts") {
    CefRefPtr<CefListValue> items = CefListValue::Create();
    size_t i = 0;
    for (const ShortcutAction& a : ShortcutActions()) {
      Dict e = CefDictionaryValue::Create();
      e->SetString("id", a.id);
      e->SetString("group", a.group);
      e->SetString("label", a.label);
      e->SetString("key", FormatChord(app->shortcuts().Effective(a.id)));
      e->SetString("default", FormatChord(a.key));
      e->SetBool("custom", app->shortcuts().overrides().count(a.id) > 0);
      items->SetDictionary(i++, e);
    }
    Dict d = CefDictionaryValue::Create();
    d->SetList("items", items);
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
  if (api == L"passwords") {
    // The list never includes the secret; revealing one is a separate action.
    CefRefPtr<CefListValue> items = CefListValue::Create();
    size_t i = 0;
    if (!profile->is_private()) {
      for (const auto& p : store.LoadPasswords(profile->id())) {
        Dict d = CefDictionaryValue::Create();
        d->SetDouble("id", static_cast<double>(p.id));
        d->SetString("origin", p.origin);
        d->SetString("username", p.username);
        items->SetDictionary(i++, d);
      }
    }
    Dict d = CefDictionaryValue::Create();
    d->SetList("items", items);
    d->SetBool("private", profile->is_private());
    return Json(d);
  }
  if (api == L"import-sources") {
    CefRefPtr<CefListValue> items = CefListValue::Create();
    size_t i = 0;
    if (!profile->is_private()) {
      for (const auto& s : importer::DetectSources()) {
        Dict d = CefDictionaryValue::Create();
        d->SetInt("index", static_cast<int>(i));
        d->SetString("name", s.name);
        d->SetBool("bookmarks", s.has_bookmarks);
        d->SetBool("passwords", s.has_passwords);
        d->SetBool("history", s.has_history);
        items->SetDictionary(i++, d);
      }
    }
    Dict d = CefDictionaryValue::Create();
    d->SetList("items", items);
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  Dict body = ParseBody(req);
  if (api == L"password-reveal") {
    if (profile->is_private()) return Error(403, "no passwords in private windows");
    const int64_t id = Number(body, "id");
    for (const auto& p : store.LoadPasswords(profile->id())) {
      if (p.id != id) continue;
      Dict d = CefDictionaryValue::Create();
      d->SetString("password", passwords::Unprotect(p.secret));
      return Json(d);
    }
    return Error(404, "no such login");
  }
  if (api == L"password-delete") {
    if (profile->is_private()) return Error(403, "forbidden");
    store.DeletePassword(profile->id(), Number(body, "id"));
    return Ok();
  }
  if (api == L"import-run") {
    if (profile->is_private()) return Error(403, "no import into private windows");
    const int index = static_cast<int>(Number(body, "index"));
    const bool want_bm = !body->HasKey("bookmarks") || body->GetBool("bookmarks");
    const bool want_pw = !body->HasKey("passwords") || body->GetBool("passwords");
    const bool want_hist = body->HasKey("history") && body->GetBool("history");
    const auto sources = importer::DetectSources();
    if (index < 0 || index >= static_cast<int>(sources.size())) return Error(400, "no such source");
    const importer::Data data = importer::Read(sources[static_cast<size_t>(index)], want_bm, want_pw, want_hist);
    int bookmarks = 0, logins = 0, history = 0;
    {
      Database::Transaction txn(store.db());
      if (want_bm && !data.bookmarks.empty()) {
        BookmarkRecord folder;
        folder.profile_id = profile->id();
        folder.is_folder = true;
        folder.title = L"Imported — " + sources[static_cast<size_t>(index)].name;
        const int64_t root = store.AddBookmark(folder);
        AddBookmarkTree(store, profile->id(), root, data.bookmarks, bookmarks);
      }
      for (const auto& l : data.logins) {
        const std::wstring secret = passwords::Protect(l.password);
        if (secret.empty()) continue;
        PasswordRecord p;
        p.profile_id = profile->id();
        p.origin = l.origin;
        p.username = l.username;
        p.secret = secret;
        if (store.UpsertPassword(p)) ++logins;
      }
      for (const auto& h : data.history) {
        if (h.visit_time <= 0) continue;
        store.AddVisit(profile->id(), h.url, h.title, h.visit_time);
        ++history;
      }
      txn.Commit();
    }
    if (bookmarks) NotifyBookmarksChanged(profile);
    Dict d = CefDictionaryValue::Create();
    d->SetInt("bookmarks", bookmarks);
    d->SetInt("passwords", logins);
    d->SetInt("history", history);
    return Json(d);
  }
  if (api == L"password-import-csv") {
    // A passwords file the user picked (exported from a browser or password
    // manager). Stored as imports are: DPAPI-protected, per profile.
    if (profile->is_private()) return Error(403, "no import into private windows");
    if (!body || !body->HasKey("csv")) return Error(400, "no file");
    const importer::CsvLogins parsed = importer::ParsePasswordCsv(body->GetString("csv").ToString());
    if (!parsed.recognized) return Error(400, "This file has no web address and password columns.");
    int imported = 0;
    {
      Database::Transaction txn(store.db());
      for (const auto& l : parsed.logins) {
        const std::wstring secret = passwords::Protect(l.password);
        if (secret.empty()) continue;
        PasswordRecord p;
        p.profile_id = profile->id();
        p.origin = l.origin;
        p.username = l.username;
        p.secret = secret;
        if (store.UpsertPassword(p)) ++imported;
      }
      txn.Commit();
    }
    ORBIT_LOG_INFO("imported %d passwords from a file (%d rows skipped)", imported, parsed.skipped);
    Dict d = CefDictionaryValue::Create();
    d->SetInt("imported", imported);
    d->SetInt("skipped", parsed.skipped);
    return Json(d);
  }
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
    } else if (key == L"single_toolbar") {
      // zen.view.use-single-toolbar
      store.SetPref(Store::kGlobalScope, L"single_toolbar", value == L"0" ? L"0" : L"1");
      app->ApplyThemeToAllWindows();
    } else if (key == L"hidden") {
      store.SetPref(Store::kGlobalScope, L"sidebar_hidden", value == L"1" ? L"1" : L"0");
      app->ApplyCompactModeToAllWindows(value == L"1");
    } else if (key == L"pinned_close_behavior") {
      static constexpr const wchar_t* kAllowed[] = {L"reset-unload-switch", L"unload-switch",
                                                    L"reset-switch", L"switch", L"reset",
                                                    L"close"};
      if (std::none_of(std::begin(kAllowed), std::end(kAllowed),
                       [&](const wchar_t* a) { return value == a; })) {
        return Error(400, "bad");
      }
      store.SetPref(Store::kGlobalScope, key, value);
    } else if (key == L"restore_pinned_to_pinned_url") {
      store.SetPref(Store::kGlobalScope, key, value == L"1" ? L"1" : L"0");
    } else if (key == L"select_recent_on_close" || key == L"close_on_back_no_history") {
      store.SetPref(Store::kGlobalScope, key, value == L"1" ? L"1" : L"0");
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
    } else if (key == L"newtab_button" || key == L"newtab_button_top" ||
               key == L"ctrl_tab_skip_pending" || key == L"glance_enabled" ||
               key == L"grey_out_inactive" || key == L"drag_window_from_content" || key == L"urlbar_wide" ||
               key == L"quick_actions" || key == L"tabs_right" || key == L"compact_flash" ||
               key == L"search_suggestions" || key == L"ctrl_tab_ignore_essentials" ||
               key == L"continue_where_left_off" || key == L"autofill_passwords" ||
               key == L"update_auto_check") {
      store.SetPref(Store::kGlobalScope, key, value == L"1" ? L"1" : L"0");
      app->ApplyThemeToAllWindows();  // windows re-read their settings
    } else if (key == L"glance_trigger" || key == L"urlbar_behavior") {
      static constexpr const wchar_t* kAllowed[] = {L"alt", L"ctrl", L"shift", L"normal",
                                                    L"floating-on-type", L"float"};
      const bool trigger = key == L"glance_trigger";
      const bool ok = std::any_of(std::begin(kAllowed) + (trigger ? 0 : 3),
                                  std::begin(kAllowed) + (trigger ? 3 : 6),
                                  [&](const wchar_t* a) { return value == a; });
      if (!ok) return Error(400, "bad");
      store.SetPref(Store::kGlobalScope, key, value);
      app->ApplyThemeToAllWindows();
    } else if (key == L"download_ask") {
      store.SetPref(Store::kGlobalScope, L"download_ask", value == L"1" ? L"1" : L"0");
    } else {
      return Error(400, "unknown setting");
    }
    return Ok();
  }
  if (api == L"routes-set") {
    // [{match: "contains"|"equal-to", reference, space}], external default.
    std::vector<SpaceRoute> routes;
    if (body->HasKey("routes") && body->GetType("routes") == VTYPE_LIST) {
      CefRefPtr<CefListValue> list = body->GetList("routes");
      for (size_t i = 0; i < list->GetSize() && i < 200; ++i) {
        CefRefPtr<CefDictionaryValue> e = list->GetDictionary(i);
        if (!e) continue;
        SpaceRoute r;
        r.match = Str(e, "match") == L"equal-to" ? SpaceRoute::Match::kEqualTo : SpaceRoute::Match::kContains;
        r.reference = Str(e, "reference").substr(0, 2000);
        r.space_id = Number(e, "space");
        if (r.space_id && (!app->FindSpace(r.space_id) || app->FindSpace(r.space_id)->is_linked())) continue;
        routes.push_back(std::move(r));
      }
    }
    store.SetPref(Store::kGlobalScope, L"space_routes", SerializeRoutes(routes));
    const int64_t external = Number(body, "external");
    store.SetPref(Store::kGlobalScope, L"space_route_external",
                  external && app->FindSpace(external) ? std::to_wstring(external) : L"0");
    return Ok();
  }
  if (api == L"shortcut-record") {
    app->SetShortcutRecording(body->HasKey("on") && body->GetBool("on"));
    return Ok();
  }
  if (api == L"shortcut-set") {
    const std::wstring id = Str(body, "id");
    if (!FindShortcutAction(id)) return Error(400, "unknown shortcut");
    KeyChord chord;
    if (!(body->HasKey("disable") && body->GetBool("disable"))) {
      chord.vk = static_cast<int>(Number(body, "vk"));
      chord.ctrl = body->HasKey("ctrl") && body->GetBool("ctrl");
      chord.shift = body->HasKey("shift") && body->GetBool("shift");
      chord.alt = body->HasKey("alt") && body->GetBool("alt");
      if (!AssignableChord(chord)) return Error(400, "use Ctrl or Alt with a key, or a function key");
    }
    const auto conflicts = app->shortcuts().Conflicts(id, chord);
    if (!conflicts.empty() && !(body->HasKey("replace") && body->GetBool("replace"))) {
      CefRefPtr<CefListValue> list = CefListValue::Create();
      for (size_t i = 0; i < conflicts.size(); ++i) {
        list->SetString(i, FindShortcutAction(conflicts[i])->label);
      }
      Dict d = CefDictionaryValue::Create();
      d->SetList("conflicts", list);
      d->SetString("key", FormatChord(chord));
      return Json(d);
    }
    for (const std::wstring& other : conflicts) app->SetShortcut(other, {});  // replaced: unassigned
    app->SetShortcut(id, chord);
    app->SetShortcutRecording(false);
    return Ok();
  }
  if (api == L"shortcut-reset") {
    app->ResetShortcut(Str(body, "id"));
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
      for (Workspace* ws : p.containers()) {
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

// ---------------------------------------------------------------------------
// Library panel (orbit://library). Window actions run as posted UI tasks so
// that nothing modal or tab-switching happens inside a resource request; the
// window is found again by its panel's browser id when the task runs.

namespace {

BrowserWindow* LibraryWindow(int browser_id) {
  for (const auto& w : Application::Get()->windows()) {
    if (browser_id && w->library_browser_id() == browser_id && !w->closing()) return w.get();
  }
  return nullptr;
}

void PostToLibraryWindow(int browser_id, std::function<void(BrowserWindow*)> action) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](int id, std::function<void(BrowserWindow*)> run) {
                            if (BrowserWindow* w = LibraryWindow(id)) run(w);
                          },
                          browser_id, std::move(action)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Site data panel (orbit://sitedata): acts only on the active tab of the
// window whose panel calls it; the origin comes from that tab, never from
// the request.

namespace {

BrowserWindow* SiteDataWindow(int browser_id) {
  for (const auto& w : Application::Get()->windows()) {
    if (browser_id && w->site_data_browser_id() == browser_id && !w->closing()) return w.get();
  }
  return nullptr;
}

std::wstring OriginOf(const std::wstring& url) {
  CefURLParts parts;
  if (!CefParseURL(url, parts)) return L"";
  const std::wstring scheme = CefString(&parts.scheme).ToWString();
  if (scheme != L"https" && scheme != L"http") return L"";
  std::wstring origin = scheme + L"://" + CefString(&parts.host).ToWString();
  const std::wstring port = CefString(&parts.port).ToWString();
  if (!port.empty()) origin += L":" + port;
  return origin;
}

std::wstring TrimSlash(std::wstring s) {
  while (!s.empty() && s.back() == L'/') s.pop_back();
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Theme picker (orbit://theme): edits the space its window's picker shows.

InternalResponse ThemeApi(const InternalRequest& req, Profile* profile) {
  BrowserWindow* window = nullptr;
  for (const auto& w : Application::Get()->windows()) {
    if (req.browser_id && w->theme_picker_browser_id() == req.browser_id && !w->closing()) window = w.get();
  }
  Workspace* ws = window ? window->theme_editing_space() : nullptr;
  if (!ws || profile->is_private()) return Error(403, "not a theme picker");
  Store& store = Application::Get()->store();
  const std::wstring api = ApiName(req);
  if (api == L"state") {
    const SpaceTheme theme = ParseSpaceTheme(ws->record().theme).value_or(SpaceTheme{});
    Dict d = CefDictionaryValue::Create();
    d->SetDouble("space", static_cast<double>(ws->id()));
    d->SetString("name", ws->record().name);
    d->SetDouble("opacity", theme.opacity);
    d->SetDouble("texture", theme.texture);
    d->SetString("algorithm", theme.algorithm);
    d->SetInt("lightness", theme.lightness);
    CefRefPtr<CefListValue> dots = CefListValue::Create();
    for (size_t i = 0; i < theme.dots.size(); ++i) {
      const ThemeDot& t = theme.dots[i];
      Dict e = CefDictionaryValue::Create();
      e->SetInt("r", t.r);
      e->SetInt("g", t.g);
      e->SetInt("b", t.b);
      e->SetDouble("x", t.x);
      e->SetDouble("y", t.y);
      e->SetString("type", t.type);
      e->SetBool("primary", t.primary);
      dots->SetDictionary(i, e);
    }
    d->SetList("dots", dots);
    d->SetString("scheme", store.GetPref(Store::kGlobalScope, L"theme").value_or(L"system"));
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  const Dict body = ParseBody(req);
  if (api == L"set") {
    auto num = [&](const Dict& o, const char* key, double fallback) {
      if (!o->HasKey(key)) return fallback;
      const auto type = o->GetType(key);
      const double v = type == VTYPE_INT ? o->GetInt(key) : type == VTYPE_DOUBLE ? o->GetDouble(key) : fallback;
      return std::isfinite(v) ? v : fallback;
    };
    SpaceTheme theme;
    theme.opacity = static_cast<float>(std::clamp(num(body, "opacity", 0.5), 0.0, 1.0));
    theme.texture = static_cast<float>(std::clamp(num(body, "texture", 0.0), 0.0, 1.0));
    theme.lightness = static_cast<int>(std::clamp(num(body, "lightness", 50.0), 0.0, 100.0));
    theme.algorithm = Str(body, "algorithm").substr(0, 32);
    if (body->HasKey("dots") && body->GetType("dots") == VTYPE_LIST) {
      CefRefPtr<CefListValue> list = body->GetList("dots");
      for (size_t i = 0; i < list->GetSize() && theme.dots.size() < SpaceTheme::kMaxDots; ++i) {
        Dict e = list->GetDictionary(i);
        if (!e) continue;
        ThemeDot t;
        t.r = static_cast<int>(std::clamp(num(e, "r", 0), 0.0, 255.0));
        t.g = static_cast<int>(std::clamp(num(e, "g", 0), 0.0, 255.0));
        t.b = static_cast<int>(std::clamp(num(e, "b", 0), 0.0, 255.0));
        t.x = static_cast<float>(std::clamp(num(e, "x", 0), -1000.0, 1000.0));
        t.y = static_cast<float>(std::clamp(num(e, "y", 0), -1000.0, 1000.0));
        t.type = Str(e, "type").substr(0, 32);
        t.primary = e->HasKey("primary") && e->GetType("primary") == VTYPE_BOOL && e->GetBool("primary");
        theme.dots.push_back(std::move(t));
      }
    }
    // The stored form only keeps safe words; anything else becomes empty.
    const auto checked = ParseSpaceTheme(SerializeSpaceTheme(theme));
    const bool save = body->HasKey("save") && body->GetType("save") == VTYPE_BOOL && body->GetBool("save");
    window->SetSpaceTheme(ws, checked, save);
    return Ok();
  }
  if (api == L"scheme") {
    // zen.view.window.scheme: auto / light / dark for every window.
    const std::wstring scheme = Str(body, "scheme");
    if (scheme != L"system" && scheme != L"light" && scheme != L"dark") return Error(400, "bad");
    store.SetPref(Store::kGlobalScope, L"theme", scheme);
    Application::Get()->ApplyThemeToAllWindows();
    return Ok();
  }
  return Error(404, "unknown");
}

// ---------------------------------------------------------------------------
// Boost editor (orbit://boost): edits the boost of the site its window's
// editor was opened for.

InternalResponse BoostApi(const InternalRequest& req, Profile* profile) {
  BrowserWindow* window = nullptr;
  for (const auto& w : Application::Get()->windows()) {
    if (req.browser_id && w->boost_editor_browser_id() == req.browser_id && !w->closing()) window = w.get();
  }
  if (!window || window->boost_domain().empty() || profile->is_private()) return Error(403, "not a boost editor");
  const std::wstring domain = window->boost_domain();
  const std::wstring api = ApiName(req);
  if (api == L"state") {
    const std::optional<Boost> stored = LoadBoostForSite(profile, domain);
    const Boost b = stored.value_or(Boost{});
    Dict d = CefDictionaryValue::Create();
    d->SetString("domain", domain);
    d->SetBool("exists", stored.has_value());
    d->SetString("name", b.name);
    d->SetBool("enabled", b.enabled);
    d->SetBool("color", b.color);
    d->SetDouble("dotAngle", b.dot_angle);
    d->SetDouble("dotDistance", b.dot_distance);
    d->SetDouble("secondaryDelta", b.secondary_delta);
    d->SetDouble("brightness", b.brightness);
    d->SetDouble("saturation", b.saturation);
    d->SetDouble("contrast", b.contrast);
    d->SetBool("smartInvert", b.smart_invert);
    d->SetString("font", b.font_family);
    d->SetString("textCase", b.text_case);
    d->SetDouble("size", b.size);
    d->SetInt("zaps", static_cast<int>(b.zap.size()));
    d->SetInt("focus", static_cast<int>(b.focus.size()));
    d->SetString("css", b.custom_css);
    d->SetString("pick", window->boost_pick_mode());
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  const Dict body = ParseBody(req);
  auto num = [&](const char* key, double fallback) {
    if (!body->HasKey(key)) return fallback;
    const auto t = body->GetType(key);
    const double v = t == VTYPE_INT ? body->GetInt(key) : t == VTYPE_DOUBLE ? body->GetDouble(key) : fallback;
    return std::isfinite(v) ? v : fallback;
  };
  auto flag = [&](const char* key, bool fallback) {
    return body->HasKey(key) && body->GetType(key) == VTYPE_BOOL ? body->GetBool(key) : fallback;
  };
  if (api == L"set") {
    // The editor's fields; zaps and focus targets are kept (they change by
    // picking and clearing).
    Boost b = LoadBoostForSite(profile, domain).value_or(Boost{});
    if (body->HasKey("name")) b.name = Str(body, "name").substr(0, 100);
    b.enabled = flag("enabled", b.enabled);
    b.color = flag("color", b.color);
    b.dot_angle = static_cast<float>(std::clamp(num("dotAngle", b.dot_angle), -720.0, 720.0));
    b.dot_distance = static_cast<float>(std::clamp(num("dotDistance", b.dot_distance), 0.0, 1.0));
    b.secondary_delta = static_cast<float>(std::clamp(num("secondaryDelta", b.secondary_delta), -360.0, 360.0));
    b.brightness = static_cast<float>(std::clamp(num("brightness", b.brightness), 0.0, 1.0));
    b.saturation = static_cast<float>(std::clamp(num("saturation", b.saturation), 0.0, 1.0));
    b.contrast = static_cast<float>(std::clamp(num("contrast", b.contrast), 0.0, 1.0));
    b.smart_invert = flag("smartInvert", b.smart_invert);
    if (body->HasKey("font")) b.font_family = Str(body, "font").substr(0, 200);
    if (body->HasKey("textCase")) b.text_case = Str(body, "textCase");
    b.size = static_cast<float>(std::clamp(num("size", b.size), 0.3, 3.0));
    if (body->HasKey("css")) b.custom_css = Str(body, "css").substr(0, 100000);
    const auto checked = ParseBoost(SerializeBoost(b));  // normalises the text case and limits
    SaveBoostForSite(profile, domain, checked);
    return Ok();
  }
  if (api == L"pick") {
    const std::wstring mode = Str(body, "mode");
    window->SetBoostPickMode(mode);
    return Ok();
  }
  if (api == L"clear") {
    Boost b = LoadBoostForSite(profile, domain).value_or(Boost{});
    if (Str(body, "list") == L"focus") {
      b.focus.clear();
    } else {
      b.zap.clear();
    }
    SaveBoostForSite(profile, domain, b);
    return Ok();
  }
  if (api == L"delete") {
    window->SetBoostPickMode(L"off");
    SaveBoostForSite(profile, domain, std::nullopt);
    return Ok();
  }
  if (api == L"close") {
    const int id = req.browser_id;
    CefPostTask(TID_UI, base::BindOnce([](int browser) {
      for (const auto& w : Application::Get()->windows()) {
        if (w->boost_editor_browser_id() == browser) w->CloseBoostEditor();
      }
    }, id));
    return Ok();
  }
  return Error(404, "unknown");
}

InternalResponse SiteDataApi(const InternalRequest& req, Profile* profile) {
  BrowserWindow* window = SiteDataWindow(req.browser_id);
  Tab* tab = window ? window->active_tab() : nullptr;
  if (!tab) return Error(403, "not a site data panel");
  Application* app = Application::Get();
  const std::wstring api = ApiName(req);
  const std::wstring origin = OriginOf(tab->url());
  if (api == L"info") {
    Dict d = CefDictionaryValue::Create();
    d->SetString("title", tab->DisplayTitle());
    d->SetString("url", tab->url());
    d->SetString("origin", origin);
    const wchar_t* security = L"none";
    switch (tab->security()) {
      case SecurityLevel::kSecure: security = L"secure"; break;
      case SecurityLevel::kInsecure: security = L"not-secure"; break;
      case SecurityLevel::kDangerous: security = L"dangerous"; break;
      case SecurityLevel::kInternal: security = L"internal"; break;
      default: break;
    }
    d->SetString("security", security);
    d->SetBool("canCopy", tab->url().rfind(L"http", 0) == 0);
    d->SetBool("canBoost", !profile->is_private() && !BoostDomain(tab->url()).empty());
    d->SetBool("boosted", LoadBoostForSite(profile, BoostDomain(tab->url())).has_value());
    d->SetBool("bookmarked", !profile->is_private() && app->store().FindBookmarkByUrl(profile->id(), tab->url()).has_value());
    CefRefPtr<CefListValue> perms = CefListValue::Create();
    size_t i = 0;
    if (!origin.empty()) {
      for (const auto& e : app->permissions().List(profile)) {
        if (TrimSlash(e.origin) != origin) continue;
        Dict p = CefDictionaryValue::Create();
        p->SetInt("bit", static_cast<int>(e.permission));
        std::wstring label = PermissionManager::Describe(e.permission);
        while (!label.empty() && (label.back() == L'\n')) label.pop_back();
        if (label.rfind(L"\u2022 ", 0) == 0) label = label.substr(2);
        p->SetString("label", label);
        p->SetBool("allowed", e.allowed);
        perms->SetDictionary(i++, p);
      }
    }
    d->SetList("permissions", perms);
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  Dict body = ParseBody(req);
  const int id = req.browser_id;
  auto on_window = [id](std::function<void(BrowserWindow*)> run) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](int browser, std::function<void(BrowserWindow*)> action) {
                              if (BrowserWindow* w = SiteDataWindow(browser)) action(w);
                            },
                            id, std::move(run)));
  };
  if (api == L"permission") {
    const uint32_t bit = static_cast<uint32_t>(Number(body, "bit"));
    const bool allow = body->HasKey("allow") && body->GetBool("allow");
    // Only a permission this origin already has an entry for.
    for (const auto& e : app->permissions().List(profile)) {
      if (TrimSlash(e.origin) == origin && e.permission == bit) {
        app->permissions().Set(profile, e.origin, bit, allow);
        return Ok();
      }
    }
    return Error(400, "no such permission");
  }
  if (api == L"permission-reset") {
    const uint32_t bit = static_cast<uint32_t>(Number(body, "bit"));
    for (const auto& e : app->permissions().List(profile)) {
      if (TrimSlash(e.origin) == origin && e.permission == bit) app->permissions().Forget(profile, e.origin, bit);
    }
    return Ok();
  }
  if (api == L"clear-site-data") {
    if (origin.empty()) return Error(400, "not a web page");
    // Storage.clearDataForOrigin: cookies, storage, caches of this origin.
    if (CefRefPtr<CefBrowser> b = tab->browser()) {
      CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
      params->SetString("origin", origin);
      params->SetString("storageTypes", "all");
      b->GetHost()->ExecuteDevToolsMethod(0, "Storage.clearDataForOrigin", params);
    }
    on_window([](BrowserWindow* w) {
      w->CloseSiteData();
      if (Tab* t = w->active_tab()) t->Reload(false);
    });
    return Ok();
  }
  if (api == L"copy") {
    on_window([](BrowserWindow* w) {
      w->CloseSiteData();
      w->CopyCurrentUrl();
    });
    return Ok();
  }
  if (api == L"bookmark") {
    on_window([](BrowserWindow* w) { w->ToggleBookmarkActive(); });
    return Ok();
  }
  if (api == L"settings") {
    on_window([](BrowserWindow* w) {
      w->CloseSiteData();
      w->OpenTab(L"orbit://settings/", true);
    });
    return Ok();
  }
  if (api == L"boost") {
    // zen-site-data-boost: the boost editor for this site.
    on_window([](BrowserWindow* w) {
      w->CloseSiteData();
      w->OpenBoostEditor();
    });
    return Ok();
  }
  if (api == L"close") {
    on_window([](BrowserWindow* w) { w->CloseSiteData(); });
    return Ok();
  }
  return Error(404, "unknown endpoint");
}

// ---------------------------------------------------------------------------
// Welcome (orbit://welcome, ZenWelcome). The page is an ordinary tab; its
// choices are ids from fixed lists, never URLs or colours from the request.

namespace {

struct WelcomeApp {
  const wchar_t* name;
  const wchar_t* url;
  uint32_t color;
};
// ZenWelcome's suggested Essentials.
constexpr WelcomeApp kWelcomeApps[] = {
    {L"Obsidian", L"https://obsidian.md", 0x7C3AED}, {L"Discord", L"https://discord.com", 0x5865F2},
    {L"Trello", L"https://trello.com", 0x0052CC},    {L"Slack", L"https://slack.com", 0xE01E5A},
    {L"GitHub", L"https://github.com", 0x57606A},    {L"Tuta", L"https://app.tuta.com/", 0xC00002},
    {L"Notion", L"https://notion.com", 0x37352F},    {L"Calendar", L"https://calendar.google.com", 0x4285F4},
    {L"Figma", L"https://figma.com", 0xF24E1E},
};

}  // namespace

InternalResponse WelcomeApi(const InternalRequest& req, Profile* profile) {
  Application* app = Application::Get();
  Tab* tab = app->client()->FindTab(req.browser_id);
  BrowserWindow* window = tab && tab->workspace() ? tab->workspace()->window() : nullptr;
  if (!window) return Error(403, "not a welcome tab");
  const std::wstring api = ApiName(req);
  if (api == L"state") {
    Dict d = CefDictionaryValue::Create();
    CefRefPtr<CefListValue> engines = CefListValue::Create();
    for (size_t i = 0; i < std::size(kSearchPresets); ++i) {
      Dict e = CefDictionaryValue::Create();
      e->SetString("name", kSearchPresets[i].name);
      e->SetBool("current", app->search_template() == kSearchPresets[i].url);
      engines->SetDictionary(i, e);
    }
    d->SetList("engines", engines);
    CefRefPtr<CefListValue> apps = CefListValue::Create();
    for (size_t i = 0; i < std::size(kWelcomeApps); ++i) {
      Dict e = CefDictionaryValue::Create();
      e->SetString("name", kWelcomeApps[i].name);
      wchar_t color[8];
      swprintf_s(color, L"%06X", kWelcomeApps[i].color);
      e->SetString("color", color);
      apps->SetDictionary(i, e);
    }
    d->SetList("apps", apps);
    CefRefPtr<CefListValue> colors = CefListValue::Create();
    for (size_t i = 0; i < std::size(ui::kAccentChoices); ++i) {
      wchar_t color[8];
      swprintf_s(color, L"%06X", ui::kAccentChoices[i]);
      colors->SetString(i, color);
    }
    d->SetList("colors", colors);
    d->SetBool("private", profile->is_private());
    d->SetBool("defaultBrowser", installer::IsDefaultBrowser());
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  if (api == L"import") {  // the Bookmarks page's importer (Netscape HTML)
    InternalRequest forwarded = req;
    forwarded.path = L"/api/import";
    return BookmarksApi(forwarded, profile);
  }
  Dict body = ParseBody(req);
  const int64_t index = Number(body, "index");
  if (api == L"search") {
    if (index < 0 || index >= static_cast<int64_t>(std::size(kSearchPresets))) return Error(400, "bad");
    app->store().SetPref(Store::kGlobalScope, L"search_template", kSearchPresets[index].url);
    return Ok();
  }
  const int browser = req.browser_id;
  auto on_window = [browser](std::function<void(BrowserWindow*, Tab*)> run) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](int id, std::function<void(BrowserWindow*, Tab*)> action) {
                              Tab* t = Application::Get()->client()->FindTab(id);
                              BrowserWindow* w = t && t->workspace() ? t->workspace()->window() : nullptr;
                              if (w) action(w, t);
                            },
                            browser, std::move(run)));
  };
  if (api == L"essentials") {
    std::vector<std::wstring> urls;
    if (body->HasKey("apps") && body->GetType("apps") == VTYPE_LIST) {
      CefRefPtr<CefListValue> list = body->GetList("apps");
      for (size_t i = 0; i < list->GetSize() && i < std::size(kWelcomeApps); ++i) {
        const int k = list->GetInt(i);
        if (k >= 0 && k < static_cast<int>(std::size(kWelcomeApps))) urls.push_back(kWelcomeApps[k].url);
      }
    }
    on_window([urls](BrowserWindow* w, Tab* welcome) {
      for (const std::wstring& url : urls) {
        if (Tab* t = w->OpenTabNear(welcome, url, false); t && w->CanAddToEssentials(t)) w->AddToEssentials(t);
      }
    });
    return Ok();
  }
  if (api == L"space-color") {
    if (index < 0 || index >= static_cast<int64_t>(std::size(ui::kAccentChoices))) return Error(400, "bad");
    const uint32_t color = ui::kAccentChoices[index];
    on_window([color](BrowserWindow* w, Tab*) { w->SetSpaceColor(color); });
    return Ok();
  }
  if (api == L"default-browser") {
    installer::MakeDefaultBrowser();
    return Ok();
  }
  if (api == L"done") {
    app->store().SetPref(Store::kGlobalScope, L"welcome_done", L"1");
    on_window([](BrowserWindow* w, Tab* welcome) { w->CloseTabShortcut(welcome); });
    return Ok();
  }
  return Error(404, "unknown endpoint");
}

InternalResponse LibraryApi(const InternalRequest& req, Profile* profile) {
  const std::wstring api = ApiName(req);
  // The library's History and Downloads sections use those pages' APIs.
  for (const wchar_t* section : {L"history/", L"downloads/"}) {
    if (api.rfind(section, 0) == 0) {
      InternalRequest forwarded = req;
      forwarded.path = L"/api/" + api.substr(wcslen(section));
      return section[0] == L'h' ? HistoryApi(forwarded, profile) : DownloadsApi(forwarded, profile);
    }
  }
  BrowserWindow* window = LibraryWindow(req.browser_id);
  if (!window) return Error(403, "not a library panel");
  if (api == L"spaces") {
    // The spaces the window switches between (every normal profile's).
    CefRefPtr<CefListValue> spaces = CefListValue::Create();
    size_t i = 0;
    for (Workspace* ws : window->Spaces()) {
      if (ws->is_linked()) continue;  // a linked window's space is not a space of its own here
      Dict s = CefDictionaryValue::Create();
      s->SetDouble("id", static_cast<double>(ws->id()));
      s->SetString("name", ws->record().name);
      s->SetString("icon", ws->record().icon);
      s->SetBool("current", ws == window->workspace());
      CefRefPtr<CefListValue> tabs = CefListValue::Create();
      size_t j = 0;
      for (size_t k = 0; k < ws->tabs().size(); ++k) {
        const Tab* t = ws->tabs()[k].get();
        if (t->empty_placeholder()) continue;
        Dict e = CefDictionaryValue::Create();
        e->SetInt("index", static_cast<int>(k));
        e->SetString("title", t->DisplayTitle());
        e->SetString("url", t->url());
        e->SetBool("pinned", t->pinned());
        tabs->SetDictionary(j++, e);
      }
      s->SetList("tabs", tabs);
      spaces->SetDictionary(i++, s);
    }
    Dict d = CefDictionaryValue::Create();
    d->SetList("spaces", spaces);
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  Dict body = ParseBody(req);
  const int id = req.browser_id;
  if (api == L"open") {
    const std::wstring url = Str(body, "url");
    if (url.rfind(L"https://", 0) != 0 && url.rfind(L"http://", 0) != 0) {
      return Error(400, "only web pages open from the library");
    }
    PostToLibraryWindow(id, [url](BrowserWindow* w) {
      w->CloseLibrary();
      w->OpenTab(url, true);
    });
    return Ok();
  }
  if (api == L"close") {
    PostToLibraryWindow(id, [](BrowserWindow* w) { w->CloseLibrary(); });
    return Ok();
  }
  if (api == L"section") {
    const std::wstring name = Str(body, "name");
    if (name == L"spaces" || name == L"downloads" || name == L"history") {
      Application::Get()->store().SetPref(Store::kGlobalScope, L"library_last_tab", name);  // zen.library.last-tab
    }
    return Ok();
  }
  if (api == L"spaces/show") {
    const int64_t space = Number(body, "space");
    const int tab = static_cast<int>(Number(body, "tab"));
    PostToLibraryWindow(id, [space, tab](BrowserWindow* w) { w->LibraryShowSpace(space, tab); });
    return Ok();
  }
  if (api == L"spaces/new") {
    if (profile->is_private()) return Error(403, "no spaces in private windows");
    PostToLibraryWindow(id, [](BrowserWindow* w) { w->LibraryNewSpace(); });
    return Ok();
  }
  return Error(404, "unknown endpoint");
}

// orbit://extensions: Orbit's own management page over Chrome's extension
// system (see ExtensionBridge). The list is the bridge's last report; the
// actions are asynchronous, so the page polls /api/list.
InternalResponse ExtensionsApi(const InternalRequest& req, Profile* profile) {
  Application* app = Application::Get();
  const std::wstring api = ApiName(req);
  ExtensionBridge* bridge = app->Extensions(profile);
  if (api == L"list") {
    Dict d = CefDictionaryValue::Create();
    d->SetBool("available", bridge != nullptr);
    if (!bridge) return Json(d);
    d->SetBool("ready", bridge->ready());
    d->SetBool("developer", bridge->developer_mode());
    d->SetDouble("revision", static_cast<double>(bridge->revision()));
    d->SetString("error", bridge->last_error());
    CefRefPtr<CefListValue> items = CefListValue::Create();
    size_t i = 0;
    for (const ExtensionInfo& e : bridge->extensions()) {
      Dict x = CefDictionaryValue::Create();
      x->SetString("id", e.id);
      x->SetString("name", e.name);
      x->SetString("version", e.version);
      x->SetString("description", e.description);
      x->SetString("icon", e.icon);
      x->SetString("location", e.location);
      x->SetString("options", e.options_url);
      x->SetString("home", e.home_url);
      x->SetBool("enabled", e.enabled);
      x->SetBool("mayDisable", e.may_disable);
      x->SetBool("popup", !e.popup.empty());
      CefRefPtr<CefListValue> perms = CefListValue::Create();
      for (size_t k = 0; k < e.permissions.size(); ++k) perms->SetString(k, e.permissions[k]);
      x->SetList("permissions", perms);
      items->SetDictionary(i++, x);
    }
    d->SetList("extensions", items);
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  if (!bridge) return Error(403, "no extensions in private windows");
  Dict body = ParseBody(req);
  const std::string id = ToUtf8(Str(body, "id"));
  BrowserWindow* window = WindowFor(profile);
  if (api == L"refresh") {
    bridge->Refresh();
    return Ok();
  }
  if (api == L"enable") {
    if (!ExtensionBridge::IsExtensionId(id)) return Error(400, "bad id");
    bridge->SetEnabled(id, body->HasKey("enabled") && body->GetBool("enabled"));
    return Ok();
  }
  if (api == L"remove") {
    if (!ExtensionBridge::IsExtensionId(id)) return Error(400, "bad id");
    bridge->Uninstall(id);  // Chrome asks for confirmation itself
    return Ok();
  }
  if (api == L"developer-mode") {
    bridge->SetDeveloperMode(body->HasKey("enabled") && body->GetBool("enabled"));
    return Ok();
  }
  if (api == L"load-unpacked") {
    bridge->LoadUnpacked();
    return Ok();
  }
  if (api == L"open-options") {
    if (!ExtensionBridge::IsExtensionId(id) || !window) return Error(400, "bad request");
    for (const ExtensionInfo& e : bridge->extensions()) {
      // Only the extension's own page, as reported by Chrome.
      if (e.id == id && e.options_url.rfind("chrome-extension://" + id + "/", 0) == 0) {
        window->OpenTab(FromUtf8(e.options_url), true);
        return Ok();
      }
    }
    return Error(404, "no options page");
  }
  if (api == L"open-store") {
    if (!window) return Error(400, "no window");
    window->OpenTab(L"https://chromewebstore.google.com/category/extensions", true);
    return Ok();
  }
  if (api == L"open") {
    const std::wstring url = Str(body, "url");
    if (!window || url.rfind(L"https://", 0) != 0) return Error(400, "only https pages open from here");
    window->OpenTab(url, true);
    return Ok();
  }
  return Error(404, "unknown endpoint");
}

// orbit://find/: the find bar's page. It acts on the active tab of the window
// the bar belongs to.
InternalResponse FindApi(const InternalRequest& req, Profile*) {
  const std::wstring api = ApiName(req);
  if (api == L"state") {
    const FindBar::State st = FindBar::CurrentState();
    Dict d = CefDictionaryValue::Create();
    d->SetBool("open", st.open);
    d->SetInt("count", st.count);
    d->SetInt("active", st.active);
    d->SetString("text", FindBar::LastText());
    return Json(d);
  }
  if (!req.trusted_action) return Error(403, "forbidden");
  Dict body = ParseBody(req);
  if (api == L"search") {
    FindBar::Search(Str(body, "text"), !body->HasKey("forward") || body->GetBool("forward"),
                    body->HasKey("matchCase") && body->GetBool("matchCase"),
                    body->HasKey("next") && body->GetBool("next"));
    return Ok();
  }
  if (api == L"close") {
    FindBar::Close();
    return Ok();
  }
  return Error(404, "unknown endpoint");
}

}  // namespace orbit
