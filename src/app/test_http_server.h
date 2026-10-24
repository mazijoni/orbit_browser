#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace orbit::self_test {

// Minimal loopback HTTP/1.1 server for self-tests (no internet dependency).
// Owns one worker thread that is always joined in Stop()/the destructor.
//   /page?t=<title>[&script=<js>]  HTML page with that title
//   /text?t=<title>&body=<text>    HTML page with that title and text
//   /popupbtn                      a link that opens a popup window (window.open with features);
//                                  ?signin / ?signinnav pick the sign-in popups below, ?plain no features
//   /signin                        sign-in popup: submit messages the opener and closes at once
//   /signin-nav, /signin-done      sign-in popup that posts, then messages the opener and closes
//   /iconpage                      a page with a red 16x16 favicon
//   /login-nav                     a sign-in form that posts and navigates
//   /login                         a sign-in form (#u, #p), submit stays on the page
//   /file.bin                      1 MiB attachment
//   /attack?target=<orbit url>    page that tries to reach a orbit:// API
//   /audio                         page playing /tone.wav (title PLAYING once audible)
//   /feed?items=a,b                RSS feed with those item ids (live folders)
//   /gh/pulls, /gh/issues          GitHub-like pull request JSON / issue list HTML
//   /gh/missing                    404 (a signed-out GitHub)
class TestHttpServer {
 public:
  TestHttpServer() = default;
  ~TestHttpServer();
  TestHttpServer(const TestHttpServer&) = delete;
  TestHttpServer& operator=(const TestHttpServer&) = delete;

  bool Start();
  void Stop();
  int port() const { return port_; }
  std::wstring Url(const std::wstring& path) const;

  static constexpr size_t kFileSize = 1 << 20;

 private:
  void Run();
  void Serve(uintptr_t client);

  uintptr_t listen_socket_ = ~uintptr_t{0};
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

}  // namespace orbit::self_test
