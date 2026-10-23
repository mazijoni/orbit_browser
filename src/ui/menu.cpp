#include "ui/menu.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#include "ui/graphics.h"

namespace orbit::ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kMenuClass[] = L"OrbitMenu";
constexpr float kPad = 5.f;       // inside the panel
constexpr float kRowHeight = 30.f;
constexpr float kSeparatorHeight = 9.f;
constexpr float kCheckColumn = 30.f;
constexpr float kMinWidth = 190.f;
constexpr float kMaxWidth = 460.f;
constexpr wchar_t kCheckGlyph = 0xE73E;  // CheckMark

struct Item {
  std::wstring label;
  std::wstring shortcut;
  wchar_t mnemonic = 0;
  UINT id = 0;
  bool separator = false;
  bool disabled = false;
  bool checked = false;
  HMENU submenu = nullptr;
  bool selectable() const { return !separator && !disabled && !label.empty(); }
};

struct Level {
  std::vector<Item> items;
  HWND hwnd = nullptr;
  float width = 0.f;   // DIPs
  float height = 0.f;
  int hot = -1;
  int open_child = -1;  // the item whose submenu is the next level
  ComPtr<ID2D1HwndRenderTarget> rt;
  ComPtr<ID2D1SolidColorBrush> brush;
};

struct State {
  HWND owner = nullptr;
  Palette palette;
  float scale = 1.f;  // px per DIP
  std::vector<std::unique_ptr<Level>> levels;
  bool done = false;
  UINT result = 0;
  ComPtr<IDWriteTextFormat> shortcut_format;
};

int g_depth = 0;

D2D1_COLOR_F Background(const Palette& p) {
  return p.dark ? Over(p.element_bg, p.chrome_bg) : ColorFromRgb(0xFFFFFF);
}

D2D1_COLOR_F HotColor(const Palette& p) {
  return p.dark ? ColorFromRgb(0xFFFFFF, 0.11f) : ColorFromRgb(0x000000, 0.07f);
}

std::vector<Item> ReadItems(HMENU menu) {
  std::vector<Item> out;
  const int n = ::GetMenuItemCount(menu);
  for (int i = 0; i < n; ++i) {
    wchar_t buffer[512] = {};
    MENUITEMINFOW mii{sizeof(mii)};
    mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
    mii.dwTypeData = buffer;
    mii.cch = 511;
    if (!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii)) continue;
    Item item;
    if (mii.fType & MFT_SEPARATOR) {
      // Two separators in a row, or one at an end, draw as one / none.
      if (!out.empty() && !out.back().separator) {
        item.separator = true;
        out.push_back(item);
      }
      continue;
    }
    std::wstring text = buffer;
    const size_t tab = text.find(L'\t');
    if (tab != std::wstring::npos) {
      item.shortcut = text.substr(tab + 1);
      text.resize(tab);
    }
    // "&File" marks the mnemonic; "&&" is a literal ampersand.
    for (size_t k = 0; k < text.size(); ++k) {
      if (text[k] != L'&') {
        item.label += text[k];
      } else if (k + 1 < text.size()) {
        if (text[k + 1] != L'&' && !item.mnemonic) item.mnemonic = static_cast<wchar_t>(std::towlower(text[k + 1]));
        item.label += text[++k];
      }
    }
    item.id = mii.wID;
    item.disabled = (mii.fState & (MFS_GRAYED | MFS_DISABLED)) != 0;
    item.checked = (mii.fState & MFS_CHECKED) != 0;
    item.submenu = mii.hSubMenu;
    out.push_back(std::move(item));
  }
  while (!out.empty() && out.back().separator) out.pop_back();
  return out;
}

float TextWidth(IDWriteTextFormat* format, const std::wstring& text) {
  if (text.empty()) return 0.f;
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(Graphics::Get().dwrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                                        2000.f, 100.f, &layout))) {
    return 7.f * static_cast<float>(text.size());
  }
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  return m.widthIncludingTrailingWhitespace;
}

float ItemTop(const Level& level, int index) {
  float y = kPad;
  for (int i = 0; i < index; ++i) y += level.items[static_cast<size_t>(i)].separator ? kSeparatorHeight : kRowHeight;
  return y;
}

int ItemAt(const Level& level, float y) {
  float top = kPad;
  for (size_t i = 0; i < level.items.size(); ++i) {
    const float h = level.items[i].separator ? kSeparatorHeight : kRowHeight;
    if (y >= top && y < top + h) return level.items[i].separator ? -1 : static_cast<int>(i);
    top += h;
  }
  return -1;
}

void Paint(State& st, Level& level) {
  if (!level.rt) {
    RECT rc;
    ::GetClientRect(level.hwnd, &rc);
    const float dpi = st.scale * 96.f;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                      D2D1_ALPHA_MODE_IGNORE),
                                                    dpi, dpi);
    if (FAILED(Graphics::Get().d2d()->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(level.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &level.rt))) {
      return;
    }
    level.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    level.rt->CreateSolidColorBrush(st.palette.text, &level.brush);
  }
  const Palette& p = st.palette;
  auto& g = Graphics::Get();
  ID2D1HwndRenderTarget* rt = level.rt.Get();
  ID2D1SolidColorBrush* brush = level.brush.Get();
  rt->BeginDraw();
  rt->Clear(Background(p));
  float y = kPad;
  for (size_t i = 0; i < level.items.size(); ++i) {
    const Item& item = level.items[i];
    if (item.separator) {
      brush->SetColor(p.separator);
      rt->FillRectangle({kPad + 8.f, y + kSeparatorHeight / 2.f - 0.5f, level.width - kPad - 8.f,
                         y + kSeparatorHeight / 2.f + 0.5f},
                        brush);
      y += kSeparatorHeight;
      continue;
    }
    const D2D1_RECT_F row{kPad, y, level.width - kPad, y + kRowHeight};
    const bool hot = static_cast<int>(i) == level.hot && !item.disabled;
    if (hot || static_cast<int>(i) == level.open_child) {
      brush->SetColor(HotColor(p));
      rt->FillRoundedRectangle(D2D1::RoundedRect(row, 6.f, 6.f), brush);
    }
    const D2D1_COLOR_F ink = item.disabled ? p.text_disabled : p.text;
    if (item.checked) {
      brush->SetColor(item.disabled ? p.text_disabled : p.accent);
      DrawGlyph(rt, g.icons_small(), kCheckGlyph, {row.left + 6.f, row.top, row.left + kCheckColumn - 6.f, row.bottom},
                brush);
    }
    const float right = row.right - (item.submenu ? 24.f : 10.f);
    float label_right = right;
    if (!item.shortcut.empty() && st.shortcut_format) {
      brush->SetColor(item.disabled ? p.text_disabled : p.text_secondary);
      const D2D1_RECT_F r{row.left + kCheckColumn, row.top, right, row.bottom};
      rt->DrawText(item.shortcut.c_str(), static_cast<UINT32>(item.shortcut.size()), st.shortcut_format.Get(), r, brush,
                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
      label_right = right - TextWidth(st.shortcut_format.Get(), item.shortcut) - 16.f;
    }
    brush->SetColor(ink);
    DrawTextIn(rt, g.text(), item.label, {row.left + kCheckColumn, row.top, label_right, row.bottom}, brush);
    if (item.submenu) {
      brush->SetColor(item.disabled ? p.text_disabled : p.text_secondary);
      DrawGlyph(rt, g.icons_small(), 0xE76C /* ChevronRight */,
                {row.right - 22.f, row.top + 8.f, row.right - 8.f, row.bottom - 8.f}, brush);
    }
    y += kRowHeight;
  }
  if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
    level.rt.Reset();
    level.brush.Reset();
    ::InvalidateRect(level.hwnd, nullptr, FALSE);
  }
}

State* g_state = nullptr;

LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* level = reinterpret_cast<Level*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      if (level && g_state) Paint(*g_state, *level);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
      if (g_state && level && !g_state->levels.empty() && g_state->levels.front().get() == level &&
          (msg == WM_CANCELMODE || reinterpret_cast<HWND>(lp) != hwnd)) {
        g_state->done = true;  // another window took the mouse (Alt+Tab, a click elsewhere)
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterClassOnce() {
  static bool registered = false;
  if (registered) return;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = &MenuProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kMenuClass;
  ::RegisterClassExW(&wc);
  registered = true;
}

RECT WorkAreaAt(POINT pt) {
  MONITORINFO mi{sizeof(mi)};
  ::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
  return mi.rcWork;
}

int Px(const State& st, float dip) { return static_cast<int>(dip * st.scale + 0.5f); }

// Opens |menu| as a new level. Root: at (x, y) per |flags|; submenu: beside
// |anchor|'s row |anchor_item| (screen px of the parent level).
bool OpenLevel(State& st, HMENU menu, int x, int y, UINT flags, Level* parent, int parent_item) {
  auto level = std::make_unique<Level>();
  level->items = ReadItems(menu);
  if (level->items.empty()) return false;
  auto& g = Graphics::Get();
  float label_w = 0.f, shortcut_w = 0.f;
  bool any_sub = false;
  for (const Item& item : level->items) {
    if (item.separator) continue;
    label_w = std::max(label_w, TextWidth(g.text(), item.label));
    if (st.shortcut_format) shortcut_w = std::max(shortcut_w, TextWidth(st.shortcut_format.Get(), item.shortcut));
    any_sub |= item.submenu != nullptr;
  }
  level->width = std::clamp(2.f * kPad + kCheckColumn + label_w + (shortcut_w > 0.f ? shortcut_w + 28.f : 0.f) +
                                (any_sub ? 26.f : 14.f),
                            kMinWidth, kMaxWidth);
  level->height = ItemTop(*level, static_cast<int>(level->items.size())) + kPad;
  const int w = Px(st, level->width), h = Px(st, level->height);
  int left = x, top = y;
  if (parent) {
    RECT pr;
    ::GetWindowRect(parent->hwnd, &pr);
    left = pr.right - Px(st, 4.f);
    top = pr.top + Px(st, ItemTop(*parent, parent_item) - kPad);
    const RECT work = WorkAreaAt({pr.right, top});
    if (left + w > work.right) left = pr.left - w + Px(st, 4.f);
    if (top + h > work.bottom) top = std::max(work.top, static_cast<LONG>(work.bottom - h));
  } else {
    if (flags & TPM_BOTTOMALIGN) top -= h;
    if (flags & TPM_RIGHTALIGN) left -= w;
    const RECT work = WorkAreaAt({x, y});
    if (left + w > work.right) left = std::max(work.left, static_cast<LONG>(work.right - w));
    if (left < work.left) left = work.left;
    if (top + h > work.bottom) top = (y - h >= work.top) ? y - h : std::max(work.top, static_cast<LONG>(work.bottom - h));
    if (top < work.top) top = work.top;
  }
  RegisterClassOnce();
  level->hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kMenuClass, L"",
                                  WS_POPUP, left, top, w, h, st.owner, nullptr, ::GetModuleHandleW(nullptr),
                                  nullptr);
  if (!level->hwnd) return false;
  ::SetWindowLongPtrW(level->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(level.get()));
  const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(level->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
  const BOOL dark = st.palette.dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(level->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  const COLORREF border = ToColorRef(Over(st.palette.separator, Background(st.palette)));
  ::DwmSetWindowAttribute(level->hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
  ::ShowWindow(level->hwnd, SW_SHOWNOACTIVATE);
  st.levels.push_back(std::move(level));
  return true;
}

void CloseLevelsAfter(State& st, size_t keep) {
  while (st.levels.size() > keep) {
    ::DestroyWindow(st.levels.back()->hwnd);
    st.levels.pop_back();
  }
  if (!st.levels.empty()) {
    st.levels.back()->open_child = -1;
    ::InvalidateRect(st.levels.back()->hwnd, nullptr, FALSE);
  }
}

void OpenSubmenu(State& st, size_t index, int item, bool select_first) {
  Level& level = *st.levels[index];
  CloseLevelsAfter(st, index + 1);
  const Item& it = level.items[static_cast<size_t>(item)];
  if (!it.submenu || it.disabled) return;
  if (OpenLevel(st, it.submenu, 0, 0, 0, &level, item)) {
    st.levels[index]->open_child = item;
    if (select_first) {
      Level& child = *st.levels.back();
      for (size_t i = 0; i < child.items.size(); ++i) {
        if (child.items[i].selectable()) {
          child.hot = static_cast<int>(i);
          break;
        }
      }
    }
  }
  ::InvalidateRect(level.hwnd, nullptr, FALSE);
}

// The level and item under a screen point (level -1: outside every level).
std::pair<int, int> HitAt(const State& st, POINT pt) {
  for (int l = static_cast<int>(st.levels.size()) - 1; l >= 0; --l) {
    const Level& level = *st.levels[static_cast<size_t>(l)];
    RECT r;
    ::GetWindowRect(level.hwnd, &r);
    if (::PtInRect(&r, pt)) {
      return {l, ItemAt(level, static_cast<float>(pt.y - r.top) / st.scale)};
    }
  }
  return {-1, -1};
}

void SetHot(Level& level, int item) {
  if (level.hot == item) return;
  level.hot = item;
  ::InvalidateRect(level.hwnd, nullptr, FALSE);
}

void Choose(State& st, size_t index, int item) {
  Level& level = *st.levels[index];
  if (item < 0 || item >= static_cast<int>(level.items.size())) return;
  const Item& it = level.items[static_cast<size_t>(item)];
  if (!it.selectable()) return;
  if (it.submenu) {
    OpenSubmenu(st, index, item, true);
    return;
  }
  st.result = it.id;
  st.done = true;
}

void MoveHot(Level& level, int delta) {
  const int n = static_cast<int>(level.items.size());
  if (n == 0) return;
  int i = level.hot;
  for (int step = 0; step < n; ++step) {
    i = i < 0 ? (delta > 0 ? 0 : n - 1) : ((i + delta) % n + n) % n;
    if (level.items[static_cast<size_t>(i)].selectable()) {
      SetHot(level, i);
      return;
    }
  }
}

void OnKey(State& st, WPARAM vk) {
  const size_t index = st.levels.size() - 1;
  Level& level = *st.levels[index];
  switch (vk) {
    case VK_DOWN: MoveHot(level, 1); break;
    case VK_UP: MoveHot(level, -1); break;
    case VK_HOME:
      level.hot = -1;
      MoveHot(level, 1);
      break;
    case VK_END:
      level.hot = -1;
      MoveHot(level, -1);
      break;
    case VK_RIGHT:
      if (level.hot >= 0 && level.items[static_cast<size_t>(level.hot)].submenu) OpenSubmenu(st, index, level.hot, true);
      break;
    case VK_LEFT:
      if (index > 0) CloseLevelsAfter(st, index);
      break;
    case VK_ESCAPE:
      if (index > 0) {
        CloseLevelsAfter(st, index);
      } else {
        st.done = true;
      }
      break;
    case VK_RETURN:
    case VK_SPACE:
      Choose(st, index, level.hot);
      break;
    case VK_MENU:
    case VK_F10:
      st.done = true;
      break;
    default:
      break;
  }
}

void OnChar(State& st, wchar_t ch) {
  const size_t index = st.levels.size() - 1;
  Level& level = *st.levels[index];
  const wchar_t c = static_cast<wchar_t>(std::towlower(ch));
  // The marked mnemonic, else the first letter of the label.
  std::vector<int> matches;
  for (size_t i = 0; i < level.items.size(); ++i) {
    const Item& it = level.items[i];
    if (!it.selectable()) continue;
    const wchar_t key = it.mnemonic ? it.mnemonic : static_cast<wchar_t>(std::towlower(it.label[0]));
    if (key == c) matches.push_back(static_cast<int>(i));
  }
  if (matches.empty()) return;
  if (matches.size() == 1) {
    SetHot(level, matches[0]);
    Choose(st, index, matches[0]);
    return;
  }
  // Several: cycle through them.
  auto next = std::upper_bound(matches.begin(), matches.end(), level.hot);
  SetHot(level, next == matches.end() ? matches.front() : *next);
}

void OnMouseMove(State& st, POINT pt) {
  const auto [l, item] = HitAt(st, pt);
  if (l < 0) {
    // Outside: the deepest level loses its highlight (an opened submenu
    // keeps its parent row lit).
    Level& deepest = *st.levels.back();
    if (deepest.open_child < 0) SetHot(deepest, -1);
    return;
  }
  Level& level = *st.levels[static_cast<size_t>(l)];
  SetHot(level, item);
  if (item < 0) return;
  if (level.open_child == item) {
    CloseLevelsAfter(st, static_cast<size_t>(l) + 2);
    return;
  }
  const Item& it = level.items[static_cast<size_t>(item)];
  if (it.submenu && !it.disabled) {
    OpenSubmenu(st, static_cast<size_t>(l), item, false);
  } else {
    CloseLevelsAfter(st, static_cast<size_t>(l) + 1);
  }
}

}  // namespace

bool MenuActive() { return g_depth > 0; }

UINT TrackMenu(HMENU menu, UINT flags, int x, int y, HWND owner, const Palette& palette) {
  if (!menu) return 0;
  State st;
  st.owner = owner;
  st.palette = palette;
  st.scale = static_cast<float>(owner ? ::GetDpiForWindow(owner) : ::GetDpiForSystem()) / 96.f;
  {
    auto& g = Graphics::Get();
    g.dwrite()->CreateTextFormat(g.ui_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 12.f, L"", &st.shortcut_format);
    if (st.shortcut_format) {
      st.shortcut_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
      st.shortcut_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      st.shortcut_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
  }
  State* previous = g_state;
  g_state = &st;
  ++g_depth;
  if (!OpenLevel(st, menu, x, y, flags, nullptr, -1)) {
    g_state = previous;
    --g_depth;
    return 0;
  }
  ::SetCapture(st.levels.front()->hwnd);
  // A menu opened from the keyboard starts with nothing lit; arrows light the
  // first row.
  MSG msg;
  while (!st.done) {
    const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
    if (got <= 0) {
      if (got == 0) ::PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    // The owner was deactivated (another application came forward).
    const HWND foreground = ::GetForegroundWindow();
    if (foreground && foreground != owner && ::GetAncestor(foreground, GA_ROOTOWNER) != owner) {
      DWORD pid = 0;
      ::GetWindowThreadProcessId(foreground, &pid);
      if (pid != ::GetCurrentProcessId()) break;
    }
    switch (msg.message) {
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
        OnKey(st, msg.wParam);
        continue;
      case WM_CHAR:
      case WM_SYSCHAR:
        if (msg.wParam > L' ') OnChar(st, static_cast<wchar_t>(msg.wParam));
        continue;
      case WM_KEYUP:
      case WM_SYSKEYUP:
      case WM_SYSDEADCHAR:
      case WM_DEADCHAR:
        continue;
      case WM_MOUSEMOVE:
      case WM_NCMOUSEMOVE: {
        POINT pt{static_cast<short>(LOWORD(msg.lParam)), static_cast<short>(HIWORD(msg.lParam))};
        if (msg.message == WM_MOUSEMOVE) ::ClientToScreen(msg.hwnd, &pt);
        OnMouseMove(st, pt);
        continue;
      }
      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_NCLBUTTONDOWN:
      case WM_NCRBUTTONDOWN: {
        POINT pt{static_cast<short>(LOWORD(msg.lParam)), static_cast<short>(HIWORD(msg.lParam))};
        if (msg.message < WM_NCMOUSEMOVE || msg.message > WM_NCMBUTTONDBLCLK) ::ClientToScreen(msg.hwnd, &pt);
        if (HitAt(st, pt).first < 0) st.done = true;  // a click outside dismisses (and is swallowed)
        continue;
      }
      case WM_LBUTTONUP:
      case WM_RBUTTONUP: {
        POINT pt{static_cast<short>(LOWORD(msg.lParam)), static_cast<short>(HIWORD(msg.lParam))};
        ::ClientToScreen(msg.hwnd, &pt);
        const auto [l, item] = HitAt(st, pt);
        if (l >= 0 && item >= 0) Choose(st, static_cast<size_t>(l), item);
        continue;
      }
      case WM_LBUTTONDBLCLK:
      case WM_RBUTTONDBLCLK:
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL:
        continue;
      default:
        break;
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
    if (!st.levels.empty() && ::GetCapture() != st.levels.front()->hwnd && !st.done) {
      ::SetCapture(st.levels.front()->hwnd);  // keep the mouse while open
    }
  }
  if (!st.levels.empty() && ::GetCapture() == st.levels.front()->hwnd) ::ReleaseCapture();
  CloseLevelsAfter(st, 0);
  g_state = previous;
  --g_depth;
  return st.result;
}

}  // namespace orbit::ui
