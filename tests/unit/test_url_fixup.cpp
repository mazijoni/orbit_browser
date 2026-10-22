#include "core/external_scheme.h"
#include "core/url_fixup.h"
#include "test.h"

using orbit::FixupOmniboxInput;
using orbit::OmniboxResult;
using orbit::StripTrackingParams;

namespace {
constexpr wchar_t kSearch[] = L"https://search.example/?q={query}";

std::wstring Url(const wchar_t* input) { return FixupOmniboxInput(input, kSearch).url; }
OmniboxResult::Kind Kind(const wchar_t* input) { return FixupOmniboxInput(input, kSearch).kind; }
}  // namespace

TEST(Omnibox_EmptyInputDoesNothing) {
  EXPECT_TRUE(Kind(L"") == OmniboxResult::Kind::kNone);
  EXPECT_TRUE(Kind(L"   ") == OmniboxResult::Kind::kNone);
}

TEST(Omnibox_FullUrlsPassThrough) {
  EXPECT_WEQ(Url(L"https://example.com/a?b=c"), L"https://example.com/a?b=c");
  EXPECT_WEQ(Url(L"  http://example.com  "), L"http://example.com");
  EXPECT_WEQ(Url(L"about:blank"), L"about:blank");
  EXPECT_WEQ(Url(L"orbit://history/"), L"orbit://history/");
}

TEST(Omnibox_BareHostsGetHttps) {
  EXPECT_WEQ(Url(L"example.com"), L"https://example.com");
  EXPECT_WEQ(Url(L"sub.example.co.uk/path"), L"https://sub.example.co.uk/path");
  EXPECT_WEQ(Url(L"example.com:8443/x"), L"https://example.com:8443/x");
}

TEST(Omnibox_LocalHostsGetHttp) {
  EXPECT_WEQ(Url(L"localhost"), L"http://localhost");
  EXPECT_WEQ(Url(L"localhost:3000/app"), L"http://localhost:3000/app");
  EXPECT_WEQ(Url(L"192.168.1.10"), L"http://192.168.1.10");
  EXPECT_WEQ(Url(L"[::1]:8080"), L"http://[::1]:8080");
}

TEST(Omnibox_SearchQueries) {
  EXPECT_WEQ(Url(L"hello world"), L"https://search.example/?q=hello+world");
  EXPECT_WEQ(Url(L"what is c++"), L"https://search.example/?q=what+is+c%2B%2B");
  EXPECT_WEQ(Url(L"singleword"), L"https://search.example/?q=singleword");
  EXPECT_WEQ(Url(L"\x00E6\x00F8\x00E5"), L"https://search.example/?q=%C3%A6%C3%B8%C3%A5");
  EXPECT_TRUE(Kind(L"1.5") == OmniboxResult::Kind::kSearch);   // numeric "TLD"
  EXPECT_TRUE(Kind(L"999.999.1.1") == OmniboxResult::Kind::kSearch);
}

TEST(Omnibox_DangerousSchemesBecomeSearches) {
  EXPECT_TRUE(Kind(L"javascript:alert(1)") == OmniboxResult::Kind::kSearch);
  EXPECT_TRUE(Kind(L"vbscript:msgbox") == OmniboxResult::Kind::kSearch);
  EXPECT_TRUE(Kind(L"mailto:someone@example.com") == OmniboxResult::Kind::kSearch);
}

TEST(Omnibox_InvalidPortIsSearch) {
  EXPECT_TRUE(Kind(L"example.com:99999") == OmniboxResult::Kind::kSearch);
}

TEST(ExternalScheme_AllowsOrdinaryHandlers) {
  std::wstring scheme;
  EXPECT_TRUE(orbit::IsExternalUrlLaunchable(L"mailto:a@example.com", &scheme));
  EXPECT_WEQ(scheme, L"mailto");
  EXPECT_TRUE(orbit::IsExternalUrlLaunchable(L"zoommtg://zoom.us/join?x=1", &scheme));
  EXPECT_TRUE(orbit::IsExternalUrlLaunchable(L"MS-SETTINGS:display", &scheme));
  EXPECT_WEQ(scheme, L"ms-settings");
}

TEST(ExternalScheme_RejectsDangerousOrMalformed) {
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"ms-msdt:/id PCWDiagnostic", nullptr));
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"search-ms:query=x", nullptr));
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"file:///C:/Windows/System32/calc.exe", nullptr));
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"C:\\Windows\\notepad.exe", nullptr));
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"javascript:alert(1)", nullptr));
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"foo:bar\"baz", nullptr));
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"foo:bar\nbaz", nullptr));
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"no-colon", nullptr));
  EXPECT_FALSE(orbit::IsExternalUrlLaunchable(L"1abc:x", nullptr));
}

TEST(SidebarDisplayUrl_HttpsShowsDomainOnly) {
  using orbit::SidebarDisplayUrl;
  EXPECT_WEQ(SidebarDisplayUrl(L"https://github.com/"), L"github.com");
  EXPECT_WEQ(SidebarDisplayUrl(L"https://www.example.com/a/b?c=d"), L"example.com");
  EXPECT_WEQ(SidebarDisplayUrl(L"https://example.com:8443/x"), L"example.com:8443");
  EXPECT_WEQ(SidebarDisplayUrl(L"https://sub.www.example.com/"), L"sub.www.example.com");
}

TEST(SidebarDisplayUrl_OtherSchemesStayFull) {
  using orbit::SidebarDisplayUrl;
  // Zen strips only https; http keeps its scheme so it stays visibly insecure.
  EXPECT_WEQ(SidebarDisplayUrl(L"http://example.com/"), L"http://example.com");
  EXPECT_WEQ(SidebarDisplayUrl(L"http://example.com/path/"), L"http://example.com/path/");
  EXPECT_WEQ(SidebarDisplayUrl(L"orbit://history/"), L"orbit://history/");
  EXPECT_WEQ(SidebarDisplayUrl(L""), L"");
}

TEST(PinnedUrlMatches_IgnoresFragmentAndRootSlash) {
  using orbit::PinnedUrlMatches;
  EXPECT_TRUE(PinnedUrlMatches(L"https://github.com/", L"https://github.com"));
  EXPECT_TRUE(PinnedUrlMatches(L"https://mail.example/inbox#msg-1", L"https://mail.example/inbox"));
  EXPECT_FALSE(PinnedUrlMatches(L"https://github.com/orbit", L"https://github.com/"));
  EXPECT_FALSE(PinnedUrlMatches(L"https://github.com/?tab=1", L"https://github.com/"));
}

TEST(UrlFixup_StripTrackingParams) {
  EXPECT_WEQ(StripTrackingParams(L"https://example.com/a?utm_source=x&id=7&utm_medium=y#top"),
             L"https://example.com/a?id=7#top");
  EXPECT_WEQ(StripTrackingParams(L"https://example.com/?fbclid=abc"), L"https://example.com/");
  EXPECT_WEQ(StripTrackingParams(L"http://shop.example/p?GCLID=1&color=red"), L"http://shop.example/p?color=red");
  // Site-specific share ids only on their sites.
  EXPECT_WEQ(StripTrackingParams(L"https://www.youtube.com/watch?v=abc&si=xyz"), L"https://www.youtube.com/watch?v=abc");
  EXPECT_WEQ(StripTrackingParams(L"https://example.com/watch?v=abc&si=xyz"), L"https://example.com/watch?v=abc&si=xyz");
  EXPECT_WEQ(StripTrackingParams(L"https://notyoutube.com/?si=1"), L"https://notyoutube.com/?si=1");
  // Untouched: no query, other schemes, a '?' inside the fragment.
  EXPECT_WEQ(StripTrackingParams(L"https://example.com/path"), L"https://example.com/path");
  EXPECT_WEQ(StripTrackingParams(L"orbit://settings/?utm_source=x"), L"orbit://settings/?utm_source=x");
  EXPECT_WEQ(StripTrackingParams(L"https://example.com/#a?utm_source=x"), L"https://example.com/#a?utm_source=x");
}
