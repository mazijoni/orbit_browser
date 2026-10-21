#pragma once

#include <d2d1.h>

#include <cstdint>

namespace orbit::ui {

// Shared design tokens. Sizes are in DIPs (1/96 inch); colors are sRGB.
// Values follow Zen Browser's single-toolbar layout (zen-theme.css,
// vertical-tabs.css, zen-browser-ui.css; MPL-2.0), translated to native drawing.
namespace metrics {
inline constexpr float kSidebarDefaultWidth = 220.f;
inline constexpr float kSidebarMinWidth = 163.f;   // --zen-toolbox-min-width
inline constexpr float kSidebarMaxWidth = 500.f;   // zen.view.sidebar-expanded.max-width
inline constexpr float kSidebarCompactWidth = 60.f;  // --tab-min-width + 2 * padding
inline constexpr float kToolboxPadding = 5.f;      // --zen-toolbox-padding
inline constexpr float kSeparation = 8.f;          // zen.theme.content-element-separation
inline constexpr float kContentRadius = 8.f;
inline constexpr float kTopRowHeight = 38.f;       // sidebar top buttons
inline constexpr float kCaptionHeight = 32.f;      // revealed window-control strip
inline constexpr float kCaptionButtonWidth = 46.f;
inline constexpr float kUrlBarHeight = 36.f;
inline constexpr float kWorkspaceIndicatorHeight = 44.f;
inline constexpr float kPinnedSeparatorHeight = 22.f;
inline constexpr float kFooterHeight = 42.f;
inline constexpr float kRowHeight = 38.f;      // pitch of a sidebar tab row
inline constexpr float kRowInset = 2.f;        // --tab-margin-block
inline constexpr float kRowRadius = 9.f;
inline constexpr float kButtonRadius = 6.f;    // --toolbarbutton-border-radius
inline constexpr float kFieldRadius = 9.f;
inline constexpr float kIconButton = 30.f;
inline constexpr float kPadding = 8.f;
inline constexpr float kFavicon = 16.f;
inline constexpr float kProgressHeight = 2.f;
inline constexpr float kFontSize = 13.5f;
inline constexpr float kFontSizeSmall = 11.5f;
inline constexpr float kIconSize = 15.f;
}  // namespace metrics

// Segoe Fluent Icons / Segoe MDL2 Assets code points.
namespace glyph {
inline constexpr wchar_t kBack = 0xE72B;
inline constexpr wchar_t kForward = 0xE72A;
inline constexpr wchar_t kReload = 0xE72C;
inline constexpr wchar_t kStop = 0xE711;
inline constexpr wchar_t kAdd = 0xE710;
inline constexpr wchar_t kClose = 0xE8BB;
inline constexpr wchar_t kLock = 0xE72E;
inline constexpr wchar_t kWarning = 0xE7BA;
inline constexpr wchar_t kInfo = 0xE946;
inline constexpr wchar_t kSettings = 0xE713;
inline constexpr wchar_t kDownload = 0xE896;
inline constexpr wchar_t kHistory = 0xE81C;
inline constexpr wchar_t kBookmarks = 0xE8A4;
inline constexpr wchar_t kStar = 0xE734;
inline constexpr wchar_t kStarFilled = 0xE735;
inline constexpr wchar_t kMenu = 0xE700;       // GlobalNavButton
inline constexpr wchar_t kSidebar = 0xE89F;    // DockLeft: compact mode toggle
inline constexpr wchar_t kPin = 0xE718;
inline constexpr wchar_t kSearch = 0xE721;
inline constexpr wchar_t kGlobe = 0xE774;
inline constexpr wchar_t kChevronDown = 0xE70D;
inline constexpr wchar_t kChevronRight = 0xE76C;
inline constexpr wchar_t kSleep = 0xE708;      // QuietHours (moon) – discarded tab
inline constexpr wchar_t kShield = 0xEA18;
inline constexpr wchar_t kPrivate = 0xE727;
inline constexpr wchar_t kMore = 0xE712;
inline constexpr wchar_t kFolder = 0xE8B7;
inline constexpr wchar_t kFolderOpen = 0xE838;
inline constexpr wchar_t kArrowDown = 0xE74B;
inline constexpr wchar_t kMinimize = 0xE921;   // ChromeMinimize
inline constexpr wchar_t kMaximize = 0xE922;   // ChromeMaximize
inline constexpr wchar_t kRestore = 0xE923;    // ChromeRestore
inline constexpr wchar_t kChromeClose = 0xE8BB;
}  // namespace glyph

enum class ThemeMode { kSystem = 0, kDark = 1, kLight = 2 };

struct Palette {
  bool dark = true;
  D2D1_COLOR_F chrome_bg;       // window background behind sidebar and content
  D2D1_COLOR_F content_border;  // hairline around the web content frame
  D2D1_COLOR_F text;
  D2D1_COLOR_F text_secondary;
  D2D1_COLOR_F text_disabled;
  D2D1_COLOR_F hover;           // translucent: toolbar button / tab hover
  D2D1_COLOR_F pressed;
  D2D1_COLOR_F selected;        // active tab row (translucent white)
  D2D1_COLOR_F selected_shadow;
  D2D1_COLOR_F element_bg;      // --zen-toolbar-element-bg (url bar, chips)
  D2D1_COLOR_F field_bg;        // url bar while editing
  D2D1_COLOR_F field_border;
  D2D1_COLOR_F accent;
  D2D1_COLOR_F accent_text;     // text drawn on accent
  D2D1_COLOR_F focus_ring;
  D2D1_COLOR_F separator;
  D2D1_COLOR_F danger;
  D2D1_COLOR_F secure;
  D2D1_COLOR_F content_bg;      // shown before a page paints
  D2D1_COLOR_F shadow;          // --zen-big-shadow
  D2D1_COLOR_F close_hover;     // window close button
};

Palette MakePalette(bool dark, uint32_t accent_rgb);
bool SystemPrefersDark();
COLORREF ToColorRef(const D2D1_COLOR_F& c);
D2D1_COLOR_F ColorFromRgb(uint32_t rgb, float alpha = 1.f);
// Linear blend: |t| = 1 gives |a|, 0 gives |b| (Zen's blendColors).
D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
// Composites a translucent |over| onto an opaque |under|.
D2D1_COLOR_F Over(const D2D1_COLOR_F& over, const D2D1_COLOR_F& under);

// Workspace theme: Zen's gradient generator, for a single workspace color.
// Returns the two gradient stops (top-left, bottom-right) over the palette's
// base color; |strength| 0 gives the plain base.
struct Backdrop {
  D2D1_COLOR_F from;
  D2D1_COLOR_F to;
};
Backdrop MakeBackdrop(const Palette& palette, uint32_t workspace_rgb, float strength);

// Default accent choices offered in settings (0xRRGGBB).
inline constexpr uint32_t kAccentChoices[] = {0x7C8CFF, 0x4FB3A9, 0xE0905A, 0xD46A8E,
                                              0x8F7CE6, 0x5AA0E0, 0x9CB85C, 0xC9A93C};

}  // namespace orbit::ui
