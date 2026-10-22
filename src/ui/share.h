#pragma once

#include <windows.h>

#include <string>

namespace orbit::ui {

// The Windows share sheet for a web link (Services.zen.share on Windows:
// DataTransferManager through IDataTransferManagerInterop). Returns false
// when the system cannot share (older Windows, activation failure).
bool ShareWebLink(HWND window, const std::wstring& url, const std::wstring& title);

}  // namespace orbit::ui
