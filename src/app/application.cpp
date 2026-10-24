#include "app/application.h"

#include <fstream>
#include <sstream>

#include "app/self_test.h"
#include "core/space_routing.h"

#include <objbase.h>

#include <algorithm>
#include <cwctype>

#include "browser/browser_client.h"
#include "browser/extension_bridge.h"
#include "browser/extension_popup.h"
#include "browser/find_bar.h"
#include "core/json.h"
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
    profiles_.push_back(std::move(profile));
  }
  // A profile may have no space of its own (its spaces moved to another
  // profile), but there is always at least one space.
  if (Spaces().empty() && !profiles_.empty()) {
    CreateWorkspace(profiles_.front().get(), L"Home", profiles_.front()->record().color);
  }
  NormalizeSpaceOrder();
}

std::vector<Workspace*> Application::Spaces() const {
  std::vector<Workspace*> out;
  for (const auto& p : profiles_) {
    for (const auto& ws : p->workspaces()) out.push_back(ws.get());
  }
  std::stable_sort(out.begin(), out.end(), [](const Workspace* a, const Workspace* b) {
    return a->record().position < b->record().position;
  });
  return out;
}

Workspace* Application::FindSpace(int64_t id) const {
  if (!id) return nullptr;
  for (const auto& p : profiles_) {
    if (Workspace* ws = p->FindWorkspace(id)) return ws;
  }
  return nullptr;
}

std::vector<Workspace*> Application::AllContainers() const {
  std::vector<Workspace*> out;
  for (const auto* list : {&profiles_, &private_profiles_}) {
    for (const auto& p : *list) {
      for (Workspace* ws : p->containers()) out.push_back(ws);
    }
  }
  return out;
}

void Application::NormalizeSpaceOrder() {
  const std::vector<Workspace*> spaces = Spaces();
  std::vector<int64_t> ids;
  bool changed = false;
  for (size_t i = 0; i < spaces.size(); ++i) {
    changed |= spaces[i]->record().position != static_cast<int>(i);
    spaces[i]->mutable_record().position = static_cast<int>(i);
    ids.push_back(spaces[i]->id());
  }
  if (changed) store_->SetWorkspaceOrder(ids);
  for (const auto& p : profiles_) p->SortWorkspaces();
}

void Application::MoveSpace(Workspace* ws, int new_index) {
  if (!ws || ws->is_essentials() || ws->is_linked()) return;
  if (ws->profile()->is_private()) {
    ws->profile()->MoveWorkspace(ws, new_index);
    return;
  }
  std::vector<Workspace*> spaces = Spaces();
  const auto it = std::find(spaces.begin(), spaces.end(), ws);
  if (it == spaces.end()) return;
  spaces.erase(it);
  new_index = std::clamp(new_index, 0, static_cast<int>(spaces.size()));
  spaces.insert(spaces.begin() + new_index, ws);
  std::vector<int64_t> ids;
  for (size_t i = 0; i < spaces.size(); ++i) {
    spaces[i]->mutable_record().position = static_cast<int>(i);
    ids.push_back(spaces[i]->id());
  }
  store_->SetWorkspaceOrder(ids);
  for (const auto& p : profiles_) p->SortWorkspaces();
}

bool Application::MoveSpaceToProfile(Workspace* ws, Profile* target) {
  // Zen binds a space to a container; here the space (with its Essentials
  // and the windows linked to it) moves to another profile. A Chromium
  // browser keeps the storage it was created with, so every tab is unloaded:
  // it loads again, in the new profile's storage, when it is selected.
  if (!ws || !target || target->is_private() || ws->is_essentials() || ws->is_linked()) return false;
  Profile* from = ws->profile();
  if (from == target || from->is_private()) return false;
  std::vector<Workspace*> moving{ws};
  for (const auto& linked : from->linked_spaces()) {
    if (linked->parent() == ws) moving.push_back(linked.get());
  }
  for (Workspace* m : moving) {
    for (Workspace* c : {m, m->essentials()}) {
      if (!c) continue;
      for (const auto& t : c->tabs()) t->UnloadForProfileChange();
    }
    std::unique_ptr<Workspace> owned = from->RemoveWorkspace(m);
    owned->set_profile(target);
    store_->UpdateWorkspace(owned->record());
    if (Workspace* grid = owned->essentials()) store_->UpdateWorkspace(grid->record());
    target->AddWorkspace(std::move(owned));
  }
  NormalizeSpaceOrder();
  for (const auto& w : windows_) w->OnSpaceProfileChanged(ws);
  ScheduleSave();
  return true;
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
  // A route names a space of any profile; the tab opens in that space's profile.
  Workspace* ws = target && *target ? FindSpace(*target) : nullptr;
  return ws && !ws->is_linked() ? ws : nullptr;
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
  // After "Restart to update" the windows come back as they were, once.
  const bool after_update = store_->GetPref(Store::kGlobalScope, L"restore_selected_once").value_or(L"0") == L"1";
  if (after_update) store_->SetPref(Store::kGlobalScope, L"restore_selected_once", L"0");
  const bool start_empty =
      !after_update &&
      store_->GetPref(Store::kGlobalScope, L"continue_where_left_off").value_or(L"0") != L"1";
  for (const WindowRecord& rec : store_->LoadWindows()) {
    // The window comes back on its space, whichever profile that space is in
    // now (it may have moved since).
    Workspace* ws = FindSpace(rec.workspace_id);
    Profile* profile = ws ? ws->profile() : FindProfile(rec.profile_id);
    if (!profile) continue;
    if (!ws || ws->window()) ws = FreeWorkspaceFor(profile);
    BrowserWindow* w = OpenWindow(profile, ws, &rec, start_empty);
    if (w && rec.linked_id) w->AdoptLinkedSpace(FindSpace(rec.linked_id));
  }
  // A linked window space whose window did not come back (a crash, a lost
  // window record) is closed as its window would have been: its tabs go
  // back to the parent space, in a folder.
  for (const auto& p : profiles_) {
    std::vector<Workspace*> orphans;
    for (const auto& ls : p->linked_spaces()) {
      if (!WindowOwningLinked(ls.get())) orphans.push_back(ls.get());
    }
    for (Workspace* ls : orphans) ReturnLinkedSpace(ls, /*into_folder=*/true, nullptr);
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

Profile* Application::CreateProfile(const std::wstring& name, uint32_t color, bool with_space) {
  auto rec = store_->CreateProfile(NewUuid(), name, color, L"");
  if (!rec) return nullptr;
  auto profile =
      std::make_unique<Profile>(*rec, ProfileDir(rec->uuid), false);
  Profile* raw = profile.get();
  profiles_.push_back(std::move(profile));
  if (with_space) CreateWorkspace(raw, L"Home", color);
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

Workspace* Application::CreateLinkedSpace(Workspace* parent) {
  if (!parent || parent->is_linked() || parent->is_essentials() || parent->profile()->is_private()) {
    return nullptr;
  }
  Profile* profile = parent->profile();
  // "<parent> · Window N": the parent's own window is window 1.
  int n = 2;
  for (bool taken = true; taken; ) {
    taken = false;
    const std::wstring name = parent->record().name + L" \u00B7 Window " + std::to_wstring(n);
    for (const auto& ls : profile->linked_spaces()) taken |= ls->record().name == name;
    if (taken) ++n;
  }
  WorkspaceRecord rec;
  rec.profile_id = profile->id();
  rec.name = parent->record().name + L" \u00B7 Window " + std::to_wstring(n);
  rec.color = parent->record().color;
  rec.icon = parent->record().icon;
  rec.theme = parent->record().theme;
  rec.kind = WorkspaceRecord::kKindLinked;
  rec.parent_id = parent->id();
  auto stored = store_->CreateWorkspace(rec);
  if (!stored) return nullptr;
  Workspace* linked = profile->AddWorkspace(std::make_unique<Workspace>(profile, *stored));
  linked->set_parent(parent);
  return linked;
}

BrowserWindow* Application::WindowOwningLinked(const Workspace* linked) const {
  for (const auto& w : windows_) {
    if (!w->closing() && w->linked_space() == linked) return w.get();
  }
  return nullptr;
}

void Application::ReturnLinkedSpace(Workspace* linked, bool into_folder, BrowserWindow* closing) {
  Workspace* parent = linked ? linked->parent() : nullptr;
  if (!parent) return;
  // The closing window lets go of everything first, so nothing it shows
  // points at the tabs while they move.
  if (closing) closing->DetachWorkspace();
  BrowserWindow* home = parent->window();
  std::vector<Tab*> tabs;
  for (const auto& t : linked->tabs()) tabs.push_back(t.get());
  std::vector<Tab*> moved;
  for (Tab* t : tabs) {
    for (BrowserWindow* w : WindowsShowing(linked)) w->OnTabRemoving(t);
    std::unique_ptr<Tab> owned = linked->RemoveTab(t);
    if (owned->empty_placeholder()) {
      owned->Unload(false);
      if (owned->load_state() != Tab::LoadState::kUnloaded) closing_tabs_.push_back(std::move(owned));
      continue;
    }
    Tab* back = parent->InsertTab(std::move(owned), -1);
    back->MarkDirty();
    // A live page keeps its state in the parent's window; without one, the
    // closing window unloads it.
    if (home && back->window() && back->window() != home) {
      back->Hide();
      back->Reparent(home);
    }
    moved.push_back(back);
  }
  // The parent's Essentials shown in the closing window go home too.
  if (closing && home && parent->essentials()) {
    for (const auto& t : parent->essentials()->tabs()) {
      if (t->window() == closing) {
        t->Hide();
        t->Reparent(home);
      }
    }
  }
  if (into_folder) {
    // Kept together in a folder named after the linked space (Zen's window
    // sync has no equivalent; this is Orbit's "temp folder" coming home).
    std::vector<Tab*> unpinned;
    for (Tab* t : moved) {
      if (!t->pinned()) unpinned.push_back(t);
    }
    if (!unpinned.empty()) parent->CreateGroup(linked->record().name, linked->record().color, unpinned);
  }
  store_->DeleteWorkspace(linked->id());
  RetireWorkspace(linked);
  if (home) home->OnTabsChanged();
  if (closing) {
    closing->BeginClose();
    CheckPendingWindowCloses();
  }
  ScheduleSave();
}

void Application::BringBackLinkedSpace(Workspace* linked) {
  Workspace* parent = linked ? linked->parent() : nullptr;
  if (!parent) return;
  BrowserWindow* window = WindowOwningLinked(linked);
  const bool showing = window && window->workspace() == linked;
  if (showing && !parent->window()) {
    // Nowhere else to go: this window returns to the parent space.
    window->SwitchToSpace(parent);
    ReturnLinkedSpace(linked, /*into_folder=*/false, nullptr);
    return;
  }
  ReturnLinkedSpace(linked, /*into_folder=*/false, showing ? window : nullptr);
  if (BrowserWindow* home = parent->window()) {
    if (::IsIconic(home->hwnd())) ::ShowWindow(home->hwnd(), SW_RESTORE);
    ::SetForegroundWindow(home->hwnd());
  }
}

void Application::RetireWorkspace(Workspace* workspace) {
  Profile* profile = workspace->profile();
  for (const auto& w : windows_) w->ForgetSpace(workspace);
  // Its linked window spaces stay, as ordinary spaces (the store already
  // cleared their parent_id).
  std::vector<Workspace*> children;
  for (const auto& ls : profile->linked_spaces()) {
    if (ls->parent() == workspace) children.push_back(ls.get());
  }
  // Its Essentials go with it (the store cascades the rows); the windows that
  // show one let go of it first.
  for (Workspace* ws : {workspace->essentials(), workspace}) {
    if (!ws) continue;
    std::vector<Tab*> tabs;
    for (const auto& t : ws->tabs()) tabs.push_back(t.get());
    const std::vector<BrowserWindow*> windows = WindowsShowing(ws);
    for (Tab* t : tabs) {
      if (ws->is_essentials()) {
        for (BrowserWindow* w : windows) w->OnTabRemoving(t);
      }
      std::unique_ptr<Tab> owned = ws->RemoveTab(t);
      // A live tab is kept until Chromium closes its browser (closed here
      // when the caller has not already).
      if (owned->load_state() != Tab::LoadState::kUnloaded) {
        owned->Unload(false);
        closing_tabs_.push_back(std::move(owned));
      }
    }
  }
  profile->RemoveWorkspace(workspace);
  for (Workspace* child : children) {
    for (const auto& w : windows_) {
      if (w->linked_space() == child) w->ForgetSpace(child);  // no longer linked to anything
    }
    std::unique_ptr<Workspace> owned = profile->RemoveWorkspace(child);
    owned->set_parent(nullptr);
    owned->mutable_record().kind = WorkspaceRecord::kKindSpace;
    owned->mutable_record().parent_id = 0;
    owned->mutable_record().position = 1 << 30;  // last
    if (!profile->is_private()) store_->UpdateWorkspace(owned->record());
    profile->AddWorkspace(std::move(owned));
  }
  if (!children.empty()) {
    NormalizeSpaceOrder();
    for (const auto& w : windows_) w->OnSpaceProfileChanged(nullptr);  // relayout: new spaces listed
  }
}

void Application::DeleteProfile(Profile* profile) {
  if (profile->is_private() || profiles_.size() <= 1) return;
  if (std::find(profiles_pending_delete_.begin(), profiles_pending_delete_.end(), profile) !=
      profiles_pending_delete_.end()) {
    return;
  }
  Profile* heir = nullptr;
  for (const auto& p : profiles_) {
    if (p.get() != profile && !heir) heir = p.get();
  }
  profiles_pending_delete_.push_back(profile);
  // Windows on one of its spaces go to another space (any profile); the
  // windows of its linked spaces close.
  for (const auto& w : windows_) {
    if (w->closing() || w->profile() != profile) continue;
    if (w->workspace() && w->workspace()->is_linked()) {
      w->BeginClose();
      continue;
    }
    Workspace* next = nullptr;
    for (Workspace* ws : Spaces()) {
      if (ws->profile() != profile && !ws->window()) {
        next = ws;
        break;
      }
    }
    if (!next) next = FreeWorkspaceFor(heir);
    w->ShowWorkspace(next, 0);
    if (w->profile() == profile) w->BeginClose();  // could not leave it
  }
  for (const auto& w : windows_) w->ForgetProfile(profile);
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
    if (profile->HasLiveBrowsers() || CloseExtensionBridges(profile, nullptr) ||
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
    for (const auto& w : windows_) w->ForgetProfile(profile);
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
  // Pages follow the theme too (prefers-color-scheme).
  for (const auto* list : {&profiles_, &private_profiles_}) {
    for (const auto& p : *list) {
      for (Workspace* ws : p->containers()) {
        for (const auto& t : ws->tabs()) t->ApplyWebColorScheme();
      }
    }
  }
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
  const size_t count = profile->is_private() ? profile->workspaces().size() : Spaces().size();
  const std::wstring name = L"Space " + std::to_wstring(count + 1);
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
  if (!profile->is_private() && HasInstalledExtensions(*profile)) Extensions(profile);  // toolbar buttons
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
  ORBIT_LOG_INFO("window close requested (%zu window(s) open)", windows_.size());
  // Closing a linked window brings its tabs back to the parent space, in a
  // folder, unless it is the last window (the session keeps it then).
  Workspace* linked = window->closing() ? nullptr : window->linked_space();
  const bool others = std::any_of(windows_.begin(), windows_.end(), [&](const auto& w) {
    return w.get() != window && !w->closing() && !w->profile()->is_private();
  });
  if (linked && linked->parent() && others) {
    ReturnLinkedSpace(linked, /*into_folder=*/true, window);  // closes the window
    SaveNow();
    return;
  }
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
  if (ws->is_essentials()) {
    // A space's grid shows with the space, and in the windows of the spaces
    // linked to it.
    for (const auto& w : windows_) {
      if (!w->closing() && w->workspace() && w->workspace()->essentials_space() == ws->owner()) {
        out.push_back(w.get());
      }
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
          if (t->devtools_window() == w && t->devtools_alive()) return true;
        }
      }
      return false;
    };
    waiting = waiting || Tab::PendingHostDestroys() > 0;
    if (ExtensionPopup::Alive()) {
      ExtensionPopup::CloseCurrent();  // it belongs to a window that is going away
      waiting = true;
    }
    if (FindBar::Alive()) {
      FindBar::Close();
      waiting = true;
    }
    waiting = waiting || CloseExtensionBridges(w->profile(), w);
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

ExtensionBridge* Application::Extensions(Profile* profile) {
  if (!profile || profile->is_private()) return nullptr;
  auto& bridge = extension_bridges_[profile];
  if (!bridge) bridge = new ExtensionBridge(profile);
  if (!bridge->alive()) bridge->Start();
  return bridge.get();
}

ExtensionBridge* Application::FindExtensions(const Profile* profile) const {
  for (const auto& [owner, bridge] : extension_bridges_) {
    if (owner == profile && bridge->alive()) return bridge.get();
  }
  return nullptr;
}

void Application::OnExtensionAdded(Profile* profile, const std::string& name) {
  for (const auto& w : windows_) {
    if (w->profile() != profile || w->closing()) continue;
    ui::Toast::Content content;
    content.id = L"extension-added";
    content.text = FromUtf8(name) + L" was added to Orbit";
    content.timeout_ms = 4000;
    w->ShowToast(content);
    break;
  }
}

std::vector<std::string> Application::PinnedExtensions(const Profile* profile) {
  std::vector<std::string> out;
  if (!profile || profile->is_private()) return out;
  const std::string list = ToUtf8(store_->GetPref(profile->id(), L"extensions_pinned").value_or(L""));
  size_t start = 0;
  while (start < list.size()) {
    size_t comma = list.find(',', start);
    if (comma == std::string::npos) comma = list.size();
    const std::string id = list.substr(start, comma - start);
    if (ExtensionBridge::IsExtensionId(id)) out.push_back(id);
    start = comma + 1;
  }
  return out;
}

void Application::SetExtensionPinned(Profile* profile, const std::string& id, bool pinned) {
  if (!profile || profile->is_private() || !ExtensionBridge::IsExtensionId(id)) return;
  std::vector<std::string> list = PinnedExtensions(profile);
  std::erase(list, id);
  if (pinned) list.push_back(id);
  std::string joined;
  for (const std::string& x : list) joined += (joined.empty() ? "" : ",") + x;
  store_->SetPref(profile->id(), L"extensions_pinned", FromUtf8(joined));
  OnExtensionsChanged(profile);
}

void Application::OnExtensionsChanged(Profile* profile) {
  for (const auto& w : windows_) {
    if (w->profile() == profile && !w->closing()) w->OnExtensionsChanged();
  }
}

bool Application::HasInstalledExtensions(const Profile& profile) {
  // Chrome puts a default extension into every new profile; only a store
  // install (location 1) or an unpacked one (4) is the user's own.
  std::ifstream in(profile.data_dir() / L"Secure Preferences", std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  const auto prefs = ParseJson(text.str());
  if (!prefs || !prefs->is_object()) return false;
  const JsonValue* extensions = prefs->Get("extensions");
  const JsonValue* settings = extensions ? extensions->Get("settings") : nullptr;
  if (!settings || !settings->is_object()) return false;
  for (const auto& [id, entry] : settings->object) {
    const JsonValue* location = entry.is_object() ? entry.Get("location") : nullptr;
    if (location && location->type == JsonValue::Type::kNumber &&
        (location->number == 1 || location->number == 4)) {
      return true;
    }
  }
  return false;
}

bool Application::CloseExtensionBridges(Profile* profile, const BrowserWindow* except) {
  bool alive = false;
  for (auto& [owner, bridge] : extension_bridges_) {
    if (profile && owner != profile) continue;
    if (!bridge->alive()) continue;
    // A bridge lives as long as one of its profile's windows is open.
    const bool in_use = std::any_of(windows_.begin(), windows_.end(), [&](const auto& w) {
      return w.get() != except && !w->closing() && w->profile() == owner;
    });
    if (in_use) continue;
    bridge->Close();
    alive = alive || bridge->alive();
  }
  return alive;
}

void Application::MaybeQuit() {
  if (quitting_ || !started_ || !windows_.empty() || !closing_tabs_.empty()) return;
  if (CloseExtensionBridges(nullptr, nullptr) || Tab::PendingHostDestroys() > 0 || ExtensionPopup::Alive() ||
      FindBar::Alive()) {
    return;
  }
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
