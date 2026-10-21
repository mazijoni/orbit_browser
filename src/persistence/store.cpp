#include "persistence/store.h"

#include <chrono>

namespace orbit {

int64_t NowUnixMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

TabRecord ReadTab(const Database::Statement& s) {
  TabRecord t;
  t.id = s.ColumnInt64(0);
  t.workspace_id = s.ColumnInt64(1);
  t.url = s.ColumnText(2);
  t.title = s.ColumnText(3);
  t.favicon_url = s.ColumnText(4);
  t.pinned = s.ColumnBool(5);
  t.is_protected = s.ColumnBool(6);
  t.group_id = s.ColumnIsNull(7) ? 0 : s.ColumnInt64(7);
  t.position = s.ColumnInt(8);
  t.last_active = s.ColumnInt64(9);
  return t;
}

void BindGroup(Database::Statement& s, int index, int64_t group_id) {
  if (group_id) {
    s.Bind(index, group_id);
  } else {
    s.BindNull(index);
  }
}

}  // namespace

std::vector<ProfileRecord> Store::LoadProfiles() {
  std::vector<ProfileRecord> out;
  auto s = db_.Prepare(
      "SELECT id, uuid, name, color, icon, position FROM profiles ORDER BY position, id");
  while (s.Step()) {
    ProfileRecord p;
    p.id = s.ColumnInt64(0);
    p.uuid = s.ColumnText(1);
    p.name = s.ColumnText(2);
    p.color = static_cast<uint32_t>(s.ColumnInt64(3));
    p.icon = s.ColumnText(4);
    p.position = s.ColumnInt(5);
    out.push_back(std::move(p));
  }
  return out;
}

std::optional<ProfileRecord> Store::CreateProfile(const std::wstring& uuid,
                                                  const std::wstring& name, uint32_t color,
                                                  const std::wstring& icon) {
  auto s = db_.Prepare(
      "INSERT INTO profiles (uuid, name, color, icon, position, created_at) VALUES "
      "(?, ?, ?, ?, (SELECT COALESCE(MAX(position) + 1, 0) FROM profiles), ?)");
  s.Bind(1, uuid).Bind(2, name).Bind(3, static_cast<int64_t>(color)).Bind(4, icon);
  s.Bind(5, NowUnixMillis());
  if (!s.Run()) return std::nullopt;
  const int64_t id = db_.LastInsertId();
  for (auto& p : LoadProfiles()) {
    if (p.id == id) return p;
  }
  return std::nullopt;
}

bool Store::UpdateProfile(const ProfileRecord& p) {
  auto s = db_.Prepare("UPDATE profiles SET name = ?, color = ?, icon = ?, position = ? "
                       "WHERE id = ?");
  s.Bind(1, p.name).Bind(2, static_cast<int64_t>(p.color)).Bind(3, p.icon);
  s.Bind(4, p.position).Bind(5, p.id);
  return s.Run();
}

bool Store::DeleteProfile(int64_t id) {
  Database::Transaction txn(db_);
  auto prefs = db_.Prepare("DELETE FROM prefs WHERE scope = ?");
  prefs.Bind(1, id);
  auto s = db_.Prepare("DELETE FROM profiles WHERE id = ?");
  s.Bind(1, id);
  return prefs.Run() && s.Run() && txn.Commit();
}

std::vector<WorkspaceRecord> Store::LoadWorkspaces(int64_t profile_id) {
  std::vector<WorkspaceRecord> out;
  auto s = db_.Prepare(
      "SELECT id, profile_id, name, color, icon, position FROM workspaces "
      "WHERE profile_id = ? ORDER BY position, id");
  s.Bind(1, profile_id);
  while (s.Step()) {
    WorkspaceRecord w;
    w.id = s.ColumnInt64(0);
    w.profile_id = s.ColumnInt64(1);
    w.name = s.ColumnText(2);
    w.color = static_cast<uint32_t>(s.ColumnInt64(3));
    w.icon = s.ColumnText(4);
    w.position = s.ColumnInt(5);
    out.push_back(std::move(w));
  }
  return out;
}

std::optional<WorkspaceRecord> Store::CreateWorkspace(int64_t profile_id,
                                                      const std::wstring& name, uint32_t color,
                                                      const std::wstring& icon) {
  auto s = db_.Prepare(
      "INSERT INTO workspaces (profile_id, name, color, icon, position) VALUES (?, ?, ?, ?, "
      "(SELECT COALESCE(MAX(position) + 1, 0) FROM workspaces WHERE profile_id = ?))");
  s.Bind(1, profile_id).Bind(2, name).Bind(3, static_cast<int64_t>(color)).Bind(4, icon);
  s.Bind(5, profile_id);
  if (!s.Run()) return std::nullopt;
  const int64_t id = db_.LastInsertId();
  for (auto& w : LoadWorkspaces(profile_id)) {
    if (w.id == id) return w;
  }
  return std::nullopt;
}

bool Store::UpdateWorkspace(const WorkspaceRecord& w) {
  auto s = db_.Prepare(
      "UPDATE workspaces SET name = ?, color = ?, icon = ?, position = ? WHERE id = ?");
  s.Bind(1, w.name).Bind(2, static_cast<int64_t>(w.color)).Bind(3, w.icon);
  s.Bind(4, w.position).Bind(5, w.id);
  return s.Run();
}

bool Store::DeleteWorkspace(int64_t id) {
  auto s = db_.Prepare("DELETE FROM workspaces WHERE id = ?");
  s.Bind(1, id);
  return s.Run();
}

bool Store::SetWorkspaceOrder(const std::vector<int64_t>& ids) {
  Database::Transaction txn(db_);
  auto s = db_.Prepare("UPDATE workspaces SET position = ? WHERE id = ?");
  for (size_t i = 0; i < ids.size(); ++i) {
    s.Reset();
    s.Bind(1, static_cast<int64_t>(i)).Bind(2, ids[i]);
    if (!s.Run()) return false;
  }
  return txn.Commit();
}

std::vector<TabRecord> Store::LoadTabs(int64_t workspace_id) {
  std::vector<TabRecord> out;
  auto s = db_.Prepare(
      "SELECT id, workspace_id, url, title, favicon_url, pinned, protected, group_id, "
      "position, last_active FROM tabs WHERE workspace_id = ? ORDER BY position, id");
  s.Bind(1, workspace_id);
  while (s.Step()) out.push_back(ReadTab(s));
  return out;
}

int64_t Store::InsertTab(const TabRecord& t) {
  auto s = db_.Prepare(
      "INSERT INTO tabs (workspace_id, url, title, favicon_url, pinned, protected, group_id, "
      "position, last_active) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
  s.Bind(1, t.workspace_id).Bind(2, t.url).Bind(3, t.title).Bind(4, t.favicon_url);
  s.Bind(5, t.pinned).Bind(6, t.is_protected);
  BindGroup(s, 7, t.group_id);
  s.Bind(8, t.position).Bind(9, t.last_active);
  return s.Run() ? db_.LastInsertId() : 0;
}

bool Store::UpdateTab(const TabRecord& t) {
  auto s = db_.Prepare(
      "UPDATE tabs SET workspace_id = ?, url = ?, title = ?, favicon_url = ?, pinned = ?, "
      "protected = ?, group_id = ?, position = ?, last_active = ? WHERE id = ?");
  s.Bind(1, t.workspace_id).Bind(2, t.url).Bind(3, t.title).Bind(4, t.favicon_url);
  s.Bind(5, t.pinned).Bind(6, t.is_protected);
  BindGroup(s, 7, t.group_id);
  s.Bind(8, t.position).Bind(9, t.last_active).Bind(10, t.id);
  return s.Run();
}

bool Store::DeleteTab(int64_t id) {
  auto s = db_.Prepare("DELETE FROM tabs WHERE id = ?");
  s.Bind(1, id);
  return s.Run();
}

bool Store::SetTabOrder(const std::vector<int64_t>& ids) {
  Database::Transaction txn(db_);
  auto s = db_.Prepare("UPDATE tabs SET position = ? WHERE id = ?");
  for (size_t i = 0; i < ids.size(); ++i) {
    s.Reset();
    s.Bind(1, static_cast<int64_t>(i)).Bind(2, ids[i]);
    if (!s.Run()) return false;
  }
  return txn.Commit();
}

std::vector<TabGroupRecord> Store::LoadTabGroups(int64_t workspace_id) {
  std::vector<TabGroupRecord> out;
  auto s = db_.Prepare(
      "SELECT id, workspace_id, name, color, collapsed FROM tab_groups WHERE workspace_id = ? "
      "ORDER BY id");
  s.Bind(1, workspace_id);
  while (s.Step()) {
    TabGroupRecord g;
    g.id = s.ColumnInt64(0);
    g.workspace_id = s.ColumnInt64(1);
    g.name = s.ColumnText(2);
    g.color = static_cast<uint32_t>(s.ColumnInt64(3));
    g.collapsed = s.ColumnBool(4);
    out.push_back(std::move(g));
  }
  return out;
}

int64_t Store::InsertTabGroup(const TabGroupRecord& g) {
  auto s = db_.Prepare(
      "INSERT INTO tab_groups (workspace_id, name, color, collapsed) VALUES (?, ?, ?, ?)");
  s.Bind(1, g.workspace_id).Bind(2, g.name).Bind(3, static_cast<int64_t>(g.color));
  s.Bind(4, g.collapsed);
  return s.Run() ? db_.LastInsertId() : 0;
}

bool Store::UpdateTabGroup(const TabGroupRecord& g) {
  auto s = db_.Prepare(
      "UPDATE tab_groups SET workspace_id = ?, name = ?, color = ?, collapsed = ? WHERE id = ?");
  s.Bind(1, g.workspace_id).Bind(2, g.name).Bind(3, static_cast<int64_t>(g.color));
  s.Bind(4, g.collapsed).Bind(5, g.id);
  return s.Run();
}

bool Store::DeleteTabGroup(int64_t id) {
  // Member tabs keep their rows; group_id is cleared by ON DELETE SET NULL.
  auto s = db_.Prepare("DELETE FROM tab_groups WHERE id = ?");
  s.Bind(1, id);
  return s.Run();
}

bool Store::PushClosedTab(const ClosedTabRecord& t) {
  Database::Transaction txn(db_);
  auto s = db_.Prepare(
      "INSERT INTO closed_tabs (profile_id, workspace_id, url, title, closed_at) "
      "VALUES (?, ?, ?, ?, ?)");
  s.Bind(1, t.profile_id).Bind(2, t.workspace_id).Bind(3, t.url).Bind(4, t.title);
  s.Bind(5, NowUnixMillis());
  auto trim = db_.Prepare(
      "DELETE FROM closed_tabs WHERE profile_id = ?1 AND id NOT IN (SELECT id FROM "
      "closed_tabs WHERE profile_id = ?1 ORDER BY closed_at DESC, id DESC LIMIT ?2)");
  trim.Bind(1, t.profile_id).Bind(2, kMaxClosedTabsPerProfile);
  return s.Run() && trim.Run() && txn.Commit();
}

std::optional<ClosedTabRecord> Store::PopClosedTab(int64_t profile_id) {
  auto s = db_.Prepare(
      "SELECT id, profile_id, COALESCE(workspace_id, 0), url, title FROM closed_tabs "
      "WHERE profile_id = ? ORDER BY closed_at DESC, id DESC LIMIT 1");
  s.Bind(1, profile_id);
  if (!s.Step()) return std::nullopt;
  ClosedTabRecord t;
  t.id = s.ColumnInt64(0);
  t.profile_id = s.ColumnInt64(1);
  t.workspace_id = s.ColumnInt64(2);
  t.url = s.ColumnText(3);
  t.title = s.ColumnText(4);
  auto del = db_.Prepare("DELETE FROM closed_tabs WHERE id = ?");
  del.Bind(1, t.id);
  if (!del.Run()) return std::nullopt;
  return t;
}

std::vector<WindowRecord> Store::LoadWindows() {
  std::vector<WindowRecord> out;
  auto s = db_.Prepare(
      "SELECT id, profile_id, COALESCE(workspace_id, 0), COALESCE(active_tab_id, 0), x, y, "
      "w, h, maximized FROM windows ORDER BY position, id");
  while (s.Step()) {
    WindowRecord w;
    w.id = s.ColumnInt64(0);
    w.profile_id = s.ColumnInt64(1);
    w.workspace_id = s.ColumnInt64(2);
    w.active_tab_id = s.ColumnInt64(3);
    w.x = s.ColumnInt(4);
    w.y = s.ColumnInt(5);
    w.w = s.ColumnInt(6);
    w.h = s.ColumnInt(7);
    w.maximized = s.ColumnBool(8);
    out.push_back(w);
  }
  return out;
}

bool Store::SaveWindows(const std::vector<WindowRecord>& windows) {
  Database::Transaction txn(db_);
  if (!db_.Execute("DELETE FROM windows")) return false;
  auto s = db_.Prepare(
      "INSERT INTO windows (profile_id, workspace_id, active_tab_id, x, y, w, h, maximized, "
      "position) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
  for (size_t i = 0; i < windows.size(); ++i) {
    const auto& w = windows[i];
    s.Reset();
    s.Bind(1, w.profile_id);
    if (w.workspace_id) {
      s.Bind(2, w.workspace_id);
    } else {
      s.BindNull(2);
    }
    s.Bind(3, w.active_tab_id).Bind(4, w.x).Bind(5, w.y).Bind(6, w.w).Bind(7, w.h);
    s.Bind(8, w.maximized).Bind(9, static_cast<int64_t>(i));
    if (!s.Run()) return false;
  }
  return txn.Commit();
}

std::optional<std::wstring> Store::GetPref(int64_t scope, const std::wstring& key) {
  auto s = db_.Prepare("SELECT value FROM prefs WHERE scope = ? AND key = ?");
  s.Bind(1, scope).Bind(2, key);
  if (!s.Step()) return std::nullopt;
  return s.ColumnText(0);
}

bool Store::SetPref(int64_t scope, const std::wstring& key, const std::wstring& value) {
  auto s = db_.Prepare(
      "INSERT INTO prefs (scope, key, value) VALUES (?, ?, ?) "
      "ON CONFLICT(scope, key) DO UPDATE SET value = excluded.value");
  s.Bind(1, scope).Bind(2, key).Bind(3, value);
  return s.Run();
}

}  // namespace orbit
