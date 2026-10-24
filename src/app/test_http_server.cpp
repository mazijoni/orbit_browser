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
  } else if (path == "/media") {
    // Plays the test tone with MediaSession metadata (the media card's source).
    body = "<!doctype html><title>media</title><audio id=a src=\"/tone.wav\" loop></audio><script>"
           "navigator.mediaSession.metadata = new MediaMetadata({title: 'Orbit Tone', artist: 'Self Test'});"
           "a.play().then(() => document.title = 'PLAYING');</script>";
  } else if (path == "/login") {
    // A form with a username and a password; submitting it (with the fields
    // set by the test) never leaves the page.
    body = "<!doctype html><title>login</title><body><form id=f onsubmit=\"return false\">"
           "<input id=u type=text name=user><input id=p type=password name=pass>"
           "<button type=submit>Sign in</button></form></body>";
  } else if (path == "/popupbtn") {
    // A big link at the top-left that opens a sign-in style popup window.
    body = "<!doctype html><title>popupbtn</title><style>body{margin:0}a{display:block;width:400px;height:200px;"
           "background:#ddd}</style><a href=\"#\" onclick=\"window.open(" +
           std::string(target.find("signinnav") != std::string::npos ? "'/signin-nav'"
                       : target.find("signin") != std::string::npos ? "'/signin'"
                                                                     : "'/page?t=PopupGlance'") +
           ",'login'," + std::string(target.find("plain") != std::string::npos ? "''" : "'width=420,height=320'") +
           ");return false\">sign in</a><script>addEventListener('message', (e) => {"
           " if (e.data === 'signed-in') location = '/page?t=Welcome'; });</script>";
  } else if (path == "/signin") {
    // Like a provider's sign-in popup: submitting reports to the opener and
    // closes the window at once (Google's sign-in does this).
    body = "<!doctype html><title>signin</title><body><form id=f>"
           "<input id=u type=email value=someone@example.com><input id=p type=password>"
           "<button type=submit>Next</button></form><script>"
           "f.addEventListener('submit', (e) => { e.preventDefault();"
           " if (window.opener) window.opener.postMessage('signed-in', '*'); window.close(); });"
           "</script></body>";
  } else if (path == "/signin-nav") {
    // A provider's sign-in popup that really submits: the password page posts,
    // the next page tells the opener and closes the popup.
    body = "<!doctype html><title>signin-nav</title><body><form id=f method=post action=\"/signin-done\">"
           "<input id=u type=email name=user autocomplete=username><input id=p type=password name=pass "
           "autocomplete=current-password><button type=submit>Next</button></form></body>";
  } else if (path == "/signin-done") {
    body = "<!doctype html><title>signin-done</title><body>signed in<script>"
           "if (window.opener) window.opener.postMessage('signed-in', '*'); setTimeout(() => window.close(), 200);"
           "</script></body>";
  } else if (path == "/login-nav") {
    // A sign-in form that really submits (the page navigates), which is what
    // makes Chromium's own password manager offer to save the login.
    body = "<!doctype html><title>login-nav</title><body><form id=f method=post action=\"/page?t=SignedIn\">"
           "<input id=u type=text name=user autocomplete=username><input id=p type=password name=pass autocomplete=current-password>"
           "<button type=submit>Sign in</button></form></body>";
  } else if (path == "/iconpage") {
    // A page with a red 16x16 favicon (a data: PNG).
    body = "<!doctype html><title>iconpage</title><link rel=icon href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAGUlEQVR4nGO4Iyf3nxLMMGrAqAGjBgwXAwDHMhcf6ozWIQAAAABJRU5ErkJggg==\">"
           "<body>icon</body>";
  } else if (path == "/fullscreen") {
    // A big button at the top-left that puts the page in fullscreen (like a
    // video's fullscreen button); the title follows the state.
    body = "<!doctype html><title>fs-off</title><style>body{margin:0}button{width:400px;height:200px}</style>"
           "<button onclick=\"document.documentElement.requestFullscreen()\">full</button><script>"
           "document.onfullscreenchange = () => document.title = document.fullscreenElement ? 'fs-on' : 'fs-off';"
           "</script>";
  } else if (path == "/text") {
    // /text?t=<title>&body=<text>: a page of plain text (find in page).
    body = "<!doctype html><title>" + EscapeHtml(QueryParam(target, "t")) + "</title><body><p>" +
           EscapeHtml(QueryParam(target, "body")) + "</p></body>";
  } else if (path == "/playmp4") {
    // Records a second of canvas video as H.264 (the engine's own encoder),
    // then plays it back: proves the H.264 decoder, not just canPlayType.
    body = R"html(<!doctype html><title>mp4 test</title><script>
(() => {
    try {
      const types = ['video/webm;codecs=h264', 'video/mp4;codecs=avc1.42E01E', 'video/mp4;codecs=avc1'];
      const mime = types.find((t) => MediaRecorder.isTypeSupported(t));
      if (!mime) { document.title = 'MP4:FAIL:no H.264 recorder'; return; }
      const c = document.createElement('canvas'); c.width = 160; c.height = 120;
      const g = c.getContext('2d');
      let n = 0;
      const tick = setInterval(() => { g.fillStyle = 'hsl(' + (n++ * 9) + ',70%,50%)'; g.fillRect(0, 0, 160, 120); }, 33);
      const rec = new MediaRecorder(c.captureStream(30), {mimeType: mime});
      const chunks = [];
      rec.ondataavailable = (e) => chunks.push(e.data);
      rec.onstop = () => {
        clearInterval(tick);
        const v = document.createElement('video');
        v.muted = true;
        v.src = URL.createObjectURL(new Blob(chunks, {type: mime.split(';')[0]}));
        const size = chunks.reduce((a, b) => a + b.size, 0);
        v.onerror = () => { document.title = 'MP4:FAIL:decode error ' + mime + ' bytes=' + size; };
        v.play().then(() => {
          const t0 = Date.now();
          const poll = setInterval(() => {
            if (v.currentTime > 0.1) { clearInterval(poll); document.title = 'MP4:OK:' + mime; }
            else if (Date.now() - t0 > 8000) { clearInterval(poll); document.title = 'MP4:FAIL:time stuck'; }
          }, 100);
        }, (e) => { document.title = 'MP4:FAIL:' + e + ' ' + mime + ' bytes=' + size; });
      };
      rec.start();
      setTimeout(() => rec.stop(), 1200);
    } catch (e) { document.title = 'MP4:FAIL:' + e; }
})();
</script>)html";
  } else if (path == "/links") {
    // A big link at the top-left; synthetic (untrusted) clicks are recorded
    // in the title and must never open a glance.
    body = "<!doctype html><title>links</title><style>body{margin:0}a{display:block;width:400px;"
           "height:200px;background:#ddd}</style><a id=l href=\"/page?t=Glanced\">glance me</a>"
           "<script>l.addEventListener('click', e => { if (!e.isTrusted) { e.preventDefault();"
           "document.title = 'SYNTH'; } });</script>";
  } else if (path == "/feed") {
    // An RSS feed for live folders: one item per id in "items".
    body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><rss version=\"2.0\"><channel>"
           "<title>Orbit Test Feed</title><link>http://127.0.0.1/</link>";
    const std::string items = QueryParam(target, "items");
    size_t start = 0;
    while (start < items.size()) {
      size_t comma = items.find(',', start);
      if (comma == std::string::npos) comma = items.size();
      const std::string id = EscapeHtml(items.substr(start, comma - start));
      body += "<item><title>Item " + id + "</title><link>http://127.0.0.1:" + std::to_string(port_) +
              "/page?t=Item-" + id + "</link><guid>item-" + id +
              "</guid><pubDate>Tue, 20 Oct 2026 10:00:00 GMT</pubDate></item>";
      start = comma + 1;
    }
    body += "</channel></rss>";
    type = "application/rss+xml";
  } else if (path == "/gh/pulls") {
    // github.com/pulls asked for JSON (pullsDashboardSurfaceContentRoute).
    const std::string base = "http://127.0.0.1:" + std::to_string(port_);
    body = "{\"payload\":{\"pullsDashboardSurfaceContentRoute\":{\"results\":["
           "{\"repoNameWithOwner\":\"orbit/browser\",\"number\":7,\"title\":\"Add live folders\","
           "\"author\":{\"displayLogin\":\"alice\"},\"permalink\":\"https://github.com/orbit/browser/pull/7\"},"
           "{\"repoNameWithOwner\":\"orbit/site\",\"number\":3,\"title\":\"Docs\","
           "\"author\":{\"displayLogin\":\"bob\"},\"permalink\":\"https://github.com/orbit/site/pull/3\"}]}}}";
    (void)base;
    type = "application/json";
  } else if (path == "/gh/issues") {
    body = "<!doctype html><ul><li><a data-testid=\"issue-listitem-title-link\" "
           "href=\"/orbit/browser/issues/12\">Crash on start</a><a data-testid=\"author-filter-link\" "
           "href=\"#\">carol</a></li></ul>";
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
