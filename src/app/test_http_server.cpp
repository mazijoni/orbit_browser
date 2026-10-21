#include "app/test_http_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdlib>
#include <string>

#include "core/log.h"

namespace orbit::self_test {
namespace {

std::string QueryParam(const std::string& target, const std::string& key) {
  const size_t q = target.find('?');
  if (q == std::string::npos) return {};
  std::string query = target.substr(q + 1);
  size_t start = 0;
  while (start < query.size()) {
    size_t end = query.find('&', start);
    if (end == std::string::npos) end = query.size();
    const std::string pair = query.substr(start, end - start);
    if (pair.rfind(key + "=", 0) == 0) {
      std::string v = pair.substr(key.size() + 1), out;
      for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size()) {
          out.push_back(static_cast<char>(std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16)));
          i += 2;
        } else {
          out.push_back(v[i] == '+' ? ' ' : v[i]);
        }
      }
      return out;
    }
    start = end + 1;
  }
  return {};
}

std::string EscapeHtml(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '<') out += "&lt;";
    else if (c == '&') out += "&amp;";
    else out.push_back(c);
  }
  return out;
}

void SendAll(SOCKET s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int n = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
    if (n <= 0) return;
    sent += static_cast<size_t>(n);
  }
}

}  // namespace

TestHttpServer::~TestHttpServer() { Stop(); }

bool TestHttpServer::Start() {
  WSADATA wsa;
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral
  int len = sizeof(addr);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(s, 16) != 0 ||
      ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::closesocket(s);
    return false;
  }
  listen_socket_ = s;
  port_ = ntohs(addr.sin_port);
  worker_ = std::thread(&TestHttpServer::Run, this);
  return true;
}

void TestHttpServer::Stop() {
  if (!worker_.joinable()) return;
  stop_ = true;
  ::closesocket(static_cast<SOCKET>(listen_socket_));  // unblocks accept()
  worker_.join();
  ::WSACleanup();
}

std::wstring TestHttpServer::Url(const std::wstring& path) const {
  return L"http://127.0.0.1:" + std::to_wstring(port_) + path;
}

void TestHttpServer::Run() {
  while (!stop_) {
    SOCKET client = ::accept(static_cast<SOCKET>(listen_socket_), nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
    Serve(client);
    ::closesocket(client);
  }
}

void TestHttpServer::Serve(uintptr_t client_handle) {
  const SOCKET client = static_cast<SOCKET>(client_handle);
  DWORD timeout = 3000;
  ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
  std::string request;
  char buf[4096];
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
    const int n = ::recv(client, buf, sizeof(buf), 0);
    if (n <= 0) return;
    request.append(buf, static_cast<size_t>(n));
  }
  const size_t sp1 = request.find(' ');
  const size_t sp2 = request.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return;
  const std::string target = request.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string path = target.substr(0, target.find('?'));

  std::string body, type = "text/html; charset=utf-8", extra;
  int status = 200;
  if (path == "/page") {
    body = "<!doctype html><title>" + EscapeHtml(QueryParam(target, "t")) +
           "</title><body>test page</body>";
  } else if (path == "/file.bin") {
    body.assign(kFileSize, 'x');
    type = "application/octet-stream";
    extra = "Content-Disposition: attachment; filename=\"file.bin\"\r\n";
  } else if (path == "/audio") {
    body = "<!doctype html><title>audio</title><audio id=a src=\"/tone.wav\" loop autoplay></audio>"
           "<script>document.getElementById('a').addEventListener('playing',"
           "() => document.title = 'PLAYING');</script>";
  } else if (path == "/tone.wav") {
    // 2 s, 8 kHz, 16-bit mono, 440 Hz square-ish tone (quiet).
    const uint32_t rate = 8000, samples = rate * 2, data_bytes = samples * 2;
    auto u32 = [&](uint32_t v) { body.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { body.append(reinterpret_cast<const char*>(&v), 2); };
    body = "RIFF";
    u32(36 + data_bytes);
    body += "WAVEfmt ";
    u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    body += "data";
    u32(data_bytes);
    for (uint32_t i = 0; i < samples; ++i) u16(static_cast<uint16_t>((i / 9) % 2 ? 1500 : -1500));
    type = "audio/wav";
  } else if (path == "/vis") {
    // Reports Page Visibility state through the title (hidden tabs must be "hidden").
    body = "<!doctype html><title>VIS:?</title><script>"
           "const upd = () => document.title = 'VIS:' + document.visibilityState;"
           "document.addEventListener('visibilitychange', upd); upd();</script>";
  } else if (path == "/geo") {
    body = "<!doctype html><title>geo</title><script>"
           "navigator.geolocation.getCurrentPosition(() => document.title = 'GEO:ok',"
           "(e) => document.title = 'GEO:' + e.code, {timeout: 8000});</script>";
  } else if (path == "/notify") {
    body = "<!doctype html><title>notify</title><script>"
           "Notification.requestPermission().then(p => document.title = 'NOTIFY:' + p);</script>";
  } else if (path == "/attack") {
    // A hostile page trying to read and mutate orbit:// internals.
    const std::string t = QueryParam(target, "target");
    body = "<!doctype html><title>attack</title><script>(async () => {"
           "let read = 'blocked', post = 'blocked';"
           "try { const r = await fetch('orbit://history/api/list'); read = 'READ:' + r.status; } catch (e) {}"
           "try { await fetch('orbit://history/api/clear', {method: 'POST', mode: 'no-cors',"
           "  body: JSON.stringify({range: 'all'})}); post = 'sent'; } catch (e) {}"
           // With the gate header: needs a CORS preflight, which must fail.
           "try { await fetch('orbit://history/api/clear', {method: 'POST',"
           "  headers: {'X-Orbit-Request': '1'}, body: JSON.stringify({range: 'all'})});"
           "  post += '+header-sent'; } catch (e) { post += '+header-blocked'; }"
           "document.title = 'ATTACK:' + read + '|' + post; })();</script>";
  } else {
    status = 404;
    body = "not found";
  }
  std::string response = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found") +
                         "\r\nContent-Type: " + type + "\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n" + extra +
                         "\r\n" + body;
  SendAll(client, response);
}

}  // namespace orbit::self_test
