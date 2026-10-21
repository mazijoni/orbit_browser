#include "core/strings.h"

#include <windows.h>

namespace orbit {

std::string ToUtf8(std::wstring_view s) {
  if (s.empty()) return {};
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len,
                        nullptr, nullptr);
  return out;
}

std::wstring FromUtf8(std::string_view s) {
  if (s.empty()) return {};
  const int len =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
  return out;
}

std::wstring ToLowerAscii(std::wstring_view s) {
  std::wstring out(s);
  for (auto& c : out) {
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  }
  return out;
}

std::wstring_view TrimWhitespace(std::wstring_view s) {
  constexpr std::wstring_view kWs = L" \t\r\n\x00A0\x3000";
  const size_t first = s.find_first_not_of(kWs);
  if (first == std::wstring_view::npos) return {};
  const size_t last = s.find_last_not_of(kWs);
  return s.substr(first, last - first + 1);
}

bool StartsWithNoCase(std::wstring_view s, std::wstring_view prefix) {
  if (s.size() < prefix.size()) return false;
  return ToLowerAscii(s.substr(0, prefix.size())) == ToLowerAscii(prefix);
}

std::wstring EscapeQueryComponent(std::wstring_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const std::string utf8 = ToUtf8(s);
  std::wstring out;
  out.reserve(utf8.size() * 3);
  for (unsigned char c : utf8) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                            c == '~';
    if (unreserved) {
      out.push_back(static_cast<wchar_t>(c));
    } else if (c == ' ') {
      out.push_back(L'+');
    } else {
      out.push_back(L'%');
      out.push_back(static_cast<wchar_t>(kHex[c >> 4]));
      out.push_back(static_cast<wchar_t>(kHex[c & 0xF]));
    }
  }
  return out;
}

std::string HtmlEscape(std::wstring_view s) {
  std::string out;
  for (char c : ToUtf8(s)) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::string JsonString(std::wstring_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  size_t run_start = 0;
  auto flush = [&](size_t end) {
    // Convert unescaped runs in one go so surrogate pairs stay intact.
    if (end > run_start) out += ToUtf8(s.substr(run_start, end - run_start));
  };
  for (size_t i = 0; i < s.size(); ++i) {
    const wchar_t c = s[i];
    const bool escape = c < 0x20 || c == L'"' || c == L'\\' || c == L'<' || c == L'>' ||
                        c == L'&' || c == 0x2028 || c == 0x2029;
    if (!escape) continue;
    flush(i);
    run_start = i + 1;
    out += "\\u";
    out.push_back(kHex[(c >> 12) & 0xF]);
    out.push_back(kHex[(c >> 8) & 0xF]);
    out.push_back(kHex[(c >> 4) & 0xF]);
    out.push_back(kHex[c & 0xF]);
  }
  flush(s.size());
  out += "\"";
  return out;
}

}  // namespace orbit
