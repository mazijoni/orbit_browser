// Customizable keyboard shortcuts (Zen's ZenKeyboardShortcuts / the
// Keyboard Shortcuts settings pane), without UI dependencies.
//
// Each editable action has one default chord. A user override replaces it
// (or disables the action). BrowserWindow keeps dispatching on default
// chords: Resolve() maps a pressed chord to the default chord of the action
// it now belongs to, and blocks a default chord whose action was moved.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace orbit {

struct KeyChord {
  int vk = 0;  // Windows virtual-key code; 0 = none (disabled)
  bool ctrl = false;
  bool shift = false;
  bool alt = false;

  bool empty() const { return vk == 0; }
  bool operator==(const KeyChord& o) const {
    return vk == o.vk && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
  }
};

struct ShortcutAction {
  const wchar_t* id;
  const wchar_t* group;
  const wchar_t* label;
  KeyChord key;  // default (empty: unbound by default, as several of Zen's)
};

const std::vector<ShortcutAction>& ShortcutActions();
const ShortcutAction* FindShortcutAction(const std::wstring& id);

// "Ctrl+Shift+K", "Alt+Left", "F11"; "" for an empty chord.
std::wstring FormatChord(const KeyChord& chord);
std::optional<KeyChord> ParseChord(const std::wstring& text);  // "" parses to empty
// A chord the editor accepts: a modifier, or a function key (plain keys
// would swallow typing). Shift alone is not a modifier for letters.
bool AssignableChord(const KeyChord& chord);

class ShortcutMap {
 public:
  // An override; an empty chord disables the action.
  void Set(const std::wstring& id, const KeyChord& chord) { overrides_[id] = chord; }
  void Reset(const std::wstring& id) { overrides_.erase(id); }
  void ResetAll() { overrides_.clear(); }
  const std::map<std::wstring, KeyChord>& overrides() const { return overrides_; }

  KeyChord Effective(const std::wstring& id) const;
  // Ids of other actions whose effective chord is |chord|.
  std::vector<std::wstring> Conflicts(const std::wstring& id, const KeyChord& chord) const;

  struct Resolution {
    bool pass = true;   // no editable action involved: dispatch |in| as is
    bool blocked = false;  // a moved or disabled default chord: do nothing
    KeyChord dispatch;  // the default chord of the action to run
    std::wstring action;  // its id (actions without a default run by id)
  };
  Resolution Resolve(const KeyChord& in) const;

 private:
  std::map<std::wstring, KeyChord> overrides_;
};

}  // namespace orbit
