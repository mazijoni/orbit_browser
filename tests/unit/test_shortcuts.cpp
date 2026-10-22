#include <windows.h>

#include "core/shortcuts.h"
#include "test.h"

using orbit::AssignableChord;
using orbit::FormatChord;
using orbit::KeyChord;
using orbit::ParseChord;
using orbit::ShortcutActions;
using orbit::ShortcutMap;

TEST(Shortcuts_FormatAndParseRoundTrip) {
  EXPECT_WEQ(FormatChord({'K', true, true, false}), L"Ctrl+Shift+K");
  EXPECT_WEQ(FormatChord({VK_LEFT, false, false, true}), L"Alt+Left");
  EXPECT_WEQ(FormatChord({VK_F11, false, false, false}), L"F11");
  EXPECT_WEQ(FormatChord({VK_OEM_PLUS, true, false, false}), L"Ctrl+=");
  for (const auto& a : ShortcutActions()) {
    if (a.key.empty()) continue;
    const auto parsed = ParseChord(FormatChord(a.key));
    EXPECT_TRUE(parsed && *parsed == a.key);
  }
  EXPECT_TRUE(ParseChord(L"") && ParseChord(L"")->empty());
  EXPECT_TRUE(!ParseChord(L"Ctrl+"));
  EXPECT_TRUE(!ParseChord(L"Hyper+K"));
  EXPECT_TRUE(!ParseChord(L"Ctrl+Nonsense"));
}

TEST(Shortcuts_DefaultsAreUniqueAndAssignable) {
  bool unique = true, assignable = true;
  const auto& all = ShortcutActions();
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].key.empty()) continue;  // unbound by default
    assignable = assignable && AssignableChord(all[i].key);
    for (size_t j = i + 1; j < all.size(); ++j) unique = unique && !(all[i].key == all[j].key);
  }
  EXPECT_TRUE(unique);
  EXPECT_TRUE(assignable);
  EXPECT_TRUE(!AssignableChord({'K', false, false, false}));  // would swallow typing
  EXPECT_TRUE(!AssignableChord({'K', false, true, false}));
  EXPECT_TRUE(AssignableChord({VK_F2, false, false, false}));
}

TEST(Shortcuts_ResolveRemapsAndBlocks) {
  ShortcutMap map;
  const KeyChord ctrl_t{'T', true, false, false};
  const KeyChord ctrl_b{'B', true, false, false};
  EXPECT_TRUE(map.Resolve(ctrl_t).pass);  // no overrides: dispatch as pressed
  map.Set(L"new-tab", ctrl_b);
  auto r = map.Resolve(ctrl_b);
  EXPECT_TRUE(!r.pass && !r.blocked && r.dispatch == ctrl_t);  // runs New tab
  r = map.Resolve(ctrl_t);
  EXPECT_TRUE(!r.pass && r.blocked);  // the old chord does nothing now
  EXPECT_TRUE(map.Resolve({'W', true, false, false}).pass);  // others unaffected
  // Giving Ctrl+T to another action makes it run that action.
  map.Set(L"close-tab", ctrl_t);
  r = map.Resolve(ctrl_t);
  const KeyChord ctrl_w{'W', true, false, false};
  EXPECT_TRUE(!r.pass && !r.blocked && r.dispatch == ctrl_w);
  // Disabling.
  map.Set(L"reload", {});
  EXPECT_TRUE(map.Resolve({'R', true, false, false}).blocked);
  map.ResetAll();
  EXPECT_TRUE(map.Resolve(ctrl_t).pass);
}

TEST(Shortcuts_UnboundActionsResolveById) {
  ShortcutMap map;
  EXPECT_TRUE(map.Effective(L"space-2").empty());
  map.Set(L"space-2", {'2', true, false, true});
  const auto r = map.Resolve({'2', true, false, true});
  EXPECT_TRUE(!r.pass && !r.blocked && r.dispatch.empty() && r.action == L"space-2");
  EXPECT_TRUE(map.Conflicts(L"space-3", {}).empty());  // empty chords never conflict
}

TEST(Shortcuts_ConflictsUseEffectiveChords) {
  ShortcutMap map;
  const auto c = map.Conflicts(L"new-tab", {'W', true, false, false});
  EXPECT_TRUE(c.size() == 1 && c[0] == L"close-tab");
  map.Set(L"close-tab", {'Q', true, false, false});
  EXPECT_TRUE(map.Conflicts(L"new-tab", {'W', true, false, false}).empty());
  EXPECT_TRUE(map.Conflicts(L"new-tab", {'Q', true, false, false}).size() == 1);
  EXPECT_TRUE(map.Conflicts(L"close-tab", {'Q', true, false, false}).empty());  // itself
}
