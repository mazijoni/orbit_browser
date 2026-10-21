#include "core/external_scheme.h"

#include <array>

#include "core/strings.h"

namespace orbit {

bool IsExternalUrlLaunchable(std::wstring_view url, std::wstring* scheme_out) {
  if (url.empty() || url.size() > 2048) return false;
  for (wchar_t c : url) {
    if (c < 0x20 || c == 0x7F || c == L'"') return false;  // control chars / quoting tricks
  }
  const size_t colon = url.find(L':');
  if (colon == std::wstring_view::npos || colon < 2) return false;  // "c:" drive letters too
  for (size_t i = 0; i < colon; ++i) {
    const wchar_t c = url[i];
    const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                    (i > 0 && ((c >= L'0' && c <= L'9') || c == L'+' || c == L'-' || c == L'.'));
    if (!ok) return false;
  }
  const std::wstring scheme = ToLowerAscii(url.substr(0, colon));
  static constexpr std::array<std::wstring_view, 21> kDenied = {
      L"http",        L"https",           L"file",       L"ftp",          L"javascript",
      L"vbscript",    L"data",            L"blob",       L"filesystem",   L"about",
      L"shell",       L"ms-msdt",         L"search-ms",  L"search",       L"ms-cxh",
      L"ms-cxh-full", L"ms-appinstaller", L"hcp",        L"its",          L"ms-its",
      L"mk"};
  for (auto denied : kDenied) {
    if (scheme == denied) return false;
  }
  if (scheme_out) *scheme_out = scheme;
  return true;
}

}  // namespace orbit
