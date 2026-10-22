#include "core/space_theme.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace orbit {
namespace {

constexpr float kPi = 3.14159265f;

// getToolbarModifiedBaseRaw (no transparent sidebar).
Rgb ToolbarBase(bool dark) { return dark ? Rgb{23, 23, 26} : Rgb{240, 240, 244}; }
// getBrowserBg when the window cannot be transparent.
Rgb BrowserBase(bool dark) { return dark ? Rgb{19, 19, 19} : Rgb{233, 233, 233}; }

Rgb DotColor(const ThemeDot& d) {
  return {static_cast<float>(d.r), static_cast<float>(d.g), static_cast<float>(d.b)};
}

float Luminance(const Rgb& c) {
  auto lin = [](float v) {
    v /= 255.f;
    return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
  };
  return lin(c.r) * 0.2126f + lin(c.g) * 0.7152f + lin(c.b) * 0.0722f;
}

float Contrast(const Rgb& a, const Rgb& b) {
  const float la = Luminance(a), lb = Luminance(b);
  return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

void RgbToHsl(const Rgb& c, float& h, float& s, float& l) {
  const float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  if (d == 0.f) {
    h = 0.f;
  } else if (mx == r) {
    h = std::fmod((g - b) / d, 6.f);
  } else if (mx == g) {
    h = (b - r) / d + 2.f;
  } else {
    h = (r - g) / d + 4.f;
  }
  h *= 60.f;
  l = (mn + mx) / 2.f;
  s = d == 0.f ? 0.f : d / (1.f - std::fabs(2.f * l - 1.f));
}

float HueToRgb(float p, float q, float t) {
  if (t < 0.f) t += 1.f;
  if (t > 1.f) t -= 1.f;
  if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
  if (t < 1.f / 2.f) return q;
  if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
  return p;
}

Rgb HslToRgb(float h, float s, float l) {
  float r, g, b;
  if (s == 0.f) {
    r = g = b = l;
  } else {
    const float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
    const float p = 2.f * l - q;
    r = HueToRgb(p, q, h + 1.f / 3.f);
    g = HueToRgb(p, q, h);
    b = HueToRgb(p, q, h - 1.f / 3.f);
  }
  return {std::round(r * 255.f), std::round(g * 255.f), std::round(b * 255.f)};
}

float LayerAlpha(const GradientLayer& layer, float x, float y, float w, float h) {
  float t;
  if (layer.kind == GradientLayer::Kind::kLinear) {
    // CSS: the gradient line runs through the centre at the angle; its
    // length makes the corners land on 0% and 100%.
    const float a = layer.angle_deg * kPi / 180.f;
    const float dx = std::sin(a), dy = -std::cos(a);
    const float length = std::fabs(w * dx) + std::fabs(h * dy);
    t = length > 0.f ? ((x - w / 2.f) * dx + (y - h / 2.f) * dy) / length + 0.5f : 0.f;
  } else {
    // circle at cx cy, farthest-corner.
    const float cx = layer.cx * w, cy = layer.cy * h;
    const float rx = std::max(cx, w - cx), ry = std::max(cy, h - cy);
    const float radius = std::sqrt(rx * rx + ry * ry);
    t = radius > 0.f ? std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / radius : 0.f;
  }
  if (t <= layer.solid) return 1.f;
  if (t >= layer.clear) return 0.f;
  return (layer.clear - t) / (layer.clear - layer.solid);
}

bool SafeWord(const std::wstring& s) {
  return std::all_of(s.begin(), s.end(), [](wchar_t c) { return std::iswalnum(c) || c == L'-'; });
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  size_t start = 0;
  while (true) {
    const size_t i = s.find(sep, start);
    out.push_back(s.substr(start, i == std::wstring::npos ? std::wstring::npos : i - start));
    if (i == std::wstring::npos) break;
    start = i + 1;
  }
  return out;
}

bool ToFloat(const std::wstring& s, float& out) {
  if (s.empty() || s.size() > 32) return false;
  wchar_t* end = nullptr;
  const double v = std::wcstod(s.c_str(), &end);
  if (!end || *end != 0 || !std::isfinite(v)) return false;
  out = static_cast<float>(v);
  return true;
}

}  // namespace

Rgb BlendRgb(const Rgb& a, const Rgb& b, float percentage) {
  const float p = percentage / 100.f;
  return {std::round(a.r * p + b.r * (1.f - p)), std::round(a.g * p + b.g * (1.f - p)),
          std::round(a.b * p + b.b * (1.f - p))};
}

std::wstring SerializeSpaceTheme(const SpaceTheme& t) {
  wchar_t head[96];
  swprintf(head, 96, L"%.3f %.4f %d ", t.opacity, t.texture, t.lightness);
  std::wstring out = head;
  out += SafeWord(t.algorithm) ? t.algorithm : L"";
  for (size_t i = 0; i < t.dots.size() && i < SpaceTheme::kMaxDots; ++i) {
    const ThemeDot& d = t.dots[i];
    wchar_t dot[96];
    swprintf(dot, 96, L"|%d,%d,%d,%.1f,%.1f,", d.r, d.g, d.b, d.x, d.y);
    out += dot;
    out += SafeWord(d.type) ? d.type : L"";
    out += d.primary ? L",1" : L",0";
  }
  return out;
}

std::optional<SpaceTheme> ParseSpaceTheme(const std::wstring& text) {
  if (text.empty() || text.size() > 1024) return std::nullopt;
  const auto parts = Split(text, L'|');
  const auto head = Split(parts[0], L' ');
  if (head.size() != 4) return std::nullopt;
  SpaceTheme t;
  float lightness = 0.f;
  if (!ToFloat(head[0], t.opacity) || !ToFloat(head[1], t.texture) || !ToFloat(head[2], lightness) ||
      !SafeWord(head[3])) {
    return std::nullopt;
  }
  t.opacity = std::clamp(t.opacity, 0.f, 1.f);
  t.texture = std::clamp(t.texture, 0.f, 1.f);
  t.lightness = static_cast<int>(std::clamp(lightness, 0.f, 100.f));
  t.algorithm = head[3];
  for (size_t i = 1; i < parts.size() && t.dots.size() < SpaceTheme::kMaxDots; ++i) {
    const auto f = Split(parts[i], L',');
    if (f.size() != 7) return std::nullopt;
    float v[5];
    for (int k = 0; k < 5; ++k) {
      if (!ToFloat(f[static_cast<size_t>(k)], v[k])) return std::nullopt;
    }
    if (!SafeWord(f[5])) return std::nullopt;
    ThemeDot d;
    d.r = static_cast<int>(std::clamp(v[0], 0.f, 255.f));
    d.g = static_cast<int>(std::clamp(v[1], 0.f, 255.f));
    d.b = static_cast<int>(std::clamp(v[2], 0.f, 255.f));
    d.x = std::clamp(v[3], -1000.f, 1000.f);
    d.y = std::clamp(v[4], -1000.f, 1000.f);
    d.type = f[5];
    d.primary = f[6] == L"1";
    t.dots.push_back(std::move(d));
  }
  return t;
}

bool ThemeShouldBeDark(const Rgb& dominant, float opacity, bool system_dark) {
  // The tinted surface, then which of white / black text (at 0.9 alpha)
  // stands out more on it.
  const Rgb bg = BlendRgb(ToolbarBase(system_dark), dominant, (1.f - opacity) * 100.f);
  const Rgb white_text = BlendRgb(bg, {255, 255, 255}, 10.f);
  const Rgb black_text = BlendRgb(bg, {0, 0, 0}, 10.f);
  return Contrast(bg, white_text) > Contrast(bg, black_text);
}

Rgb ThemeAccentForUi(const Rgb& dominant, bool theme_dark, bool system_dark) {
  if (theme_dark) return dominant;
  if (dominant.r == dominant.g && dominant.g == dominant.b) return dominant;  // gh-15142
  float h, s, l;
  RgbToHsl(dominant, h, s, l);
  const float target = system_dark ? 0.62f : 0.42f;
  return HslToRgb(h / 360.f, std::min(1.f, s + 0.3f), l * 0.4f + target * 0.6f);
}

ThemePaint BuildThemePaint(const SpaceTheme& theme, bool system_dark, Rgb native_accent) {
  ThemePaint paint;
  paint.base = BrowserBase(system_dark);
  paint.grain = theme.texture;
  const auto& dots = theme.dots;
  // #getSingleRGBColor: each colour tints the toolbar base by the opacity.
  auto tint = [&](const ThemeDot& d) {
    return BlendRgb(DotColor(d), ToolbarBase(system_dark), theme.opacity * 100.f);
  };
  using Kind = GradientLayer::Kind;
  if (dots.size() == 1) {
    paint.base = tint(dots[0]);
  } else if (dots.size() == 2) {
    // linear-gradient(150deg, c0 30%, transparent 120%) over
    // linear-gradient(-30deg, c1 30%, transparent 120%).
    paint.layers.push_back({Kind::kLinear, tint(dots[1]), -30.f, 0.f, 0.f, 0.3f, 1.2f});
    paint.layers.push_back({Kind::kLinear, tint(dots[0]), 150.f, 0.f, 0.f, 0.3f, 1.2f});
  } else if (dots.size() >= 3) {
    paint.layers.push_back({Kind::kRadial, tint(dots[0]), 0.f, 0.f, 0.f, 0.1f, 0.7f});
    paint.layers.push_back({Kind::kRadial, tint(dots[1]), 0.f, 0.95f, 0.f, 0.f, 0.75f});
    paint.layers.push_back({Kind::kLinear, tint(dots[2]), -5.f, 0.f, 0.f, 0.1f, 0.8f});
  }
  // getMostDominantColor: the primary dot, else the middle one.
  const ThemeDot* dominant = nullptr;
  for (const ThemeDot& d : dots) {
    if (d.primary) {
      dominant = &d;
      break;
    }
  }
  if (!dominant && !dots.empty()) dominant = &dots[dots.size() / 2];
  if (dominant) {
    paint.explicit_mode = true;
    paint.dark = ThemeShouldBeDark(DotColor(*dominant), theme.opacity, system_dark);
    paint.accent = ThemeAccentForUi(DotColor(*dominant), paint.dark, system_dark);
  } else {
    paint.dark = system_dark;
    const Rgb native = system_dark ? BlendRgb(native_accent, {0, 0, 0}, 40.f) : native_accent;
    paint.accent = ThemeAccentForUi(native, system_dark, system_dark);
  }
  return paint;
}

Rgb ThemeColorAt(const ThemePaint& paint, float x, float y, float w, float h) {
  Rgb c = paint.base;
  for (const GradientLayer& layer : paint.layers) {
    const float a = LayerAlpha(layer, x, y, w, h);
    c = {layer.color.r * a + c.r * (1.f - a), layer.color.g * a + c.g * (1.f - a),
         layer.color.b * a + c.b * (1.f - a)};
  }
  return c;
}

}  // namespace orbit
