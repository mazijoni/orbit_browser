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
  static constexpr std::array<std::wstring_view, 9> kAllowed = {
      L"http", L"https", L"file", L"about", L"orbit", L"data", L"view-source", L"chrome",
      L"chrome-extension"};
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

namespace {
std::wstring NormalizeForPinCompare(std::wstring_view url) {
  std::wstring text(url.substr(0, url.find(L'#')));
  const size_t scheme_end = text.find(L"://");
  if (scheme_end != std::wstring::npos && text.find(L'/', scheme_end + 3) == std::wstring::npos) {
    text += L'/';
  }
  return text;
}
}  // namespace

bool PinnedUrlMatches(std::wstring_view current, std::wstring_view pinned) {
  return NormalizeForPinCompare(current) == NormalizeForPinCompare(pinned);
}

std::wstring SidebarDisplayUrl(std::wstring_view url) {
  std::wstring text(url);
  // BrowserUIUtils.removeSingleTrailingSlashFromURL: ^((http|https|ftp)://[^/]+)/$
  for (std::wstring_view scheme : {L"http://", L"https://", L"ftp://"}) {
    if (text.rfind(scheme, 0) == 0 && text.size() > scheme.size() + 1 &&
        text.find(L'/', scheme.size()) == text.size() - 1) {
      text.pop_back();
      break;
    }
  }
  constexpr std::wstring_view kHttps = L"https://";
  if (text.rfind(kHttps, 0) == 0) {
    const size_t host_end = text.find(L'/', kHttps.size());
    text = text.substr(kHttps.size(), host_end == std::wstring::npos
                                          ? std::wstring::npos
                                          : host_end - kHttps.size());
  }
  if (text.rfind(L"www.", 0) == 0) text.erase(0, 4);
  return text;
}

namespace {

// Curated from Firefox's query-stripping and strip-on-share lists.
constexpr std::wstring_view kStripGlobal[] = {
    L"fbclid", L"gclid", L"gclsrc", L"dclid", L"gbraid", L"wbraid", L"msclkid", L"mc_eid", L"mc_cid",
    L"oly_anon_id", L"oly_enc_id", L"__s", L"vero_id", L"vero_conv", L"_hsenc", L"_hsmi", L"mkt_tok",
    L"igshid", L"igsh", L"twclid", L"ttclid", L"wickedid", L"yclid", L"ysclid", L"srsltid", L"_openstat",
    L"rb_clickid", L"s_cid", L"ss_email_id", L"bsft_clkid", L"bsft_uid", L"ml_subscriber",
    L"ml_subscriber_hash", L"epik", L"pk_campaign", L"pk_kwd", L"piwik_campaign", L"piwik_kwd",
    L"mtm_campaign", L"mtm_cid", L"mtm_content", L"mtm_keyword", L"mtm_medium", L"mtm_source"};

struct SiteRule {
  std::wstring_view host;  // the host or a parent domain
  std::wstring_view param;
};
constexpr SiteRule kStripSites[] = {
    {L"youtube.com", L"si"}, {L"youtu.be", L"si"}, {L"youtube.com", L"pp"},
    {L"open.spotify.com", L"si"}, {L"twitter.com", L"s"}, {L"twitter.com", L"t"},
    {L"x.com", L"s"}, {L"x.com", L"t"}, {L"instagram.com", L"igsh"},
    {L"linkedin.com", L"trk"}, {L"linkedin.com", L"trackingId"}, {L"reddit.com", L"share_id"},
    {L"amazon.com", L"ref_"}, {L"tiktok.com", L"_t"}, {L"tiktok.com", L"_r"}};

bool HostIs(std::wstring_view host, std::wstring_view domain) {
  if (host.size() < domain.size()) return false;
  if (host.substr(host.size() - domain.size()) != domain) return false;
  return host.size() == domain.size() || host[host.size() - domain.size() - 1] == L'.';
}

}  // namespace

std::wstring StripTrackingParams(std::wstring_view url) {
  const bool https = StartsWithNoCase(url, L"https://");
  if (!https && !StartsWithNoCase(url, L"http://")) return std::wstring(url);
  const size_t host_start = https ? 8 : 7;
  const size_t host_end = url.find_first_of(L"/?#", host_start);
  std::wstring host = ToLowerAscii(url.substr(host_start, host_end == std::wstring_view::npos
                                                              ? std::wstring_view::npos
                                                              : host_end - host_start));
  if (const size_t at = host.rfind(L'@'); at != std::wstring::npos) host = host.substr(at + 1);
  if (const size_t colon = host.find(L':'); colon != std::wstring::npos) host.resize(colon);
  const size_t q = url.find(L'?');
  const size_t hash = url.find(L'#');
  if (q == std::wstring_view::npos || (hash != std::wstring_view::npos && hash < q)) return std::wstring(url);
  const size_t query_end = hash == std::wstring_view::npos ? url.size() : hash;
  const std::wstring_view query = url.substr(q + 1, query_end - q - 1);
  auto stripped = [&](std::wstring_view name) {
    const std::wstring lower = ToLowerAscii(name);
    if (lower.rfind(L"utm_", 0) == 0) return true;
    for (std::wstring_view p : kStripGlobal) {
      if (lower == p) return true;
    }
    for (const SiteRule& rule : kStripSites) {
      if (name == rule.param && HostIs(host, rule.host)) return true;
    }
    return false;
  };
  std::wstring kept;
  size_t start = 0;
  while (start <= query.size()) {
    size_t end = query.find(L'&', start);
    if (end == std::wstring_view::npos) end = query.size();
    const std::wstring_view pair = query.substr(start, end - start);
    const std::wstring_view name = pair.substr(0, pair.find(L'='));
    if (!pair.empty() && !stripped(name)) {
      if (!kept.empty()) kept += L'&';
      kept += pair;
    }
    if (end == query.size()) break;
    start = end + 1;
  }
  std::wstring out(url.substr(0, q));
  if (!kept.empty()) out += L"?" + kept;
  out += url.substr(query_end);
  return out;
}

}  // namespace orbit
