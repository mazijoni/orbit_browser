#pragma once

#include <string>

namespace orbit {

// In-process integration tests that drive the real browser (real Chromium
// processes, real windows, real storage). Enabled only by
// --orbit-self-test=<suite> together with an explicit --user-data-dir, so a
// user's real profile is never touched. Results go to
// <user-data>/selftest-<suite>.txt and the process exit code.
namespace self_test {

bool IsActive();
// Called from OnContextInitialized after the session has been restored.
void Start(const std::wstring& suite);

}  // namespace self_test
}  // namespace orbit
