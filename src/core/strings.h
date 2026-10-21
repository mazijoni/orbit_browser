#pragma once

#include <string>
#include <string_view>

namespace orbit {

std::string ToUtf8(std::wstring_view s);
std::wstring FromUtf8(std::string_view s);

std::wstring ToLowerAscii(std::wstring_view s);
std::wstring_view TrimWhitespace(std::wstring_view s);
bool StartsWithNoCase(std::wstring_view s, std::wstring_view prefix);

// Percent-encodes UTF-8 bytes of |s| for use in a URL query component.
std::wstring EscapeQueryComponent(std::wstring_view s);

// Escapes text for HTML element content and quoted attribute values.
std::string HtmlEscape(std::wstring_view s);
// Returns a JSON string literal (with quotes) that is also safe inside a
// <script> element ("<", ">", "&" and U+2028/2029 are escaped).
std::string JsonString(std::wstring_view s);

}  // namespace orbit
