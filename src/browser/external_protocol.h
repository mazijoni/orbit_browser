#pragma once

#include <string>

namespace orbit {

// UI thread. Asks the user whether to open |url| (an unknown-scheme link from
// the browser with |browser_id|) in an external application, then launches it
// via the shell if allowed.
void ConfirmExternalProtocol(int browser_id, const std::wstring& url);

}  // namespace orbit
