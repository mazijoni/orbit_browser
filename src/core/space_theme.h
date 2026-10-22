// A space's theme (ZenGradientGenerator / nsZenThemePicker): up to three
// colour dots picked on a disc, an opacity (how strongly they tint the base)
// and a grain texture. This part is platform independent and unit-tested:
// the stored form, Zen's CSS gradient recipes as paint layers, the colour
// at a point, the light/dark text decision and the UI accent colour.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace orbit {

struct Rgb {
  float r = 0.f, g = 0.f, b = 0.f;  // 0..255
  bool operator==(const Rgb&) const = default;
};

struct ThemeDot {
  int r = 0, g = 0, b = 0;
  float x = 0.f, y = 0.f;  // where it sits on the picker (for editing)
  std::wstring type;       // "", "explicit-lightness", "explicit-black-white"
  bool primary = false;
};

struct SpaceTheme {
  std::vector<ThemeDot> dots;  // at most kMaxDots
  float opacity = 0.5f;        // PanelUI-zen-gradient-generator-opacity (0.25..0.8)
  float texture = 0.f;         // grain, 0..1 in 1/16 steps
  std::wstring algorithm;      // the colour harmony the dots follow
  int lightness = 50;
  static constexpr size_t kMaxDots = 3;  // nsZenThemePicker.MAX_DOTS
};

// One line: "opacity texture lightness algorithm|r,g,b,x,y,type,primary|...".
std::wstring SerializeSpaceTheme(const SpaceTheme& theme);
std::optional<SpaceTheme> ParseSpaceTheme(const std::wstring& text);

// getGradient(colors) as layers over a base, bottom to top. A layer is the
// colour fully at |solid| and fading to transparent at |clear| (fractions
// of the CSS gradient line / radius, may lie outside 0..1).
struct GradientLayer {
  enum class Kind { kLinear, kRadial };
  Kind kind = Kind::kLinear;
  Rgb color;
  float angle_deg = 0.f;       // linear: CSS angle (0 = to top, clockwise)
  float cx = 0.f, cy = 0.f;    // radial: centre as fractions of the box
  float solid = 0.f, clear = 1.f;
};

struct ThemePaint {
  Rgb base;                           // the browser background under the layers
  std::vector<GradientLayer> layers;  // bottom to top
  float grain = 0.f;
  bool explicit_mode = false;  // a coloured theme decides light / dark itself
  bool dark = true;            // light text (zen-should-be-dark-mode)
  Rgb accent;                  // --zen-primary-color
};

// |system_dark|: the window's colour scheme; |native_accent|: the accent used
// by a theme without colours (getNativeAccentColor before darkening).
ThemePaint BuildThemePaint(const SpaceTheme& theme, bool system_dark, Rgb native_accent);

// The composited colour at (x, y) in a w x h box.
Rgb ThemeColorAt(const ThemePaint& paint, float x, float y, float w, float h);

// Building blocks, exposed for tests.
Rgb BlendRgb(const Rgb& a, const Rgb& b, float percentage);  // blendColors
bool ThemeShouldBeDark(const Rgb& dominant, float opacity, bool system_dark);  // shouldBeDarkMode
Rgb ThemeAccentForUi(const Rgb& dominant, bool theme_dark, bool system_dark);  // getAccentColorForUI

}  // namespace orbit
