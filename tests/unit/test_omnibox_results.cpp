#include <algorithm>
#include "core/omnibox_results.h"
#include "test.h"

using orbit::BuildOmniboxRows;
using orbit::OmniboxRow;
using orbit::OmniboxSources;

namespace {
constexpr wchar_t kDdg[] = L"https://duckduckgo.com/?q={query}";

OmniboxSources Sample() {
  OmniboxSources s;
  s.open_tabs = {{L"https://github.com/zen-browser/desktop", L"zen-browser/desktop", 11},
                 {L"https://news.ycombinator.com/", L"Hacker News", 12},
                 {L"https://example.com/current", L"Current tab", 13}};
  s.bookmarks = {{L"https://github.com/zen-browser/desktop", L"Zen repo", 0},
                 {L"https://docs.example.org/zen", L"Zen docs", 0}};
  s.history = {{L"https://zen-browser.app/", L"Zen Browser", 0},
               {L"https://docs.example.org/zen#intro", L"Zen docs", 0},
               {L"https://example.com/current", L"Current tab", 0}};
  s.current_tab_key = 13;
  return s;
}
}  // namespace

TEST(Omnibox_EmptyInputHasNoRows) {
  EXPECT_TRUE(BuildOmniboxRows(L"   ", kDdg, Sample()).empty());
}

TEST(Omnibox_SearchHeuristicComesFirst) {
  auto rows = BuildOmniboxRows(L"zen browser", kDdg, Sample());
  EXPECT_TRUE(!rows.empty() && rows[0].kind == OmniboxRow::Kind::kSearch);
  if (!rows.empty()) {
    EXPECT_WEQ(rows[0].action, L"Search with DuckDuckGo");
    EXPECT_WEQ(rows[0].url, L"https://duckduckgo.com/?q=zen+browser");
  }
}

TEST(Omnibox_OpenTabsBeatBookmarksAndHistory) {
  auto rows = BuildOmniboxRows(L"zen", kDdg, Sample());
  // search, switch-to-tab (github), bookmark (docs), history (zen-browser.app)
  EXPECT_EQ(rows.size(), size_t{4});
  if (rows.size() == 4) {
    EXPECT_TRUE(rows[1].kind == OmniboxRow::Kind::kSwitchTab && rows[1].tab_key == 11);
    EXPECT_WEQ(rows[1].action, L"Switch to Tab");
    EXPECT_TRUE(rows[2].kind == OmniboxRow::Kind::kBookmark);
    EXPECT_WEQ(rows[2].url, L"https://docs.example.org/zen");  // history copy with #intro deduped
    EXPECT_TRUE(rows[3].kind == OmniboxRow::Kind::kHistory);
    EXPECT_WEQ(rows[3].url, L"https://zen-browser.app/");
  }
}

TEST(Omnibox_EveryWordMustMatch) {
  auto rows = BuildOmniboxRows(L"hacker news", kDdg, Sample());
  EXPECT_EQ(rows.size(), size_t{2});  // search + the Hacker News tab
  rows = BuildOmniboxRows(L"hacker zen", kDdg, Sample());
  EXPECT_EQ(rows.size(), size_t{1});  // only the search row
}

TEST(Omnibox_CurrentTabIsNeverSuggested) {
  auto rows = BuildOmniboxRows(L"current", kDdg, Sample());
  EXPECT_EQ(rows.size(), size_t{1});  // neither as switch target nor as history
}

TEST(Omnibox_VisitHeuristicDedupesHistory) {
  auto rows = BuildOmniboxRows(L"zen-browser.app", kDdg, Sample());
  EXPECT_TRUE(!rows.empty() && rows[0].kind == OmniboxRow::Kind::kVisit);
  if (!rows.empty()) EXPECT_WEQ(rows[0].url, L"https://zen-browser.app");
  for (size_t i = 1; i < rows.size(); ++i) {
    EXPECT_FALSE(rows[i].url == L"https://zen-browser.app/");
  }
}

TEST(Omnibox_RowsAreCapped) {
  OmniboxSources s;
  for (int i = 0; i < 20; ++i) {
    s.history.push_back({L"https://site" + std::to_wstring(i) + L".example/", L"Zen " +
                         std::to_wstring(i), 0});
  }
  EXPECT_EQ(BuildOmniboxRows(L"zen", kDdg, s).size(), orbit::kMaxOmniboxRows);
}

TEST(Omnibox_SearchSuggestionsFollowTheHeuristic) {
  OmniboxSources s;
  s.search_suggestions = {L"zen browser", L"ZEN", L"zen garden", L"zen 2", L"zen 3", L"zen 4"};
  s.history.push_back({L"https://zen.example/", L"Zen page", 0});
  const auto rows = BuildOmniboxRows(L"zen", kDdg, s);
  EXPECT_TRUE(rows.size() >= 6);
  if (rows.size() < 6) return;
  EXPECT_TRUE(rows[0].kind == orbit::OmniboxRow::Kind::kSearch && rows[0].title == L"zen");
  EXPECT_TRUE(rows[1].kind == orbit::OmniboxRow::Kind::kSearch && rows[1].title == L"zen browser");
  EXPECT_WEQ(rows[1].url, L"https://duckduckgo.com/?q=zen+browser");
  EXPECT_TRUE(rows[2].title == L"zen garden");  // "ZEN" repeats the input: skipped
  EXPECT_TRUE(rows[5].kind == orbit::OmniboxRow::Kind::kHistory);  // at most 4 suggestions
  // A URL-like input gets no search suggestions.
  const auto url_rows = BuildOmniboxRows(L"zen.example", kDdg, s);
  EXPECT_TRUE(!url_rows.empty() && url_rows[0].kind == orbit::OmniboxRow::Kind::kVisit);
  for (size_t i = 1; i < url_rows.size(); ++i) EXPECT_TRUE(url_rows[i].kind != orbit::OmniboxRow::Kind::kSearch);
  EXPECT_WEQ(orbit::SearchSuggestUrl(L"https://www.google.com/search?q={query}", L"a b"),
             L"https://www.google.com/complete/search?client=firefox&q=a+b");
  EXPECT_WEQ(orbit::SearchSuggestUrl(L"https://www.kagi.com/search?q={query}", L"x"), L"");
  // Selecting a row fills the input.
  EXPECT_WEQ(orbit::OmniboxRowInputText(rows[1], 1, L"zen"), L"zen browser");
  EXPECT_WEQ(orbit::OmniboxRowInputText(rows[0], 0, L"zen"), L"zen");
  EXPECT_WEQ(orbit::OmniboxRowInputText(rows[5], 5, L"zen"), L"https://zen.example/");
}

TEST(Omnibox_SearchEngineNames) {
  EXPECT_WEQ(orbit::SearchEngineName(L"https://www.google.com/search?q={query}"), L"Google");
  EXPECT_WEQ(orbit::SearchEngineName(L"https://search.brave.com/search?q={query}"), L"Brave Search");
  EXPECT_WEQ(orbit::SearchEngineName(L"https://www.kagi.com/search?q={query}"), L"kagi.com");
}

TEST(Omnibox_QuickActionScoringFollowsZen) {
  using orbit::QuickActionScore;
  EXPECT_TRUE(QuickActionScore(L"Close Tab", L"close tab") == 1000000);
  EXPECT_TRUE(QuickActionScore(L"Close Tab", L"clo") == 100003);
  // "close tab" inside a longer label scores below the exact label.
  EXPECT_TRUE(QuickActionScore(L"Reopen Closed Tab", L"close tab") < 100000);
  EXPECT_TRUE(QuickActionScore(L"Toggle Compact Mode", L"tcm") > 0);
  EXPECT_TRUE(QuickActionScore(L"Toggle Compact Mode", L"xyz") == 0);
  EXPECT_TRUE(QuickActionScore(L"Pin", L"pin tab") == 0);  // longer than the label
}

TEST(Omnibox_QuickActionsComeAfterTheHeuristic) {
  OmniboxSources s;
  s.actions = {{L"compact", L"Toggle Compact Mode", L""}, {L"close", L"Close Tab", L""},
               {L"space:7", L"Focus on Work", L"Work"}};
  auto rows = BuildOmniboxRows(L"compact", kDdg, s);
  EXPECT_TRUE(rows.size() >= 2 && rows[0].kind == OmniboxRow::Kind::kSearch &&
              rows[1].kind == OmniboxRow::Kind::kAction && rows[1].action_id == L"compact");
  rows = BuildOmniboxRows(L"work", kDdg, s);  // a space by name
  EXPECT_TRUE(rows.size() >= 2 && rows[1].action_id == L"space:7");
  EXPECT_TRUE(BuildOmniboxRows(L"co", kDdg, s).size() == 1);  // too short
  rows = BuildOmniboxRows(L"https://close", kDdg, s);           // looks like a URL
  EXPECT_TRUE(std::none_of(rows.begin(), rows.end(),
                           [](const OmniboxRow& r) { return r.kind == OmniboxRow::Kind::kAction; }));
}
