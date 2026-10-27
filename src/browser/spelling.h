// Spelling suggestions from the Windows spell checker (the one Chromium uses
// on Windows). Chromium fills its own suggestion items in after the page menu
// is built, which Orbit's native menu never sees; Orbit asks for them itself.
#pragma once

#include <string>
#include <vector>

namespace orbit::spelling {

// Up to |max| suggestions for |word| in |language| (BCP 47, e.g. "en-US").
// Empty when the language has no Windows dictionary or nothing is close.
std::vector<std::wstring> Suggest(const std::wstring& word, const std::wstring& language, size_t max = 5);

}  // namespace orbit::spelling
