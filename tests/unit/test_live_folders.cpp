#include "core/live_folders.h"

#include "test.h"

using namespace orbit;

TEST(LiveFolders_ParsesRss) {
  const char* rss =
      "<?xml version=\"1.0\"?>\n<!-- a comment -->\n"
      "<rss version=\"2.0\" xmlns:atom=\"http://www.w3.org/2005/Atom\"><channel>"
      "<title>Orbit &amp; Friends</title><atom:link href=\"https://x/feed\" rel=\"self\"/>"
      "<link>https://example.com/</link>"
      "<item><title><![CDATA[First <b>post</b>]]></title><link>https://example.com/1</link>"
      "<guid>g1</guid><pubDate>Tue, 20 Oct 2026 10:00:00 GMT</pubDate></item>"
      "<item><title>Caf&#233; &#x1F600;</title><link>https://example.com/2</link>"
      "<pubDate>Mon, 19 Oct 2026 08:30:00 +0200</pubDate></item>"
      "</channel></rss>";
  const auto feed = ParseFeed(rss);
  EXPECT_TRUE(feed.has_value());
  if (!feed) return;
  EXPECT_WEQ(feed->title, L"Orbit & Friends");
  EXPECT_WEQ(feed->link, L"https://example.com/");
  EXPECT_EQ(feed->items.size(), size_t{2});
  if (feed->items.size() != 2) return;
  EXPECT_WEQ(feed->items[0].title, L"First <b>post</b>");
  EXPECT_WEQ(feed->items[0].id, L"g1");
  EXPECT_WEQ(feed->items[1].id, L"https://example.com/2");  // no guid: the link
  EXPECT_WEQ(feed->items[1].title, L"Café \U0001F600");
  EXPECT_EQ(feed->items[0].date_ms, int64_t{1792490400000});
  EXPECT_EQ(feed->items[1].date_ms, int64_t{1792391400000});  // 06:30 UTC
}

TEST(LiveFolders_ParsesAtomAndRejectsNonFeeds) {
  const char* atom =
      "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Atom feed</title>"
      "<link rel=\"self\" href=\"https://a/feed\"/><link rel=\"alternate\" href=\"https://a/\"/>"
      "<entry><title>E1</title><link href=\"https://a/e1\"/><id>urn:e1</id>"
      "<updated>2026-10-20T10:00:00Z</updated></entry>"
      "<entry><title>E2</title><link href=\"https://a/e2\"/><id>urn:e2</id>"
      "<updated>2026-10-20T12:00:00.5+02:00</updated></entry></feed>";
  const auto feed = ParseFeed(atom);
  EXPECT_TRUE(feed.has_value());
  if (!feed) return;
  EXPECT_WEQ(feed->title, L"Atom feed");
  EXPECT_WEQ(feed->link, L"https://a/");
  EXPECT_EQ(feed->items.size(), size_t{2});
  if (feed->items.size() == 2) {
    EXPECT_WEQ(feed->items[0].url, L"https://a/e1");
    EXPECT_WEQ(feed->items[1].id, L"urn:e2");
    EXPECT_EQ(feed->items[0].date_ms, feed->items[1].date_ms);
  }
  EXPECT_FALSE(ParseFeed("<!doctype html><html><body>error</body></html>").has_value());
  EXPECT_FALSE(ParseFeed("").has_value());
  EXPECT_FALSE(ParseFeed("not xml at all").has_value());
  // Truncated documents still yield what was read.
  const auto cut = ParseFeed("<rss><channel><item><title>T</title><link>https://t/</link>");
  EXPECT_TRUE(cut.has_value() && cut->items.size() == 1);
}

TEST(LiveFolders_Dates) {
  EXPECT_EQ(ParseFeedDate(L"Thu, 01 Jan 1970 00:00:01 GMT"), int64_t{1000});
  EXPECT_EQ(ParseFeedDate(L"1 Jan 70 00:01 UT"), int64_t{60000});
  EXPECT_EQ(ParseFeedDate(L"Thu, 01 Jan 1970 00:00:00 EST"), int64_t{5 * 3600 * 1000});
  EXPECT_EQ(ParseFeedDate(L"1970-01-02"), int64_t{86400000});
  EXPECT_EQ(ParseFeedDate(L"1970-01-01T01:00:00+01:00"), int64_t{0});
  EXPECT_EQ(ParseFeedDate(L"yesterday"), int64_t{0});
  EXPECT_EQ(ParseFeedDate(L"Tue, 32 Oct 2026 10:00:00 GMT"), int64_t{0});
}

TEST(LiveFolders_FilterItems) {
  std::vector<LiveItem> items(5);
  for (int i = 0; i < 5; ++i) {
    items[i].id = std::to_wstring(i);
    items[i].url = L"https://e/" + std::to_wstring(i);
    items[i].has_date = true;
    items[i].date_ms = 1000000 - i * 1000;
  }
  items[1].url = L"javascript:alert(1)";
  items[2].has_date = false;
  auto out = FilterFeedItems(items, 1000000, 0, 10);
  EXPECT_EQ(out.size(), size_t{3});  // 0, 3, 4
  out = FilterFeedItems(items, 1000000, 3500, 10);
  EXPECT_EQ(out.size(), size_t{2});  // 0, 3 (4 is 4 s old)
  out = FilterFeedItems(items, 1000000, 0, 1);
  EXPECT_TRUE(out.size() == 1 && out[0].id == L"0");
}

TEST(LiveFolders_StateRoundTrips) {
  LiveFolderState s;
  s.kind = LiveFolderState::Kind::kGithubPulls;
  s.url = L"https://github.com/pulls";
  s.interval_ms = 3600000;
  s.max_items = 25;
  s.review_requested = true;
  s.include_drafts = false;
  s.json_api = 1;
  s.repos = {L"a/b", L"c/d"};
  s.repo_excludes = {L"c/d"};
  s.last_fetched = 12345;
  s.last_error = kLiveGithubNoAuth;
  const auto back = ParseLiveState(SerializeLiveState(s));
  EXPECT_TRUE(back.has_value());
  if (!back) return;
  EXPECT_TRUE(back->kind == s.kind && back->url == s.url && back->interval_ms == s.interval_ms);
  EXPECT_TRUE(back->max_items == 25 && back->review_requested && !back->include_drafts);
  EXPECT_TRUE(back->json_api == 1 && back->repos == s.repos && back->repo_excludes == s.repo_excludes);
  EXPECT_TRUE(back->last_fetched == 12345 && back->last_error == s.last_error);
  LiveFolderState rss;
  rss.url = L"https://e/feed?x=1\nnewline";
  const auto rss_back = ParseLiveState(SerializeLiveState(rss));
  EXPECT_TRUE(rss_back && rss_back->url == rss.url && rss_back->kind == LiveFolderState::Kind::kRss);
  EXPECT_FALSE(ParseLiveState(L"url=x\n").has_value());
  EXPECT_FALSE(ParseLiveState(L"kind=ftp\n").has_value());
}

TEST(LiveFolders_GithubQueries) {
  LiveFolderState s;
  s.kind = LiveFolderState::Kind::kGithubIssues;
  auto q = GithubQueries(s);
  EXPECT_TRUE(q.size() == 1 &&
              q[0] == L"is:issue is:open archived:false sort:updated-desc (assignee:@me)");
  s.kind = LiveFolderState::Kind::kGithubPulls;
  s.author_me = true;
  s.include_drafts = false;
  s.repo_excludes = {L"x/y"};
  q = GithubQueries(s);  // classic page: one query per filter
  EXPECT_EQ(q.size(), size_t{2});
  if (q.size() == 2) {
    EXPECT_WEQ(q[0], L"is:pr is:open archived:false sort:updated-desc draft:false -repo:x/y author:@me");
  }
  s.json_api = 1;
  q = GithubQueries(s);
  EXPECT_TRUE(q.size() == 1 && q[0].find(L"(author:@me OR assignee:@me)") != std::wstring::npos);
  s.author_me = s.assigned_me = false;
  EXPECT_TRUE(GithubQueries(s).empty());
}

TEST(LiveFolders_GithubPages) {
  const char* issues =
      "<ul><li><a class=\"x\" data-testid=\"issue-listitem-title-link\" href=\"/o/r/issues/12\">"
      "Fix &amp; ship</a><span><a data-testid=\"author-filter-link\" href=\"#\"><img/> alice</a>"
      "</span></li><li><a data-testid=\"issue-listitem-title-link\" href=\"/o/r2/pull/3\">PR row</a>"
      "</li><li><a data-testid=\"issue-listitem-title-link\" href=\"/o/r/wiki\">no</a></li></ul>";
  auto page = ParseGithubIssuesHtml(issues, L"https://github.com/issues/assigned");
  EXPECT_EQ(page.items.size(), size_t{2});
  if (page.items.size() == 2) {
    EXPECT_WEQ(page.items[0].id, L"o/r#12");
    EXPECT_WEQ(page.items[0].title, L"Fix & ship");
    EXPECT_WEQ(page.items[0].subtitle, L"alice");
    EXPECT_WEQ(page.items[0].url, L"https://github.com/o/r/issues/12");
    EXPECT_WEQ(page.items[1].subtitle, L"");
  }
  EXPECT_TRUE(page.repos.size() == 2 && page.repos.count(L"o/r2"));
  const char* pulls =
      "<div id=\"issue_7\"><a class=\"Link\" href=\"/o/r\">o/r</a>"
      "<a id=\"issue_7_link\" href=\"/o/r/pull/7\">Seven</a>"
      "<span class=\"opened-by\">#7 opened by <a href=\"/bob\">bob</a></span></div>";
  page = ParseGithubPullsHtml(pulls, L"https://github.com/pulls");
  EXPECT_TRUE(page.items.size() == 1 && page.items[0].id == L"o/r#7" &&
              page.items[0].subtitle == L"bob" && page.items[0].title == L"Seven");
}

TEST(LiveFolders_Diff) {
  std::vector<LiveItem> items(3);
  items[0].id = L"a";
  items[1].id = L"b";
  items[2].id = L"a";  // listed twice
  std::set<std::wstring> dismissed{L"7:b", L"7:gone", L"8:other"};
  const auto diff = DiffLiveItems(7, {L"7:a", L"7:old"}, items, dismissed);
  EXPECT_TRUE(diff.remove.size() == 1 && diff.remove[0] == L"7:old");
  EXPECT_TRUE(diff.add.empty());  // a is present, b dismissed
  EXPECT_TRUE(dismissed.count(L"7:b") && !dismissed.count(L"7:gone") && dismissed.count(L"8:other"));
  std::set<std::wstring> none;
  const auto fresh = DiffLiveItems(7, {}, items, none);
  EXPECT_EQ(fresh.add.size(), size_t{2});
  // An empty result keeps dismissals (it may be a transient failure).
  std::set<std::wstring> kept{L"7:b"};
  DiffLiveItems(7, {L"7:a"}, {}, kept);
  EXPECT_TRUE(kept.count(L"7:b"));
}

TEST(LiveFolders_TimeAgo) {
  const int64_t now = 1000000000;
  EXPECT_WEQ(FormatTimeAgo(0, now), L"-");
  EXPECT_WEQ(FormatTimeAgo(now, now), L"now");
  EXPECT_WEQ(FormatTimeAgo(now - 30 * 1000, now), L"30 seconds ago");
  EXPECT_WEQ(FormatTimeAgo(now - 60 * 1000, now), L"1 minute ago");
  EXPECT_WEQ(FormatTimeAgo(now - 5 * 3600 * 1000, now), L"5 hours ago");
  EXPECT_WEQ(FormatTimeAgo(now - 86400 * 1000, now), L"yesterday");
  EXPECT_WEQ(FormatTimeAgo(now - 3 * 86400 * 1000LL, now), L"3 days ago");
}
