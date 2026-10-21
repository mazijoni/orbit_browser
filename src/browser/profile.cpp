#include "browser/profile.h"

#include <algorithm>

#include "app/application.h"
#include "browser/internal_pages.h"
#include "browser/tab.h"
#include "core/log.h"

namespace orbit {

Workspace::Workspace(Profile* profile, WorkspaceRecord record)
    : profile_(profile), record_(std::move(record)) {}

Workspace::~Workspace() = default;

int Workspace::IndexOf(const Tab* tab) const {
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].get() == tab) return static_cast<int>(i);
  }
  return -1;
}

int Workspace::PinnedCount() const {
  return static_cast<int>(
      std::count_if(tabs_.begin(), tabs_.end(), [](const auto& t) { return t->pinned(); }));
}

Tab* Workspace::InsertTab(std::unique_ptr<Tab> tab, int index) {
  const int pinned = PinnedCount();
  const int lo = tab->pinned() ? 0 : pinned;
  const int hi = tab->pinned() ? pinned : static_cast<int>(tabs_.size());
  index = (index < 0) ? hi : std::clamp(index, lo, hi);
  tab->set_workspace(this);
  Tab* raw = tab.get();
  tabs_.insert(tabs_.begin() + index, std::move(tab));
  if (!profile_->is_private()) {
    TabRecord& rec = raw->mutable_record();
    rec.workspace_id = record_.id;
    if (rec.id == 0) rec.id = Application::Get()->store().InsertTab(rec);
  }
  PersistOrder();
  return raw;
}

std::unique_ptr<Tab> Workspace::RemoveTab(Tab* tab) {
  const int index = IndexOf(tab);
  if (index < 0) return nullptr;
  std::unique_ptr<Tab> out = std::move(tabs_[static_cast<size_t>(index)]);
  tabs_.erase(tabs_.begin() + index);
  if (active_tab_ == tab) active_tab_ = nullptr;
  out->set_workspace(nullptr);
  if (out->record().group_id) {
    out->set_group(0);
    PruneEmptyGroups();
  }
  PersistOrder();
  return out;
}

const TabGroupRecord* Workspace::FindGroup(int64_t id) const {
  for (const auto& g : groups_) {
    if (g.id == id) return &g;
  }
  return nullptr;
}

int Workspace::GroupSize(int64_t group_id) const {
  return static_cast<int>(std::count_if(tabs_.begin(), tabs_.end(), [&](const auto& t) {
    return t->record().group_id == group_id;
  }));
}

int64_t Workspace::CreateGroup(const std::wstring& name, uint32_t color,
                               const std::vector<Tab*>& tabs) {
  TabGroupRecord g;
  g.workspace_id = record_.id;
  g.name = name;
  g.color = color;
  if (profile_->is_private()) {
    g.id = next_session_group_id_--;
  } else {
    g.id = Application::Get()->store().InsertTabGroup(g);
    if (!g.id) return 0;
  }
  groups_.push_back(g);
  for (Tab* t : tabs) SetTabGroup(t, g.id);
  return g.id;
}

void Workspace::SetTabGroup(Tab* tab, int64_t group_id) {
  if (IndexOf(tab) < 0) return;
  if (tab->pinned() || (group_id && !FindGroup(group_id))) group_id = 0;
  if (tab->record().group_id != group_id) {
    tab->set_group(group_id);
    if (!profile_->is_private() && tab->id()) {
      Application::Get()->store().UpdateTab(tab->record());
    }
  }
  NormalizeGroups();
  PruneEmptyGroups();
  PersistOrder();
}

void Workspace::UpdateGroup(const TabGroupRecord& group) {
  for (auto& g : groups_) {
    if (g.id != group.id) continue;
    g.name = group.name;
    g.color = group.color;
    g.collapsed = group.collapsed;
    if (!profile_->is_private()) Application::Get()->store().UpdateTabGroup(g);
  }
}

void Workspace::Ungroup(int64_t group_id) {
  for (const auto& t : tabs_) {
    if (t->record().group_id == group_id) t->set_group(0);
  }
  std::erase_if(groups_, [&](const TabGroupRecord& g) { return g.id == group_id; });
  if (!profile_->is_private()) Application::Get()->store().DeleteTabGroup(group_id);
}

void Workspace::PruneEmptyGroups() {
  std::vector<int64_t> empty;
  for (const auto& g : groups_) {
    if (GroupSize(g.id) == 0) empty.push_back(g.id);
  }
  for (int64_t id : empty) Ungroup(id);
}

void Workspace::NormalizeGroups() {
  // Pinned tabs never belong to a group.
  for (const auto& t : tabs_) {
    if (t->pinned() && t->record().group_id) t->set_group(0);
  }
  std::vector<std::unique_ptr<Tab>> ordered;
  ordered.reserve(tabs_.size());
  std::vector<bool> taken(tabs_.size(), false);
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (taken[i]) continue;
    const int64_t g = tabs_[i]->record().group_id;
    if (!g) {
      taken[i] = true;
      ordered.push_back(std::move(tabs_[i]));
      continue;
    }
    // First member of group g: pull every member up behind it, in order.
    for (size_t j = i; j < tabs_.size(); ++j) {
      if (!taken[j] && tabs_[j] && tabs_[j]->record().group_id == g) {
        taken[j] = true;
        ordered.push_back(std::move(tabs_[j]));
      }
    }
  }
  tabs_ = std::move(ordered);
}

void Workspace::MoveTab(Tab* tab, int new_index) {
  const int from = IndexOf(tab);
  if (from < 0) return;
  std::unique_ptr<Tab> owned = std::move(tabs_[static_cast<size_t>(from)]);
  tabs_.erase(tabs_.begin() + from);
  const int pinned = PinnedCount();
  const int lo = owned->pinned() ? 0 : pinned;
  const int hi = owned->pinned() ? pinned : static_cast<int>(tabs_.size());
  new_index = std::clamp(new_index, lo, hi);
  tabs_.insert(tabs_.begin() + new_index, std::move(owned));
  NormalizeGroups();
  PruneEmptyGroups();
  PersistOrder();
}

void Workspace::AppendLoadedTab(std::unique_ptr<Tab> tab) {
  tab->set_workspace(this);
  tabs_.push_back(std::move(tab));
}

void Workspace::PersistOrder() {
  if (profile_->is_private()) return;
  std::vector<int64_t> ids;
  ids.reserve(tabs_.size());
  for (size_t i = 0; i < tabs_.size(); ++i) {
    ids.push_back(tabs_[i]->id());
    tabs_[i]->set_position(static_cast<int>(i));  // stored below; not dirty
  }
  Application::Get()->store().SetTabOrder(ids);
}

Profile::Profile(ProfileRecord record, std::filesystem::path data_dir, bool is_private)
    : record_(std::move(record)), data_dir_(std::move(data_dir)), is_private_(is_private) {}

Profile::~Profile() = default;

CefRefPtr<CefRequestContext> Profile::request_context() {
  if (!context_) {
    CefRequestContextSettings settings;
    if (!is_private_) {
      // Must be a child of CefSettings.root_cache_path; one directory per
      // profile gives Chromium-level isolation of all website data.
      CefString(&settings.cache_path) = data_dir_.wstring();
    }
    settings.persist_session_cookies = false;
    context_ = CefRequestContext::CreateContext(settings, nullptr);
    if (!is_private_ && context_->GetCachePath().ToWString() != data_dir_.wstring()) {
      // CEF falls back to an in-memory profile if it rejects the path. That
      // would silently lose all site data, so make it loud.
      ORBIT_LOG_ERROR("profile %lld: Chromium rejected its storage path; data will NOT persist",
                       record_.id);
    }
    RegisterInternalSchemeHandler(context_, record_.id, is_private_);
    ORBIT_LOG_INFO("created request context for profile %lld (private=%d)", record_.id,
                    is_private_ ? 1 : 0);
  }
  return context_;
}

Workspace* Profile::FindWorkspace(int64_t id) const {
  for (const auto& w : workspaces_) {
    if (w->id() == id) return w.get();
  }
  return nullptr;
}

Workspace* Profile::AddWorkspace(std::unique_ptr<Workspace> workspace) {
  workspaces_.push_back(std::move(workspace));
  return workspaces_.back().get();
}

std::unique_ptr<Workspace> Profile::RemoveWorkspace(Workspace* workspace) {
  for (auto it = workspaces_.begin(); it != workspaces_.end(); ++it) {
    if (it->get() == workspace) {
      std::unique_ptr<Workspace> out = std::move(*it);
      workspaces_.erase(it);
      return out;
    }
  }
  return nullptr;
}

void Profile::MoveWorkspace(Workspace* workspace, int new_index) {
  auto owned = RemoveWorkspace(workspace);
  if (!owned) return;
  new_index = std::clamp(new_index, 0, static_cast<int>(workspaces_.size()));
  workspaces_.insert(workspaces_.begin() + new_index, std::move(owned));
  if (is_private_) return;
  std::vector<int64_t> ids;
  for (size_t i = 0; i < workspaces_.size(); ++i) {
    workspaces_[i]->mutable_record().position = static_cast<int>(i);
    ids.push_back(workspaces_[i]->id());
  }
  Application::Get()->store().SetWorkspaceOrder(ids);
}

void Profile::LoadFromStore(Store& store) {
  workspaces_.clear();
  for (auto& wr : store.LoadWorkspaces(record_.id)) {
    auto ws = std::make_unique<Workspace>(this, wr);
    for (auto& g : store.LoadTabGroups(wr.id)) ws->AppendLoadedGroup(std::move(g));
    auto tabs = store.LoadTabs(wr.id);
    // Stored order is authoritative, but enforce pinned-first.
    std::stable_sort(tabs.begin(), tabs.end(),
                     [](const TabRecord& a, const TabRecord& b) { return a.pinned > b.pinned; });
    Workspace* raw = AddWorkspace(std::move(ws));
    for (auto& tr : tabs) {
      raw->AppendLoadedTab(std::make_unique<Tab>(raw, tr));
    }
  }
}

bool Profile::HasLiveBrowsers() const {
  for (const auto& ws : workspaces_) {
    for (const auto& t : ws->tabs()) {
      if (t->load_state() != Tab::LoadState::kUnloaded) return true;
    }
  }
  return false;
}

}  // namespace orbit
