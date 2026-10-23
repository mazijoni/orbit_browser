#pragma once

#include <windows.h>

#include "ui/theme.h"

namespace orbit::ui {

// Orbit's own popup menu, drawn like the rest of the chrome (rounded panel,
// the palette's colours, hover rows, shortcuts in the secondary colour, a
// check glyph, cascading submenus) instead of the Win32 menu.
//
// It takes an ordinary HMENU (built with AppendMenuW: strings with an
// optional "\tShortcut", separators, MF_CHECKED / MF_GRAYED, MF_POPUP
// submenus) and runs modally like TrackPopupMenu(TPM_RETURNCMD): the result
// is the chosen command id, or 0 when the menu was dismissed. |flags| honours
// TPM_BOTTOMALIGN and TPM_RIGHTALIGN. Keyboard: arrows, Home/End, Enter,
// Space, Escape, Right/Left for submenus and mnemonic letters ("&File").
// UI thread only; other messages keep being dispatched while it is open.
UINT TrackMenu(HMENU menu, UINT flags, int x, int y, HWND owner, const Palette& palette);

// True while a TrackMenu loop runs (any thread-local nesting).
bool MenuActive();

}  // namespace orbit::ui
