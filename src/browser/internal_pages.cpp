#include "browser/internal_pages.h"

#include <cstring>
#include <mutex>

#include "core/log.h"
#include "core/strings.h"
#include "include/base/cef_callback.h"
#include "include/cef_parser.h"
#include "include/cef_resource_handler.h"
#include "include/wrapper/cef_closure_task.h"

namespace orbit {
namespace {

// Serves one orbit:// request. Chromium calls the handler on the IO thread;
// the response is built on the UI thread (where all browser state lives) and
// handed back under a lock.
class InternalPageHandler : public CefResourceHandler {
 public:
  explicit InternalPageHandler(InternalRequest request) : request_(std::move(request)) {}

  bool Open(CefRefPtr<CefRequest> request, bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    handle_request = false;  // asynchronous: continue via |callback|
    CefRefPtr<InternalPageHandler> self(this);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<InternalPageHandler> handler,
                               CefRefPtr<CefCallback> cb) {
                              InternalResponse response = BuildInternalResponse(handler->request_);
                              ORBIT_LOG_VERBOSE("orbit:// response %d: %.120s", response.status,
                                                 response.mime_type == "application/json"
                                                     ? response.body.c_str()
                                                     : "(html)");
                              {
                                std::lock_guard<std::mutex> lock(handler->mutex_);
                                handler->response_ = std::move(response);
                              }
                              cb->Continue();
                            },
                            self, callback));
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& response_length,
                          CefString& redirect_url) override {
    std::lock_guard<std::mutex> lock(mutex_);
    response->SetStatus(response_.status);
    response->SetStatusText(response_.status == 200 ? "OK" : "Error");
    response->SetMimeType(response_.mime_type);
    response->SetCharset("utf-8");
    CefResponse::HeaderMap headers;
    // Internal pages run no third-party code; lock them down regardless.
    headers.emplace("Content-Security-Policy",
                    "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                    "img-src data: https: http:; connect-src orbit:; form-action 'none'; "
                    "frame-ancestors 'none'; base-uri 'none'");
    headers.emplace("X-Content-Type-Options", "nosniff");
    headers.emplace("Cache-Control", "no-store");
    response->SetHeaderMap(headers);
    response_length = static_cast<int64_t>(response_.body.size());
  }

  bool Read(void* data_out, int bytes_to_read, int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    bytes_read = 0;
    if (offset_ >= response_.body.size()) return false;
    const size_t n = std::min(static_cast<size_t>(bytes_to_read), response_.body.size() - offset_);
    std::memcpy(data_out, response_.body.data() + offset_, n);
    offset_ += n;
    bytes_read = static_cast<int>(n);
    return true;
  }

  void Cancel() override {}

 private:
  InternalRequest request_;
  std::mutex mutex_;
  InternalResponse response_;
  size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(InternalPageHandler);
};

class InternalSchemeFactory : public CefSchemeHandlerFactory {
 public:
  InternalSchemeFactory(int64_t profile_id, bool is_private)
      : profile_id_(profile_id), is_private_(is_private) {}

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    InternalRequest r;
    r.profile_id = profile_id_;
    r.is_private = is_private_;
    r.url = request->GetURL().ToWString();
    r.method = request->GetMethod().ToWString();
    CefURLParts parts;
    if (!CefParseURL(request->GetURL(), parts)) return nullptr;
    r.host = CefString(&parts.host).ToWString();
    r.path = CefString(&parts.path).ToWString();
    r.query = CefString(&parts.query).ToWString();

    if (r.method == L"POST") {
      if (CefRefPtr<CefPostData> post = request->GetPostData()) {
        CefPostData::ElementVector elements;
        post->GetElements(elements);
        for (auto& e : elements) {
          if (e->GetType() != PDE_TYPE_BYTES) continue;
          std::string chunk(e->GetBytesCount(), '\0');
          e->GetBytes(chunk.size(), chunk.data());
          r.body += chunk;
          if (r.body.size() > (16u << 20)) return nullptr;  // 16 MiB cap (bookmark import)
        }
      }
    }
    // API calls must come from the page that owns them. The gate is a custom
    // request header: browsers never let a cross-origin document (a web page
    // or another orbit:// host) attach custom headers without a CORS
    // preflight, and this scheme is not CORS-enabled, so no preflight can
    // succeed. "no-cors" requests cannot carry custom headers at all. This
    // holds regardless of timing, unlike the frame URL, which the IO thread can
    // briefly see as empty or stale during navigation (observed in self-tests).
    // When the frame URL is known it must also match, as defense in depth.
    auto is_same_page = [&](const CefString& url) {
      CefURLParts parts;
      if (url.empty() || !CefParseURL(url, parts)) return false;
      return ToLowerAscii(CefString(&parts.scheme).ToWString()) == L"orbit" &&
             ToLowerAscii(CefString(&parts.host).ToWString()) == ToLowerAscii(r.host);
    };
    const CefString frame_url = frame ? frame->GetURL() : CefString();
    const bool has_header = request->GetHeaderByName("X-Orbit-Request") == "1";
    r.same_page = frame && has_header && (frame_url.empty() || is_same_page(frame_url));
    if (!r.same_page && r.path.starts_with(L"/api/")) {
      // Logs internal hosts only, never web URLs (privacy rule in core/log.h).
      auto describe = [](const CefString& url) -> std::wstring {
        if (url.empty()) return L"none";
        const std::wstring u = ToLowerAscii(url.ToWString());
        return u.starts_with(L"orbit://") ? u.substr(0, u.find(L'/', 9)) : L"web";
      };
      ORBIT_LOG_INFO("rejected orbit://%ls%ls (frame: %ls, header: %d)", r.host.c_str(),
                      r.path.c_str(), describe(frame_url).c_str(), has_header ? 1 : 0);
    }
    r.trusted_action = r.same_page && r.method == L"POST";
    r.browser_id = browser ? browser->GetIdentifier() : 0;
    return new InternalPageHandler(std::move(r));
  }

 private:
  const int64_t profile_id_;
  const bool is_private_;
  IMPLEMENT_REFCOUNTING(InternalSchemeFactory);
};

}  // namespace

void RegisterInternalScheme(CefRawPtr<CefSchemeRegistrar> registrar) {
  registrar->AddCustomScheme(kInternalScheme, CEF_SCHEME_OPTION_STANDARD |
                                                  CEF_SCHEME_OPTION_SECURE |
                                                  CEF_SCHEME_OPTION_FETCH_ENABLED);
}

void RegisterInternalSchemeHandler(CefRefPtr<CefRequestContext> context, int64_t profile_id,
                                   bool is_private) {
  context->RegisterSchemeHandlerFactory(kInternalScheme, "",
                                        new InternalSchemeFactory(profile_id, is_private));
}

}  // namespace orbit
