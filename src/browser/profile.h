#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "include/cef_request_context.h"
#include "include/cef_request_context_handler.h"
#include "persistence/store.h"

namespace orbit {

class BrowserWindow;
class Profile;
class Tab;

// Organisational container for tabs. Not a security boundary: all workspaces
// of a profile share that profile's request context. A workspace is shown in
// at most one window at a time (window()).
class Workspace {
 public:
  Workspace(Profile* profile, WorkspaceRecord record);
  ~Workspace();
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

  Profile* profile() const { return profile_; }
  int64_t id() const { return record_.id; }
  const WorkspaceRecord& record() const { return record_; }
  WorkspaceRecord& mutable_record() { return record_; }

  BrowserWindow* window() const { return window_; }
  void set_window(BrowserWindow* window) { window_ = window; }

  // Sidebar order. Pinned tabs always precede regular tabs.
  const std::vector<std::unique_ptr<Tab>>& tabs() const { return tabs_; }
  Tab* active_tab() const { return active_tab_; }
  void set_active_tab(Tab* tab) { active_tab_ = tab; }
  int IndexOf(const Tab* tab) const;
  int PinnedCount() const;

  // Inserts |tab| at |index| (clamped to the pinned/regular section) and
  // persists the order. Returns the raw pointer.
  Tab* InsertTab(std::unique_ptr<Tab> tab, int index);
  std::unique_ptr<Tab> RemoveTab(Tab* tab);
  void MoveTab(Tab* tab, int new_index);
  // Appends a tab whose row and position already exist in the store.
  void AppendLoadedTab(std::unique_ptr<Tab> tab);
  void PersistOrder();

  // Tab groups. Invariant: members of a group are contiguous and never
  // pinned. Every mutation re-establishes the invariant and persists.
  const std::vector<TabGroupRecord>& groups() const { return groups_; }
  const TabGroupRecord* FindGroup(int64_t id) const;
  // Creates a group containing |tabs| (unpinned ones). Returns its id.
  int64_t CreateGroup(const std::wstring& name, uint32_t color, const std::vector<Tab*>& tabs);
  // Moves |tab| into |group_id| (0 = no group). Pinned tabs cannot join.
  void SetTabGroup(Tab* tab, int64_t group_id);
  void UpdateGroup(const TabGroupRecord& group);  // name / color / collapsed
  void Ungroup(int64_t group_id);                 // keeps the tabs
  int GroupSize(int64_t group_id) const;
  // Removes groups without members (after tab closes or moves).
  void PruneEmptyGroups();
  void AppendLoadedGroup(TabGroupRecord group) { groups_.push_back(std::move(group)); }

 private:
  // Stable reorder so every group's members follow its first member.
  void NormalizeGroups();
  Profile* profile_;
  WorkspaceRecord record_;
  BrowserWindow* window_ = nullptr;
  std::vector<std::unique_ptr<Tab>> tabs_;
  std::vector<TabGroupRecord> groups_;
  Tab* active_tab_ = nullptr;
  int64_t next_session_group_id_ = -1;  // private profiles: not stored
};

// A browsing identity with its own Chromium storage (cookies, local storage,
// IndexedDB, cache, permissions). Private profiles have no on-disk storage
// and no database rows (record().id == 0 is never used for them).
class Profile {
 public:
  Profile(ProfileRecord record, std::filesystem::path data_dir, bool is_private);
  ~Profile();
  Profile(const Profile&) = delete;
  Profile& operator=(const Profile&) = delete;

  int64_t id() const { return record_.id; }
  bool is_private() const { return is_private_; }
  const ProfileRecord& record() const { return record_; }
  ProfileRecord& mutable_record() { return record_; }
  const std::filesystem::path& data_dir() const { return data_dir_; }

  // Created on first use (must be after CEF context initialization).
  CefRefPtr<CefRequestContext> request_context();
  bool has_request_context() const { return context_ != nullptr; }
  void ReleaseRequestContext() { context_ = nullptr; }

  const std::vector<std::unique_ptr<Workspace>>& workspaces() const { return workspaces_; }
  Workspace* FindWorkspace(int64_t id) const;
  Workspace* AddWorkspace(std::unique_ptr<Workspace> workspace);
  std::unique_ptr<Workspace> RemoveWorkspace(Workspace* workspace);
  void MoveWorkspace(Workspace* workspace, int new_index);

  // Loads workspaces and their tabs (as unloaded tabs) from the store.
  void LoadFromStore(Store& store);

  // True if any tab of this profile still has a live/creating/closing browser.
  bool HasLiveBrowsers() const;

 private:
  ProfileRecord record_;
  std::filesystem::path data_dir_;
  bool is_private_;
  CefRefPtr<CefRequestContext> context_;
  std::vector<std::unique_ptr<Workspace>> workspaces_;
};

}  // namespace orbit
