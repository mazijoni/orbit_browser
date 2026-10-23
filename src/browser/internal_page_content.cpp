// HTML for orbit:// pages, built on the UI thread from browser state. Pages
// are self-contained (inline CSS/JS, strict CSP set by the handler). Any
// string from the database or the web is escaped with HtmlEscape/JsonString.

#include <string>

#include "app/application.h"
#include "app/self_test.h"
#include "browser/internal_page_parts.h"
#include "browser/internal_pages.h"
#include "core/strings.h"

namespace orbit {

std::string PageShell(const std::string& title, const std::string& body,
                      const std::string& script) {
  Application* app = Application::Get();
  const bool dark = app->dark_mode();
  const uint32_t accent = app->accent_color();
  char accent_css[16];
  snprintf(accent_css, sizeof(accent_css), "#%06X", accent);
  std::string html;
  html.reserve(8192 + body.size());
  html += "<!doctype html><html lang=\"en\" data-theme=\"";
  html += dark ? "dark" : "light";
  html += "\"><head><meta charset=\"utf-8\"><title>";
  html += title;
  html += "</title><style>"
          ":root{--accent:";
  html += accent_css;
  html += R"css(;--radius:12px;font-family:"Segoe UI Variable Text","Segoe UI",system-ui,sans-serif;font-size:14px}
[data-theme=dark]{--bg:#202020;--surface:#2a2a2a;--surface2:#333333;--border:#3a3a3a;--text:#ececec;--muted:#a3a3a3;--danger:#f07c7c;color-scheme:dark}
[data-theme=light]{--bg:#ffffff;--surface:#f4f4f6;--surface2:#ebebee;--border:#e1e1e6;--text:#1b1b1d;--muted:#66666d;--danger:#c43b3b;color-scheme:light}
*{box-sizing:border-box}
[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--text);line-height:1.45}
main{max-width:880px;margin:0 auto;padding:40px 28px 64px}
h1{font-size:22px;font-weight:600;margin:0 0 20px}
h2{font-size:12px;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.05em;margin:28px 0 8px}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
input,select,button{font:inherit;color:inherit}
input[type=text],input[type=search],select{background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:8px 10px;outline:none}
input:focus-visible,select:focus-visible,button:focus-visible{outline:2px solid var(--accent);outline-offset:1px}
button{background:color-mix(in srgb,currentColor 10%,transparent);border:1px solid transparent;border-radius:8px;padding:7px 14px;cursor:pointer;font-weight:500}
button:hover{background:color-mix(in srgb,currentColor 15%,transparent)}
button.primary{background:color-mix(in srgb,var(--accent) 40%,var(--text));color:var(--bg)}
button.danger{color:var(--danger)}
.row{display:flex;align-items:center;gap:12px;padding:8px 10px;border-radius:8px}
.row:hover{background:var(--surface)}
.row .grow{flex:1;min-width:0}
.row .title{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.row .sub{color:var(--muted);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.muted{color:var(--muted)}
.toolbar{display:flex;gap:8px;align-items:center;margin-bottom:16px;flex-wrap:wrap}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:16px 18px;margin-bottom:12px}
.empty{color:var(--muted);padding:24px 10px}
)css";
  html += "</style></head><body>";
  html += body;
  if (!script.empty()) {
    html += "<script>";
    html += R"js(
// Every call to this page's own /api/ carries the X-Orbit-Request header;
// the browser rejects API requests without it (see internal_pages.cpp).
const nativeFetch = window.fetch.bind(window);
window.fetch = (input, init = {}) => {
  if (typeof input === 'string' && input.startsWith('/api/')) {
    init = Object.assign({}, init, {headers: Object.assign({'X-Orbit-Request': '1'}, init.headers || {})});
  }
  return nativeFetch(input, init);
};
async function orbitAction(path, data) {
  const r = await fetch(path, {method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(data || {})});
  if (!r.ok) throw new Error('action failed: ' + r.status);
  return r.json();
}
)js";
    html += script;
    html += "</script>";
  }
  html += "</body></html>";
  return html;
}

namespace {

InternalResponse NewTabPage(const InternalRequest& req) {
  // Zen's new tab is an empty page under the floating URL bar; Orbit adds
  // the most visited sites as a quiet row of tiles.
  std::string body = R"html(
<style>
main.ntp{display:flex;flex-direction:column;align-items:center;justify-content:flex-end;min-height:100vh;padding:0 28px 14vh}
#top{display:flex;flex-wrap:wrap;justify-content:center;gap:8px;width:min(720px,100%)}
#top a{width:96px}
#top a{opacity:.85}#top a:hover{opacity:1}
</style>
<main class="ntp">
  <nav id="top" aria-label="Frequently visited"></nav>
</main>)html";
  std::string script;
  script += R"js(
fetch('/api/top').then(r => r.json()).then(({items}) => {
  const top = document.getElementById('top');
  for (const it of items) {
    let host = it.url; try { host = new URL(it.url).hostname.replace(/^www\./, ''); } catch {}
    const a = document.createElement('a');
    a.href = /^https?:/i.test(it.url) ? it.url : '#';
    a.title = it.title || it.url;
    a.style.cssText = 'display:flex;flex-direction:column;align-items:center;gap:8px;padding:12px 6px;border-radius:12px;color:var(--text);text-decoration:none';
    a.onmouseenter = () => a.style.background = 'var(--surface)';
    a.onmouseleave = () => a.style.background = '';
    const tile = document.createElement('span');
    tile.textContent = host.charAt(0).toUpperCase();
    tile.style.cssText = 'width:44px;height:44px;border-radius:12px;display:grid;place-items:center;background:var(--surface2);font-weight:600;font-size:18px';
    const label = document.createElement('span');
    label.textContent = host;
    label.style.cssText = 'font-size:12px;max-width:88px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap';
    a.append(tile, label);
    top.append(a);
  }
});
)js";
  return {200, "text/html", PageShell("New Tab", body, script)};
}

// Test fixture page, served only while a self-test runs. Reports results
// through document.title, which the test reads via OnTitleChange.
InternalResponse SelfTestPage(const InternalRequest& req) {
  const std::string script = R"js(
const p = new URLSearchParams(location.search);
function idb(mode, value) {
  return new Promise((resolve) => {
    const open = indexedDB.open('selftest', 1);
    open.onupgradeneeded = () => open.result.createObjectStore('s');
    open.onerror = () => resolve('error');
    open.onsuccess = () => {
      const tx = open.result.transaction('s', 'readwrite');
      const store = tx.objectStore('s');
      if (mode === 'write') {
        store.put(value, 'k');
        tx.oncomplete = () => resolve('ok');
      } else {
        const get = store.get('k');
        get.onsuccess = () => resolve(get.result === undefined ? 'none' : get.result);
      }
    };
  });
}
(async () => {
  const op = p.get('op');
  if (op === 'write') {
    localStorage.setItem('k', p.get('v'));
    await idb('write', p.get('v'));
    document.title = 'DONE';
  } else if (op === 'read') {
    document.title = 'R:' + localStorage.getItem('k') + '|' + await idb('read');
  } else if (op === 'codecs') {
    const v = document.createElement('video');
    const r = (t) => v.canPlayType(t) || 'no';
    document.title = 'C:h264=' + r('video/mp4; codecs="avc1.42E01E"') + '|aac=' + r('audio/mp4; codecs="mp4a.40.2"') +
      '|vp9=' + r('video/webm; codecs="vp9"') + '|av1=' + r('video/mp4; codecs="av01.0.05M.08"') +
      '|hls=' + r('application/vnd.apple.mpegurl');
  } else if (op === 'popup') {
    window.open('orbit://selftest/?t=Opened');  // no user gesture: must be blocked
    setTimeout(() => { document.title = 'POPUP-TRIED'; }, 300);
  } else {
    document.title = p.get('t') || 'Untitled';
  }
})();
)js";
  return {200, "text/html", PageShell("Loading", "<main><p>self-test</p></main>", script)};
}

InternalResponse NotFound(const InternalRequest& req) {
  const std::string body = "<main><h1>Page not found</h1><p class=\"muted\">" +
                           HtmlEscape(req.url) + " does not exist.</p></main>";
  return {404, "text/html", PageShell("Not found", body, "")};
}

}  // namespace

InternalResponse BuildInternalResponse(const InternalRequest& req) {
  Application* app = Application::Get();
  if (!app) return {503, "text/plain", "unavailable"};
  Profile* profile = app->FindAnyProfile(req.profile_id);
  if (req.path.starts_with(L"/api/")) {
    // APIs answer only their own page (see InternalRequest::same_page).
    if (!req.same_page || !profile) {
      return {403, "application/json", "{\"error\":\"forbidden\"}"};
    }
    if (req.host == L"history") return HistoryApi(req, profile);
    if (req.host == L"bookmarks") return BookmarksApi(req, profile);
    if (req.host == L"downloads") return DownloadsApi(req, profile);
    if (req.host == L"settings") return SettingsApi(req, profile);
    if (req.host == L"newtab") return NewTabApi(req, profile);
    if (req.host == L"diagnostics") return DiagnosticsApi(req, profile);
    if (req.host == L"library") return LibraryApi(req, profile);
    if (req.host == L"sitedata") return SiteDataApi(req, profile);
    if (req.host == L"welcome") return WelcomeApi(req, profile);
    if (req.host == L"theme") return ThemeApi(req, profile);
    if (req.host == L"boost") return BoostApi(req, profile);
    if (req.host == L"extensions") return ExtensionsApi(req, profile);
    if (req.host == L"find") return FindApi(req, profile);
    return {404, "application/json", "{\"error\":\"not found\"}"};
  }
  if (req.host == L"newtab") return NewTabPage(req);
  if (req.host == L"history") return HistoryPage(req);
  if (req.host == L"bookmarks") return BookmarksPage(req);
  if (req.host == L"downloads") return DownloadsPage(req);
  if (req.host == L"settings") return SettingsPage(req);
  if (req.host == L"diagnostics") return DiagnosticsPage(req);
  if (req.host == L"library") return LibraryPage(req);
  if (req.host == L"sitedata") return SiteDataPage(req);
  if (req.host == L"welcome") return WelcomePage(req);
  if (req.host == L"theme") return ThemePage(req);
  if (req.host == L"boost") return BoostPage(req);
  if (req.host == L"extensions") return ExtensionsPage(req);
  if (req.host == L"find") return FindPage(req);
  if (req.host == L"selftest" && self_test::IsActive()) return SelfTestPage(req);
  return NotFound(req);
}

}  // namespace orbit
