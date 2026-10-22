// Boosts (src/zen/boosts): per-site look tweaks — hide elements (zap),
// show one part of the page (focus), font and text case, size, a colour
// boost, smart invert and custom CSS. Platform independent, unit-tested:
// the stored form, the style sheet, and the colour filter.
//
// Zen recolours every style colour inside Gecko's style resolution (an
// Oklab duotone) and leaves images alone; Chromium has no such hook, so the
// colour boost here is an SVG duotone filter over the page, which also
// tints images.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace orbit {

struct Boost {
  std::wstring name = L"My Boost";
  // Colour boost (dotAngleDeg, secondaryDotAngleDegDelta, brightness,
  // saturation, contrast as in getEmptyBoostEntry).
  bool color = false;
  float dot_angle = 131.61f;     // primary hue, degrees
  float dot_distance = 0.91f;    // where the dot sits on the editor's disc (0..1)
  float secondary_delta = 55.f;  // complementary hue offset, degrees
  float brightness = 0.5f;
  float saturation = 0.5f;
  float contrast = 0.75f;
  bool smart_invert = false;
  std::wstring font_family;        // empty: the page's own
  std::wstring text_case = L"none";  // none / uppercase / lowercase / capitalize
  float size = 1.f;                // sizeOverride (full zoom)
  std::vector<std::wstring> zap;    // selectors hidden
  std::vector<std::wstring> focus;  // selectors kept alone
  std::wstring custom_css;
  bool enabled = true;
};

std::wstring SerializeBoost(const Boost& boost);
std::optional<Boost> ParseBoost(const std::wstring& text);

// The domain a boost is kept for ("www." dropped), or empty when the URL
// cannot be boosted (only http(s) pages: canBoostSite).
std::wstring BoostDomain(const std::wstring& url);

// A selector the zap picker may store: bounded, no braces or comment
// markers (it is placed in front of a declaration block).
bool IsSafeBoostSelector(const std::wstring& selector);

// The style sheet (ZenBoostStyles.#generateStyleString, plus the colour
// filter and smart invert): empty when the boost changes nothing.
std::wstring BoostCss(const Boost& boost);

// The colour boost's two accents (#buildBoostColor: hsl(angle, 1 -
// saturation, 0.1 + 0.9 * brightness), the second rotated by the delta),
// 0..1 channels, and the duotone's transfer tables.
struct BoostFilter {
  float accent[3] = {};
  float complementary[3] = {};
  float blend = 0.f;  // 1 - contrast: how strongly the page is tinted
  std::vector<float> table[3];  // feFuncR/G/B tableValues over luminance
};
BoostFilter MakeBoostFilter(const Boost& boost);
std::wstring BoostFilterSvg(const Boost& boost);  // an <svg> holding filter #orbit-boost-color

// The script that applies |boost| to a page (or removes a previous one when
// null): a constructed style sheet (not subject to the page's style CSP) in
// document.adoptedStyleSheets, and the colour filter built with DOM calls
// (no HTML parsing, so Trusted Types pages accept it). UTF-8. Idempotent.
std::string BoostApplyScript(const Boost* boost);

}  // namespace orbit
