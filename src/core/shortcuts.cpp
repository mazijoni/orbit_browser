#include "core/shortcuts.h"

#include <windows.h>

#include <cwctype>

namespace orbit {
namespace {

constexpr KeyChord Ctrl(int vk) { return {vk, true, false, false}; }
constexpr KeyChord CtrlShift(int vk) { return {vk, true, true, false}; }
constexpr KeyChord CtrlAlt(int vk) { return {vk, true, false, true}; }
constexpr KeyChord Alt(int vk) { return {vk, false, false, true}; }
constexpr KeyChord AltShift(int vk) { return {vk, false, true, true}; }
constexpr KeyChord Plain(int vk) { return {vk, false, false, false}; }

struct KeyName {
  int vk;
  const wchar_t* name;
};
constexpr KeyName kNames[] = {
    {VK_TAB, L"Tab"},       {VK_RETURN, L"Enter"},   {VK_ESCAPE, L"Esc"},
    {VK_SPACE, L"Space"},   {VK_PRIOR, L"PageUp"},   {VK_NEXT, L"PageDown"},
    {VK_END, L"End"},       {VK_HOME, L"Home"},      {VK_LEFT, L"Left"},
    {VK_UP, L"Up"},         {VK_RIGHT, L"Right"},    {VK_DOWN, L"Down"},
    {VK_INSERT, L"Insert"}, {VK_DELETE, L"Delete"},  {VK_BACK, L"Backspace"},
    {VK_OEM_PLUS, L"="},    {VK_OEM_MINUS, L"-"},    {VK_OEM_COMMA, L","},
    {VK_OEM_PERIOD, L"."},  {VK_OEM_1, L";"},        {VK_OEM_2, L"/"},
    {VK_OEM_3, L"`"},       {VK_OEM_4, L"["},        {VK_OEM_5, L"\\"},
    {VK_OEM_6, L"]"},       {VK_OEM_7, L"'"},        {VK_MULTIPLY, L"*"},
    {VK_ADD, L"Num+"},      {VK_SUBTRACT, L"Num-"},
};

std::wstring KeyLabel(int vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
  for (const KeyName& k : kNames) {
    if (k.vk == vk) return k.name;
  }
  return L"";
}

int KeyFromLabel(const std::wstring& label) {
  if (label.size() == 1) {
    const wchar_t c = static_cast<wchar_t>(std::towupper(label[0]));
    if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return c;
  }
  for (int vk = VK_F1; vk <= VK_F24; ++vk) {
    if (label == KeyLabel(vk)) return vk;
  }
  for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk) {
    if (label == KeyLabel(vk)) return vk;
  }
  for (const KeyName& k : kNames) {
    if (label == k.name) return k.vk;
  }
  return 0;
}

}  // namespace

const std::vector<ShortcutAction>& ShortcutActions() {
  // Defaults as in ZenKeyboardShortcuts (with Orbit's Chromium equivalents
  // where Zen inherits Firefox's).
  static const std::vector<ShortcutAction> actions = {
      {L"new-tab", L"Tabs", L"New tab", Ctrl('T')},
      {L"close-tab", L"Tabs", L"Close tab", Ctrl('W')},
      {L"reopen-tab", L"Tabs", L"Reopen closed tab", CtrlShift('T')},
      {L"next-tab", L"Tabs", L"Next tab", Ctrl(VK_TAB)},
      {L"previous-tab", L"Tabs", L"Previous tab", CtrlShift(VK_TAB)},
      {L"toggle-pin", L"Tabs", L"Pin / unpin tab", CtrlShift('D')},
      {L"close-unpinned", L"Tabs", L"Close all unpinned tabs", CtrlShift('K')},
      {L"copy-url", L"Tabs", L"Copy current URL", CtrlShift('C')},
      {L"copy-url-markdown", L"Tabs", L"Copy current URL as Markdown", {'C', true, true, true}},
      {L"location", L"Navigation", L"Open the URL bar", Ctrl('L')},
      {L"back", L"Navigation", L"Back", Alt(VK_LEFT)},
      {L"forward", L"Navigation", L"Forward", Alt(VK_RIGHT)},
      {L"reload", L"Navigation", L"Reload", Ctrl('R')},
      {L"hard-reload", L"Navigation", L"Reload ignoring cache", CtrlShift('R')},
      {L"bookmark", L"Navigation", L"Bookmark this page", Ctrl('D')},
      {L"next-space", L"Spaces", L"Next space", CtrlAlt(VK_RIGHT)},
      {L"previous-space", L"Spaces", L"Previous space", CtrlAlt(VK_LEFT)},
      {L"compact-mode", L"View", L"Toggle compact mode", Ctrl('S')},
      {L"collapse-sidebar", L"View", L"Collapse the sidebar", CtrlShift('S')},
      {L"library", L"View", L"Toggle the library", AltShift('L')},
      {L"fullscreen", L"View", L"Full screen", Plain(VK_F11)},
      {L"zoom-in", L"View", L"Zoom in", Ctrl(VK_OEM_PLUS)},
      {L"zoom-out", L"View", L"Zoom out", Ctrl(VK_OEM_MINUS)},
      {L"zoom-reset", L"View", L"Reset zoom", Ctrl('0')},
      {L"devtools", L"View", L"Developer tools", CtrlShift('I')},
      {L"split-grid", L"Split View", L"Split view: grid", CtrlAlt('G')},
      {L"split-vertical", L"Split View", L"Split view: vertical", CtrlAlt('V')},
      {L"split-horizontal", L"Split View", L"Split view: horizontal", CtrlAlt('H')},
      {L"unsplit", L"Split View", L"Unsplit", CtrlAlt('U')},
      {L"new-split", L"Split View", L"New empty split", CtrlShift('8')},
      {L"glance-expand", L"Glance", L"Expand glance to a tab", Ctrl('O')},
      {L"new-window", L"Windows", L"New window", Ctrl('N')},
      {L"new-blank-window", L"Windows", L"New unsynced window", CtrlShift('N')},
      {L"private-window", L"Windows", L"New private window", CtrlShift('P')},
      {L"history", L"Pages", L"History", Ctrl('H')},
      {L"downloads", L"Pages", L"Downloads", Ctrl('J')},
      {L"bookmarks", L"Pages", L"Bookmarks", CtrlShift('O')},
      // Unbound by default (ZenKeyboardShortcuts on Windows).
      {L"duplicate-tab", L"Tabs", L"Duplicate tab", {}},
      {L"reset-pinned-tab", L"Tabs", L"Reset pinned tab", {}},
      {L"create-space", L"Spaces", L"Create space", {}},
      {L"show-sidebar", L"View", L"Compact mode: show sidebar", {}},
      {L"space-1", L"Spaces", L"Switch to space 1", {}},
      {L"space-2", L"Spaces", L"Switch to space 2", {}},
      {L"space-3", L"Spaces", L"Switch to space 3", {}},
      {L"space-4", L"Spaces", L"Switch to space 4", {}},
      {L"space-5", L"Spaces", L"Switch to space 5", {}},
      {L"space-6", L"Spaces", L"Switch to space 6", {}},
      {L"space-7", L"Spaces", L"Switch to space 7", {}},
      {L"space-8", L"Spaces", L"Switch to space 8", {}},
      {L"space-9", L"Spaces", L"Switch to space 9", {}},
      {L"space-10", L"Spaces", L"Switch to space 10", {}},
  };
  return actions;
}

const ShortcutAction* FindShortcutAction(const std::wstring& id) {
  for (const auto& a : ShortcutActions()) {
    if (id == a.id) return &a;
  }
  return nullptr;
}

std::wstring FormatChord(const KeyChord& chord) {
  if (chord.empty()) return L"";
  const std::wstring key = KeyLabel(chord.vk);
  if (key.empty()) return L"";
  std::wstring out;
  if (chord.ctrl) out += L"Ctrl+";
  if (chord.alt) out += L"Alt+";
  if (chord.shift) out += L"Shift+";
  return out + key;
}

std::optional<KeyChord> ParseChord(const std::wstring& text) {
  KeyChord chord;
  if (text.empty()) return chord;
  size_t start = 0;
  while (true) {
    const size_t plus = text.find(L'+', start);
    // A trailing "+" is the key itself only if nothing follows ("Ctrl+=" uses '=').
    const std::wstring part = plus == std::wstring::npos ? text.substr(start)
                                                         : text.substr(start, plus - start);
    if (plus == std::wstring::npos) {
      chord.vk = KeyFromLabel(part);
      if (!chord.vk) return std::nullopt;
      return chord;
    }
    if (part == L"Ctrl") {
      chord.ctrl = true;
    } else if (part == L"Alt") {
      chord.alt = true;
    } else if (part == L"Shift") {
      chord.shift = true;
    } else {
      return std::nullopt;
    }
    start = plus + 1;
  }
}

bool AssignableChord(const KeyChord& chord) {
  if (chord.empty() || KeyLabel(chord.vk).empty()) return false;
  if (chord.vk >= VK_F1 && chord.vk <= VK_F24) return true;
  return chord.ctrl || chord.alt;
}

KeyChord ShortcutMap::Effective(const std::wstring& id) const {
  if (auto it = overrides_.find(id); it != overrides_.end()) return it->second;
  const ShortcutAction* a = FindShortcutAction(id);
  return a ? a->key : KeyChord{};
}

std::vector<std::wstring> ShortcutMap::Conflicts(const std::wstring& id,
                                                 const KeyChord& chord) const {
  std::vector<std::wstring> out;
  if (chord.empty()) return out;
  for (const auto& a : ShortcutActions()) {
    if (id != a.id && Effective(a.id) == chord) out.push_back(a.id);
  }
  return out;
}

ShortcutMap::Resolution ShortcutMap::Resolve(const KeyChord& in) const {
  Resolution r;
  if (overrides_.empty()) return r;
  // The pressed chord now belongs to an action: run that action's default.
  for (const auto& [id, chord] : overrides_) {
    if (!chord.empty() && chord == in) {
      if (const ShortcutAction* a = FindShortcutAction(id)) {
        r.pass = false;
        r.dispatch = a->key;
        r.action = a->id;
        return r;
      }
    }
  }
  // A default chord whose action was moved elsewhere or disabled.
  for (const auto& a : ShortcutActions()) {
    if (!a.key.empty() && a.key == in && overrides_.count(a.id)) {
      r.pass = false;
      r.blocked = true;
      return r;
    }
  }
  return r;
}

}  // namespace orbit
