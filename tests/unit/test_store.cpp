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
