#include "core/space_theme.h"

#include <cmath>

#include "test.h"

using namespace orbit;

namespace {
bool Near(const Rgb& a, const Rgb& b, float tolerance = 1.5f) {
  return std::fabs(a.r - b.r) <= tolerance && std::fabs(a.g - b.g) <= tolerance &&
         std::fabs(a.b - b.b) <= tolerance;
}
ThemeDot Dot(int r, int g, int b, bool primary = false) {
  ThemeDot d;
  d.r = r;
  d.g = g;
  d.b = b;
  d.x = 120.f;
  d.y = 80.5f;
  d.type = L"explicit-lightness";
  d.primary = primary;
  return d;
}
}  // namespace

TEST(SpaceTheme_RoundTrips) {
  SpaceTheme t;
  t.opacity = 0.42f;
  t.texture = 0.25f;
  t.lightness = 70;
  t.algorithm = L"splitComplementary";
  t.dots = {Dot(200, 100, 50, true), Dot(10, 20, 30)};
  const auto back = ParseSpaceTheme(SerializeSpaceTheme(t));
  EXPECT_TRUE(back.has_value());
  if (!back) return;
  EXPECT_TRUE(std::fabs(back->opacity - 0.42f) < 0.001f && std::fabs(back->texture - 0.25f) < 0.001f);
  EXPECT_TRUE(back->lightness == 70 && back->algorithm == L"splitComplementary");
  EXPECT_EQ(back->dots.size(), size_t{2});
  EXPECT_TRUE(back->dots[0].r == 200 && back->dots[0].primary && !back->dots[1].primary);
  EXPECT_TRUE(back->dots[0].x == 120.f && back->dots[0].y == 80.5f && back->dots[1].type == L"explicit-lightness");
  // An empty theme (Zen's default: no colours) is still a theme.
  SpaceTheme none;
  const auto empty = ParseSpaceTheme(SerializeSpaceTheme(none));
  EXPECT_TRUE(empty && empty->dots.empty());
  EXPECT_FALSE(ParseSpaceTheme(L"").has_value());
  EXPECT_FALSE(ParseSpaceTheme(L"0.5 0 50 x|1,2,3").has_value());
  EXPECT_FALSE(ParseSpaceTheme(L"0.5 0 50 bad;word").has_value());
}

TEST(SpaceTheme_Paints) {
  SpaceTheme t;
  t.opacity = 0.5f;
  // No colours: the plain browser background.
  ThemePaint p = BuildThemePaint(t, true, {120, 140, 255});
  EXPECT_TRUE(p.layers.empty() && p.base == (Rgb{19, 19, 19}) && !p.explicit_mode && p.dark);
  // One colour: half way between it and the toolbar base.
  t.dots = {Dot(255, 0, 0, true)};
  p = BuildThemePaint(t, true, {});
  EXPECT_TRUE(p.layers.empty() && Near(p.base, {139, 12, 13}));
  EXPECT_TRUE(p.explicit_mode);
  // Two: two linear layers; the top-left corner leans to the first colour.
  t.dots = {Dot(255, 0, 0, true), Dot(0, 0, 255)};
  p = BuildThemePaint(t, true, {});
  EXPECT_EQ(p.layers.size(), size_t{2});
  const Rgb tl = ThemeColorAt(p, 0, 0, 1000, 600);
  const Rgb br = ThemeColorAt(p, 1000, 600, 1000, 600);
  EXPECT_TRUE(tl.r > tl.b && br.b > br.r);
  // Three: two radial corners and a linear band at the bottom.
  t.dots = {Dot(255, 0, 0, true), Dot(0, 255, 0), Dot(0, 0, 255)};
  p = BuildThemePaint(t, false, {});
  EXPECT_EQ(p.layers.size(), size_t{3});
  const Rgb bottom = ThemeColorAt(p, 500, 600, 1000, 600);
  const Rgb top_right = ThemeColorAt(p, 990, 0, 1000, 600);
  EXPECT_TRUE(bottom.b > bottom.r && bottom.b > bottom.g);
  EXPECT_TRUE(top_right.g > top_right.r);
  t.texture = 0.5f;
  EXPECT_TRUE(BuildThemePaint(t, false, {}).grain == 0.5f);
}

TEST(SpaceTheme_TextAndAccent) {
  // A pale colour gets dark text; a deep one light text.
  EXPECT_FALSE(ThemeShouldBeDark({245, 237, 214}, 0.5f, false));
  EXPECT_TRUE(ThemeShouldBeDark({23, 17, 34}, 0.5f, true));
  // In light mode the accent is pulled toward a readable lightness; grey
  // and dark-mode accents stay as they are.
  const Rgb accent = ThemeAccentForUi({245, 237, 214}, false, false);
  EXPECT_TRUE(accent == (Rgb{246, 200, 66}));  // s + 0.3, l = 0.9 * 0.4 + 0.42 * 0.6
  EXPECT_TRUE(ThemeAccentForUi({128, 128, 128}, false, false) == (Rgb{128, 128, 128}));
  EXPECT_TRUE(ThemeAccentForUi({10, 200, 30}, true, true) == (Rgb{10, 200, 30}));
  EXPECT_TRUE(BlendRgb({255, 0, 0}, {0, 0, 255}, 25.f) == (Rgb{64, 0, 191}));
}
