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

// The URL as the sidebar's address bar shows it while not editing. Follows
// Zen's trimURL (zen.urlbar.show-domain-only-in-sidebar): a single trailing
// slash is dropped, https URLs show only their host (and port), and a leading
// "www." is removed. Other schemes, including http, are shown in full.
std::wstring SidebarDisplayUrl(std::wstring_view url);

// ZenPinnedTabManager.onLocationChange: whether a pinned tab still shows its
// pinned page. Fragments are ignored and "scheme://host" equals
// "scheme://host/" (nsIURI spec normalization).
bool PinnedUrlMatches(std::wstring_view current, std::wstring_view pinned);

// QueryStringStripper.stripForCopyOrShare (zenStrippedURI): an http(s) URL
// without tracking parameters (utm_*, click ids, mailing-list ids, and a few
// site-specific share ids), the rest of the query and the fragment kept.
// Other URLs come back unchanged.
std::wstring StripTrackingParams(std::wstring_view url);

// True for schemes the browser is willing to navigate to from the address bar.
bool IsAllowedTypedScheme(std::wstring_view scheme_lower);

}  // namespace orbit
