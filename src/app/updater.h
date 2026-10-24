// Updates from GitHub releases. Orbit asks the GitHub API for the newest
// release of the project; a newer version's "Orbit Browser Setup.exe" is
// downloaded (HTTPS, checked against the SHA-256 GitHub lists for the asset)
// and, when the user says so, Orbit closes cleanly and runs it silently; the
// setup starts Orbit again. Checks: once a day in the background (Settings can
// turn that off) and from Settings. Nothing is downloaded without a click.
// UI thread, except the network work, which runs on its own threads.
#pragma once

#include <string>

namespace orbit::updater {

inline constexpr wchar_t kRepository[] = L"mazijoni/orbit_browser";

enum class State { kIdle, kChecking, kUpToDate, kAvailable, kDownloading, kReady, kError };

struct Status {
  State state = State::kIdle;
  std::string latest;     // "0.1.4"
  std::string notes_url;  // the release page
  int progress = 0;       // 0-100 while downloading
  std::string error;      // kError: a short reason
};

Status Current();
const char* StateName(State state);
// "0.1.10" > "0.1.9"; a leading "v" is ignored.
bool IsNewer(const std::string& candidate, const std::string& current);

// Starts a check (no-op while one runs or an update is downloading). |quiet|:
// the daily background check, which shows a toast only when one is found.
void Check(bool quiet);
// At startup: a quiet check, if checks are on and the last one is a day old.
void MaybeCheckInBackground();
// Downloads the update found by the last check.
void Download();
// kReady: closes every window; once Orbit has shut down, main() calls
// RunPendingInstaller, which starts the setup.
void InstallAndRestart();
void RunPendingInstaller();

}  // namespace orbit::updater
