#include "browser/text_fetch.h"

#include <windows.h>

#include <cctype>

#include "browser/profile.h"
#include "core/strings.h"
#include "include/cef_urlrequest.h"

namespace orbit {
namespace {

constexpr size_t kMaxContentLength = 5 * 1024 * 1024;  // fetch() maxContentLength

bool IsTextMime(const std::string& mime) {
  if (mime.empty()) return true;
  return mime.rfind("text/", 0) == 0 || mime.find("xml") != std::string::npos ||
         mime.find("json") != std::string::npos || mime.find("javascript") != std::string::npos;
}

bool IsLatin1(std::string label) {
  for (char& c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return label == "iso-8859-1" || label == "latin1" || label == "windows-1252" ||
         label == "us-ascii" || label == "iso-8859-15";
}

// The body as UTF-8: a BOM or the header / XML declaration picks the
// encoding; anything else is read as UTF-8 (TextDecoder's default).
std::string DecodeBody(std::string body, const std::string& charset) {
  if (body.rfind("\xEF\xBB\xBF", 0) == 0) return body.substr(3);
  std::string label = charset;
  if (label.empty() && body.rfind("<?xml", 0) == 0) {
    const size_t end = body.find("?>");
    const size_t enc = body.find("encoding=");
    if (enc != std::string::npos && enc < end && enc + 10 < body.size()) {
      const char q = body[enc + 9];
      const size_t close = body.find(q, enc + 10);
      if (close != std::string::npos) label = body.substr(enc + 10, close - enc - 10);
    }
  }
  if (!IsLatin1(label) || body.empty()) return body;
  const int n = ::MultiByteToWideChar(1252, 0, body.data(), static_cast<int>(body.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(1252, 0, body.data(), static_cast<int>(body.size()), wide.data(), n);
  return ToUtf8(wide);
}

class TextFetch : public CefURLRequestClient {
 public:
  // |status|: the HTTP status, or -1 when the request failed / was refused.
  using Done = std::function<void(int status, const std::string& body)>;

  static void Start(Profile* profile, const std::wstring& url, const std::wstring& accept, bool cookies,
                    Done done) {
    CefRefPtr<CefRequest> request = CefRequest::Create();
    request->SetURL(url);
    request->SetMethod("GET");
    request->SetFlags(cookies ? UR_FLAG_ALLOW_STORED_CREDENTIALS : UR_FLAG_NONE);
    if (!accept.empty()) {
      CefRequest::HeaderMap headers;
      headers.insert({"Accept", accept});
      request->SetHeaderMap(headers);
    }
    CefRefPtr<TextFetch> client = new TextFetch(std::move(done));
    client->request_ = CefURLRequest::Create(request, client, profile->request_context());
    if (!client->request_) client->Finish(-1);
  }

  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
    CefRefPtr<CefResponse> response = request->GetResponse();
    int status = -1;
    if (request->GetRequestStatus() == UR_SUCCESS && response && !too_big_) {
      status = response->GetStatus();
      if (!IsTextMime(response->GetMimeType().ToString())) status = -1;
      charset_ = response->GetCharset().ToString();
    }
    Finish(status);
  }
  void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
  void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
  void OnDownloadData(CefRefPtr<CefURLRequest> request, const void* data, size_t length) override {
    if (body_.size() + length > kMaxContentLength) {
      too_big_ = true;
      request->Cancel();
      return;
    }
    body_.append(static_cast<const char*>(data), length);
  }
  bool GetAuthCredentials(bool, const CefString&, int, const CefString&, const CefString&,
                          CefRefPtr<CefAuthCallback>) override {
    return false;
  }

 private:
  explicit TextFetch(Done done) : done_(std::move(done)) {}
  void Finish(int status) {
    request_ = nullptr;  // breaks the request <-> client cycle
    if (Done done = std::move(done_)) done(status, status >= 0 ? DecodeBody(std::move(body_), charset_) : std::string());
  }

  Done done_;
  CefRefPtr<CefURLRequest> request_;
  std::string body_, charset_;
  bool too_big_ = false;
  IMPLEMENT_REFCOUNTING(TextFetch);
};

}  // namespace

void FetchText(Profile* profile, const std::wstring& url, const std::wstring& accept, bool cookies,
               TextFetchDone done) {
  TextFetch::Start(profile, url, accept, cookies, std::move(done));
}

}  // namespace orbit
