#include "app/application.h"

#include "app/self_test.h"
#include "core/space_routing.h"

#include <objbase.h>

#include <algorithm>
#include <cwctype>

#include "browser/browser_client.h"
#include "browser/downloads.h"
#include "browser/permissions.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/log.h"
#include "core/strings.h"
#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_closure_task.h"
#include "persistence/schema.h"
#include "ui/browser_window.h"

namespace orbit {
namespace {

Application* g_application = nullptr;
constexpr int kSaveDelayMs = 1500;
constexpr int kSleepTickMs = 30 * 1000;

std::wstring NewUuid() {
  GUID guid{};
  ::CoCreateGuid(&guid);
  wchar_t buffer[64] = {};
  ::StringFromGUID2(guid, buffer, 64);
  std::wstring s(buffer);
  s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c == L'{' || c == L'}'; }),
          s.end());
  return s;
}

}  // namespace

Application* Application::Get() { return g_application; }

std::filesystem::path Application::ProfileDir(const std::wstring& uuid) const {
  // Chromium (Chrome bootstrap) only accepts profile directories that are
  // direct children of root_cache_path; anything deeper silently falls back
  // to a non-persistent profile.
  return user_data_dir_ / (L"Profile-" + uuid);
}

Application::Application(std::filesystem::path user_data_dir)
    : user_data_dir_(std::move(user_data_dir)) {
  g_application = this;
}

Application::~Application() { g_application = nullptr; }

BrowserClient* Application::client() const { return client_.get(); }

bool Application::Initialize() {
  std::error_code ec;
  std::filesystem::create_directories(user_data_dir_, ec);
  if (!db_.Open(user_data_dir_ / L"orbit.db") || !MigrateSchema(db_)) {
    ORBIT_LOG_ERROR("failed to open metadata database: %s", db_.last_error().c_str());
    return false;
  }
  store_ = std::make_unique<Store>(db_);
  DeletePendingProfileDirectories();
  if (const int n = store_->MarkInterruptedDownloads(); n > 0) {
    ORBIT_LOG_INFO("%d download(s) were interrupted by the previous shutdown", n);
  }
  client_ = new BrowserClient();
  downloads_ = std::make_unique<DownloadManager>();
  permissions_ = std::make_unique<PermissionManager>();
  LoadProfiles();
  return true;
}

void Application::LoadProfiles() {
  auto records = store_->LoadProfiles();
  if (records.empty()) {
    if (auto rec = store_->CreateProfile(NewUuid(), L"Personal", ui::kAccentChoices[0], L"")) {
      records.push_back(*rec);
    }
  }
  for (auto& rec : records) {
    auto profile = std::make_unique<Profile>(rec, ProfileDir(rec.uuid), /*is_private=*/false);
    profile->LoadFromStore(*store_);
    Profile* raw = profile.get();
    profiles_.push_back(std::move(profile));
    if (raw->workspaces().empty()) CreateWorkspace(raw, L"Home", rec.color);
  }
}

void Application::LoadShortcuts() {
  shortcuts_.ResetAll();
  for (const ShortcutAction& a : ShortcutActions()) {
    const auto value = store_->GetPref(Store::kGlobalScope, std::wstring(L"shortcut:") + a.id);
    if (!value) continue;
    if (*value == L"none") {
      shortcuts_.Set(a.id, {});
    } else if (auto chord = ParseChord(*value); chord && AssignableChord(*chord)) {
      shortcuts_.Set(a.id, *chord);
    }
  }
}

void Application::SetShortcut(const std::wstring& id, const KeyChord& chord) {
  if (!FindShortcutAction(id)) return;
  const ShortcutAction* a = FindShortcutAction(id);
  const std::wstring key = L"shortcut:" + id;
  if (chord == a->key) {
    store_->DeletePref(Store::kGlobalScope, key);
    shortcuts_.Reset(id);
    return;
  }
  store_->SetPref(Store::kGlobalScope, key, chord.empty() ? L"none" : FormatChord(chord));
  shortcuts_.Set(id, chord);
}

void Application::ResetShortcut(const std::wstring& id) {
  for (const ShortcutAction& a : ShortcutActions()) {
    if (id.empty() || id == a.id) {
      store_->DeletePref(Store::kGlobalScope, std::wstring(L"shortcut:") + a.id);
      shortcuts_.Reset(a.id);
    }
  }
}

Workspace* Application::RouteTarget(Profile* profile, const std::wstring& url, bool external) {
  if (!profile || profile->is_private()) return nullptr;
  const auto routes = ParseRoutes(store_->GetPref(Store::kGlobalScope, L"space_routes").value_or(L""));
  const int64_t external_default =
      _wtoi64(store_->GetPref(Store::kGlobalScope, L"space_route_external").value_or(L"0").c_str());
  const auto target = RouteUrl(routes, url, external, external_default);
  return target && *target ? profile->FindWorkspace(*target) : nullptr;
}

void Application::SetShortcutRecording(bool on) {
  shortcut_recording_until_ = on ? ::GetTickCount64() + 15000 : 0;
}

bool Application::ShortcutRecording() const {
  return ::GetTickCount64() < shortcut_recording_until_;
}

void Application::Start(const std::vector<std::wstring>& urls) {
  started_ = true;
  LoadShortcuts();
  UpdateSleepPolicy();
  // zen.workspaces.continue-where-left-off (default off): the session is
  // restored, but each window opens on an empty tab with the URL bar
  // (ZenSpaceManager selects its empty tab) rather than the last tab.
  const bool start_empty =
      store_->GetPref(Store::kGlobalScope, L"continue_where_left_off").value_or(L"0") != L"1";
  for (const WindowRecord& rec : store_->LoadWindows()) {
    Profile* profile = FindProfile(rec.profile_id);
    if (!profile) continue;
    Workspace* ws = profile->FindWorkspace(rec.workspace_id);
    if (!ws || ws->window()) ws = FreeWorkspaceFor(profile);
    OpenWindow(profile, ws, &rec, start_empty);
  }
  const bool first_run = windows_.empty();
  if (windows_.empty() && !profiles_.empty()) {
    OpenWindow(profiles_.front().get(), FreeWorkspaceFor(profiles_.front().get()), nullptr);
  }
  if (!windows_.empty()) {
    for (const auto& url : urls) windows_.front()->OpenRoutedTab(url, true, /*external=*/true);
    // ZenWelcome: the first start of a new profile shows the onboarding
    // once (never during self-tests, which count tabs).
    if (first_run && urls.empty() && !self_test::IsActive() &&
        !store_->GetPref(Store::kGlobalScope, L"welcome_shown")) {
      store_->SetPref(Store::kGlobalScope, L"welcome_shown", L"1");
      windows_.front()->OpenTab(L"orbit://welcome/", true);
    }
  }
  live_folders_.Load();
}

void Application::OnRelaunch(const std::vector<std::wstring>& urls) {
  if (profiles_.empty()) return;
  // Links opened from other applications go to the most recently used normal
  // window; a bare relaunch opens a new window.
  BrowserWindow* target = nullptr;
  if (!urls.empty()) {
    for (const auto& w : windows_) {
      if (!w->closing() && !w->profile()->is_private()) target = w.get();
    }
  }
  if (!target) {
    target = OpenWindow(profiles_.front().get(), FreeWorkspaceFor(profiles_.front().get()),
                        nullptr);
  }
  if (!target) return;
  for (const auto& url : urls) target->OpenRoutedTab(url, true, /*external=*/true);
  if (::IsIconic(target->hwnd())) ::ShowWindow(target->hwnd(), SW_RESTORE);
  ::SetForegroundWindow(target->hwnd());
}

void Application::Shutdown() {
  SaveNow();
  live_folders_.Shutdown();
  windows_.clear();
  closing_tabs_.clear();
  private_profiles_.clear();
  profiles_.clear();
  client_ = nullptr;
  downloads_.reset();
  permissions_.reset();
  store_.reset();
  db_.Close();
}

std::wstring Application::search_template() {
  auto value = store_->GetPref(Store::kGlobalScope, L"search_template");
  return (value && value->find(L"{query}") != std::wstring::npos) ? *value : kDefaultSearch;
}

ui::ThemeMode Application::theme_mode() {
  auto value = store_->GetPref(Store::kGlobalScope, L"theme");
  if (value == L"dark") return ui::ThemeMode::kDark;
  if (value == L"light") return ui::ThemeMode::kLight;
  return ui::ThemeMode::kSystem;
}

bool Application::dark_mode() {
  switch (theme_mode()) {
    case ui::ThemeMode::kDark:
      return true;
    case ui::ThemeMode::kLight:
      return false;
    case ui::ThemeMode::kSystem:
      break;
  }
  return ui::SystemPrefersDark();
}

uint32_t Application::accent_color() {
  auto value = store_->GetPref(Store::kGlobalScope, L"accent");
  if (!value || value->empty()) return ui::kAccentChoices[0];
  return static_cast<uint32_t>(std::wcstoul(value->c_str(), nullptr, 16)) & 0xFFFFFF;
}

Profile* Application::FindProfile(int64_t id) const {
  for (const auto& p : profiles_) {
    if (p->id() == id) return p.get();
  }
  return nullptr;
}

Profile* Application::FindAnyProfile(int64_t id) const {
  if (Profile* p = FindProfile(id)) return p;
  for (const auto& p : private_profiles_) {
    if (p->id() == id) return p.get();
  }
  return nullptr;
}

Profile* Application::CreateProfile(const std::wstring& name, uint32_t color) {
  auto rec = store_->CreateProfile(NewUuid(), name, color, L"");
  if (!rec) return nullptr;
  auto profile =
      std::make_unique<Profile>(*rec, ProfileDir(rec->uuid), false);
  Profile* raw = profile.get();
  profiles_.push_back(std::move(profile));
  CreateWorkspace(raw, L"Home", color);
  return raw;
}

Workspace* Application::CreateWorkspace(Profile* profile, const std::wstring& name,
                                        uint32_t color) {
  WorkspaceRecord rec;
  if (profile->is_private()) {
    rec.name = name;
    rec.color = color;
  } else {
    auto stored = store_->CreateWorkspace(profile->id(), name, color, L"");
    if (!stored) return nullptr;
    rec = *stored;
  }
  return profile->AddWorkspace(std::make_unique<Workspace>(profile, rec));
}

void Application::RetireWorkspace(Workspace* workspace) {
  Profile* profile = workspace->profile();
  std::vector<Tab*> tabs;
  for (const auto& t : workspace->tabs()) tabs.push_back(t.get());
  for (Tab* t : tabs) {
    std::unique_ptr<Tab> owned = workspace->RemoveTab(t);
    if (owned->load_state() != Tab::LoadState::kUnloaded) closing_tabs_.push_back(std::move(owned));
  }
  profile->RemoveWorkspace(workspace);
}

void Application::DeleteProfile(Profile* profile) {
  if (profile->is_private() || profiles_.size() <= 1) return;
  if (std::find(profiles_pending_delete_.begin(), profiles_pending_delete_.end(), profile) !=
      profiles_pending_delete_.end()) {
    return;
  }
  // Keep the application alive: if only this profile has windows, open one
  // for another profile first.
  const bool other_window = std::any_of(windows_.begin(), windows_.end(), [&](const auto& w) {
    return w->profile() != profile && !w->closing();
  });
  if (!other_window) {
    for (const auto& p : profiles_) {
      if (p.get() != profile) {
        OpenWindow(p.get(), FreeWorkspaceFor(p.get()), nullptr);
        break;
      }
    }
  }
  profiles_pending_delete_.push_back(profile);
  for (const auto& w : windows_) {
    if (w->profile() == profile) w->BeginClose();
  }
  // Unload hidden live tabs hosted by other windows of this profile (none can
  // exist after BeginClose, but be defensive about popups in creation).
  for (Workspace* ws : profile->containers()) {
    for (const auto& t : ws->tabs()) t->Unload(false);
  }
  CheckPendingWindowCloses();
  FinishProfileDeletions();
}

void Application::FinishProfileDeletions() {
  for (auto it = profiles_pending_delete_.begin(); it != profiles_pending_delete_.end();) {
    Profile* profile = *it;
    if (profile->HasLiveBrowsers() ||
        std::any_of(windows_.begin(), windows_.end(),
                    [&](const auto& w) { return w->profile() == profile; })) {
      ++it;
      continue;
    }
    // Chromium may keep files open until the context is fully torn down, so
    // the directory is removed at the next start (before CefInitialize).
    std::wstring pending = store_->GetPref(Store::kGlobalScope, L"pending_profile_deletes")
                               .value_or(L"");
    pending += profile->record().uuid + L";";
    store_->SetPref(Store::kGlobalScope, L"pending_profile_deletes", pending);
    store_->DeleteProfile(profile->id());  // cascades to workspaces, tabs, history, ...
    profile->ReleaseRequestContext();
    downloads_->ForgetProfile(profile);
    ORBIT_LOG_INFO("deleted profile %lld", profile->id());
    std::erase_if(profiles_, [&](const auto& p) { return p.get() == profile; });
    it = profiles_pending_delete_.erase(it);
  }
}

void Application::DeletePendingProfileDirectories() {
  const std::wstring pending =
      store_->GetPref(Store::kGlobalScope, L"pending_profile_deletes").value_or(L"");
  size_t start = 0;
  while (start < pending.size()) {
    const size_t end = pending.find(L';', start);
    const std::wstring uuid = pending.substr(start, end - start);
    start = (end == std::wstring::npos) ? pending.size() : end + 1;
    // Only well-formed UUIDs (hex and dashes) are accepted, so a corrupted
    // pref can never point the deletion at anything but a profile directory.
    const bool valid = uuid.size() == 36 && std::all_of(uuid.begin(), uuid.end(), [](wchar_t c) {
                         return iswxdigit(c) || c == L'-';
                       });
    if (!valid) continue;
    std::error_code ec;
    std::filesystem::remove_all(ProfileDir(uuid), ec);
    if (ec) ORBIT_LOG_WARNING("could not delete profile directory: %s", ec.message().c_str());
  }
  store_->SetPref(Store::kGlobalScope, L"pending_profile_deletes", L"");
}

namespace {
bool IsHistoryUrl(const std::wstring& url) {
  const std::wstring lower = ToLowerAscii(url.substr(0, 8));
  return lower.starts_with(L"http://") || lower.starts_with(L"https://");
}
}  // namespace

void Application::RecordVisit(Tab* tab) {
  Profile* profile = tab->profile();
  if (!profile || profile->is_private() || !IsHistoryUrl(tab->url())) return;
  store_->AddVisit(profile->id(), tab->url(), tab->record().title, NowUnixMillis());
}

void Application::UpdateVisitTitle(Tab* tab) {
  Profile* profile = tab->profile();
  if (!profile || profile->is_private() || !IsHistoryUrl(tab->url())) return;
  store_->UpdateLatestVisitTitle(profile->id(), tab->url(), tab->record().title);
}

std::wstring Application::GlanceTrigger() {
  if (store_->GetPref(Store::kGlobalScope, L"glance_enabled").value_or(L"1") != L"1") return L"";
  const std::wstring trigger =
      store_->GetPref(Store::kGlobalScope, L"glance_trigger").value_or(L"alt");
  return trigger == L"ctrl" || trigger == L"shift" ? trigger : L"alt";
}

CefRefPtr<CefDictionaryValue> Application::RendererInfo() {
  CefRefPtr<CefDictionaryValue> info = CefDictionaryValue::Create();
  info->SetString("glance", GlanceTrigger());
  info->SetBool("drag", store_->GetPref(Store::kGlobalScope, L"drag_window_from_content").value_or(L"1") == L"1");
  return info;
}

void Application::ApplyThemeToAllWindows() {
  for (const auto& w : windows_) w->ApplyTheme();
}

void Application::ApplySidebarModeToAllWindows(bool compact) {
  for (const auto& w : windows_) w->SetCompact(compact);
}

void Application::ApplyCompactModeToAllWindows(bool hidden) {
  for (const auto& w : windows_) w->SetCompactMode(hidden);
}

void Application::CloseAllWindows() {
  SaveNow();  // every window is kept in the session for the next start
  std::vector<BrowserWindow*> all;
  for (const auto& w : windows_) all.push_back(w.get());
  for (BrowserWindow* w : all) w->BeginClose();
  CheckPendingWindowCloses();
}

Workspace* Application::FreeWorkspaceFor(Profile* profile) {
  for (const auto& ws : profile->workspaces()) {
    if (!ws->window()) return ws.get();
  }
  const std::wstring name = L"Space " + std::to_wstring(profile->workspaces().size() + 1);
  return CreateWorkspace(profile, name, profile->record().color);
}

BrowserWindow* Application::OpenWindow(Profile* profile, Workspace* workspace,
                                       const WindowRecord* placement, bool start_empty) {
  if (!profile || !workspace) return nullptr;
  auto window = std::make_unique<BrowserWindow>(profile);
  BrowserWindow* raw = window.get();
  windows_.push_back(std::move(window));
  if (!raw->Create(placement)) {
    windows_.pop_back();
    return nullptr;
  }
  raw->ShowWorkspace(workspace, placement ? placement->active_tab_id : 0, start_empty);
  ScheduleSave();
  return raw;
}

BrowserWindow* Application::OpenPrivateWindow() {
  ProfileRecord rec;
  rec.id = next_private_id_--;  // never stored; distinguishes private sessions
  rec.name = L"Private";
  rec.color = 0x8F7CE6;
  auto profile = std::make_unique<Profile>(rec, std::filesystem::path(), /*is_private=*/true);
  Profile* raw = profile.get();
  private_profiles_.push_back(std::move(profile));
  Workspace* ws = CreateWorkspace(raw, L"Private", rec.color);
  return OpenWindow(raw, ws, nullptr);
}

void Application::OnWindowCloseRequested(BrowserWindow* window) {
  // Persist before tearing down so that closing the last window keeps it in
  // the session for the next start.
  SaveNow();
  window->BeginClose();
  CheckPendingWindowCloses();
}

void Application::OnWindowDestroyed(BrowserWindow* window) {
  Profile* profile = window->profile();
  auto it = std::find_if(windows_.begin(), windows_.end(),
                         [&](const auto& w) { return w.get() == window; });
  if (it == windows_.end()) return;
  std::unique_ptr<BrowserWindow> owned = std::move(*it);
  windows_.erase(it);
  owned.reset();
  if (profile->is_private()) {
    downloads_->ForgetProfile(profile);
    permissions_->ForgetProfile(profile);
    std::erase_if(private_profiles_, [&](const auto& p) { return p.get() == profile; });
  }
  FinishProfileDeletions();
  MaybeQuit();
}

namespace {
int64_t NowUnixMillisForBatch() {
  FILETIME ft;
  ::GetSystemTimeAsFileTime(&ft);
  const ULONGLONG t = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return static_cast<int64_t>(t / 10000ULL);
}
}  // namespace

void Application::RememberClosedTab(Tab* tab) {
  Profile* profile = tab->profile();
  if (!profile || profile->is_private()) return;
  if (tab->url().empty() || tab->url() == kNewTabUrl) return;
  ClosedTabRecord rec;
  rec.profile_id = profile->id();
  rec.workspace_id = tab->workspace() ? tab->workspace()->id() : 0;
  rec.url = tab->url();
  rec.title = tab->record().title;
  rec.batch_id = tab->close_batch();
  store_->PushClosedTab(rec);
}

std::vector<BrowserWindow*> Application::WindowsShowing(Workspace* ws) const {
  std::vector<BrowserWindow*> out;
  if (ws->profile()->essentials() == ws) {
    for (const auto& w : windows_) {
      if (w->profile() == ws->profile() && !w->closing()) out.push_back(w.get());
    }
  } else if (ws->window()) {
    out.push_back(ws->window());
  }
  return out;
}

void Application::RemoveTab(Tab* tab, bool remember) {
  Workspace* ws = tab->workspace();
  if (!ws) return;
  if (!remember) live_folders_.ForgetTab(tab);  // not the user's close
  if (remember) RememberClosedTab(tab);
  const std::vector<BrowserWindow*> windows = WindowsShowing(ws);
  for (BrowserWindow* w : windows) w->OnTabRemoving(tab);
  const bool persistent = !ws->profile()->is_private();
  const int64_t id = tab->id();
  std::unique_ptr<Tab> owned = ws->RemoveTab(tab);
  if (persistent && id) store_->DeleteTab(id);
  owned.reset();
  for (BrowserWindow* w : windows) w->OnTabsChanged();
}

void Application::DetachClosingTab(Tab* tab) {
  ORBIT_LOG_VERBOSE("detach closing tab (state=%d)", static_cast<int>(tab->load_state()));
  Workspace* ws = tab->workspace();
  if (!ws) return;
  RememberClosedTab(tab);
  const std::vector<BrowserWindow*> windows = WindowsShowing(ws);
  for (BrowserWindow* w : windows) w->OnTabRemoving(tab);
  const bool persistent = !ws->profile()->is_private();
  const int64_t id = tab->id();
  closing_tabs_.push_back(ws->RemoveTab(tab));
  if (persistent && id) store_->DeleteTab(id);
  for (BrowserWindow* w : windows) w->OnTabsChanged();
}

void Application::OnBrowserClosed(Tab* tab) {
  std::erase_if(closing_tabs_, [&](const auto& t) { return t.get() == tab; });
  CheckPendingWindowCloses();
  FinishProfileDeletions();
  MaybeQuit();
}

bool Application::ReopenClosedTab(BrowserWindow* window) {
  Profile* profile = window->profile();
  if (profile->is_private()) return false;
  // Firefox's undoCloseTab restores every tab of the last close action.
  const auto batch = store_->PopClosedTabBatch(profile->id());
  if (batch.empty()) return false;
  for (size_t i = 0; i < batch.size(); ++i) {
    window->OpenTab(batch[i].url, /*activate=*/i + 1 == batch.size());
  }
  return true;
}

void Application::CloseTabsTogether(const std::vector<Tab*>& tabs) {
  if (tabs.empty()) return;
  static int64_t last_batch = 0;
  last_batch = std::max(last_batch + 1, NowUnixMillisForBatch());
  for (Tab* t : tabs) t->set_close_batch(tabs.size() > 1 ? last_batch : 0);
  // RequestClose may delete an unloaded tab immediately; iterate a copy.
  const std::vector<Tab*> copy = tabs;
  for (Tab* t : copy) t->RequestClose();
}

void Application::CheckPendingWindowCloses() {
  // Copy: destroying a window mutates windows_.
  std::vector<BrowserWindow*> closing;
  for (const auto& w : windows_) {
    if (w->closing()) closing.push_back(w.get());
  }
  for (BrowserWindow* w : closing) {
    bool waiting = std::any_of(closing_tabs_.begin(), closing_tabs_.end(),
                               [&](const auto& t) { return t->window() == w; });
    auto check_profile = [&](const Profile& p) {
      for (Workspace* ws : p.containers()) {
        for (const auto& t : ws->tabs()) {
          if (t->window() == w && t->load_state() != Tab::LoadState::kUnloaded) return true;
        }
      }
      return false;
    };
    waiting = waiting || w->library_alive();  // its library panel's browser
    for (const auto& p : profiles_) waiting = waiting || check_profile(*p);
    for (const auto& p : private_profiles_) waiting = waiting || check_profile(*p);
    if (!waiting) {
      w->Destroy();  // may delete |w| via OnWindowDestroyed
    } else {
      ORBIT_LOG_INFO("window close waiting: %zu closing tab(s), %zu live browser(s)",
                      closing_tabs_.size(), client_->browser_count());
      for (const auto& t : closing_tabs_) {
        ORBIT_LOG_VERBOSE("  closing tab state=%d has_browser=%d", static_cast<int>(t->load_state()),
                           t->has_browser() ? 1 : 0);
      }
    }
  }
}

void Application::MaybeQuit() {
  if (quitting_ || !started_ || !windows_.empty() || !closing_tabs_.empty()) return;
  quitting_ = true;
  ORBIT_LOG_INFO("last window closed; quitting");
  CefQuitMessageLoop();
}

void Application::UpdateSleepPolicy() {
  const std::wstring pref =
      store_->GetPref(Store::kGlobalScope, L"sleep_minutes").value_or(L"0");
  sleep_minutes_ = std::clamp(_wtoi(pref.c_str()), 0, 24 * 60);
  const uint64_t generation = ++sleep_generation_;
  if (sleep_minutes_ <= 0) return;  // no timer at all while the policy is off
  CefPostDelayedTask(TID_UI, base::BindOnce([](uint64_t g) {
                       if (Application* app = Application::Get()) app->SleepPolicyTick(g);
                     }, generation),
                     kSleepTickMs);
}

void Application::SleepPolicyTick(uint64_t generation) {
  if (generation != sleep_generation_ || sleep_minutes_ <= 0 || quitting_) return;
  ApplySleepPolicy(static_cast<int64_t>(sleep_minutes_) * 60 * 1000);
  CefPostDelayedTask(TID_UI, base::BindOnce([](uint64_t g) {
                       if (Application* app = Application::Get()) app->SleepPolicyTick(g);
                     }, generation),
                     kSleepTickMs);
}

int Application::ApplySleepPolicy(int64_t idle_ms) {
  int slept = 0;
  const int64_t now = NowUnixMillis();
  const ULONGLONG tick = ::GetTickCount64();
  auto consider = [&](const Profile& profile) {
    for (Workspace* ws : profile.containers()) {
      for (const auto& t : ws->tabs()) {
        Tab* tab = t.get();
        const bool visible = ws->window() && ws->active_tab() == tab;
        const bool eligible = tab->has_browser() && !visible && !tab->pinned() &&
                              !tab->is_protected() && !tab->media_capture() &&
                              now - tab->last_active_ms() >= idle_ms;
        if (!eligible) {
          tab->ResetMediaQuery();
          continue;
        }
        // Two-phase: ask the renderers first, decide on the next tick.
        if (tab->media_query_sent_at() == 0) {
          tab->QueryMediaPlayback();
        } else if (tick - tab->media_query_sent_at() >= 1000) {
          if (!tab->media_reported_playing()) {
            ORBIT_LOG_INFO("putting an inactive tab to sleep");
            tab->Unload(/*mark_discarded=*/true);
            ++slept;
          }
          tab->ResetMediaQuery();
        }
      }
    }
  };
  for (const auto& p : profiles_) consider(*p);
  for (const auto& p : private_profiles_) consider(*p);
  return slept;
}

void Application::ScheduleSave() {
  if (save_scheduled_ || !store_) return;
  save_scheduled_ = true;
  CefPostDelayedTask(TID_UI, base::BindOnce([] {
                       if (Application* app = Application::Get()) {
                         app->save_scheduled_ = false;
                         app->SaveNow();
                       }
                     }),
                     kSaveDelayMs);
}

std::vector<WindowRecord> Application::CollectWindowRecords() const {
  // Windows being closed while others stay open leave the session; if every
  // window is closing (application exit) they are all kept for restore.
  const bool any_open = std::any_of(windows_.begin(), windows_.end(), [](const auto& w) {
    return !w->closing() && !w->profile()->is_private();
  });
  std::vector<WindowRecord> out;
  for (const auto& w : windows_) {
    if (w->profile()->is_private()) continue;
    if (any_open && w->closing()) continue;
    out.push_back(w->ToRecord());
  }
  return out;
}

void Application::SaveNow() {
  if (!store_) return;
  Database::Transaction txn(db_);
  for (const auto& p : profiles_) {
    for (Workspace* ws : p->containers()) {
      for (const auto& t : ws->tabs()) {
        if (t->dirty() && t->id()) {
          store_->UpdateTab(t->record());
          t->ClearDirty();
        }
      }
    }
  }
  txn.Commit();
  if (!windows_.empty()) {
    auto records = CollectWindowRecords();
    if (!records.empty()) store_->SaveWindows(records);
  }
}

}  // namespace orbit
