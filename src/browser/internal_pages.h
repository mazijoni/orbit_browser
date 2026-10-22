#pragma once

#include <cstdint>
#include <string>

#include "include/cef_request_context.h"
#include "include/cef_scheme.h"

namespace orbit {

inline constexpr char kInternalScheme[] = "orbit";

// Registers orbit:// as a standard, secure, fetch-enabled scheme. Must run in
// every process (CefApp::OnRegisterCustomSchemes).
void RegisterInternalScheme(CefRawPtr<CefSchemeRegistrar> registrar);

// Installs the orbit:// handler on a profile's request context. Pages served
// for |profile_id| only ever show that profile's data.
void RegisterInternalSchemeHandler(CefRefPtr<CefRequestContext> context, int64_t profile_id,
                                   bool is_private);

// Request/response model used by the UI-thread page builders.
struct InternalRequest {
  int64_t profile_id = 0;
  bool is_private = false;
  std::wstring url;       // full orbit:// URL
  std::wstring host;      // e.g. "history"
  std::wstring path;      // e.g. "/api/delete"
  std::wstring query;     // without '?'
  std::wstring method;    // "GET" / "POST"
  std::string body;       // POST body (UTF-8)
  // True when the request comes from a orbit:// document of the same host
  // (e.g. orbit://history calling orbit://history/api/...). Required for
  // every /api/ endpoint.
  bool same_page = false;
  // same_page + POST + the X-Orbit-Action header. Required for state changes.
  bool trusted_action = false;
  // The requesting browser (0 when unknown); the library's API acts on the
  // window whose panel it is.
  int browser_id = 0;
};

struct InternalResponse {
  int status = 200;
  std::string mime_type = "text/html";
  std::string body;
};

// Implemented in internal_page_content.cpp; runs on the UI thread.
InternalResponse BuildInternalResponse(const InternalRequest& request);

}  // namespace orbit
