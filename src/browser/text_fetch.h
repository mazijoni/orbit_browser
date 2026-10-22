// One background GET in a profile's request context, returning a text body
// (nsZenLiveFolderProvider.fetch): at most 5 MB, text types only, decoded to
// UTF-8 (a BOM, the header or an XML declaration may say Latin-1). Used by
// live folders (with the profile's cookies) and the URL bar's search
// suggestions (without). UI thread.
#pragma once

#include <functional>
#include <string>

namespace orbit {

class Profile;

// |status|: the HTTP status, or -1 when the request failed or was refused.
using TextFetchDone = std::function<void(int status, const std::string& body)>;
void FetchText(Profile* profile, const std::wstring& url, const std::wstring& accept, bool cookies,
               TextFetchDone done);

}  // namespace orbit
