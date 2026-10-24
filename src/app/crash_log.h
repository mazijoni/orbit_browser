#pragma once

#include <filesystem>

namespace orbit::crash_log {

// Browser process only: makes crashes diagnosable after the fact.
// - A fatal exception (access violation, stack overflow, fail-fast...) is
//   logged to orbit.log with the faulting module + offset and the stack
//   (named through the PDB when it is next to the DLL).
// - An exception nobody handles also writes a small minidump to
//   <user data>\Crashes\orbit-<time>.dmp (the five newest are kept).
// - Debug builds: failed CRT assertions and abort() are logged.
// No page data is logged; the minidump holds thread stacks only.
// Safe to call again (re-asserts the unhandled-exception filter).
void Install(const std::filesystem::path& user_data);

// main(): the shutdown was clean. Otherwise an ExitProcess from anywhere is
// logged (with its caller) when the process ends.
void MarkCleanExit();

// Logs the current stack (used by the self-tests too).
void LogStack(const char* prefix);

}  // namespace orbit::crash_log
