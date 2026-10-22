#include "core/boosts.h"

#include <cmath>

#include "test.h"

using namespace orbit;

TEST(Boosts_RoundTrip) {
  Boost b;
  b.name = L"Reading";
  b.color = true;
  b.dot_angle = 200.f;
  b.contrast = 0.4f;
  b.smart_invert = true;
  b.font_family = L"Georgia, serif";
  b.text_case = L"uppercase";
  b.size = 1.25f;
  b.zap = {L"#cookie-banner", L"div.ad > span"};
  b.focus = {L"article"};
  b.custom_css = L"p {\n  line-height: 1.6;\n}";
  const auto back = ParseBoost(SerializeBoost(b));
  EXPECT_TRUE(back.has_value());
  if (!back) return;
  EXPECT_TRUE(back->name == L"Reading" && back->color && back->smart_invert && back->text_case == L"uppercase");
  EXPECT_TRUE(std::fabs(back->dot_angle - 200.f) < 0.01f && std::fabs(back->size - 1.25f) < 0.001f);
  EXPECT_TRUE(back->zap.size() == 2 && back->zap[1] == L"div.ad > span" && back->focus[0] == L"article");
  EXPECT_WEQ(back->custom_css, L"p {\n  line-height: 1.6;\n}");
  // Bad values fall back; bad selectors are dropped.
  const auto odd = ParseBoost(L"case=shout\nsize=99\nzap=a{b}\nzap=.ok\n");
  EXPECT_TRUE(odd && odd->text_case == L"none" && odd->size == 3.f && odd->zap.size() == 1);
  EXPECT_FALSE(ParseBoost(L"").has_value());
}

TEST(Boosts_DomainAndSelectors) {
  EXPECT_WEQ(BoostDomain(L"https://www.Example.com:8080/a?b"), L"example.com");
  EXPECT_WEQ(BoostDomain(L"http://news.example.org/"), L"news.example.org");
  EXPECT_WEQ(BoostDomain(L"orbit://settings/"), L"");
  EXPECT_WEQ(BoostDomain(L"file:///C:/x.html"), L"");
  EXPECT_TRUE(IsSafeBoostSelector(L"main > div:nth-of-type(2)"));
  EXPECT_FALSE(IsSafeBoostSelector(L"a{color:red}"));
  EXPECT_FALSE(IsSafeBoostSelector(L"a /* x */"));
  EXPECT_FALSE(IsSafeBoostSelector(L""));
}

TEST(Boosts_Css) {
  Boost b;
  EXPECT_TRUE(BoostCss(b).empty());  // nothing changed
  b.zap = {L"#ads"};
  b.text_case = L"lowercase";
  b.size = 1.5f;
  b.custom_css = L"body { margin: 0 }";
  std::wstring css = BoostCss(b);
  EXPECT_TRUE(css.find(L"#ads:not([zen-zap-unhide]){ display: none !important; }") != std::wstring::npos);
  EXPECT_TRUE(css.find(L"text-transform: lowercase !important;") != std::wstring::npos);
  EXPECT_TRUE(css.find(L"zoom: 1.5") != std::wstring::npos);
  EXPECT_TRUE(css.find(L"body { margin: 0 }") != std::wstring::npos);
  b.smart_invert = true;
  b.color = true;
  css = BoostCss(b);
  EXPECT_TRUE(css.find(L"filter: invert(1) hue-rotate(180deg) url(#orbit-boost-color)") != std::wstring::npos);
  EXPECT_TRUE(css.find(L"img, video") != std::wstring::npos);
  b.enabled = false;
  EXPECT_TRUE(BoostCss(b).empty());
}

TEST(Boosts_ColorFilter) {
  Boost b;
  b.color = true;
  b.dot_angle = 0.f;        // red
  b.secondary_delta = 180.f;  // cyan
  b.saturation = 0.f;
  b.brightness = 0.444f;     // lightness 0.5
  b.contrast = 0.25f;
  const BoostFilter f = MakeBoostFilter(b);
  EXPECT_TRUE(f.accent[0] > 0.95f && f.accent[1] < 0.05f && f.accent[2] < 0.05f);
  EXPECT_TRUE(f.complementary[0] < 0.05f && f.complementary[1] > 0.95f && f.complementary[2] > 0.95f);
  EXPECT_TRUE(std::fabs(f.blend - 0.75f) < 0.001f);
  EXPECT_EQ(f.table[0].size(), size_t{17});
  // Dark input leans red, light input leans cyan.
  EXPECT_TRUE(f.table[0][3] > f.table[1][3]);
  EXPECT_TRUE(f.table[1][13] > f.table[0][13]);
  // Lightness follows the input.
  EXPECT_TRUE(f.table[0][0] + f.table[1][0] + f.table[2][0] < f.table[0][16] + f.table[1][16] + f.table[2][16]);
  const std::wstring svg = BoostFilterSvg(b);
  EXPECT_TRUE(svg.find(L"id=\"orbit-boost-color\"") != std::wstring::npos && svg.find(L"k2=\"0.75\"") != std::wstring::npos);
}

TEST(Boosts_ApplyScript) {
  Boost b;
  b.zap = {L"#x"};
  const std::string on = BoostApplyScript(&b);
  EXPECT_TRUE(on.find("adoptedStyleSheets") != std::string::npos && on.find("#x:not") != std::string::npos);
  EXPECT_TRUE(on.find("filter = null") != std::string::npos);
  b.color = true;
  EXPECT_TRUE(BoostApplyScript(&b).find("tables:[\"") != std::string::npos);
  // A selector cannot break out of the string literal.
  b.custom_css = L"</script><script>alert(1)</script>\"";
  EXPECT_TRUE(BoostApplyScript(&b).find("</script>") == std::string::npos);
  EXPECT_TRUE(BoostApplyScript(nullptr).find("const css = \"\"") != std::string::npos);
}
