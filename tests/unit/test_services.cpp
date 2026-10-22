#include "core/bookmark_html.h"
#include "persistence/database.h"
#include "persistence/schema.h"
#include "persistence/store.h"
#include "test.h"

using namespace orbit;

namespace {

struct Fixture {
  Fixture() : store(db) {
    db.Open({});
    MigrateSchema(db);
    profile = store.CreateProfile(L"u1", L"A", 0, L"")->id;
    other = store.CreateProfile(L"u2", L"B", 0, L"")->id;
  }
  Database db;
  Store store;
  int64_t profile = 0;
  int64_t other = 0;
};

}  // namespace

TEST(BookmarkHtml_RoundTrip) {
  std::vector<BookmarkNode> roots(2);
  roots[0].is_folder = true;
  roots[0].title = L"Work & <Stuff>";
  roots[0].children.push_back({L"Docs \"quoted\"", L"https://example.com/?a=1&b=2", false, {}});
  roots[1] = {L"Bl\x00E5b\x00E6r", L"https://xn--blbr-roa.example/", false, {}};
  const auto parsed = ImportBookmarksHtml(ExportBookmarksHtml(roots));
  EXPECT_EQ(parsed.size(), 2u);
  if (parsed.size() != 2) return;
  EXPECT_TRUE(parsed[0].is_folder);
  EXPECT_WEQ(parsed[0].title, L"Work & <Stuff>");
  EXPECT_EQ(parsed[0].children.size(), 1u);
  if (!parsed[0].children.empty()) {
    EXPECT_WEQ(parsed[0].children[0].title, L"Docs \"quoted\"");
    EXPECT_WEQ(parsed[0].children[0].url, L"https://example.com/?a=1&b=2");
  }
  EXPECT_WEQ(parsed[1].title, L"Bl\x00E5b\x00E6r");
}

TEST(BookmarkHtml_ImportsChromeExportAndDropsScriptUrls) {
  const char* html =
      "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<DL><p>\n"
      "  <DT><H3 ADD_DATE=\"1\" PERSONAL_TOOLBAR_FOLDER=\"true\">Bookmarks bar</H3>\n"
      "  <DL><p>\n"
      "    <DT><A HREF=\"https://a.example/\" ADD_DATE=\"1\" ICON=\"data:x\">A</A>\n"
      "    <DT><A HREF=\"javascript:alert(1)\">Bookmarklet</A>\n"
      "    <DT><H3>Nested</H3>\n    <DL><p>\n"
      "      <DT><a href='http://b.example/'>B &amp; C &#x1F600;</a>\n"
      "    </DL><p>\n"
      "  </DL><p>\n"
      "  <DT><A HREF=\"data:text/html,x\">Data</A>\n"
      "</DL><p>\n";
  const auto roots = ImportBookmarksHtml(html);
  EXPECT_EQ(roots.size(), 1u);
  if (roots.empty()) return;
  EXPECT_WEQ(roots[0].title, L"Bookmarks bar");
  EXPECT_EQ(roots[0].children.size(), 2u);  // A + Nested (bookmarklet dropped)
  if (roots[0].children.size() == 2) {
    EXPECT_WEQ(roots[0].children[0].url, L"https://a.example/");
    const auto& nested = roots[0].children[1];
    EXPECT_TRUE(nested.is_folder);
    EXPECT_EQ(nested.children.size(), 1u);
    if (!nested.children.empty()) {
      EXPECT_WEQ(nested.children[0].title, L"B & C \xD83D\xDE00");
    }
  }
}

TEST(BookmarkHtml_SurvivesMalformedInput) {
  EXPECT_TRUE(ImportBookmarksHtml("").empty());
  EXPECT_TRUE(ImportBookmarksHtml("<DL><DT><A HREF=\"https://x/").empty());
  std::string deep;
  for (int i = 0; i < 1000; ++i) deep += "<DT><H3>f</H3><DL>";
  deep += "<DT><A HREF=\"https://deep.example/\">d</A>";
  const auto roots = ImportBookmarksHtml(deep);  // must not overflow the stack
  EXPECT_FALSE(roots.empty());
}

TEST(History_QueryOrderSearchAndPaging) {
  Fixture f;
  for (int i = 0; i < 5; ++i) {
    f.store.AddVisit(f.profile, L"https://site" + std::to_wstring(i) + L".example/",
                     i == 3 ? L"Special Page" : L"Page", 1000 + i);
  }
  f.store.AddVisit(f.other, L"https://other.example/", L"Special Page", 2000);
  auto all = f.store.QueryHistory(f.profile, L"", 0, 100);
  EXPECT_EQ(all.size(), 5u);
  if (all.size() == 5) EXPECT_EQ(all[0].visit_time, 1004);  // newest first
  auto found = f.store.QueryHistory(f.profile, L"special", 0, 100);
  EXPECT_EQ(found.size(), 1u);  // other profile's match must not leak
  auto page = f.store.QueryHistory(f.profile, L"", 1002, 100);
  EXPECT_EQ(page.size(), 2u);
  auto wildcard = f.store.QueryHistory(f.profile, L"%", 0, 100);
  EXPECT_EQ(wildcard.size(), 0u);  // user input is not a LIKE pattern
}

TEST(History_FrecencyRanksFrequentRecentPages) {
  Fixture f;
  const int64_t day = 86400000;
  const int64_t now = 400 * day;
  // Visited once yesterday vs. five times last week vs. once long ago.
  f.store.AddVisit(f.profile, L"https://once.example/", L"Once", now - day);
  for (int i = 0; i < 5; ++i) f.store.AddVisit(f.profile, L"https://often.example/", L"Often " + std::to_wstring(i), now - 7 * day - i);
  f.store.AddVisit(f.profile, L"https://old.example/", L"Old", now - 200 * day);
  const auto rows = f.store.QueryHistoryFrecent(f.profile, L"example", now, 10);
  EXPECT_EQ(rows.size(), 3u);  // one row per URL
  if (rows.size() == 3) {
    EXPECT_WEQ(rows[0].url, L"https://often.example/");  // 5 x 70 beats 1 x 100
    EXPECT_WEQ(rows[0].title, L"Often 0");               // the latest visit's title
    EXPECT_WEQ(rows[1].url, L"https://once.example/");
    EXPECT_WEQ(rows[2].url, L"https://old.example/");
  }
}

TEST(History_DeleteRangeAndSingle) {
  Fixture f;
  for (int i = 0; i < 10; ++i) f.store.AddVisit(f.profile, L"https://a/" + std::to_wstring(i), L"", 100 * i);
  f.store.AddVisit(f.other, L"https://a/x", L"", 150);
  EXPECT_EQ(f.store.DeleteHistoryRange(f.profile, 100, 400), 3);  // 100,200,300
  EXPECT_EQ(f.store.QueryHistory(f.profile, L"", 0, 100).size(), 7u);
  EXPECT_EQ(f.store.QueryHistory(f.other, L"", 0, 100).size(), 1u);
  const int64_t id = f.store.QueryHistory(f.profile, L"", 0, 1)[0].id;
  EXPECT_FALSE(f.store.DeleteVisit(f.other, id) && f.db.Changes() > 0);  // wrong profile
  EXPECT_TRUE(f.store.DeleteVisit(f.profile, id));
  EXPECT_EQ(f.store.QueryHistory(f.profile, L"", 0, 100).size(), 6u);
  EXPECT_EQ(f.store.DeleteHistoryRange(f.profile, 0, 0), 6);  // clear all
}

TEST(Bookmarks_FoldersMovesAndCycles) {
  Fixture f;
  BookmarkRecord folder;
  folder.profile_id = f.profile;
  folder.is_folder = true;
  folder.title = L"Folder";
  const int64_t a = f.store.AddBookmark(folder);
  folder.title = L"Sub";
  folder.parent_id = a;
  const int64_t b = f.store.AddBookmark(folder);
  BookmarkRecord link;
  link.profile_id = f.profile;
  link.parent_id = b;
  link.title = L"Link";
  link.url = L"https://example.com/";
  const int64_t c = f.store.AddBookmark(link);
  EXPECT_TRUE(a && b && c);
  EXPECT_FALSE(f.store.MoveBookmark(f.profile, a, b));  // folder into its own child
  EXPECT_FALSE(f.store.MoveBookmark(f.profile, a, a));
  EXPECT_FALSE(f.store.MoveBookmark(f.profile, c, c));  // parent must be a folder
  EXPECT_TRUE(f.store.MoveBookmark(f.profile, c, 0));
  link.parent_id = c;  // a link cannot be a parent
  EXPECT_EQ(f.store.AddBookmark(link), 0);
  link.profile_id = f.other;  // cross-profile parent refused
  link.parent_id = a;
  EXPECT_EQ(f.store.AddBookmark(link), 0);
  EXPECT_TRUE(f.store.FindBookmarkByUrl(f.profile, L"https://example.com/").has_value());
  EXPECT_FALSE(f.store.FindBookmarkByUrl(f.other, L"https://example.com/").has_value());
  EXPECT_TRUE(f.store.DeleteBookmark(f.profile, a));  // removes Sub as well
  EXPECT_EQ(f.store.LoadBookmarks(f.profile).size(), 1u);
}

TEST(Downloads_InterruptedOnRestart) {
  Fixture f;
  DownloadRecord d;
  d.profile_id = f.profile;
  d.url = L"https://example.com/file.zip";
  d.started_at = 1;
  const int64_t id = f.store.InsertDownload(d);
  d.state = DownloadState::kComplete;
  d.started_at = 2;
  f.store.InsertDownload(d);
  EXPECT_EQ(f.store.MarkInterruptedDownloads(), 1);
  auto list = f.store.LoadDownloads(f.profile, 10);
  EXPECT_EQ(list.size(), 2u);
  for (const auto& item : list) {
    if (item.id == id) EXPECT_TRUE(item.state == DownloadState::kInterrupted);
  }
}
