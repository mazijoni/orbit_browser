#pragma once

#include <string>
#include <string_view>

namespace orbit {

// Decides whether a URL with an unknown (non-web) scheme may be handed to the
// Windows shell after the user confirms. Rejects malformed URLs and schemes
// whose OS handlers are known to be abusable (e.g. ms-msdt, search-ms).
// On success, |scheme_out| receives the lower-cased scheme.
bool IsExternalUrlLaunchable(std::wstring_view url, std::wstring* scheme_out);

}  // namespace orbit
