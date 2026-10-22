#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace orbit::self_test {

// Minimal loopback HTTP/1.1 server for self-tests (no internet dependency).
// Owns one worker thread that is always joined in Stop()/the destructor.
//   /page?t=<title>[&script=<js>]  HTML page with that title
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
