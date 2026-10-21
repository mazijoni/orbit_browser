#include "ui/theme.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace orbit::ui {
namespace {

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
  c.a = a;
  return c;
}

// Rotates the hue of |c| by |degrees| (HSL space), keeping lightness.
D2D1_COLOR_F RotateHue(const D2D1_COLOR_F& c, float degrees) {
  const float mx = std::max({c.r, c.g, c.b});
  const float mn = std::min({c.r, c.g, c.b});
  const float l = (mx + mn) / 2.f;
  const float d = mx - mn;
  if (d < 1e-4f) return c;
  const float s = l > 0.5f ? d / (2.f - mx - mn) : d / (mx + mn);
  float h = 0.f;
  if (mx == c.r) {
    h = (c.g - c.b) / d + (c.g < c.b ? 6.f : 0.f);
  } else if (mx == c.g) {
    h = (c.b - c.r) / d + 2.f;
  } else {
    h = (c.r - c.g) / d + 4.f;
  }
  h = std::fmod(h * 60.f + degrees + 360.f, 360.f) / 360.f;
  auto channel = [](float p, float q, float t) {
    if (t < 0.f) t += 1.f;
    if (t > 1.f) t -= 1.f;
    if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
    return p;
  };
  const float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
  const float p = 2.f * l - q;
  return D2D1::ColorF(channel(p, q, h + 1.f / 3.f), channel(p, q, h), channel(p, q, h - 1.f / 3.f),
                      c.a);
}

}  // namespace

D2D1_COLOR_F ColorFromRgb(uint32_t rgb, float alpha) {
  return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f,
                      (rgb & 0xFF) / 255.f, alpha);
}

COLORREF ToColorRef(const D2D1_COLOR_F& c) {
  auto ch = [](float v) { return static_cast<BYTE>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
  return RGB(ch(c.r), ch(c.g), ch(c.b));
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
  return D2D1::ColorF(a.r * t + b.r * (1.f - t), a.g * t + b.g * (1.f - t),
                      a.b * t + b.b * (1.f - t), a.a * t + b.a * (1.f - t));
}

D2D1_COLOR_F Over(const D2D1_COLOR_F& over, const D2D1_COLOR_F& under) {
  return WithAlpha(Mix(over, under, over.a), 1.f);
}

Palette MakePalette(bool dark, uint32_t accent_rgb) {
  Palette p;
  p.dark = dark;
  p.accent = ColorFromRgb(accent_rgb);
  // Foreground used for translucent overlays (--zen-branding-bg-reverse).
  const D2D1_COLOR_F fg = dark ? ColorFromRgb(0xFFFFFF) : ColorFromRgb(0x000000);
  if (dark) {
    p.chrome_bg = ColorFromRgb(0x17171A);  // getToolbarModifiedBaseRaw (dark)
    p.content_border = WithAlpha(fg, 0.08f);
    p.text = ColorFromRgb(0xFFFFFF, 0.9f);
    p.text_secondary = ColorFromRgb(0xFFFFFF, 0.62f);
    p.text_disabled = ColorFromRgb(0xFFFFFF, 0.3f);
    p.hover = WithAlpha(fg, 0.07f);
    p.pressed = WithAlpha(fg, 0.11f);
    p.selected = ColorFromRgb(0xFFFFFF, 0.16f);
    p.selected_shadow = ColorFromRgb(0x000000, 0.18f);
    p.element_bg = WithAlpha(fg, 0.09f);
    p.field_bg = ColorFromRgb(0x181818);
    p.field_border = WithAlpha(fg, 0.12f);
    p.separator = WithAlpha(fg, 0.1f);
    p.danger = ColorFromRgb(0xF07C7C);
    p.secure = ColorFromRgb(0xFFFFFF, 0.62f);
    p.content_bg = ColorFromRgb(0x202020);
    p.shadow = ColorFromRgb(0x000000, 0.24f);
  } else {
    p.chrome_bg = ColorFromRgb(0xF0F0F4);  // getToolbarModifiedBaseRaw (light)
    p.content_border = WithAlpha(fg, 0.08f);
    p.text = ColorFromRgb(0x000000, 0.85f);
    p.text_secondary = ColorFromRgb(0x000000, 0.58f);
    p.text_disabled = ColorFromRgb(0x000000, 0.3f);
    p.hover = WithAlpha(fg, 0.06f);
    p.pressed = WithAlpha(fg, 0.1f);
    p.selected = ColorFromRgb(0xFFFFFF, 0.85f);
    p.selected_shadow = ColorFromRgb(0x000000, 0.15f);
    p.element_bg = WithAlpha(fg, 0.06f);
    p.field_bg = ColorFromRgb(0xFFFFFF);
    p.field_border = WithAlpha(fg, 0.12f);
    p.separator = WithAlpha(fg, 0.1f);
    p.danger = ColorFromRgb(0xC43B3B);
    p.secure = ColorFromRgb(0x000000, 0.58f);
    p.content_bg = ColorFromRgb(0xFFFFFF);
    p.shadow = ColorFromRgb(0x000000, 0.2f);
  }
  p.close_hover = ColorFromRgb(0xC42B1C);
  // Pick black or white text on the accent for contrast.
  const float luma = 0.2126f * p.accent.r + 0.7152f * p.accent.g + 0.0722f * p.accent.b;
  p.accent_text = luma > 0.55f ? ColorFromRgb(0x111111) : ColorFromRgb(0xFFFFFF);
  p.focus_ring = p.accent;
  return p;
}

Backdrop MakeBackdrop(const Palette& palette, uint32_t workspace_rgb, float strength) {
  // Zen blends each theme color into the toolbar base by the theme opacity;
  // a second, hue-shifted stop gives the two-color gradient look.
  const D2D1_COLOR_F base = palette.chrome_bg;
  D2D1_COLOR_F primary = ColorFromRgb(workspace_rgb);
  if (palette.dark) primary = Mix(primary, ColorFromRgb(0x000000), 0.7f);  // keep text legible
  const D2D1_COLOR_F secondary = RotateHue(primary, 38.f);
  const float t = std::clamp(strength, 0.f, 1.f);
  return {Mix(primary, base, t), Mix(secondary, base, t * 0.55f)};
}

bool SystemPrefersDark() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  if (::RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
    return true;  // dark-first default
  }
  return value == 0;
}

}  // namespace orbit::ui
