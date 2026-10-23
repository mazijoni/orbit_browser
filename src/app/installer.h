#pragma once

#include <filesystem>
#include <optional>

namespace orbit::installer {

// Per-user installation (no administrator rights required):
//   files      -> %LOCALAPPDATA%\Programs\Orbit
//   shortcut   -> Start menu "Orbit"
//   registry   -> HKCU browser capabilities (default-browser candidate),
//                 OrbitHTML ProgID, App Paths, uninstall entry
// User data (%LOCALAPPDATA%\Orbit\User Data) is never touched by install and
// only removed by uninstall when the user explicitly agrees.

inline constexpr wchar_t kAppUserModelId[] = L"Orbit.Browser";

// Handles --install / --uninstall / --register (re-register in place). Returns
// the process exit code, or nullopt if the command line has none of them.
std::optional<int> HandleCommandLine(const wchar_t* command_line);

std::filesystem::path InstallDirectory();

// True when Windows opens http(s) links with Orbit (the user's choice in
// Default apps names Orbit's ProgID).
bool IsDefaultBrowser();
// Registers this copy of Orbit as a browser (so Windows lists it), then opens
// Default apps on Orbit's entry: since Windows 10 only the user can pick the
// default browser, so this is as far as an app may go.
void MakeDefaultBrowser();

}  // namespace orbit::installer
