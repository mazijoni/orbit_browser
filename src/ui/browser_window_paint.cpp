// Direct2D drawing for BrowserWindow. Everything here is in DIPs; the render
// target's DPI is kept in sync with the window's monitor. The look follows
// Zen Browser: a themed backdrop, translucent sidebar items and the web
// content in a rounded, shadowed card.

#include <algorithm>
#include <cmath>
#include <numbers>

#include "app/application.h"
#include "browser/downloads.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/log.h"
#include "core/url_fixup.h"
#include "ui/browser_window.h"
#include "ui/graphics.h"

namespace orbit {

using Microsoft::WRL::ComPtr;
using ui::metrics::kRowRadius;

namespace {

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float factor) {
  c.a *= factor;
  return c;
}

bool IsNewTabUrl(const std::wstring& url) {
  return url.empty() || url == Application::kNewTabUrl;
}

D2D1_RECT_F Center(const D2D1_RECT_F& r, float w, float h) {
  const float cx = (r.left + r.right) / 2.f;
  const float cy = (r.top + r.bottom) / 2.f;
  return {cx - w / 2.f, cy - h / 2.f, cx + w / 2.f, cy + h / 2.f};
}

void DrawArc(ID2D1RenderTarget* rt, ID2D1Brush* brush, D2D1_POINT_2F c, float radius, float start,
             float sweep, float width) {
  ComPtr<ID2D1PathGeometry> arc;
  if (FAILED(ui::Graphics::Get().d2d()->CreatePathGeometry(&arc))) return;
  ComPtr<ID2D1GeometrySink> sink;
  arc->Open(&sink);
  sink->BeginFigure({c.x + radius * std::cos(start), c.y + radius * std::sin(start)},
                    D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(
      {c.x + radius * std::cos(start + sweep), c.y + radius * std::sin(start + sweep)},
      {radius, radius}, 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
      sweep > std::numbers::pi_v<float> ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  rt->DrawGeometry(arc.Get(), brush, width);
}

}  // namespace

bool BrowserWindow::EnsureRenderTarget() {
  if (rt_) return true;
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  const auto props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
      static_cast<float>(dpi_), static_cast<float>(dpi_));
  const auto hwnd_props = D2D1::HwndRenderTargetProperties(
      hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
  if (FAILED(ui::Graphics::Get().d2d()->CreateHwndRenderTarget(props, hwnd_props, &rt_))) {
    ORBIT_LOG_ERROR("CreateHwndRenderTarget failed");
    return false;
  }
  rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  rt_->CreateSolidColorBrush(palette_.text, &brush_);
  favicons_.clear();
  return true;
}

void BrowserWindow::Paint() {
  if (!EnsureRenderTarget()) return;
  ID2D1RenderTarget* rt = rt_.Get();
  rt->BeginDraw();
  if (chrome_hidden()) {
    rt->Clear(palette_.content_bg);
  } else {
    PaintBackdrop(rt);
    PaintContentFrame(rt);
    if (toolbar_shown()) PaintToolbar(rt);
    if (sidebar_visible() && !overlay_visible_) PaintSidebar(rt);  // else PaintOverlay
    if (caption_revealed_ || toolbar_shown()) PaintCaption(rt);
  }
  const HRESULT hr = rt->EndDraw();
  UpdateAnimationTimer(::GetTickCount64());
  if (hr == D2DERR_RECREATE_TARGET) {
    space_snapshot_.Reset();  // belongs to the lost device
    rt_.Reset();
    brush_.Reset();
    grain_brush_.Reset();
    favicons_.clear();
    Invalidate();
  }
}

void BrowserWindow::PaintBackdrop(ID2D1RenderTarget* rt) {
  // During a space switch the new space's gradient fades in over the old one
  // (--zen-background-opacity 0 → 1).
  const uint64_t now = ::GetTickCount64();
  if (space_slide_.Running(now)) {
    rt->Clear(space_old_backdrop_.from);
    PaintGradient(rt, space_old_backdrop_, 1.f);
    PaintGradient(rt, backdrop_, space_slide_.Value(now));
    return;
  }
  rt->Clear(backdrop_.from);
  PaintGradient(rt, backdrop_, 1.f);
}


D2D1_COLOR_F BrowserWindow::Greyed(const D2D1_COLOR_F& c) const {
  // zen.view.grey-out-inactive-windows: an inactive window's chrome loses its
  // colour (Zen paints InactiveCaption; Orbit fades toward its neutral grey).
  if (!greyed_out()) return c;
  const float l = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
  return ui::Mix(ui::Mix(D2D1::ColorF(l, l, l, c.a), palette_.chrome_bg, 0.6f), c, 0.85f);
}

ID2D1BitmapBrush* BrowserWindow::GrainBrush(ID2D1RenderTarget* rt) {
  // .zen-browser-grain: a tiled noise texture (generated here rather than
  // Zen's grain-bg.png): light and dark specks with soft alpha.
  if (grain_brush_ && grain_rt_ == rt) return grain_brush_.Get();
  grain_brush_.Reset();
  grain_rt_ = rt;
  constexpr UINT32 kSize = 128;
  std::vector<uint32_t> pixels(kSize * kSize);
  uint32_t seed = 0x9E3779B9u;
  for (uint32_t& px : pixels) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    const float v = static_cast<float>(seed & 0xFFFF) / 65535.f;  // 0..1
    const bool light = v > 0.5f;
    const float alpha = std::fabs(v - 0.5f) * 0.6f;  // up to 0.3
    const uint32_t a = static_cast<uint32_t>(alpha * 255.f);
    const uint32_t c = light ? a : 0;  // premultiplied white or black
    px = (a << 24) | (c << 16) | (c << 8) | c;
  }
  ComPtr<ID2D1Bitmap> bitmap;
  const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(rt->CreateBitmap(D2D1::SizeU(kSize, kSize), pixels.data(), kSize * 4, &props, &bitmap)) ||
      FAILED(rt->CreateBitmapBrush(bitmap.Get(),
                                   D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP),
                                   &grain_brush_))) {
    grain_brush_.Reset();
    return nullptr;
  }
  return grain_brush_.Get();
}

void BrowserWindow::PaintThemeLayers(ID2D1RenderTarget* rt, const orbit::ThemePaint& paint,
                                     float opacity) {
  // ZenGradientGenerator.getGradient: the base, then each CSS gradient
  // (bottom to top): the colour at |solid| fading to transparent at |clear|.
  const float W = Width();
  const float H = Height();
  brush_->SetColor(Greyed(ui::ColorFromRgbF(paint.base, opacity)));
  rt->FillRectangle({0, 0, W, H}, brush_.Get());
  for (const orbit::GradientLayer& layer : paint.layers) {
    const D2D1_COLOR_F color = Greyed(ui::ColorFromRgbF(layer.color));
    D2D1_GRADIENT_STOP stops[2];
    float s0 = std::max(layer.solid, 0.f);
    float a0 = layer.solid >= 0.f ? 1.f : (layer.clear - 0.f) / (layer.clear - layer.solid);
    float s1 = std::min(layer.clear, 1.f);
    float a1 = layer.clear <= 1.f ? 0.f : (layer.clear - 1.f) / (layer.clear - layer.solid);
    stops[0] = {s0, D2D1::ColorF(color.r, color.g, color.b, a0)};
    stops[1] = {s1, D2D1::ColorF(color.r, color.g, color.b, a1)};
    ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(rt->CreateGradientStopCollection(stops, 2, &collection))) continue;
    ComPtr<ID2D1Brush> brush;
    if (layer.kind == orbit::GradientLayer::Kind::kLinear) {
      const float a = layer.angle_deg * 3.14159265f / 180.f;
      const float dx = std::sin(a), dy = -std::cos(a);
      const float length = std::fabs(W * dx) + std::fabs(H * dy);
      const D2D1_POINT_2F c{W / 2.f, H / 2.f};
      ComPtr<ID2D1LinearGradientBrush> linear;
      if (FAILED(rt->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties({c.x - dx * length / 2.f, c.y - dy * length / 2.f},
                                                  {c.x + dx * length / 2.f, c.y + dy * length / 2.f}),
              collection.Get(), &linear))) {
        continue;
      }
      brush = linear;
    } else {
      const float cx = layer.cx * W, cy = layer.cy * H;
      const float rx = std::max(cx, W - cx), ry = std::max(cy, H - cy);
      const float radius = std::sqrt(rx * rx + ry * ry);  // farthest-corner
      ComPtr<ID2D1RadialGradientBrush> radial;
      if (FAILED(rt->CreateRadialGradientBrush(
              D2D1::RadialGradientBrushProperties({cx, cy}, {0.f, 0.f}, radius, radius),
              collection.Get(), &radial))) {
        continue;
      }
      brush = radial;
    }
    brush->SetOpacity(opacity);
    rt->FillRectangle({0, 0, W, H}, brush.Get());
  }
  if (paint.grain > 0.f) {
    if (ID2D1BitmapBrush* grain = GrainBrush(rt)) {
      grain->SetOpacity(std::clamp(paint.grain, 0.f, 1.f) * opacity);
      rt->FillRectangle({0, 0, W, H}, grain);
    }
  }
}

void BrowserWindow::PaintGradient(ID2D1RenderTarget* rt, const ui::Backdrop& backdrop,
                                  float opacity) {
  if (backdrop.themed) {
    PaintThemeLayers(rt, backdrop.paint, opacity);
    return;
  }
  auto grey = [this](const D2D1_COLOR_F& c) { return Greyed(c); };
  const D2D1_GRADIENT_STOP stops[] = {{0.f, grey(backdrop.from)}, {1.f, grey(backdrop.to)}};
  ComPtr<ID2D1GradientStopCollection> collection;
  if (FAILED(rt->CreateGradientStopCollection(stops, 2, &collection))) return;
  ComPtr<ID2D1LinearGradientBrush> gradient;
  const float W = Width();
  const float H = Height();
  if (FAILED(rt->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties({0.f, 0.f}, {W, H}), collection.Get(),
          &gradient))) {
    return;
  }
  gradient->SetOpacity(opacity);
  rt->FillRectangle({0, 0, W, H}, gradient.Get());
}

void BrowserWindow::PaintSoftShadow(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius,
                                    float spread, float offset_y, float alpha) {
  // An approximation of a CSS box-shadow blur: concentric translucent
  // rounded rectangles, fading outwards.
  constexpr int kSteps = 6;
  for (int i = kSteps; i >= 1; --i) {
    const float grow = spread * static_cast<float>(i) / kSteps;
    const D2D1_RECT_F layer{r.left - grow, r.top - grow + offset_y, r.right + grow,
                            r.bottom + grow + offset_y};
    brush_->SetColor(Alpha(palette_.shadow, alpha / kSteps));
    rt->FillRoundedRectangle(D2D1::RoundedRect(layer, radius + grow, radius + grow),
                             brush_.Get());
  }
}

void BrowserWindow::PaintContentFrame(ID2D1RenderTarget* rt) {
  // The card behind the active tab's window: shows while a page has not
  // painted yet and carries Zen's --zen-big-shadow (0 3px 8px).
  const RECT c = ContentRectPx();
  D2D1_RECT_F card{dip(static_cast<float>(c.left)), dip(static_cast<float>(c.top)),
                   dip(static_cast<float>(c.right)), dip(static_cast<float>(c.bottom))};
  const float radius = ui::metrics::kContentRadius;
  const auto panes = SplitPanes();
  if (panes.empty()) {
    PaintSoftShadow(rt, card, radius, 8.f, 3.f, 1.f);
    brush_->SetColor(palette_.content_bg);
    rt->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), brush_.Get());
  } else {
    // Split view: a card per pane; the current one carries
    // --zen-active-split-outline-color (2px, outside the pane).
    for (const PaneRect& pane : panes) {
      PaintSoftShadow(rt, pane.rect, radius, 8.f, 3.f, 1.f);
      brush_->SetColor(palette_.content_bg);
      rt->FillRoundedRectangle(D2D1::RoundedRect(pane.rect, radius, radius), brush_.Get());
      if (pane.tab == active_tab()) {
        card = pane.rect;  // the progress bar follows the current pane
        brush_->SetColor(palette_.accent);
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(ui::Inset(pane.rect, -1.f, -1.f), radius + 1.f, radius + 1.f),
            brush_.Get(), 2.f);
      }
    }
  }

  // Load progress: a thin bar above the card while the active tab loads.
  Tab* tab = active_tab();
  if (tab && tab->loading()) {
    const float progress = static_cast<float>(std::clamp(tab->progress(), 0.05, 1.0));
    const float top = card.top - ui::metrics::kProgressHeight - 2.f;
    const float left = card.left + radius;
    const float right = card.right - radius;
    brush_->SetColor(Alpha(palette_.accent, 0.9f));
    rt->FillRoundedRectangle(
        D2D1::RoundedRect({left, top, left + (right - left) * progress,
                           top + ui::metrics::kProgressHeight},
                          1.f, 1.f),
        brush_.Get());
  }
}

void BrowserWindow::PaintCaption(ID2D1RenderTarget* rt) {
  using namespace ui;
  auto& g = Graphics::Get();
  struct Button {
    const D2D1_RECT_F* rect;
    HitKind kind;
    wchar_t glyph;
  };
  const Button buttons[] = {
      {&caption_min_, HitKind::kCaptionMin, glyph::kMinimize},
      {&caption_max_, HitKind::kCaptionMax,
       ::IsZoomed(hwnd_) ? glyph::kRestore : glyph::kMaximize},
      {&caption_close_, HitKind::kCaptionClose, glyph::kChromeClose},
  };
  for (const Button& b : buttons) {
    const bool hovered = hover_.kind == b.kind;
    const bool pressed = pressed_.kind == b.kind;
    D2D1_COLOR_F fg = palette_.text;
    if (b.kind == HitKind::kCaptionClose && (hovered || pressed)) {
      brush_->SetColor(pressed ? Alpha(palette_.close_hover, 0.8f) : palette_.close_hover);
      rt->FillRectangle(*b.rect, brush_.Get());
      fg = ColorFromRgb(0xFFFFFF);
    } else if (hovered || pressed) {
      brush_->SetColor(pressed ? palette_.pressed : palette_.hover);
      rt->FillRectangle(*b.rect, brush_.Get());
    }
    brush_->SetColor(fg);
    DrawGlyph(rt, g.icons_caption(), b.glyph, *b.rect, brush_.Get());
  }
}

void BrowserWindow::PaintIconButton(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, wchar_t glyph,
                                    Hit hit, bool enabled, const D2D1_COLOR_F* color) {
  if (enabled && hover_ == hit) {
    brush_->SetColor(pressed_ == hit ? palette_.pressed : palette_.hover);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(r, ui::metrics::kButtonRadius, ui::metrics::kButtonRadius),
        brush_.Get());
  }
  brush_->SetColor(!enabled ? palette_.text_disabled : color ? *color : palette_.text);
  ui::DrawGlyph(rt, ui::Graphics::Get().icons(), glyph, r, brush_.Get());
}

void BrowserWindow::PaintUrlBar(ID2D1RenderTarget* rt) {
  using namespace ui;
  if (address_rect_.right <= address_rect_.left) return;
  auto& g = Graphics::Get();
  Tab* tab = active_tab();
  const bool editing = ::GetFocus() == address_ && !popup_visible_;
  const auto field = D2D1::RoundedRect(address_rect_, metrics::kFieldRadius,
                                       metrics::kFieldRadius);
  if (editing) {
    PaintSoftShadow(rt, address_rect_, metrics::kFieldRadius, 6.f, 2.f, 0.8f);
  }
  brush_->SetColor(UrlFieldColor());
  rt->FillRoundedRectangle(field, brush_.Get());
  if (editing) {
    brush_->SetColor(palette_.field_border);
    rt->DrawRoundedRectangle(field, brush_.Get(), 1.f);
  }
  if (popup_visible_ && tab && !IsNewTabUrl(tab->url())) {
    // The EDIT is in the floating bar; keep showing where we are.
    const std::wstring shown = SidebarDisplayUrl(tab->url());
    brush_->SetColor(palette_.text_secondary);
    DrawTextIn(rt, g.text_medium(), shown,
               {site_info_.right + 2, address_rect_.top, address_rect_.right - 10,
                address_rect_.bottom},
               brush_.Get());
  }

  // Zen only shows the identity and page-action buttons while the sidebar is
  // hovered (or the field is being edited).
  const bool show_actions = sidebar_hovered_ || editing;
  if (!show_actions || !tab) return;
  wchar_t site_glyph = glyph::kSearch;
  D2D1_COLOR_F site_color = palette_.text_secondary;
  if (!address_editing_) {
    switch (tab->security()) {
      case SecurityLevel::kSecure:
        site_glyph = glyph::kLock;
        break;
      case SecurityLevel::kInsecure:
        site_glyph = glyph::kInfo;
        break;
      case SecurityLevel::kDangerous:
        site_glyph = glyph::kWarning;
        site_color = palette_.danger;
        break;
      case SecurityLevel::kInternal:
        site_glyph = glyph::kShield;
        break;
      case SecurityLevel::kNone:
        break;
    }
  }
  if (hover_.kind == HitKind::kSiteInfo) {
    brush_->SetColor(palette_.hover);
    rt->FillRoundedRectangle(D2D1::RoundedRect(site_info_, 6.f, 6.f), brush_.Get());
  }
  brush_->SetColor(site_color);
  DrawGlyph(rt, g.icons_small(), site_glyph, site_info_, brush_.Get());
  if (star_.right > star_.left && !IsNewTabUrl(tab->url())) {
    if (hover_.kind == HitKind::kStar) {
      brush_->SetColor(palette_.hover);
      rt->FillRoundedRectangle(D2D1::RoundedRect(star_, 6.f, 6.f), brush_.Get());
    }
    brush_->SetColor(bookmarked_ ? palette_.accent : palette_.text_secondary);
    DrawGlyph(rt, g.icons_small(), bookmarked_ ? glyph::kStarFilled : glyph::kStar, star_,
              brush_.Get());
  }
  if (ext_btn_.right > ext_btn_.left) {
    if (hover_.kind == HitKind::kExtensions) {
      brush_->SetColor(palette_.hover);
      rt->FillRoundedRectangle(D2D1::RoundedRect(ext_btn_, 6.f, 6.f), brush_.Get());
    }
    brush_->SetColor(palette_.text_secondary);
    DrawGlyph(rt, g.icons_small(), glyph::kExtensions, ext_btn_, brush_.Get());
  }
  if (CanCopyUrlFromButton()) {
    if (hover_.kind == HitKind::kCopyUrl) {
      brush_->SetColor(palette_.hover);
      rt->FillRoundedRectangle(D2D1::RoundedRect(copy_url_, 6.f, 6.f), brush_.Get());
    }
    brush_->SetColor(palette_.text_secondary);
    DrawGlyph(rt, g.icons_small(), glyph::kCopy, copy_url_, brush_.Get());
  }
}

void BrowserWindow::PaintToolbar(ID2D1RenderTarget* rt) {
  using namespace ui;
  Tab* tab = active_tab();
  PaintIconButton(rt, menu_, glyph::kMenu, {HitKind::kMenu, 0}, true);
  PaintIconButton(rt, toggle_, glyph::kSidebar, {HitKind::kSidebarToggle, 0}, true);
  PaintIconButton(rt, back_, glyph::kBack, {HitKind::kBack, 0},
                  tab && (tab->can_go_back() || BackClosesTab()));
  PaintIconButton(rt, forward_, glyph::kForward, {HitKind::kForward, 0},
                  tab && tab->can_go_forward());
  PaintIconButton(rt, reload_, tab && tab->loading() ? glyph::kStop : glyph::kReload,
                  {HitKind::kReload, 0}, tab != nullptr);
  PaintUrlBar(rt);
}

void BrowserWindow::PaintSidebar(ID2D1RenderTarget* rt) {
  using namespace ui;
  const float sw = SidebarPanelWidth();

  if (CompactHidesSidebar()) {
    // Compact mode: the sidebar floats in as a card over the backdrop.
    PaintSoftShadow(rt, panel_, 10.f, 10.f, 2.f, 1.2f);
    brush_->SetColor(BackdropAt(panel_.left, panel_.top));
    rt->FillRoundedRectangle(D2D1::RoundedRect(panel_, 10.f, 10.f), brush_.Get());
    brush_->SetColor(palette_.separator);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(Inset(panel_, 0.5f, 0.5f), 10.f, 10.f),
                             brush_.Get(), 1.f);
  }

  // Top row / column (in the toolbar with multiple toolbars).
  if (!multi_toolbar()) PaintToolbar(rt);
  PaintEssentials(rt);

  // The space's own part, sliding during a space switch (the Essentials grid
  // above it is the new space's at once).
  const uint64_t now = ::GetTickCount64();
  if (space_snapshot_ && space_slide_.Running(now)) {
    const D2D1_RECT_F area = SpaceArea();
    const float w = area.right - area.left;
    const float p = space_slide_.Value(now);
    rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    const float old_dx = -static_cast<float>(space_step_) * p * w;
    const D2D1_RECT_F& snap = space_snapshot_rect_;
    rt->DrawBitmap(space_snapshot_.Get(),
                   {snap.left + old_dx, snap.top, snap.right + old_dx, snap.bottom});
    D2D1_MATRIX_3X2_F transform;
    rt->GetTransform(&transform);
    rt->SetTransform(
        D2D1::Matrix3x2F::Translation(static_cast<float>(space_step_) * (1.f - p) * w, 0.f) *
        transform);
    PaintSpaceArea(rt);
    rt->SetTransform(transform);
    rt->PopAxisAlignedClip();
  } else if (const float dx = swipe_offset(); dx != 0.f) {
    // ZenSpacesSwipe: the space follows the fingers (fading as it goes).
    space_snapshot_.Reset();
    const D2D1_RECT_F area = SpaceArea();
    const float w = std::max(area.right - area.left, 1.f);
    rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    rt->PushLayer(D2D1::LayerParameters(area, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(),
                                        std::clamp(1.f - std::fabs(dx) / w, 0.2f, 1.f)),
                  nullptr);
    D2D1_MATRIX_3X2_F transform;
    rt->GetTransform(&transform);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(dx, 0.f) * transform);
    PaintSpaceArea(rt);
    rt->SetTransform(transform);
    rt->PopLayer();
    rt->PopAxisAlignedClip();
  } else {
    space_snapshot_.Reset();
    PaintSpaceArea(rt);
  }

  PaintMediaCard(rt);
  PaintFooter(rt);

  // Resize affordance (Zen's splitter: a rounded bar fading in after a short
  // hover, `transition: opacity 0.2s 0.2s`; at once while resizing).
  const bool splitter = !collapsed_ && (hover_.kind == HitKind::kSidebarEdge || resizing_sidebar_);
  splitter_fade_.Start(splitter ? 1.f : 0.f, now, AnimMs(200),
                       splitter && !resizing_sidebar_ ? AnimMs(200) : 0);
  if (resizing_sidebar_) splitter_fade_.Jump(1.f);
  if (const float a = splitter_fade_.Value(now); !collapsed_ && a > 0.01f) {
    brush_->SetColor(Alpha(palette_.accent, 0.8f * a));
    rt->FillRoundedRectangle(
        D2D1::RoundedRect({(right_side_ ? SidebarLeft() : sw) - 2.5f, metrics::kSeparation,
                           (right_side_ ? SidebarLeft() : sw) + 1.5f,
                           Height() - metrics::kSeparation},
                          2.f, 2.f),
        brush_.Get());
  }
}

D2D1_RECT_F BrowserWindow::SpaceArea() const {
  float top = list_.top;
  if (ws_indicator_.bottom > ws_indicator_.top) top = std::min(top, ws_indicator_.top);
  return {panel_.left, top, panel_.right, list_.bottom};
}

void BrowserWindow::PaintSpaceArea(ID2D1RenderTarget* rt) {
  using namespace ui;
  auto& g = Graphics::Get();
  const float sw = SidebarPanelWidth();
  const uint64_t now = ::GetTickCount64();

  // Workspace indicator.
  if (workspace_ && ws_indicator_.right > ws_indicator_.left) {
    const bool hovered = hover_.kind == HitKind::kWorkspaceIndicator ||
                         hover_.kind == HitKind::kWorkspaceActions;
    if (collapsed_) {
      if (hovered) {
        brush_->SetColor(palette_.hover);
        rt->FillRoundedRectangle(D2D1::RoundedRect(ws_indicator_, kRowRadius, kRowRadius),
                                 brush_.Get());
      }
      if (!workspace_->record().icon.empty()) {
        DrawWorkspaceIcon(rt, workspace_->record().icon, ws_indicator_, 14.f, 1.f);
      } else {
        brush_->SetColor(Alpha(palette_.text, 0.45f));
        rt->FillEllipse(D2D1::Ellipse({(ws_indicator_.left + ws_indicator_.right) / 2.f,
                                       (ws_indicator_.top + ws_indicator_.bottom) / 2.f},
                                      4.f, 4.f),
                        brush_.Get());
      }
    } else {
      if (hovered) {
        brush_->SetColor(palette_.hover);
        rt->FillRoundedRectangle(D2D1::RoundedRect(ws_indicator_, kRowRadius, kRowRadius),
                                 brush_.Get());
      }
      // .zen-current-workspace-indicator-icon (16px) before the name, when set.
      // With pinned tabs, a chevron takes the icon's place on hover and while
      // the pinned section is folded (rotate(90deg) open, 0deg folded).
      float name_left = ws_indicator_.left + 10.f;
      const bool linked = workspace_->is_linked() && workspace_->parent() &&
                          linked_back_.right > linked_back_.left;
      if (linked) {
        // A linked window: a back arrow, then "Linked to <parent>".
        if (hover_.kind == HitKind::kLinkedBack) {
          brush_->SetColor(palette_.hover);
          rt->FillRoundedRectangle(D2D1::RoundedRect(linked_back_, 6.f, 6.f), brush_.Get());
        }
        brush_->SetColor(palette_.text);
        DrawGlyph(rt, g.icons_small(), glyph::kBack, Inset(linked_back_, 5.f, 4.f), brush_.Get());
        name_left = linked_back_.right + 6.f;
      }
      const bool has_pins = workspace_->PinnedCount() > 0;
      const bool folded = PinnedSectionCollapsed();
      if (has_pins && (hovered || folded)) {
        brush_->SetColor(palette_.text_secondary);
        DrawGlyph(rt, g.icons_small(), folded ? glyph::kChevronRight : glyph::kChevronDown,
                  {name_left - 2.f, ws_indicator_.top, name_left + 16.f, ws_indicator_.bottom},
                  brush_.Get());
        name_left += 22.f;
      } else if (!workspace_->record().icon.empty()) {
        DrawWorkspaceIcon(rt, workspace_->record().icon,
                          {name_left - 2.f, ws_indicator_.top, name_left + 18.f, ws_indicator_.bottom},
                          15.f, 1.f);
        name_left += 24.f;
      }
      brush_->SetColor(palette_.text);
      if (!rename_workspace_ && linked) {
        DrawTextIn(rt, g.text_bold(), L"Linked to " + workspace_->parent()->record().name,
                   {name_left, ws_indicator_.top,
                    (hovered ? ws_actions_.left : ws_indicator_.right) - 6.f, ws_indicator_.bottom},
                   brush_.Get());
      } else if (!rename_workspace_) {
        DrawTextIn(rt, g.text_bold(), workspace_->record().name,
                   {name_left, ws_indicator_.top,
                    (hovered ? ws_actions_.left : ws_indicator_.right) - 6.f, ws_indicator_.bottom},
                   brush_.Get());
      }
      // .zen-workspaces-actions fades in on hover (opacity, 0.1s).
      ws_actions_fade_.Start(hovered ? 1.f : 0.f, now, AnimMs(100));
      if (const float a = ws_actions_fade_.Value(now); a > 0.01f) {
        const D2D1_COLOR_F color = Alpha(palette_.text_secondary, palette_.text_secondary.a * a);
        PaintIconButton(rt, ws_actions_, glyph::kMore, {HitKind::kWorkspaceActions, 0}, true,
                        &color);
      }
    }
  }

  // Tab list (only rows intersecting the viewport are drawn).
  if (workspace_) {
    rt->PushAxisAlignedClip(list_, D2D1_ANTIALIAS_MODE_ALIASED);
    const auto rows = Rows();
    const bool sidebar_focus = ::GetFocus() == hwnd_;
    // Row tween after a fold: rows that stay glide from their old offset,
    // new rows fade in and grow (clipped by the row below), and rows that
    // left fade out in the closing gap (ZenFolders, 0.18s ease-in-out).
    const bool tween = row_anim_.Running(now);
    const float p = tween ? row_anim_.Value(now) : 1.f;
    if (!tween) {
      row_from_.clear();
      row_ghosts_.clear();
    }
    std::map<RowKey, float> current_top;  // where each staying row is drawn now
    if (tween) {
      for (const Row& row : rows) {
        const RowKey key = KeyOf(row);
        if (auto it = row_from_.find(key); it != row_from_.end()) {
          current_top[key] = row.top + (it->second - row.top) * (1.f - p);
        }
      }
    }
    auto paint_row = [&](const Row& row, D2D1_RECT_F rect, float opacity, float clip_bottom,
                         bool focused) {
      if (rect.bottom < list_.top || rect.top > list_.bottom) return;
      const bool layer = opacity < 1.f || clip_bottom < list_.bottom;
      if (layer) {
        rt->PushLayer(D2D1::LayerParameters({list_.left, list_.top, list_.right,
                                             std::min(list_.bottom, clip_bottom)},
                                            nullptr, D2D1_ANTIALIAS_MODE_ALIASED,
                                            D2D1::IdentityMatrix(), opacity),
                      nullptr);
      }
      if (row.is_group) {
        PaintGroupRow(rt, row, rect, focused);
      } else if (row.linked) {
        PaintLinkedRow(rt, row, rect, focused);
      } else if (row.split) {
        PaintSplitRow(rt, row, rect, focused);
      } else {
        PaintTabRow(rt, row.tab, rect, focused, row.group_id);
      }
      if (layer) rt->PopLayer();
    };
    const float row_origin = list_.top - scroll_;
    for (size_t r = 0; r < rows.size(); ++r) {
      D2D1_RECT_F rect = RowRect(rows[r]);
      const bool focused = sidebar_focus && focus_row_ == static_cast<int>(r);
      float opacity = 1.f;
      float clip_bottom = list_.bottom;
      if (!indent_from_.empty() && indent_anim_.Running(::GetTickCount64())) {
        if (auto it = indent_from_.find(KeyOf(rows[r])); it != indent_from_.end()) {
          const float q = indent_anim_.Value(::GetTickCount64());
          Row before = rows[r];
          before.depth = it->second;
          rect.left += (RowRect(before).left - rect.left) * (1.f - q);
        }
      }
      if (tween) {
        const RowKey key = KeyOf(rows[r]);
        if (auto it = current_top.find(key); it != current_top.end()) {
          const float dy = it->second - rows[r].top;
          rect.top += dy;
          rect.bottom += dy;
        } else {
          opacity = p;
          for (size_t j = r + 1; j < rows.size(); ++j) {
            if (auto next = current_top.find(KeyOf(rows[j])); next != current_top.end()) {
              clip_bottom = row_origin + next->second;
              break;
            }
          }
        }
      }
      paint_row(rows[r], rect, opacity, clip_bottom, focused);
    }
    for (const RowGhost& ghost : row_ghosts_) {
      if (!tween) break;
      Row row = ghost.row;
      if (row.linked) continue;  // linked rows come and go with their windows
      if (!row.is_group) {
        row.tab = workspace_->IndexOf(reinterpret_cast<Tab*>(ghost.key.second));
        if (row.tab < 0) continue;  // closed meanwhile
      } else if (!workspace_->FindGroup(row.group_id)) {
        continue;
      }
      float clip_bottom = list_.bottom;
      if (auto next = current_top.find(ghost.next); next != current_top.end()) {
        clip_bottom = row_origin + next->second;
      }
      paint_row(row, RowRect(row), 1.f - p, clip_bottom, false);
    }
    // The separator and "+ New Tab" move with the pinned fold.
    const float periphery_dy = tween ? (periphery_from_ - PeripheryTop()) * (1.f - p) : 0.f;
    D2D1_MATRIX_3X2_F list_transform;
    rt->GetTransform(&list_transform);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0.f, periphery_dy) * list_transform);

    // Pinned separator, with Zen's "Clear" button while the sidebar is hovered.
    if (const float shown = PinnedSeparatorShown(); shown > 0.f) {
      const float top = list_.top + PeripheryTop() - scroll_ -
                        metrics::kPinnedSeparatorHeight * (1.f - shown) / 2.f;
      const float y = top + metrics::kPinnedSeparatorHeight / 2.f;
      const float sx = SidebarLeft();
      const float l = sx + (collapsed_ ? (sw - 40.f) / 2.f : 11.f);
      float r = collapsed_ ? l + 40.f : sx + sw - 11.f;
      const D2D1_RECT_F clear = ClearRect();
      if (sidebar_hovered_ && clear.right > clear.left && shown >= 1.f) {
        r = clear.left - 4.f;
        const bool hot = hover_.kind == HitKind::kClearTabs;
        brush_->SetColor(Alpha(palette_.text, hot ? 0.95f : 0.5f));
        DrawGlyph(rt, g.icons_small(), glyph::kArrowDown,
                  {clear.left + 2.f, clear.top, clear.left + 18.f, clear.bottom}, brush_.Get());
        DrawTextIn(rt, g.text_small(), L"Clear",
                   {clear.left + 20.f, clear.top, clear.right, clear.bottom}, brush_.Get());
      }
      // opacity runs a little ahead of the height (0.06s vs 0.08s).
      brush_->SetColor(Alpha(palette_.separator, palette_.separator.a * std::min(1.f, shown * 80.f / 60.f)));
      rt->FillRectangle({l, y - 0.5f, r, y + 0.5f}, brush_.Get());
    }

    // "+ New Tab" above the regular tabs (zen.view.show-newtab-button-top),
    // or after them; none when zen.tabs.show-newtab-vertical is off.
    if (const D2D1_RECT_F nt = NewTabRect(); nt.right > nt.left) {
      const Hit hit{HitKind::kNewTab, 0};
      if (new_tab_mode()) {
        brush_->SetColor(palette_.selected_shadow);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect({nt.left, nt.top + 0.8f, nt.right, nt.bottom + 1.2f}, kRowRadius,
                              kRowRadius),
            brush_.Get());
        brush_->SetColor(palette_.selected);
        rt->FillRoundedRectangle(D2D1::RoundedRect(nt, kRowRadius, kRowRadius), brush_.Get());
      } else if (hover_ == hit) {
        brush_->SetColor(pressed_ == hit ? palette_.pressed : palette_.hover);
        rt->FillRoundedRectangle(D2D1::RoundedRect(nt, kRowRadius, kRowRadius), brush_.Get());
      }
      const D2D1_COLOR_F muted = Alpha(palette_.text, new_tab_mode() ? 1.f : 0.7f);
      brush_->SetColor(muted);
      const float cy = (nt.top + nt.bottom) / 2.f;
      const float icon_x = collapsed_ ? (nt.left + nt.right) / 2.f - 8.f : nt.left + 10.f;
      DrawGlyph(rt, g.icons_small(), glyph::kAdd, {icon_x, cy - 8.f, icon_x + 16.f, cy + 8.f},
                brush_.Get());
      if (!collapsed_) {
        DrawTextIn(rt, g.text(), L"New Tab", {icon_x + 26.f, nt.top, nt.right - 8.f, nt.bottom},
                   brush_.Get());
      }
    }

    rt->SetTransform(list_transform);

    if (dragging_tab_ && drop_index_ >= 0 && !rows.empty()) {
      // Zen's drag indicator: a line with a small ring at its start.
      const float y = drop_index_ < static_cast<int>(rows.size())
                          ? RowRect(rows[static_cast<size_t>(drop_index_)]).top - 2.f
                          : RowRect(rows.back()).bottom + 2.f;
      const D2D1_COLOR_F c = Mix(palette_.accent, palette_.text, 0.5f);
      brush_->SetColor(c);
      const float sx = SidebarLeft();
      rt->DrawEllipse(D2D1::Ellipse({sx + 10.f, y}, 3.f, 3.f), brush_.Get(), 2.f);
      rt->FillRoundedRectangle(D2D1::RoundedRect({sx + 15.f, y - 1.f, sx + sw - 8.f, y + 1.f}, 1.f, 1.f),
                               brush_.Get());
    }
    rt->PopAxisAlignedClip();
  }
}

void BrowserWindow::DrawWorkspaceIcon(ID2D1RenderTarget* rt, const std::wstring& icon,
                                      const D2D1_RECT_F& r, float size, float opacity) {
  // Emoji draw in color (D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT); other
  // characters use the text color.
  using Microsoft::WRL::ComPtr;
  auto& g = ui::Graphics::Get();
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(g.dwrite()->CreateTextFormat(g.ui_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          size, L"", &format))) {
    return;
  }
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  D2D1_COLOR_F c = palette_.text;
  c.a *= opacity;
  brush_->SetColor(c);
  rt->DrawText(icon.c_str(), static_cast<UINT32>(icon.size()), format.Get(), r, brush_.Get(),
               static_cast<D2D1_DRAW_TEXT_OPTIONS>(D2D1_DRAW_TEXT_OPTIONS_CLIP | 4 /* ENABLE_COLOR_FONT */));
}

void BrowserWindow::PaintEssentials(ID2D1RenderTarget* rt) {
  // .tabbrowser-tab[zen-essential]: icon-only tiles. Unselected tiles use
  // --zen-toolbar-element-bg; the selected one shows its favicon blurred
  // behind an inner fill (zen.theme.essentials-favicon-bg).
  using namespace ui;
  const Workspace* ess = essentials();
  if (!ess) return;
  const auto rects = EssentialRects();
  constexpr float kRadius = 10.f;
  for (size_t i = 0; i < rects.size() && i < ess->tabs().size(); ++i) {
    Tab* tab = ess->tabs()[i].get();
    const D2D1_RECT_F r = rects[i];
    const bool active = tab == active_tab() && !new_tab_mode();
    const bool hovered = hover_.kind == HitKind::kEssential && hover_.index == static_cast<int>(i);
    ID2D1Bitmap* icon_bitmap = FaviconBitmap(tab);
    if (active) {
      Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clip;
      ui::Graphics::Get().d2d()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, kRadius, kRadius),
                                                                &clip);
      if (clip && icon_bitmap) {
        // Approximates the 20px blur: the favicon scaled far up, faint.
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
        const float cx = (r.left + r.right) / 2.f, cy = (r.top + r.bottom) / 2.f;
        const float big = std::max(r.right - r.left, r.bottom - r.top) * 2.f;
        rt->DrawBitmap(icon_bitmap, {cx - big / 2.f, cy - big / 2.f, cx + big / 2.f, cy + big / 2.f},
                       0.55f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        rt->PopLayer();
      }
      // --zen-essential-tab-selected-bg, 2px inside the tile.
      D2D1_COLOR_F inner;
      if (palette_.dark) {
        const D2D1_COLOR_F primary =
            workspace_ ? Mix(ColorFromRgb(workspace_->record().color), ColorFromRgb(0), 0.6f)
                       : ColorFromRgb(0x444040);
        inner = Mix(primary, ColorFromRgb(0x848484), 0.6f);
        inner.a = 0.85f;
      } else {
        inner = ColorFromRgb(0xFFFFFF, 0.85f);
      }
      if (hovered) inner.a -= 0.05f;
      brush_->SetColor(inner);
      rt->FillRoundedRectangle(D2D1::RoundedRect(Inset(r, 2.f, 2.f), kRadius - 2.f, kRadius - 2.f),
                               brush_.Get());
    } else {
      D2D1_COLOR_F bg = palette_.element_bg;
      if (hovered) {
        bg = palette_.dark ? ColorFromRgb(0xFFFFFF, 0.2f) : ColorFromRgb(0x000000, 0.1f);
      }
      brush_->SetColor(bg);
      rt->FillRoundedRectangle(D2D1::RoundedRect(r, kRadius, kRadius), brush_.Get());
    }
    const float cx = (r.left + r.right) / 2.f, cy = (r.top + r.bottom) / 2.f;
    const D2D1_RECT_F icon{cx - 8.f, cy - 8.f, cx + 8.f, cy + 8.f};
    const bool sleeping = tab->is_discarded() || tab->load_state() == Tab::LoadState::kUnloaded;
    if (tab->loading()) {
      const float start = spinner_phase_ * 2.f * std::numbers::pi_v<float>;
      brush_->SetColor(Alpha(palette_.text, 0.75f));
      DrawArc(rt, brush_.Get(), {cx, cy}, 6.5f, start, 4.2f, 1.6f);
    } else if (icon_bitmap) {
      rt->DrawBitmap(icon_bitmap, icon, sleeping ? 0.5f : 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      brush_->SetColor(Alpha(palette_.text_secondary, sleeping ? 0.5f : 1.f));
      DrawGlyph(rt, Graphics::Get().icons_small(), glyph::kGlobe, icon, brush_.Get());
    }
  }
}

void BrowserWindow::PaintMediaCard(ID2D1RenderTarget* rt) {
  // .zen-media-card: title and artist, a thin progress bar, then the focus
  // (note), play/pause and mute buttons; a close button on hover.
  using namespace ui;
  if (!media_tab_ || media_card_.right <= media_card_.left) return;
  auto& g = Graphics::Get();
  Tab::MediaState m = media_tab_->media();
  // Pages without MediaSession metadata or a title (a media file) show the
  // tab's own label and site.
  if (m.title.empty()) m.title = media_tab_->DisplayTitle();
  if (m.artist.empty()) m.artist = SidebarDisplayUrl(media_tab_->url());
  const D2D1_RECT_F& r = media_card_;
  brush_->SetColor(palette_.element_bg);
  rt->FillRoundedRectangle(D2D1::RoundedRect(r, 10.f, 10.f), brush_.Get());
  const bool hovered = hover_.kind == HitKind::kMediaFocus || hover_.kind == HitKind::kMediaPlayPause ||
                       hover_.kind == HitKind::kMediaMute || hover_.kind == HitKind::kMediaClose ||
                       hover_.kind == HitKind::kMediaSeek;
  brush_->SetColor(palette_.text);
  DrawTextIn(rt, g.text_bold(), m.title, {r.left + 10.f, r.top + 6.f, r.right - (hovered ? 28.f : 10.f), r.top + 24.f},
             brush_.Get());
  brush_->SetColor(palette_.text_secondary);
  DrawTextIn(rt, g.text_small(), m.artist, {r.left + 10.f, r.top + 23.f, r.right - 10.f, r.top + 38.f}, brush_.Get());
  if (hovered) {
    brush_->SetColor(hover_.kind == HitKind::kMediaClose ? palette_.text : palette_.text_secondary);
    DrawGlyph(rt, g.icons_small(), glyph::kClose, {r.right - 24.f, r.top + 6.f, r.right - 8.f, r.top + 22.f},
              brush_.Get());
  }
  // Progress.
  const float bar_y = r.bottom - 36.f;
  brush_->SetColor(Alpha(palette_.text, 0.15f));
  rt->FillRoundedRectangle(D2D1::RoundedRect({r.left + 10.f, bar_y, r.right - 10.f, bar_y + 3.f}, 1.5f, 1.5f),
                           brush_.Get());
  if (m.duration > 0.0) {
    const float f = static_cast<float>(std::clamp(m.time / m.duration, 0.0, 1.0));
    brush_->SetColor(palette_.accent);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect({r.left + 10.f, bar_y, r.left + 10.f + (r.right - r.left - 20.f) * f, bar_y + 3.f}, 1.5f, 1.5f),
        brush_.Get());
  }
  // Buttons.
  const float cy = r.bottom - 16.f;
  auto button = [&](HitKind kind, wchar_t glyph_char, float cx) {
    const D2D1_RECT_F b{cx - 13.f, cy - 13.f, cx + 13.f, cy + 13.f};
    if (hover_.kind == kind) {
      brush_->SetColor(palette_.hover);
      rt->FillRoundedRectangle(D2D1::RoundedRect(b, 6.f, 6.f), brush_.Get());
    }
    brush_->SetColor(palette_.text);
    DrawGlyph(rt, g.icons_small(), glyph_char, Inset(b, 5.f, 5.f), brush_.Get());
  };
  button(HitKind::kMediaFocus, 0xEC4F /* MusicNote */, r.left + 18.f);
  button(HitKind::kMediaPlayPause,
         m.state == Tab::MediaState::State::kPlaying ? 0xE769 /* Pause */ : 0xE768 /* Play */,
         (r.left + r.right) / 2.f);
  button(HitKind::kMediaMute, media_tab_->muted() ? 0xE74F /* Mute */ : 0xE767 /* Volume */, r.right - 18.f);
}

void BrowserWindow::PaintFooter(ID2D1RenderTarget* rt) {
  using namespace ui;
  // zen-library-button; the download ring below stays on it.
  PaintIconButton(rt, downloads_, glyph::kLibrary, {HitKind::kDownloads, 0}, true);
  // Download activity: a ring around the downloads icon showing progress.
  if (Application::Get()->downloads().ActiveCount(profile_) > 0) {
    const D2D1_POINT_2F c{(downloads_.left + downloads_.right) / 2.f,
                          (downloads_.top + downloads_.bottom) / 2.f};
    brush_->SetColor(Alpha(palette_.accent, 0.25f));
    rt->DrawEllipse(D2D1::Ellipse(c, 13.f, 13.f), brush_.Get(), 2.f);
    const int pct = Application::Get()->downloads().ActivePercent(profile_);
    if (pct > 0) {
      const float sweep = std::min(pct, 99) / 100.f * 2.f * std::numbers::pi_v<float>;
      brush_->SetColor(palette_.accent);
      DrawArc(rt, brush_.Get(), c, 13.f, -std::numbers::pi_v<float> / 2.f, sweep, 2.f);
    }
  }

  // Workspace switcher: one dot per space (Zen's icon-less workspaces).
  const std::vector<Workspace*> workspaces = Spaces();
  const auto& profiles = Application::Get()->profiles();
  const Profile* default_profile = profiles.empty() ? nullptr : profiles.front().get();
  for (size_t i = 0; i < workspace_dots_.size(); ++i) {
    const size_t w = static_cast<size_t>(first_dot_workspace_) + i;
    if (w >= workspaces.size()) break;
    const D2D1_RECT_F r = workspace_dots_[i];
    const bool active = workspaces[w] == workspace_;
    const bool hovered = hover_.kind == HitKind::kWorkspace &&
                         hover_.index == static_cast<int>(w);
    const uint64_t now = ::GetTickCount64();
    orbit::Transition& fade = footer_fades_[static_cast<int>(w)];
    fade.Start(hovered ? 1.f : 0.f, now, AnimMs(200));
    if (const float a = fade.Value(now); a > 0.01f) {
      brush_->SetColor(Alpha(palette_.element_bg, palette_.element_bg.a * a));
      rt->FillRoundedRectangle(D2D1::RoundedRect(r, metrics::kButtonRadius,
                                                 metrics::kButtonRadius),
                               brush_.Get());
    }
    const D2D1_POINT_2F c{(r.left + r.right) / 2.f, (r.top + r.bottom) / 2.f};
    const std::wstring& icon = workspaces[w]->record().icon;
    if (workspaces[w]->is_linked()) {
      brush_->SetColor(Alpha(palette_.text, active ? 0.85f : hovered ? 0.6f : 0.4f));
      DrawGlyph(rt, Graphics::Get().icons_small(), glyph::kOpenInNew, Inset(r, 8.f, 8.f), brush_.Get());
    } else if (!icon.empty()) {
      // .zen-workspace-icon: 12px; inactive buttons are dimmed (opacity 0.7).
      DrawWorkspaceIcon(rt, icon, r, 12.f, active || hovered ? 1.f : 0.6f);
    } else {
      brush_->SetColor(Alpha(palette_.text, active ? 0.85f : hovered ? 0.5f : 0.3f));
      rt->FillEllipse(D2D1::Ellipse(c, 4.f, 4.f), brush_.Get());
    }
    // A space of another profile carries that profile's colour, as Zen marks
    // a container space with the container's colour.
    if (const Profile* p = workspaces[w]->profile(); default_profile && p != default_profile && !p->is_private()) {
      const D2D1_POINT_2F dot{r.right - 6.f, r.bottom - 6.f};
      brush_->SetColor(BackdropAt(dot.x, dot.y));
      rt->FillEllipse(D2D1::Ellipse(dot, 3.5f, 3.5f), brush_.Get());
      brush_->SetColor(ColorFromRgb(p->record().color));
      rt->FillEllipse(D2D1::Ellipse(dot, 2.5f, 2.5f), brush_.Get());
    }
  }

  PaintIconButton(rt, create_new_, glyph::kAdd, {HitKind::kCreateNew, 0}, true);
}

void BrowserWindow::PaintGroupRow(ID2D1RenderTarget* rt, const Row& row,
                                  const D2D1_RECT_F& rect, bool focused) {
  using namespace ui;
  auto& g = Graphics::Get();
  const TabGroupRecord* group = workspace_->FindGroup(row.group_id);
  if (!group) return;
  const int index = GroupIndex(row.group_id);
  const bool hovered = hover_.kind == HitKind::kGroup && hover_.index == index;
  if (hovered) {
    brush_->SetColor(palette_.hover);
    rt->FillRoundedRectangle(D2D1::RoundedRect(rect, kRowRadius, kRowRadius), brush_.Get());
  }
  if (focused) {
    brush_->SetColor(palette_.focus_ring);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(Inset(rect, 0.75f, 0.75f), kRowRadius, kRowRadius),
                             brush_.Get(), 1.5f);
  }
  // Zen folders: a folder icon (open while expanded) and the folder's name.
  const float cy = (rect.top + rect.bottom) / 2.f;
  const float icon_x = collapsed_ ? (rect.left + rect.right) / 2.f - 8.f : rect.left + 10.f;
  const D2D1_RECT_F icon{icon_x, cy - 8.f, icon_x + 16.f, cy + 8.f};
  if (!group->icon.empty()) {
    DrawWorkspaceIcon(rt, group->icon, {icon.left - 2.f, icon.top - 2.f, icon.right + 2.f, icon.bottom + 2.f},
                      15.f, 1.f);
  } else {
    brush_->SetColor(Mix(ColorFromRgb(group->color), palette_.text, 0.35f));
    DrawGlyph(rt, g.icons(), group->collapsed ? glyph::kFolder : glyph::kFolderOpen, icon,
              brush_.Get());
  }
  if (collapsed_) return;
  // A live folder that failed shows its error where Zen puts it (the
  // folder's button): a warning and the reason; clicking retries.
  const D2D1_RECT_F err = LiveErrorRect(row);
  const bool has_error = err.right > err.left;
  brush_->SetColor(palette_.text);
  DrawTextIn(rt, g.text(), group->name,
             {icon.right + 10.f, rect.top, has_error ? err.left - 4.f : rect.right - 34.f, rect.bottom},
             brush_.Get());
  if (has_error) {
    const auto* e = Application::Get()->live_folders().Find(group->id);
    if (hover_.kind == HitKind::kLiveError && hover_.index == index) {
      brush_->SetColor(palette_.hover);
      rt->FillRoundedRectangle(D2D1::RoundedRect(err, 5.f, 5.f), brush_.Get());
    }
    const bool icon_only = err.right - err.left < 30.f;
    const float wx = icon_only ? (err.left + err.right) / 2.f - 6.f : err.left + 4.f;
    const D2D1_RECT_F warn{wx, cy - 6.f, wx + 12.f, cy + 6.f};
    brush_->SetColor(palette_.danger);
    DrawGlyph(rt, g.icons_small(), glyph::kWarning, warn, brush_.Get());
    if (icon_only) return;
    brush_->SetColor(palette_.text_secondary);
    DrawTextIn(rt, g.text_small(), e ? LiveErrorLabel(e->state.last_error) : L"",
               {warn.right + 5.f, rect.top, err.right - 4.f, rect.bottom}, brush_.Get());
    return;
  }
  if (group->collapsed) {
    brush_->SetColor(palette_.text_secondary);
    DrawTextIn(rt, g.text_small(), std::to_wstring(workspace_->GroupSize(group->id)),
               {rect.right - 30.f, rect.top, rect.right - 6.f, rect.bottom}, brush_.Get());
  }
}

void BrowserWindow::PaintLinkedRow(ID2D1RenderTarget* rt, const Row& row,
                                   const D2D1_RECT_F& rect, bool focused) {
  // The parent's view of a linked window: an always-collapsed "temp folder"
  // with a window icon, the space's name and how many tabs it holds.
  using namespace ui;
  auto& g = Graphics::Get();
  const auto children = LinkedChildren();
  const int index = LinkedIndex(row.linked);
  if (index < 0) return;
  const Workspace* ls = children[static_cast<size_t>(index)];
  const bool hovered = (hover_.kind == HitKind::kLinked && hover_.index == index) ||
                       (dragging_tab_ && drop_linked_ == index);
  if (hovered) {
    brush_->SetColor(dragging_tab_ ? Alpha(palette_.accent, 0.25f) : palette_.hover);
    rt->FillRoundedRectangle(D2D1::RoundedRect(rect, kRowRadius, kRowRadius), brush_.Get());
  }
  if (focused) {
    brush_->SetColor(palette_.focus_ring);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(Inset(rect, 0.75f, 0.75f), kRowRadius, kRowRadius),
                             brush_.Get(), 1.5f);
  }
  const float cy = (rect.top + rect.bottom) / 2.f;
  const float icon_x = collapsed_ ? (rect.left + rect.right) / 2.f - 8.f : rect.left + 10.f;
  const D2D1_RECT_F icon{icon_x, cy - 8.f, icon_x + 16.f, cy + 8.f};
  brush_->SetColor(Mix(ColorFromRgb(ls->record().color), palette_.text, 0.35f));
  DrawGlyph(rt, g.icons(), glyph::kOpenInNew, icon, brush_.Get());
  if (collapsed_) return;
  int n = 0;
  for (const auto& t : ls->tabs()) n += !t->empty_placeholder();
  brush_->SetColor(palette_.text);
  DrawTextIn(rt, g.text(), ls->record().name, {icon.right + 10.f, rect.top, rect.right - 34.f, rect.bottom},
             brush_.Get());
  brush_->SetColor(palette_.text_secondary);
  DrawTextIn(rt, g.text_small(), std::to_wstring(n), {rect.right - 30.f, rect.top, rect.right - 6.f, rect.bottom},
             brush_.Get());
}

void BrowserWindow::PaintSplitRow(ID2D1RenderTarget* rt, const Row& row,
                                  const D2D1_RECT_F& rect, bool focused) {
  // tab-group[split-view-group]: one row, the members side by side with a
  // 1px divider; the row is selected while its split is on screen, and the
  // current member gets --zen-split-view-active-tab-bg.
  using namespace ui;
  const int n = SplitMemberCount(row);
  if (n <= 0) return;
  const int active_index = workspace_->IndexOf(active_tab()) - row.tab;
  const bool on_screen = !active_essential_ && active_index >= 0 && active_index < n &&
                         !new_tab_mode();
  const bool hovered = hover_.kind == HitKind::kTab && hover_.index >= row.tab &&
                       hover_.index < row.tab + n;
  if (on_screen) {
    brush_->SetColor(palette_.selected_shadow);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect({rect.left, rect.top + 0.8f, rect.right, rect.bottom + 1.2f}, kRowRadius,
                          kRowRadius),
        brush_.Get());
    brush_->SetColor(Mix(palette_.selected, Alpha(palette_.accent, palette_.selected.a), 0.95f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(rect, kRowRadius, kRowRadius), brush_.Get());
  } else if (hovered) {
    brush_->SetColor(palette_.hover);
    rt->FillRoundedRectangle(D2D1::RoundedRect(rect, kRowRadius, kRowRadius), brush_.Get());
  }
  if (focused) {
    brush_->SetColor(palette_.focus_ring);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(Inset(rect, 0.75f, 0.75f), kRowRadius, kRowRadius),
                             brush_.Get(), 1.5f);
  }
  if (collapsed_) {
    // Collapsed sidebar: the current (or first) member, outlined.
    painting_split_segment_ = true;
    PaintTabRow(rt, row.tab + (on_screen ? active_index : 0), rect, false, row.group_id);
    painting_split_segment_ = false;
    brush_->SetColor(Alpha(palette_.text, 0.35f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(Inset(rect, 1.f, 1.f), kRowRadius, kRowRadius),
                             brush_.Get(), 2.f);
    return;
  }
  const float w = (rect.right - rect.left) / static_cast<float>(n);
  for (int k = 0; k < n; ++k) {
    const D2D1_RECT_F seg{rect.left + w * static_cast<float>(k) + 2.f, rect.top + 3.f,
                          rect.left + w * static_cast<float>(k + 1) - 2.f, rect.bottom - 3.f};
    painting_split_segment_ = true;
    PaintTabRow(rt, row.tab + k, seg, false, row.group_id);
    painting_split_segment_ = false;
    if (k + 1 < n) {
      const float x = rect.left + w * static_cast<float>(k + 1);
      brush_->SetColor(Alpha(palette_.text, 0.2f));
      rt->FillRectangle({x - 0.5f, rect.top + 9.f, x + 0.5f, rect.bottom - 9.f}, brush_.Get());
    }
  }
}

void BrowserWindow::PaintTabRow(ID2D1RenderTarget* rt, int index, const D2D1_RECT_F& row,
                                bool focused, int64_t group_id) {
  using namespace ui;
  auto& g = Graphics::Get();
  (void)group_id;  // folder membership is shown by indentation, as in Zen
  Tab* tab = workspace_->tabs()[static_cast<size_t>(index)].get();
  const bool active = tab == active_tab() && !new_tab_mode();
  const bool hovered = (hover_.kind == HitKind::kTab || hover_.kind == HitKind::kTabClose ||
                        hover_.kind == HitKind::kTabResetPin) &&
                       hover_.index == index;
  const bool reset_hot = hover_.kind == HitKind::kTabResetPin && hover_.index == index;
  const bool pin_changed = tab->pin_changed() && !collapsed_;
  const bool pressed = (pressed_.kind == HitKind::kTab) && pressed_.index == index;
  const bool drag_source = dragging_tab_ && drag_from_ == index;

  // Zen's active-tab press scale (0.98).
  D2D1_RECT_F bg = row;
  if (pressed && !dragging_tab_) {
    const float dx = (row.right - row.left) * 0.01f;
    const float dy = (row.bottom - row.top) * 0.01f;
    bg = Inset(row, dx, dy);
  }
  if (painting_split_segment_) {
    // Inside a split row: only the current member has a background.
    if (active && !collapsed_) {
      brush_->SetColor(Alpha(palette_.element_bg, palette_.element_bg.a * 1.6f));
      rt->FillRoundedRectangle(D2D1::RoundedRect(bg, kRowRadius - 2.f, kRowRadius - 2.f),
                               brush_.Get());
    }
  } else if (active) {
    brush_->SetColor(palette_.selected_shadow);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect({bg.left, bg.top + 0.8f, bg.right, bg.bottom + 1.2f}, kRowRadius,
                          kRowRadius),
        brush_.Get());
    brush_->SetColor(Mix(palette_.selected, Alpha(palette_.accent, palette_.selected.a), 0.95f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(bg, kRowRadius, kRowRadius), brush_.Get());
  } else if (hovered || drag_source) {
    brush_->SetColor(drag_source || pressed ? palette_.pressed : palette_.hover);
    rt->FillRoundedRectangle(D2D1::RoundedRect(bg, kRowRadius, kRowRadius), brush_.Get());
  }
  if (focused) {
    brush_->SetColor(palette_.focus_ring);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(Inset(row, 0.75f, 0.75f), kRowRadius, kRowRadius),
                             brush_.Get(), 1.5f);
  }

  // Favicon / spinner. Sleeping (pending) tabs are dimmed like Zen's.
  const float cy = (row.top + row.bottom) / 2.f;
  const float icon_x = collapsed_ ? (row.left + row.right) / 2.f - 8.f
                                   : row.left + (painting_split_segment_ ? 6.f : 10.f);
  const D2D1_RECT_F icon{icon_x, cy - 8.f, icon_x + 16.f, cy + 8.f};
  const bool sleeping = tab->is_discarded() || tab->load_state() == Tab::LoadState::kUnloaded;
  const float dim = sleeping ? 0.5f : 1.f;
  if (reset_hot) {
    // .tab-reset-pin-button:hover: the pinned page's icon replaces the
    // current one, on a hover background over the left part of the row.
    brush_->SetColor(Alpha(palette_.text, pressed_ == hover_ ? 0.2f : 0.1f));
    rt->FillRoundedRectangle(D2D1::RoundedRect({row.left + 2.f, row.top + 2.f, icon.right + 9.f,
                                                row.bottom - 2.f},
                                               kRowRadius - 2.f, kRowRadius - 2.f),
                             brush_.Get());
    if (ID2D1Bitmap* original = PinnedFaviconBitmap(tab)) {
      rt->DrawBitmap(original, icon, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      brush_->SetColor(palette_.text);
      DrawGlyph(rt, g.icons_small(), 0xE72C /* Refresh */, icon, brush_.Get());
    }
  } else if (tab->loading()) {
    const float start = spinner_phase_ * 2.f * std::numbers::pi_v<float>;
    brush_->SetColor(Alpha(palette_.text, 0.75f));
    DrawArc(rt, brush_.Get(), {icon_x + 8.f, cy}, 6.5f, start, 4.2f, 1.6f);
  } else if (ID2D1Bitmap* bitmap = FaviconBitmap(tab)) {
    rt->DrawBitmap(bitmap, icon, dim, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  } else {
    // Zen's blank favicon: a soft rounded square.
    brush_->SetColor(Alpha(palette_.element_bg, 1.4f * dim));
    rt->FillRoundedRectangle(D2D1::RoundedRect(icon, 4.f, 4.f), brush_.Get());
    brush_->SetColor(Alpha(palette_.text_secondary, dim));
    DrawGlyph(rt, g.icons_small(), tab->crashed() ? glyph::kWarning : glyph::kGlobe, icon,
              brush_.Get());
  }
  if (tab->media_capture()) {
    brush_->SetColor(palette_.danger);
    rt->FillEllipse(D2D1::Ellipse({icon.right, icon.top + 1.f}, 3.f, 3.f), brush_.Get());
  }
  if (collapsed_) return;

  // A pinned tab away from its page: a slanted divider after the icon
  // (.tab-reset-pin-button::after), hidden while the button is hovered.
  float title_left = icon.right + (painting_split_segment_ ? 6.f : 10.f);
  if (pin_changed) {
    title_left += 6.f;
    if (!reset_hot) {
      const float dx = icon.right + 8.f;
      D2D1_MATRIX_3X2_F old;
      rt->GetTransform(&old);
      rt->SetTransform(D2D1::Matrix3x2F::Rotation(15.f, {dx, cy}) * old);
      brush_->SetColor(palette_.dark ? ColorFromRgb(0xFFFFFF, 0.3f) : ColorFromRgb(0x423D3D, 0.3f));
      rt->FillRoundedRectangle(D2D1::RoundedRect({dx - 1.25f, cy - 8.f, dx + 1.25f, cy + 8.f}, 1.f, 1.f),
                               brush_.Get());
      rt->SetTransform(old);
    }
  }

  // Title (and the reset sublabel while its button is hovered).
  const bool loaded_pinned = tab->pinned() && tab->has_browser();
  const bool show_close = hovered && (!tab->pinned() || loaded_pinned) && !painting_split_segment_;
  const bool glance_chip = glance_ && glance_->parent == tab;
  const float title_right =
      row.right - (show_close || tab->muted() || glance_chip ? 30.f
                   : painting_split_segment_                 ? 4.f
                                                              : 8.f);
  brush_->SetColor(Alpha(palette_.text, dim));
  if (reset_hot) {
    DrawTextIn(rt, g.text(), tab->DisplayTitle(), {title_left, row.top, title_right, cy + 2.f},
               brush_.Get());
    brush_->SetColor(Alpha(palette_.text, 0.6f));
    DrawTextIn(rt, g.text_small(),
               ::GetKeyState(VK_CONTROL) < 0 ? L"Separate from pinned tab" : L"Back to pinned url",
               {title_left, cy, title_right, row.bottom - 2.f}, brush_.Get());
  } else if (tab != rename_tab_ && !tab->record().sublabel.empty()) {
    // zen-tab-sublabel: a live folder item's author under its title.
    DrawTextIn(rt, g.text(), tab->DisplayTitle(), {title_left, row.top, title_right, cy + 2.f},
               brush_.Get());
    brush_->SetColor(Alpha(palette_.text, 0.6f * dim));
    DrawTextIn(rt, g.text_small(), tab->record().sublabel, {title_left, cy, title_right, row.bottom - 2.f},
               brush_.Get());
  } else if (tab != rename_tab_) {
    DrawTextIn(rt, g.text(), tab->DisplayTitle(), {title_left, row.top, title_right, row.bottom},
               brush_.Get());
  }

  // A glance open from this tab: its icon at the end of the row (the glance
  // tab sits inside its parent's .tab-content in Zen).
  if (glance_ && glance_->parent == tab && !show_close) {
    const D2D1_RECT_F chip = Center({row.right - 28.f, row.top, row.right - 4.f, row.bottom}, 16.f, 16.f);
    if (ID2D1Bitmap* bitmap = FaviconBitmap(glance_->tab)) {
      rt->DrawBitmap(bitmap, chip, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      brush_->SetColor(palette_.text_secondary);
      DrawGlyph(rt, g.icons_small(), glyph::kGlobe, chip, brush_.Get());
    }
  }

  // Trailing close button (hover only; pinned tabs have none, as in Zen).
  const D2D1_RECT_F trailing = Center({row.right - 28.f, row.top, row.right - 4.f, row.bottom},
                                      24.f, 24.f);
  if (show_close) {
    const Hit close_hit{HitKind::kTabClose, index};
    if (hover_ == close_hit) {
      brush_->SetColor(Alpha(palette_.text, pressed_ == close_hit ? 0.2f : 0.1f));
      rt->FillRoundedRectangle(D2D1::RoundedRect(trailing, 6.f, 6.f), brush_.Get());
    }
    brush_->SetColor(palette_.text);
    // Pinned tabs get Zen's unload button (.tab-reset-button) instead.
    DrawGlyph(rt, g.icons_small(), tab->pinned() ? 0xE738 /* Remove */ : glyph::kClose, trailing,
              brush_.Get());
  } else if (tab->muted()) {
    brush_->SetColor(palette_.text_secondary);
    DrawGlyph(rt, g.icons_small(), 0xE74F /* Mute */, trailing, brush_.Get());
  }
}

void BrowserWindow::PaintPopup() {
  using namespace ui;
  if (!popup_) return;
  RECT rc;
  ::GetClientRect(popup_, &rc);
  if (!popup_rt_) {
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi_), static_cast<float>(dpi_));
    const auto hwnd_props = D2D1::HwndRenderTargetProperties(
        popup_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
    if (FAILED(Graphics::Get().d2d()->CreateHwndRenderTarget(props, hwnd_props, &popup_rt_))) {
      return;
    }
    popup_rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    popup_rt_->CreateSolidColorBrush(palette_.text, &popup_brush_d2d_);
  }
  ID2D1RenderTarget* rt = popup_rt_.Get();
  auto& g = Graphics::Get();
  ID2D1SolidColorBrush* brush = popup_brush_d2d_.Get();
  const float w = dip(static_cast<float>(rc.right));
  const float h = dip(static_cast<float>(rc.bottom));
  const float input_h = UrlBarInputHeight();
  rt->BeginDraw();
  rt->Clear(palette_.field_bg);
  brush->SetColor(palette_.field_border);
  rt->DrawRoundedRectangle(D2D1::RoundedRect({0.5f, 0.5f, w - 0.5f, h - 0.5f}, 8.f, 8.f), brush,
                           1.f);
  brush->SetColor(palette_.text_secondary);
  const float icon_l = popup_anchor_ == PopupAnchor::kCentered ? 14.f : 8.f;
  DrawGlyph(rt, g.icons(), glyph::kSearch, {icon_l, 0.f, icon_l + 24.f, input_h}, brush);

  // The urlbar view (zen-omnibox.css .urlbarView-row).
  if (!results_.empty()) {
    brush->SetColor(palette_.separator);
    rt->FillRectangle({8.f, input_h, w - 8.f, input_h + 1.f}, brush);
    float y = input_h + 4.f;
    const size_t last = std::min(results_.size(), static_cast<size_t>(results_first_) + kVisibleOmniboxRows);
    for (size_t i = static_cast<size_t>(results_first_); i < last; ++i) {
      const OmniboxRow& row = results_[i];
      const D2D1_RECT_F r{6.f, y, w - 6.f, y + 45.f};
      const bool selected = static_cast<int>(i) == selected_result_;
      if (selected) {
        D2D1_COLOR_F bg = palette_.accent;
        bg.a = 0.6f;  // color-mix(--zen-accent-button-background, transparent 40%)
        brush->SetColor(bg);
        rt->FillRoundedRectangle(D2D1::RoundedRect(Inset(r, 0, 2.f), 8.f, 8.f), brush);
      } else if (static_cast<int>(i) == hover_result_) {
        brush->SetColor(palette_.dark ? ColorFromRgb(0xFFFFFF, 0.05f) : ColorFromRgb(0x000000, 0.05f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(Inset(r, 0, 2.f), 8.f, 8.f), brush);
      }
      const D2D1_COLOR_F fg = selected ? ColorFromRgb(0xFFFFFF)
                                       : (palette_.dark ? ColorFromRgb(0xFFFFFF, 0.7f)
                                                        : ColorFromRgb(0x000000, 0.7f));
      const D2D1_COLOR_F muted = selected ? ColorFromRgb(0xFFFFFF, 0.85f)
                                          : (palette_.dark ? ColorFromRgb(0xAAAAAA)
                                                           : ColorFromRgb(0x4F4F4F));
      // Favicon box: 16px icon with 4px padding (.urlbarView-favicon).
      const float cy = (r.top + r.bottom) / 2.f;
      const D2D1_RECT_F box{r.left + 8.f, cy - 12.f, r.left + 32.f, cy + 12.f};
      if (selected) {
        brush->SetColor(ColorFromRgb(0xFFFFFF, 0.9f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(box, 4.f, 4.f), brush);
      }
      const D2D1_RECT_F icon = Inset(box, 4.f, 4.f);
      bool drew_favicon = false;
      if (row.kind == OmniboxRow::Kind::kSwitchTab) {
        Tab* t = reinterpret_cast<Tab*>(row.tab_key);
        for (Workspace* ws : TabContainers()) {
          for (const auto& tt : ws->tabs()) {
            if (tt.get() != t || tt->favicon().bgra.empty()) continue;
            const FaviconPixels& px = tt->favicon();
            ComPtr<ID2D1Bitmap> bitmap;
            const auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(px.width),
                                                       static_cast<UINT32>(px.height)),
                                           px.bgra.data(), static_cast<UINT32>(px.width) * 4,
                                           props, &bitmap))) {
              rt->DrawBitmap(bitmap.Get(), icon, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
              drew_favicon = true;
            }
          }
        }
      }
      if (!drew_favicon) {
        wchar_t glyph_char = glyph::kGlobe;
        if (row.kind == OmniboxRow::Kind::kSearch) glyph_char = glyph::kSearch;
        if (row.kind == OmniboxRow::Kind::kBookmark) glyph_char = glyph::kStar;
        if (row.kind == OmniboxRow::Kind::kHistory) glyph_char = glyph::kHistory;
        if (row.kind == OmniboxRow::Kind::kAction) glyph_char = glyph::kForward;
        brush->SetColor(selected ? palette_.accent : fg);
        DrawGlyph(rt, g.icons_small(), glyph_char, icon, brush);
      }
      // "title — url" or "text — Search with …"; Switch to Tab sits right.
      float right = r.right - 10.f;
      if (row.kind == OmniboxRow::Kind::kSwitchTab) {
        brush->SetColor(fg);
        DrawGlyph(rt, g.icons_small(), glyph::kForward, {right - 20.f, r.top, right, r.bottom}, brush);
        right -= 26.f;
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(g.dwrite()->CreateTextLayout(row.action.c_str(),
                                                   static_cast<UINT32>(row.action.size()),
                                                   g.text_medium(), 400.f, 45.f, &layout))) {
          DWRITE_TEXT_METRICS m{};
          layout->GetMetrics(&m);
          DrawTextIn(rt, g.text_medium(), row.action, {right - m.width - 2.f, r.top, right, r.bottom},
                     brush);
          right -= m.width + 14.f;
        }
      }
      std::wstring secondary;
      if (row.kind == OmniboxRow::Kind::kSearch || row.kind == OmniboxRow::Kind::kVisit ||
          row.kind == OmniboxRow::Kind::kAction) {
        secondary = row.action;
      } else if (row.title != row.url) {
        secondary = SidebarDisplayUrl(row.url);
      }
      const float text_l = box.right + 12.f;
      ComPtr<IDWriteTextLayout> title_layout;
      float title_w = 0.f;
      if (SUCCEEDED(g.dwrite()->CreateTextLayout(row.title.c_str(),
                                                 static_cast<UINT32>(row.title.size()),
                                                 g.text_medium(), 2000.f, 45.f, &title_layout))) {
        DWRITE_TEXT_METRICS m{};
        title_layout->GetMetrics(&m);
        title_w = std::min(m.widthIncludingTrailingWhitespace, (right - text_l) * 0.65f);
      }
      brush->SetColor(fg);
      DrawTextIn(rt, g.text_medium(), row.title, {text_l, r.top, text_l + title_w + 1.f, r.bottom},
                 brush);
      if (!secondary.empty()) {
        brush->SetColor(muted);
        DrawTextIn(rt, g.text_medium(), L"\u2014 " + secondary,
                   {text_l + title_w + 8.f, r.top, right, r.bottom}, brush);
      }
      y += 45.f;
    }
  }
  if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
    popup_rt_.Reset();
    popup_brush_d2d_.Reset();
    ::InvalidateRect(popup_, nullptr, FALSE);
  }
}

ID2D1Bitmap* BrowserWindow::FaviconBitmap(Tab* tab) {
  if (tab->favicon().bgra.empty()) return nullptr;
  return BitmapFor(tab->favicon(), favicons_[tab]);
}

ID2D1Bitmap* BrowserWindow::PinnedFaviconBitmap(Tab* tab) {
  if (tab->pinned_favicon().bgra.empty()) return nullptr;
  return BitmapFor(tab->pinned_favicon(), pinned_favicons_[tab]);
}

ID2D1Bitmap* BrowserWindow::BitmapFor(const FaviconPixels& px, CachedFavicon& cached) {
  if (cached.bitmap && cached.generation == px.generation) return cached.bitmap.Get();
  cached.bitmap.Reset();
  const auto props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(rt_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(px.width),
                                           static_cast<UINT32>(px.height)),
                               px.bgra.data(), static_cast<UINT32>(px.width) * 4, props,
                               &cached.bitmap))) {
    return nullptr;
  }
  cached.generation = px.generation;
  return cached.bitmap.Get();
}

}  // namespace orbit
