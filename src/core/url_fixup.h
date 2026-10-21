#pragma once

#include <string>
#include <string_view>

namespace orbit {

struct OmniboxResult {
  enum class Kind { kNone, kUrl, kSearch };
  Kind kind = Kind::kNone;
  std::wstring url;
};

// Turns address-bar input into a navigable URL. |search_template| contains
// "{query}" which is replaced by the escaped input when the input is not
// recognized as a URL. "javascript:" input is never treated as a URL.
OmniboxResult FixupOmniboxInput(std::wstring_view input, std::wstring_view search_template);

// True for schemes the browser is willing to navigate to from the address bar.
bool IsAllowedTypedScheme(std::wstring_view scheme_lower);

}  // namespace orbit
