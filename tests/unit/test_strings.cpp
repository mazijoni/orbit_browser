#include <windows.h>

#include "core/process_metrics.h"
#include "core/strings.h"
#include "test.h"

TEST(Strings_Utf8RoundTrip) {
  const std::wstring s = L"Bl\x00E5b\x00E6rsyltet\x00F8y \xD83D\xDE00";  // includes an emoji (surrogates)
  EXPECT_WEQ(orbit::FromUtf8(orbit::ToUtf8(s)), s);
  EXPECT_EQ(orbit::ToUtf8(L""), std::string());
}

TEST(Strings_HtmlEscape) {
  EXPECT_EQ(orbit::HtmlEscape(L"<a href=\"x\">'&'</a>"),
            std::string("&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;"));
}

TEST(Strings_JsonStringIsScriptSafe) {
  EXPECT_EQ(orbit::JsonString(L"a\"b\\c"), std::string("\"a\\u0022b\\u005cc\""));
  EXPECT_EQ(orbit::JsonString(L"</script>"), std::string("\"\\u003c/script\\u003e\""));
  EXPECT_EQ(orbit::JsonString(L"line\nbreak"), std::string("\"line\\u000abreak\""));
  // Non-BMP characters must survive as valid UTF-8, not as lone surrogates.
  EXPECT_EQ(orbit::JsonString(L"\xD83D\xDE00"), std::string("\"\xF0\x9F\x98\x80\""));
}

TEST(ProcessMetrics_TypeAndSelf) {
  EXPECT_WEQ(orbit::ChromiumProcessType(L"orbit.exe --type=renderer --foo"), L"renderer");
  EXPECT_WEQ(orbit::ChromiumProcessType(L"\"C:\\x\\orbit.exe\" --type=gpu-process"), L"gpu-process");
  EXPECT_WEQ(orbit::ChromiumProcessType(L"orbit.exe https://example.com"), L"browser");
  EXPECT_WEQ(orbit::ChromiumProcessType(L""), L"unknown");
  const auto tree = orbit::CollectProcessTree(::GetCurrentProcessId());
  EXPECT_FALSE(tree.empty());
  if (!tree.empty()) {
    EXPECT_TRUE(tree[0].private_working_set > 0);
    EXPECT_TRUE(tree[0].working_set >= tree[0].private_working_set);
  }
}

TEST(Strings_TrimAndCase) {
  EXPECT_WEQ(std::wstring(orbit::TrimWhitespace(L"  a b \t")), L"a b");
  EXPECT_WEQ(orbit::ToLowerAscii(L"HTTPS://Ex\x00C6"), L"https://ex\x00C6");
  EXPECT_TRUE(orbit::StartsWithNoCase(L"ORBIT://x", L"orbit://"));
  EXPECT_FALSE(orbit::StartsWithNoCase(L"str", L"orbit://"));
}
