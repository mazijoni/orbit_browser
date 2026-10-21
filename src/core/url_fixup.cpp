#include "core/url_fixup.h"

#include <array>

#include "core/strings.h"

namespace orbit {
namespace {

bool IsAlpha(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool HasWhitespace(std::wstring_view s) {
  for (wchar_t c : s) {
    if (c == L' ' || c == L'\t' || c == L'\x3000' || c == L'\x00A0') return true;
  }
  return false;
}

// Returns the scheme (lower-cased) if |s| starts with "scheme:", else empty.
std::wstring ExtractScheme(std::wstring_view s) {
  if (s.empty() || !IsAlpha(s[0])) return {};
  for (size_t i = 1; i < s.size(); ++i) {
    const wchar_t c = s[i];
    if (c == L':') return ToLowerAscii(s.substr(0, i));
    if (!(IsAlpha(c) || IsDigit(c) || c == L'+' || c == L'-' || c == L'.')) return {};
  }
  return {};
}

bool IsIPv4(std::wstring_view host) {
  int parts = 0;
  size_t i = 0;
  while (i < host.size()) {
    int value = 0;
    size_t digits = 0;
    while (i < host.size() && IsDigit(host[i])) {
      value = value * 10 + (host[i] - L'0');
      ++digits;
      ++i;
      if (digits > 3) return false;
    }
    if (digits == 0 || value > 255) return false;
    ++parts;
    if (i < host.size()) {
      if (host[i] != L'.') return false;
      ++i;
    }
  }
  return parts == 4;
}

bool IsValidPort(std::wstring_view port) {
  if (port.empty() || port.size() > 5) return false;
  int value = 0;
  for (wchar_t c : port) {
    if (!IsDigit(c)) return false;
    value = value * 10 + (c - L'0');
  }
  return value > 0 && value <= 65535;
}

struct HostInfo {
  std::wstring_view host;
  bool has_port = false;
  bool port_valid = true;
};

HostInfo SplitHost(std::wstring_view input) {
  const size_t end = input.find_first_of(L"/?#");
  std::wstring_view authority = input.substr(0, end);
  if (const size_t at = authority.rfind(L'@'); at != std::wstring_view::npos) {
    authority = authority.substr(at + 1);
  }
  HostInfo info;
  if (!authority.empty() && authority[0] == L'[') {
    const size_t close = authority.find(L']');
    if (close == std::wstring_view::npos) return info;
    info.host = authority.substr(0, close + 1);
    if (close + 1 < authority.size()) {
      info.has_port = authority[close + 1] == L':';
      info.port_valid = info.has_port && IsValidPort(authority.substr(close + 2));
    }
    return info;
  }
  const size_t colon = authority.rfind(L':');
  if (colon != std::wstring_view::npos) {
    info.host = authority.substr(0, colon);
    info.has_port = true;
    info.port_valid = IsValidPort(authority.substr(colon + 1));
  } else {
    info.host = authority;
  }
  return info;
}

bool IsValidHostLabelChars(std::wstring_view host) {
  for (wchar_t c : host) {
    // Allow non-ASCII for IDNs; Chromium performs IDNA processing.
    if (c < 0x80 && !(IsAlpha(c) || IsDigit(c) || c == L'-' || c == L'.' || c == L'_')) {
      return false;
    }
  }
  return true;
}

enum class HostKind { kNotHost, kLocal, kPublic };

HostKind ClassifyHost(std::wstring_view input) {
  const HostInfo info = SplitHost(input);
  if (info.host.empty() || !info.port_valid) return HostKind::kNotHost;
  const std::wstring host = ToLowerAscii(info.host);
  if (host == L"localhost" || host.ends_with(L".localhost")) return HostKind::kLocal;
  if (host.front() == L'[') return HostKind::kLocal;  // IPv6 literal
  if (IsIPv4(host)) return HostKind::kLocal;
  if (!IsValidHostLabelChars(host)) return HostKind::kNotHost;
  const size_t dot = host.rfind(L'.');
  if (dot == std::wstring::npos || dot == 0) return HostKind::kNotHost;
  const std::wstring_view tld = std::wstring_view(host).substr(dot + 1);
  if (tld.size() < 2) return HostKind::kNotHost;
  for (wchar_t c : tld) {
    if (c < 0x80 && !IsAlpha(c) && c != L'-') return HostKind::kNotHost;
  }
  return HostKind::kPublic;
}

}  // namespace

bool IsAllowedTypedScheme(std::wstring_view scheme_lower) {
  static constexpr std::array<std::wstring_view, 8> kAllowed = {
      L"http", L"https", L"file", L"about", L"orbit", L"data", L"view-source", L"chrome"};
  for (auto s : kAllowed) {
    if (s == scheme_lower) return true;
  }
  return false;
}

OmniboxResult FixupOmniboxInput(std::wstring_view raw, std::wstring_view search_template) {
  const std::wstring_view input = TrimWhitespace(raw);
  OmniboxResult result;
  if (input.empty()) return result;

  auto make_search = [&]() {
    std::wstring url(search_template);
    const size_t pos = url.find(L"{query}");
    if (pos != std::wstring::npos) url.replace(pos, 7, EscapeQueryComponent(input));
    return OmniboxResult{OmniboxResult::Kind::kSearch, std::move(url)};
  };

  const std::wstring scheme = ExtractScheme(input);
  if (!scheme.empty()) {
    const std::wstring_view rest = input.substr(scheme.size() + 1);
    if (IsAllowedTypedScheme(scheme)) {
      if (HasWhitespace(input) && scheme != L"data" && scheme != L"file") return make_search();
      return {OmniboxResult::Kind::kUrl, std::wstring(input)};
    }
    // "localhost:8080" or "example.com:8443/x" parse as a scheme; treat as host:port.
    const bool port_follows = !rest.empty() && IsDigit(rest[0]);
    if (!port_follows) return make_search();  // mailto:, javascript:, etc.
  }

  if (HasWhitespace(input)) return make_search();

  // Windows paths typed directly ("C:\foo" handled above as scheme "c" w/o digit).
  if (input.size() > 2 && input[0] == L'\\' && input[1] == L'\\') return make_search();

  switch (ClassifyHost(input)) {
    case HostKind::kLocal:
      return {OmniboxResult::Kind::kUrl, L"http://" + std::wstring(input)};
    case HostKind::kPublic:
      return {OmniboxResult::Kind::kUrl, L"https://" + std::wstring(input)};
    case HostKind::kNotHost:
      break;
  }
  return make_search();
}

}  // namespace orbit
