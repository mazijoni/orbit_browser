#include <windows.h>

#include <filesystem>

#include "persistence/database.h"
#include "persistence/schema.h"
#include "persistence/store.h"
#include "test.h"

using namespace orbit;

namespace {

struct TempDb {
  TempDb() {
    wchar_t dir[MAX_PATH];
    ::GetTempPathW(MAX_PATH, dir);
    path = std::filesystem::path(dir) /
           (L"orbit_test_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
            std::to_wstring(++counter) + L".db");
    std::filesystem::remove(path);
  }
  ~TempDb() {
    std::error_code ec;
    for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) {
      std::filesystem::remove(path.wstring() + suffix, ec);
    }
  }
  std::filesystem::path path;
  static inline int counter = 0;
};

}  // namespace

TEST(Schema_MigratesFreshDatabase) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_EQ(db.UserVersion(), 0);
  EXPECT_TRUE(MigrateSchema(db));
  EXPECT_EQ(db.UserVersion(), kSchemaVersion);
  EXPECT_TRUE(MigrateSchema(db));  // idempotent
}

TEST(Schema_UpgradesFromV1KeepingData) {
  TempDb tmp;
  {
    Database db;
    EXPECT_TRUE(db.Open(tmp.path));
    // Simulate a v1 database by applying only the first migration.
    EXPECT_TRUE(MigrateSchema(db));
    EXPECT_TRUE(db.Execute("DROP TABLE history; DROP TABLE bookmarks; DROP TABLE downloads;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE closed_tabs DROP COLUMN batch_id;"));  // v3
    EXPECT_TRUE(db.Execute("ALTER TABLE tabs DROP COLUMN pinned_url;"));       // v4
    EXPECT_TRUE(db.Execute("ALTER TABLE tabs DROP COLUMN custom_title;"));     // v5
    EXPECT_TRUE(db.Execute("ALTER TABLE workspaces DROP COLUMN kind;"));       // v6
    EXPECT_TRUE(db.Execute("DROP TABLE split_views;"));                        // v7
    EXPECT_TRUE(db.Execute("ALTER TABLE tab_groups DROP COLUMN parent_id;"));  // v8
    EXPECT_TRUE(db.Execute("ALTER TABLE tab_groups DROP COLUMN icon;"));
    EXPECT_TRUE(db.Execute("DROP TABLE live_folders;"));                       // v9
    EXPECT_TRUE(db.Execute("ALTER TABLE tabs DROP COLUMN live_item;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE tabs DROP COLUMN sublabel;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE workspaces DROP COLUMN theme;"));       // v10
    EXPECT_TRUE(db.Execute("DROP TABLE boosts;"));                               // v11
    EXPECT_TRUE(db.SetUserVersion(1));
    Store store(db);
    EXPECT_TRUE(store.CreateProfile(L"uuid-1", L"Kept", 1, L"").has_value());
  }
  Database db;
  EXPECT_TRUE(db.Open(tmp.path));
  EXPECT_TRUE(MigrateSchema(db));
  EXPECT_EQ(db.UserVersion(), kSchemaVersion);
  Store store(db);
  auto profiles = store.LoadProfiles();
  EXPECT_EQ(profiles.size(), 1u);
  if (!profiles.empty()) EXPECT_WEQ(profiles[0].name, L"Kept");
  EXPECT_TRUE(db.Execute("SELECT count(*) FROM history"));
}

TEST(Schema_UpgradesFromV2KeepingClosedTabs) {
  TempDb tmp;
  {
    Database db;
    EXPECT_TRUE(db.Open(tmp.path));
    EXPECT_TRUE(MigrateSchema(db));
    EXPECT_TRUE(db.Execute("ALTER TABLE closed_tabs DROP COLUMN batch_id;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE tabs DROP COLUMN pinned_url;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE tabs DROP COLUMN custom_title;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE workspaces DROP COLUMN kind;"));
    EXPECT_TRUE(db.Execute("DROP TABLE split_views;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE tab_groups DROP COLUMN parent_id;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE tab_groups DROP COLUMN icon;"));
    EXPECT_TRUE(db.Execute("DROP TABLE live_folders;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE tabs DROP COLUMN live_item;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE tabs DROP COLUMN sublabel;"));
    EXPECT_TRUE(db.Execute("ALTER TABLE workspaces DROP COLUMN theme;"));
    EXPECT_TRUE(db.Execute("DROP TABLE boosts;"));
    EXPECT_TRUE(db.SetUserVersion(2));
    EXPECT_TRUE(db.Execute(
        "INSERT INTO profiles (uuid, name, color, position, created_at) VALUES ('u', 'P', 0, 0, 0);"
        "INSERT INTO closed_tabs (profile_id, url, closed_at) VALUES (1, 'https://old.example/', 1);"));
  }
  Database db;
  EXPECT_TRUE(db.Open(tmp.path));
  EXPECT_TRUE(MigrateSchema(db));
  EXPECT_EQ(db.UserVersion(), kSchemaVersion);
  Store store(db);
  auto batch = store.PopClosedTabBatch(1);
  EXPECT_EQ(batch.size(), size_t{1});  // pre-v3 rows are single closes
  if (!batch.empty()) EXPECT_WEQ(batch[0].url, L"https://old.example/");
}

TEST(Store_PinnedUrlRoundTrips) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  auto ws = store.CreateWorkspace(p->id, L"W", 0, L"");
  TabRecord t;
  t.workspace_id = ws->id;
  t.url = L"https://example.com/elsewhere";
  t.pinned = true;
  t.pinned_url = L"https://example.com/";
  t.id = store.InsertTab(t);
  auto loaded = store.LoadTabs(ws->id);
  EXPECT_EQ(loaded.size(), size_t{1});
  if (!loaded.empty()) EXPECT_WEQ(loaded[0].pinned_url, L"https://example.com/");
  t.pinned_url = L"https://example.com/new";
  t.custom_title = L"My label";
  EXPECT_TRUE(store.UpdateTab(t));
  loaded = store.LoadTabs(ws->id);
  if (!loaded.empty()) {
    EXPECT_WEQ(loaded[0].pinned_url, L"https://example.com/new");
    EXPECT_WEQ(loaded[0].custom_title, L"My label");
  }
}

TEST(Store_EssentialsContainerKindRoundTrips) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  auto space = store.CreateWorkspace(p->id, L"Home", 0, L"");
  auto ess = store.CreateWorkspace(p->id, L"Essentials", 0, L"", WorkspaceRecord::kKindEssentials);
  EXPECT_TRUE(space.has_value() && ess.has_value());
  int spaces = 0, essentials = 0;
  for (const auto& w : store.LoadWorkspaces(p->id)) {
    spaces += w.kind == WorkspaceRecord::kKindSpace;
    essentials += w.kind == WorkspaceRecord::kKindEssentials;
  }
  EXPECT_EQ(spaces, 1);
  EXPECT_EQ(essentials, 1);
}

TEST(Store_SplitViewsRoundTripAndCascade) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  auto ws = store.CreateWorkspace(p->id, L"Home", 0, L"");
  SplitViewRecord r;
  r.workspace_id = ws->id;
  r.layout = L"r(50.00:1,50.00:2)";
  r.id = store.InsertSplitView(r);
  EXPECT_TRUE(r.id > 0);
  r.layout = L"r(30.00:1,70.00:2)";
  EXPECT_TRUE(store.UpdateSplitView(r));
  auto loaded = store.LoadSplitViews(ws->id);
  EXPECT_TRUE(loaded.size() == 1 && loaded[0].layout == L"r(30.00:1,70.00:2)");
  EXPECT_TRUE(db.Execute("PRAGMA foreign_keys = ON; DELETE FROM workspaces;"));
  EXPECT_TRUE(store.LoadSplitViews(ws->id).empty());  // removed with its space
}

TEST(Store_NestedFolderRoundTrips) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  auto ws = store.CreateWorkspace(p->id, L"Home", 0, L"");
  TabGroupRecord parent;
  parent.workspace_id = ws->id;
  parent.name = L"Work";
  parent.id = store.InsertTabGroup(parent);
  TabGroupRecord child = parent;
  child.name = L"Sub";
  child.parent_id = parent.id;
  child.icon = L"\U0001F4C1";
  child.id = store.InsertTabGroup(child);
  child.icon = L"\u2B50";
  EXPECT_TRUE(store.UpdateTabGroup(child));
  bool found = false;
  for (const auto& g : store.LoadTabGroups(ws->id)) {
    if (g.id == child.id) found = g.parent_id == parent.id && g.icon == L"\u2B50" && g.name == L"Sub";
  }
  EXPECT_TRUE(found);
}

TEST(Store_LiveFolderRoundTrips) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  auto ws = store.CreateWorkspace(p->id, L"Home", 0, L"");
  TabGroupRecord g;
  g.workspace_id = ws->id;
  g.name = L"Feed";
  g.id = store.InsertTabGroup(g);
  EXPECT_TRUE(store.SaveLiveFolder({g.id, L"kind=rss\n", L"1:a\n1:b"}));
  EXPECT_TRUE(store.SaveLiveFolder({g.id, L"kind=rss\nurl=x\n", L""}));  // replaces
  auto all = store.LoadLiveFolders();
  EXPECT_TRUE(all.size() == 1 && all[0].state == L"kind=rss\nurl=x\n" && all[0].dismissed.empty());
  TabRecord t;
  t.workspace_id = ws->id;
  t.url = L"https://e/1";
  t.group_id = g.id;
  t.live_item = L"1:a";
  t.sublabel = L"alice";
  t.id = store.InsertTab(t);
  auto tabs = store.LoadTabs(ws->id);
  EXPECT_TRUE(tabs.size() == 1 && tabs[0].live_item == L"1:a" && tabs[0].sublabel == L"alice");
  t.sublabel = L"bob";
  EXPECT_TRUE(store.UpdateTab(t));
  EXPECT_WEQ(store.LoadTabs(ws->id)[0].sublabel, L"bob");
  // Deleting the folder deletes its live state.
  EXPECT_TRUE(store.DeleteTab(t.id));
  EXPECT_TRUE(store.DeleteTabGroup(g.id));
  EXPECT_TRUE(store.LoadLiveFolders().empty());
}

TEST(Store_WorkspaceThemeRoundTrips) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  auto ws = store.CreateWorkspace(p->id, L"Home", 0x112233, L"");
  EXPECT_TRUE(ws.has_value() && ws->theme.empty());
  ws->theme = L"0.500 0.0000 50 analogous|1,2,3,4.0,5.0,,1";
  EXPECT_TRUE(store.UpdateWorkspace(*ws));
  EXPECT_WEQ(store.LoadWorkspaces(p->id)[0].theme, L"0.500 0.0000 50 analogous|1,2,3,4.0,5.0,,1");
}

TEST(Store_BoostsPerProfileAndSite) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto a = store.CreateProfile(L"a", L"A", 0, L"");
  auto b = store.CreateProfile(L"b", L"B", 0, L"");
  EXPECT_TRUE(store.SaveBoost(a->id, L"example.com", L"name=One"));
  EXPECT_TRUE(store.SaveBoost(a->id, L"example.com", L"name=Two"));  // replaces
  EXPECT_WEQ(store.LoadBoost(a->id, L"example.com"), L"name=Two");
  EXPECT_WEQ(store.LoadBoost(b->id, L"example.com"), L"");  // per profile
  EXPECT_TRUE(store.BoostedDomains(a->id).size() == 1);
  EXPECT_TRUE(store.DeleteBoost(a->id, L"example.com"));
  EXPECT_WEQ(store.LoadBoost(a->id, L"example.com"), L"");
}

TEST(Store_DeletePref) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  EXPECT_TRUE(store.SetPref(Store::kGlobalScope, L"shortcut:new-tab", L"Ctrl+B"));
  EXPECT_TRUE(store.DeletePref(Store::kGlobalScope, L"shortcut:new-tab"));
  EXPECT_FALSE(store.GetPref(Store::kGlobalScope, L"shortcut:new-tab").has_value());
}

TEST(Schema_RefusesNewerDatabase) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(db.SetUserVersion(kSchemaVersion + 1));
  EXPECT_FALSE(MigrateSchema(db));
}

TEST(Store_TabOrderPersistsAcrossReopen) {
  TempDb tmp;
  std::vector<int64_t> ids;
  int64_t ws_id = 0;
  {
    Database db;
    EXPECT_TRUE(db.Open(tmp.path));
    EXPECT_TRUE(MigrateSchema(db));
    Store store(db);
    auto profile = store.CreateProfile(L"u", L"P", 0, L"");
    auto ws = store.CreateWorkspace(profile->id, L"W", 0, L"");
    ws_id = ws->id;
    for (int i = 0; i < 4; ++i) {
      TabRecord t;
      t.workspace_id = ws_id;
      t.url = L"https://example.com/" + std::to_wstring(i);
      t.position = i;
      ids.push_back(store.InsertTab(t));
    }
    // Reverse the order.
    EXPECT_TRUE(store.SetTabOrder({ids[3], ids[2], ids[1], ids[0]}));
  }
  Database db;
  EXPECT_TRUE(db.Open(tmp.path));
  Store store(db);
  auto tabs = store.LoadTabs(ws_id);
  EXPECT_EQ(tabs.size(), 4u);
  if (tabs.size() == 4) {
    EXPECT_EQ(tabs[0].id, ids[3]);
    EXPECT_EQ(tabs[3].id, ids[0]);
    EXPECT_WEQ(tabs[0].url, L"https://example.com/3");
  }
}

TEST(Store_DeletingProfileCascades) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  auto ws = store.CreateWorkspace(p->id, L"W", 0, L"");
  TabRecord t;
  t.workspace_id = ws->id;
  t.url = L"https://example.com";
  EXPECT_TRUE(store.InsertTab(t) != 0);
  EXPECT_TRUE(store.SetPref(p->id, L"k", L"v"));
  EXPECT_TRUE(store.DeleteProfile(p->id));
  EXPECT_TRUE(store.LoadWorkspaces(p->id).empty());
  EXPECT_TRUE(store.LoadTabs(ws->id).empty());
  EXPECT_FALSE(store.GetPref(p->id, L"k").has_value());
}

TEST(Store_ClosedTabsAreBoundedAndLifo) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  for (int i = 0; i < Store::kMaxClosedTabsPerProfile + 5; ++i) {
    ClosedTabRecord c;
    c.profile_id = p->id;
    c.url = L"https://example.com/" + std::to_wstring(i);
    EXPECT_TRUE(store.PushClosedTab(c));
  }
  auto last = store.PopClosedTab(p->id);
  EXPECT_TRUE(last.has_value());
  if (last) {
    EXPECT_WEQ(last->url,
               L"https://example.com/" + std::to_wstring(Store::kMaxClosedTabsPerProfile + 4));
  }
  int remaining = 0;
  while (store.PopClosedTab(p->id)) ++remaining;
  EXPECT_EQ(remaining, Store::kMaxClosedTabsPerProfile - 1);
}

TEST(Store_ClosedTabBatchReopensTogether) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  auto push = [&](const wchar_t* url, int64_t batch) {
    ClosedTabRecord c;
    c.profile_id = p->id;
    c.url = url;
    c.batch_id = batch;
    EXPECT_TRUE(store.PushClosedTab(c));
  };
  push(L"https://single.example/", 0);
  push(L"https://a.example/", 77);
  push(L"https://b.example/", 77);
  push(L"https://c.example/", 77);
  auto batch = store.PopClosedTabBatch(p->id);
  EXPECT_EQ(batch.size(), size_t{3});
  if (batch.size() == 3) {
    EXPECT_WEQ(batch[0].url, L"https://a.example/");  // closing order
    EXPECT_WEQ(batch[2].url, L"https://c.example/");
  }
  auto single = store.PopClosedTabBatch(p->id);
  EXPECT_EQ(single.size(), size_t{1});
  if (!single.empty()) EXPECT_WEQ(single[0].url, L"https://single.example/");
  EXPECT_TRUE(store.PopClosedTabBatch(p->id).empty());
}

TEST(Store_TransactionRollsBackOnDestruction) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  {
    Database::Transaction txn(db);
    EXPECT_TRUE(store.SetPref(0, L"theme", L"dark"));
    // no Commit()
  }
  EXPECT_FALSE(store.GetPref(0, L"theme").has_value());
}

TEST(Store_WindowsReplaceAtomically) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  WindowRecord w;
  w.profile_id = p->id;
  w.x = 10;
  w.w = 800;
  w.h = 600;
  EXPECT_TRUE(store.SaveWindows({w, w}));
  EXPECT_EQ(store.LoadWindows().size(), 2u);
  EXPECT_TRUE(store.SaveWindows({w}));
  auto loaded = store.LoadWindows();
  EXPECT_EQ(loaded.size(), 1u);
  if (!loaded.empty()) EXPECT_EQ(loaded[0].w, 800);
}
