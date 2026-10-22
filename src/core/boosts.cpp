#include "core/boosts.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

#include "core/strings.h"

namespace orbit {
namespace {

std::wstring Escape(const std::wstring& v) {
  std::wstring out;
  for (wchar_t ch : v) {
    if (ch == L'\\') out += L"\\\\";
    else if (ch == L'\n') out += L"\\n";
    else if (ch == L'\r') out += L"\\r";
    else out += ch;
  }
  return out;
}

std::wstring Unescape(std::wstring_view v) {
  std::wstring out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == L'\\' && i + 1 < v.size()) {
      ++i;
      out += v[i] == L'n' ? L'\n' : v[i] == L'r' ? L'\r' : v[i];
    } else {
      out += v[i];
    }
  }
  return out;
}

std::wstring Num(float v) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%.4g", v);
  return buf;
}

float ToFloat(const std::wstring& s, float fallback, float lo, float hi) {
  wchar_t* end = nullptr;
  const double v = std::wcstod(s.c_str(), &end);
  if (s.empty() || !end || *end || !std::isfinite(v)) return fallback;
  return std::clamp(static_cast<float>(v), lo, hi);
}

float HueToRgb(float p, float q, float t) {
  if (t < 0.f) t += 1.f;
  if (t > 1.f) t -= 1.f;
  if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
  if (t < 1.f / 2.f) return q;
  if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
  return p;
}

void Hsl(float h, float s, float l, float out[3]) {  // h in 0..1, results 0..1
  if (s == 0.f) {
    out[0] = out[1] = out[2] = l;
    return;
  }
  const float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
  const float p = 2.f * l - q;
  out[0] = HueToRgb(p, q, h + 1.f / 3.f);
  out[1] = HueToRgb(p, q, h);
  out[2] = HueToRgb(p, q, h - 1.f / 3.f);
}

float Wrap01(float deg) {
  float h = std::fmod(deg / 360.f, 1.f);
  return h < 0.f ? h + 1.f : h;
}

}  // namespace

std::wstring SerializeBoost(const Boost& b) {
  std::wstring out;
  auto put = [&](const wchar_t* k, const std::wstring& v) { out += k + std::wstring(L"=") + Escape(v) + L"\n"; };
  put(L"name", b.name);
  put(L"enabled", b.enabled ? L"1" : L"0");
  put(L"color", b.color ? L"1" : L"0");
  put(L"dot_angle", Num(b.dot_angle));
  put(L"dot_distance", Num(b.dot_distance));
  put(L"secondary_delta", Num(b.secondary_delta));
  put(L"brightness", Num(b.brightness));
  put(L"saturation", Num(b.saturation));
  put(L"contrast", Num(b.contrast));
  put(L"smart_invert", b.smart_invert ? L"1" : L"0");
  put(L"font", b.font_family);
  put(L"case", b.text_case);
  put(L"size", Num(b.size));
  for (const auto& z : b.zap) put(L"zap", z);
  for (const auto& f : b.focus) put(L"focus", f);
  put(L"css", b.custom_css);
  return out;
}

std::optional<Boost> ParseBoost(const std::wstring& text) {
  if (text.empty() || text.size() > 200000) return std::nullopt;
  Boost b;
  bool any = false;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find(L'\n', start);
    if (nl == std::wstring::npos) nl = text.size();
    const std::wstring_view line(text.data() + start, nl - start);
    start = nl + 1;
    const size_t eq = line.find(L'=');
    if (eq == std::wstring_view::npos) continue;
    const std::wstring key(line.substr(0, eq));
    const std::wstring v = Unescape(line.substr(eq + 1));
    any = true;
    if (key == L"name") b.name = v.substr(0, 100);
    else if (key == L"enabled") b.enabled = v == L"1";
    else if (key == L"color") b.color = v == L"1";
    else if (key == L"dot_angle") b.dot_angle = ToFloat(v, b.dot_angle, -720.f, 720.f);
    else if (key == L"dot_distance") b.dot_distance = ToFloat(v, b.dot_distance, 0.f, 1.f);
    else if (key == L"secondary_delta") b.secondary_delta = ToFloat(v, b.secondary_delta, -360.f, 360.f);
    else if (key == L"brightness") b.brightness = ToFloat(v, b.brightness, 0.f, 1.f);
    else if (key == L"saturation") b.saturation = ToFloat(v, b.saturation, 0.f, 1.f);
    else if (key == L"contrast") b.contrast = ToFloat(v, b.contrast, 0.f, 1.f);
    else if (key == L"smart_invert") b.smart_invert = v == L"1";
    else if (key == L"font") b.font_family = v.substr(0, 200);
    else if (key == L"case") b.text_case = (v == L"uppercase" || v == L"lowercase" || v == L"capitalize") ? v : L"none";
    else if (key == L"size") b.size = ToFloat(v, 1.f, 0.3f, 3.f);
    else if (key == L"zap" && IsSafeBoostSelector(v) && b.zap.size() < 500) b.zap.push_back(v);
    else if (key == L"focus" && IsSafeBoostSelector(v) && b.focus.size() < 50) b.focus.push_back(v);
    else if (key == L"css") b.custom_css = v.substr(0, 100000);
  }
  if (!any) return std::nullopt;
  return b;
}

std::wstring BoostDomain(const std::wstring& url) {
  const bool https = StartsWithNoCase(url, L"https://");
  if (!https && !StartsWithNoCase(url, L"http://")) return {};
  const size_t start = https ? 8 : 7;
  const size_t end = url.find_first_of(L"/?#", start);
  std::wstring host = ToLowerAscii(url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
  if (const size_t at = host.rfind(L'@'); at != std::wstring::npos) host = host.substr(at + 1);
  if (!host.empty() && host[0] != L'[') {
    if (const size_t colon = host.find(L':'); colon != std::wstring::npos) host.resize(colon);
  }
  if (host.rfind(L"www.", 0) == 0) host = host.substr(4);
  return host;
}

bool IsSafeBoostSelector(const std::wstring& s) {
  if (s.empty() || s.size() > 500) return false;
  if (s.find_first_of(L"{};<@\\\r\n") != std::wstring::npos) return false;
  return s.find(L"/*") == std::wstring::npos && s.find(L"*/") == std::wstring::npos;
}

BoostFilter MakeBoostFilter(const Boost& b) {
  BoostFilter f;
  const float sat = 1.f - b.saturation;
  const float light = 0.1f + 0.9f * b.brightness;
  Hsl(Wrap01(b.dot_angle), sat, light, f.accent);
  Hsl(Wrap01(b.dot_angle + b.secondary_delta), sat, light, f.complementary);
  f.blend = std::clamp(1.f - b.contrast, 0.f, 1.f);
  // Dark colours lean to the accent, light ones to the complementary one;
  // a stronger tint narrows the crossfade (zenFilterColorChannel). The
  // lightness stays near the original's.
  const float half = std::clamp(0.5f - f.blend * 0.45f, 0.05f, 0.5f);
  constexpr int kSteps = 17;
  for (int i = 0; i < kSteps; ++i) {
    const float t = static_cast<float>(i) / (kSteps - 1);
    float u = std::clamp((t - (0.5f - half)) / (2.f * half), 0.f, 1.f);
    u = u * u * (3.f - 2.f * u);
    float sel[3];
    for (int c = 0; c < 3; ++c) sel[c] = f.accent[c] + (f.complementary[c] - f.accent[c]) * u;
    // Recolour the selected accent to this lightness.
    const float mx = std::max({sel[0], sel[1], sel[2]}), mn = std::min({sel[0], sel[1], sel[2]});
    const float sl = (mx + mn) / 2.f;
    const float ss = mx == mn ? 0.f : (mx - mn) / (1.f - std::fabs(2.f * sl - 1.f));
    float hue = 0.f;
    if (mx != mn) {
      const float d = mx - mn;
      if (mx == sel[0]) hue = std::fmod((sel[1] - sel[2]) / d + 6.f, 6.f);
      else if (mx == sel[1]) hue = (sel[2] - sel[0]) / d + 2.f;
      else hue = (sel[0] - sel[1]) / d + 4.f;
      hue /= 6.f;
    }
    const float l = t + (sl - t) * f.blend * f.blend * 0.5f;
    float out[3];
    Hsl(hue, std::clamp(ss, 0.f, 1.f), std::clamp(l, 0.f, 1.f), out);
    for (int c = 0; c < 3; ++c) f.table[c].push_back(std::clamp(out[c], 0.f, 1.f));
  }
  return f;
}

std::wstring BoostFilterSvg(const Boost& b) {
  const BoostFilter f = MakeBoostFilter(b);
  std::wstring svg =
      L"<svg xmlns=\"http://www.w3.org/2000/svg\" aria-hidden=\"true\" "
      L"style=\"position:absolute;width:0;height:0;overflow:hidden\">"
      L"<filter id=\"orbit-boost-color\" color-interpolation-filters=\"sRGB\">"
      L"<feColorMatrix type=\"matrix\" result=\"l\" values=\"0.2126 0.7152 0.0722 0 0 "
      L"0.2126 0.7152 0.0722 0 0 0.2126 0.7152 0.0722 0 0 0 0 0 1 0\"/>"
      L"<feComponentTransfer in=\"l\" result=\"d\">";
  const wchar_t* funcs[] = {L"feFuncR", L"feFuncG", L"feFuncB"};
  for (int c = 0; c < 3; ++c) {
    svg += L"<" + std::wstring(funcs[c]) + L" type=\"table\" tableValues=\"";
    for (size_t i = 0; i < f.table[c].size(); ++i) svg += (i ? L" " : L"") + Num(f.table[c][i]);
    svg += L"\"/>";
  }
  svg += L"</feComponentTransfer><feComposite in=\"d\" in2=\"SourceGraphic\" operator=\"arithmetic\" k1=\"0\" k2=\"" +
         Num(f.blend) + L"\" k3=\"" + Num(1.f - f.blend) + L"\" k4=\"0\"/></filter></svg>";
  return svg;
}

std::string BoostApplyScript(const Boost* b) {
  const std::wstring css = b ? BoostCss(*b) : std::wstring();
  std::string filter = "null";
  if (b && b->enabled && b->color) {
    const BoostFilter f = MakeBoostFilter(*b);
    filter = "{blend:" + ToUtf8(Num(f.blend)) + ",tables:[";
    for (int c = 0; c < 3; ++c) {
      filter += c ? ",\"" : "\"";
      for (size_t i = 0; i < f.table[c].size(); ++i) filter += (i ? " " : "") + ToUtf8(Num(f.table[c][i]));
      filter += "\"";
    }
    filter += "]}";
  }
  return "(() => { const css = " + JsonString(css) + ", filter = " + filter + R"js(;
  const key = Symbol.for('orbit.boost');
  const st = window[key] || (window[key] = {sheet: new CSSStyleSheet(), holder: null});
  st.sheet.replaceSync(css);
  const sheets = document.adoptedStyleSheets;
  if (css && !sheets.includes(st.sheet)) document.adoptedStyleSheets = [...sheets, st.sheet];
  if (!css && sheets.includes(st.sheet)) document.adoptedStyleSheets = sheets.filter(s => s !== st.sheet);
  const place = () => {
    if (st.holder) { st.holder.remove(); st.holder = null; }
    if (!filter) return;
    const ns = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(ns, 'svg');
    svg.setAttribute('aria-hidden', 'true');
    svg.setAttribute('style', 'position:absolute;width:0;height:0;overflow:hidden');
    const f = document.createElementNS(ns, 'filter');
    f.setAttribute('id', 'orbit-boost-color');
    f.setAttribute('color-interpolation-filters', 'sRGB');
    const m = document.createElementNS(ns, 'feColorMatrix');
    m.setAttribute('type', 'matrix'); m.setAttribute('result', 'l');
    m.setAttribute('values', '0.2126 0.7152 0.0722 0 0 0.2126 0.7152 0.0722 0 0 0.2126 0.7152 0.0722 0 0 0 0 0 1 0');
    const t = document.createElementNS(ns, 'feComponentTransfer');
    t.setAttribute('in', 'l'); t.setAttribute('result', 'd');
    ['feFuncR', 'feFuncG', 'feFuncB'].forEach((n, i) => {
      const fn = document.createElementNS(ns, n);
      fn.setAttribute('type', 'table'); fn.setAttribute('tableValues', filter.tables[i]);
      t.append(fn);
    });
    const c = document.createElementNS(ns, 'feComposite');
    c.setAttribute('in', 'd'); c.setAttribute('in2', 'SourceGraphic'); c.setAttribute('operator', 'arithmetic');
    c.setAttribute('k1', '0'); c.setAttribute('k2', String(filter.blend)); c.setAttribute('k3', String(1 - filter.blend)); c.setAttribute('k4', '0');
    f.append(m, t, c); svg.append(f);
    st.holder = svg;
    (document.body || document.documentElement).append(svg);
  };
  if (document.documentElement) place(); else document.addEventListener('DOMContentLoaded', place, {once: true});
})();)js";
}

std::wstring BoostCss(const Boost& b) {
  if (!b.enabled) return {};
  std::wstring style;
  // Zen-Zaps
  std::wstring zaps;
  for (const auto& sel : b.zap) zaps += sel + L":not([zen-zap-unhide]){ display: none !important; }\n";
  if (!zaps.empty()) style += L"/* Zen-Zaps */\n" + zaps + L"\n";
  // Zen-Focus
  if (!b.focus.empty()) {
    std::wstring list;
    for (const auto& f : b.focus) list += (list.empty() ? L"" : L", ") + f;
    style += L"/* Zen-Focus */\n:is(" + list + LR"css() {
  width: fit-content !important;
  max-width: 70vw !important;
  width: 100% !important;

  html:has(&) {
    overflow: auto !important;
    height: auto !important;

    body {
      display: flex !important;
      flex-direction: column !important;
      align-items: safe center !important;
      justify-content: safe center !important;
      box-sizing: border-box !important;
      min-height: 100vh !important;
      height: auto !important;
      overflow: visible !important;
      margin: 0 !important;
      padding: 48px 0 !important;
    }
  }

  html:has(&) body *:has(&) {
    display: contents !important;
  }

  html:has(&) body *:not(&, & *, :has(&)) {
    display: none !important;
  }
}

)css";
  }
  // Text Format
  const std::wstring font = b.font_family.empty() ? L"" : L"font-family: " + b.font_family + L" !important;";
  const std::wstring text_case = b.text_case == L"none" ? L"" : L"text-transform: " + b.text_case + L" !important;";
  if (!font.empty() || !text_case.empty()) {
    style += L"/* Text Format */\nbody *:not(.google-symbols, gf-load-icon-font, mat-icon, .google-material-icons) {\n" +
             font + L"\n" + text_case + L"\n}\n";
  }
  // Size (sizeOverride; Zen sets the full zoom).
  if (std::fabs(b.size - 1.f) > 0.001f) style += L"/* Size */\nhtml { zoom: " + Num(b.size) + L" !important; }\n";
  // Colour boost and smart invert (isZenBoostsInverted): the media are
  // turned back so pictures keep their colours.
  std::wstring filter;
  if (b.smart_invert) filter += L"invert(1) hue-rotate(180deg) ";
  if (b.color) filter += L"url(#orbit-boost-color)";
  if (!filter.empty()) {
    style += L"/* Colors */\nhtml { filter: " + filter + L" !important; }\n";
    if (b.smart_invert) {
      style += L"img, video, picture, canvas, iframe, embed, object, svg image { filter: invert(1) hue-rotate(180deg) !important; }\n";
    }
  }
  // USER CSS
  if (!std::wstring(TrimWhitespace(b.custom_css)).empty()) style += L"/* USER CSS */\n" + b.custom_css + L"\n";
  return style;
}

}  // namespace orbit
