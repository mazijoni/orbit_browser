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
    if (sidebar_visible()) PaintSidebar(rt);
    if (caption_revealed_) PaintCaption(rt);
  }
  const HRESULT hr = rt->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    rt_.Reset();
    brush_.Reset();
    favicons_.clear();
    Invalidate();
  }
}

void BrowserWindow::PaintBackdrop(ID2D1RenderTarget* rt) {
  rt->Clear(backdrop_.from);
  const D2D1_GRADIENT_STOP stops[] = {{0.f, backdrop_.from}, {1.f, backdrop_.to}};
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
  const D2D1_RECT_F card{dip(static_cast<float>(c.left)), dip(static_cast<float>(c.top)),
                         dip(static_cast<float>(c.right)), dip(static_cast<float>(c.bottom))};
  const float radius = ui::metrics::kContentRadius;
  PaintSoftShadow(rt, card, radius, 8.f, 3.f, 1.f);
  brush_->SetColor(palette_.content_bg);
  rt->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), brush_.Get());

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
    std::wstring shown = tab->url();
    for (const wchar_t* scheme : {L"https://", L"http://"}) {
      if (shown.rfind(scheme, 0) == 0) shown.erase(0, wcslen(scheme));
    }
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
}

void BrowserWindow::PaintSidebar(ID2D1RenderTarget* rt) {
  using namespace ui;
  auto& g = Graphics::Get();
  const float sw = SidebarPanelWidth();

  if (compact_mode_) {
    // Compact mode: the sidebar floats in as a card over the backdrop.
    PaintSoftShadow(rt, panel_, 10.f, 10.f, 2.f, 1.2f);
    brush_->SetColor(BackdropAt(panel_.left, panel_.top));
    rt->FillRoundedRectangle(D2D1::RoundedRect(panel_, 10.f, 10.f), brush_.Get());
    brush_->SetColor(palette_.separator);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(Inset(panel_, 0.5f, 0.5f), 10.f, 10.f),
                             brush_.Get(), 1.f);
  }

  // Top row / column.
  Tab* tab = active_tab();
  PaintIconButton(rt, menu_, glyph::kMenu, {HitKind::kMenu, 0}, true);
  PaintIconButton(rt, toggle_, glyph::kSidebar, {HitKind::kSidebarToggle, 0}, true);
  PaintIconButton(rt, back_, glyph::kBack, {HitKind::kBack, 0}, tab && tab->can_go_back());
  PaintIconButton(rt, forward_, glyph::kForward, {HitKind::kForward, 0},
                  tab && tab->can_go_forward());
  PaintIconButton(rt, reload_, tab && tab->loading() ? glyph::kStop : glyph::kReload,
                  {HitKind::kReload, 0}, tab != nullptr);

  PaintUrlBar(rt);

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
      brush_->SetColor(Alpha(palette_.text, 0.45f));
      rt->FillEllipse(D2D1::Ellipse({(ws_indicator_.left + ws_indicator_.right) / 2.f,
                                     (ws_indicator_.top + ws_indicator_.bottom) / 2.f},
                                    4.f, 4.f),
                      brush_.Get());
    } else {
      if (hovered) {
        brush_->SetColor(palette_.hover);
        rt->FillRoundedRectangle(D2D1::RoundedRect(ws_indicator_, kRowRadius, kRowRadius),
                                 brush_.Get());
      }
      brush_->SetColor(palette_.text);
      DrawTextIn(rt, g.text_bold(), workspace_->record().name,
                 {ws_indicator_.left + 10.f, ws_indicator_.top,
                  (hovered ? ws_actions_.left : ws_indicator_.right) - 6.f, ws_indicator_.bottom},
                 brush_.Get());
      if (hovered) {
        PaintIconButton(rt, ws_actions_, glyph::kMore, {HitKind::kWorkspaceActions, 0}, true,
                        &palette_.text_secondary);
      }
    }
  }

  // Tab list (only rows intersecting the viewport are drawn).
  if (workspace_) {
    rt->PushAxisAlignedClip(list_, D2D1_ANTIALIAS_MODE_ALIASED);
    const auto rows = Rows();
    const bool sidebar_focus = ::GetFocus() == hwnd_;
    for (size_t r = 0; r < rows.size(); ++r) {
      const D2D1_RECT_F rect = RowRect(rows[r]);
      if (rect.bottom < list_.top || rect.top > list_.bottom) continue;
      const bool focused = sidebar_focus && focus_row_ == static_cast<int>(r);
      if (rows[r].is_group) {
        PaintGroupRow(rt, rows[r], rect, focused);
      } else {
        PaintTabRow(rt, rows[r].tab, rect, focused, rows[r].group_id);
      }
    }

    // Pinned separator, with Zen's "Clear" button while the sidebar is hovered.
    if (workspace_->PinnedCount() > 0) {
      const float top = list_.top + PeripheryTop() - scroll_;
      const float y = top + metrics::kPinnedSeparatorHeight / 2.f;
      const float l = collapsed_ ? (sw - 40.f) / 2.f : 11.f;
      float r = collapsed_ ? l + 40.f : sw - 11.f;
      const D2D1_RECT_F clear = ClearRect();
      if (sidebar_hovered_ && clear.right > clear.left) {
        r = clear.left - 4.f;
        const bool hot = hover_.kind == HitKind::kClearTabs;
        brush_->SetColor(Alpha(palette_.text, hot ? 0.95f : 0.5f));
        DrawGlyph(rt, g.icons_small(), glyph::kArrowDown,
                  {clear.left + 2.f, clear.top, clear.left + 18.f, clear.bottom}, brush_.Get());
        DrawTextIn(rt, g.text_small(), L"Clear",
                   {clear.left + 20.f, clear.top, clear.right, clear.bottom}, brush_.Get());
      }
      brush_->SetColor(palette_.separator);
      rt->FillRectangle({l, y - 0.5f, r, y + 0.5f}, brush_.Get());
    }

    // "+ New Tab" above the regular tabs (zen.view.show-newtab-button-top).
    {
      const D2D1_RECT_F nt = NewTabRect();
      const Hit hit{HitKind::kNewTab, 0};
      if (hover_ == hit) {
        brush_->SetColor(pressed_ == hit ? palette_.pressed : palette_.hover);
        rt->FillRoundedRectangle(D2D1::RoundedRect(nt, kRowRadius, kRowRadius), brush_.Get());
      }
      const D2D1_COLOR_F muted = Alpha(palette_.text, 0.7f);
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

    if (dragging_tab_ && drop_index_ >= 0 && !rows.empty()) {
      // Zen's drag indicator: a line with a small ring at its start.
      const float y = drop_index_ < static_cast<int>(rows.size())
                          ? RowRect(rows[static_cast<size_t>(drop_index_)]).top - 2.f
                          : RowRect(rows.back()).bottom + 2.f;
      const D2D1_COLOR_F c = Mix(palette_.accent, palette_.text, 0.5f);
      brush_->SetColor(c);
      rt->DrawEllipse(D2D1::Ellipse({10.f, y}, 3.f, 3.f), brush_.Get(), 2.f);
      rt->FillRoundedRectangle(D2D1::RoundedRect({15.f, y - 1.f, sw - 8.f, y + 1.f}, 1.f, 1.f),
                               brush_.Get());
    }
    rt->PopAxisAlignedClip();
  }

  PaintFooter(rt);

  // Resize affordance (Zen's splitter: a rounded bar after a short hover).
  if (!collapsed_ && (hover_.kind == HitKind::kSidebarEdge || resizing_sidebar_)) {
    brush_->SetColor(Alpha(palette_.accent, 0.8f));
    rt->FillRoundedRectangle(
        D2D1::RoundedRect({sw - 2.5f, metrics::kSeparation, sw + 1.5f,
                           Height() - metrics::kSeparation},
                          2.f, 2.f),
        brush_.Get());
  }
}

void BrowserWindow::PaintFooter(ID2D1RenderTarget* rt) {
  using namespace ui;
  PaintIconButton(rt, downloads_, glyph::kDownload, {HitKind::kDownloads, 0}, true);
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
  const auto& workspaces = profile_->workspaces();
  for (size_t i = 0; i < workspace_dots_.size(); ++i) {
    const size_t w = static_cast<size_t>(first_dot_workspace_) + i;
    if (w >= workspaces.size()) break;
    const D2D1_RECT_F r = workspace_dots_[i];
    const bool active = workspaces[w].get() == workspace_;
    const bool hovered = hover_.kind == HitKind::kWorkspace &&
                         hover_.index == static_cast<int>(w);
    if (hovered) {
      brush_->SetColor(palette_.element_bg);
      rt->FillRoundedRectangle(D2D1::RoundedRect(r, metrics::kButtonRadius,
                                                 metrics::kButtonRadius),
                               brush_.Get());
    }
    const D2D1_POINT_2F c{(r.left + r.right) / 2.f, (r.top + r.bottom) / 2.f};
    brush_->SetColor(Alpha(palette_.text, active ? 0.85f : hovered ? 0.5f : 0.3f));
    rt->FillEllipse(D2D1::Ellipse(c, 4.f, 4.f), brush_.Get());
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
  brush_->SetColor(Mix(ColorFromRgb(group->color), palette_.text, 0.35f));
  DrawGlyph(rt, g.icons(), group->collapsed ? glyph::kFolder : glyph::kFolderOpen, icon,
            brush_.Get());
  if (collapsed_) return;
  brush_->SetColor(palette_.text);
  DrawTextIn(rt, g.text(), group->name, {icon.right + 10.f, rect.top, rect.right - 34.f, rect.bottom},
             brush_.Get());
  if (group->collapsed) {
    brush_->SetColor(palette_.text_secondary);
    DrawTextIn(rt, g.text_small(), std::to_wstring(workspace_->GroupSize(group->id)),
               {rect.right - 30.f, rect.top, rect.right - 6.f, rect.bottom}, brush_.Get());
  }
}

void BrowserWindow::PaintTabRow(ID2D1RenderTarget* rt, int index, const D2D1_RECT_F& row,
                                bool focused, int64_t group_id) {
  using namespace ui;
  auto& g = Graphics::Get();
  (void)group_id;  // folder membership is shown by indentation, as in Zen
  Tab* tab = workspace_->tabs()[static_cast<size_t>(index)].get();
  const bool active = tab == active_tab();
  const bool hovered = (hover_.kind == HitKind::kTab || hover_.kind == HitKind::kTabClose) &&
                       hover_.index == index;
  const bool pressed = (pressed_.kind == HitKind::kTab) && pressed_.index == index;
  const bool drag_source = dragging_tab_ && drag_from_ == index;

  // Zen's active-tab press scale (0.98).
  D2D1_RECT_F bg = row;
  if (pressed && !dragging_tab_) {
    const float dx = (row.right - row.left) * 0.01f;
    const float dy = (row.bottom - row.top) * 0.01f;
    bg = Inset(row, dx, dy);
  }
  if (active) {
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
  const float icon_x = collapsed_ ? (row.left + row.right) / 2.f - 8.f : row.left + 10.f;
  const D2D1_RECT_F icon{icon_x, cy - 8.f, icon_x + 16.f, cy + 8.f};
  const bool sleeping = tab->is_discarded() || tab->load_state() == Tab::LoadState::kUnloaded;
  const float dim = sleeping ? 0.5f : 1.f;
  if (tab->loading()) {
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

  // Title.
  const bool show_close = hovered && !tab->pinned();
  const float title_right = row.right - (show_close || tab->muted() ? 30.f : 8.f);
  brush_->SetColor(Alpha(palette_.text, dim));
  DrawTextIn(rt, g.text(), tab->DisplayTitle(),
             {icon.right + 10.f, row.top, title_right, row.bottom}, brush_.Get());

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
    DrawGlyph(rt, g.icons_small(), glyph::kClose, trailing, brush_.Get());
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
  const float w = dip(static_cast<float>(rc.right));
  const float h = dip(static_cast<float>(rc.bottom));
  rt->BeginDraw();
  rt->Clear(palette_.field_bg);
  popup_brush_d2d_->SetColor(palette_.field_border);
  rt->DrawRoundedRectangle(D2D1::RoundedRect({0.5f, 0.5f, w - 0.5f, h - 0.5f}, 8.f, 8.f),
                           popup_brush_d2d_.Get(), 1.f);
  popup_brush_d2d_->SetColor(palette_.text_secondary);
  DrawGlyph(rt, Graphics::Get().icons(), glyph::kSearch, {14.f, 0.f, 38.f, h},
            popup_brush_d2d_.Get());
  if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
    popup_rt_.Reset();
    popup_brush_d2d_.Reset();
    ::InvalidateRect(popup_, nullptr, FALSE);
  }
}

ID2D1Bitmap* BrowserWindow::FaviconBitmap(Tab* tab) {
  const FaviconPixels& px = tab->favicon();
  if (px.bgra.empty()) return nullptr;
  CachedFavicon& cached = favicons_[tab];
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
