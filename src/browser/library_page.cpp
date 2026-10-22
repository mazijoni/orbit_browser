// orbit://library/: the page inside a window's library panel (Zen's
// <zen-library>: section tabs on the left — Spaces, Downloads, History —
// and the section on the right). Data comes from this page's own /api/,
// which forwards history and downloads calls to their pages' handlers.

#include <string>

#include "app/application.h"
#include "browser/internal_page_parts.h"
#include "core/strings.h"

namespace orbit {
namespace {

// "RRGGBB" only (the window passes its backdrop colours in the query).
std::string HexColor(const std::wstring& query, const std::wstring& key, const char* fallback) {
  const std::wstring needle = key + L"=";
  size_t at = query.find(needle);
  while (at != std::wstring::npos && at > 0 && query[at - 1] != L'&') {
    at = query.find(needle, at + 1);
  }
  if (at == std::wstring::npos) return fallback;
  const std::wstring value = query.substr(at + needle.size(), 6);
  if (value.size() != 6) return fallback;
  for (wchar_t c : value) {
    if (!iswxdigit(c)) return fallback;
  }
  return "#" + std::string(value.begin(), value.end());
}

}  // namespace

InternalResponse LibraryPage(const InternalRequest& req) {
  const std::string bg = HexColor(req.query, L"bg", "#2b2b33");
  const std::string bg2 = HexColor(req.query, L"bg2", "#24242a");
  // :root[zen-right-side] zen-library: the section tabs on the right.
  const bool right = req.query.find(L"right=1") != std::wstring::npos;
  std::string body = R"html(
<style>
html,body{height:100%;overflow:hidden}
body{background:linear-gradient(135deg,)html" + bg + "," + bg2 + R"html();-webkit-user-select:none;user-select:none}
.ico{font-family:"Segoe Fluent Icons","Segoe MDL2 Assets";font-style:normal;font-size:16px;line-height:1}
#lib{display:flex;height:100%)html" + std::string(right ? ";flex-direction:row-reverse" : "") + R"html(}
#side{width:110px;display:flex;flex-direction:column;justify-content:space-between;padding:12px 8px;
  background:linear-gradient(to right,transparent,rgba(255,255,255,.08));box-shadow:9px 0 17px rgba(0,0,0,.04)}
.tab{display:flex;flex-direction:column;align-items:center;gap:6px;width:100%;padding:10px 4px;border-radius:12px;
  background:transparent;border:0;color:var(--muted);font-size:12px}
.tab .ico{font-size:22px}
.tab:hover{background:color-mix(in srgb,currentColor 7%,transparent);color:var(--text)}
.tab[aria-selected=true]{background:color-mix(in srgb,currentColor 12%,transparent);color:var(--text)}
#close{align-self:center;width:34px;height:34px;padding:0;border-radius:999px;display:grid;place-items:center}
#content{flex:1;min-width:0;display:flex;flex-direction:column;padding:18px 18px 0}
#content h1{font-size:18px;margin:2px 4px 12px;display:flex;align-items:center;gap:8px}
#content h1 .grow{flex:1}
#content h1 button{font-size:13px;font-weight:500;padding:5px 12px}
.list{flex:1;overflow:auto;padding:0 0 18px;scrollbar-width:thin}
.pill{background:color-mix(in srgb,currentColor 7%,transparent);border-radius:12px;padding:8px 10px;display:flex;align-items:center;gap:10px;cursor:default}
.pill:hover{background:color-mix(in srgb,currentColor 12%,transparent)}
.day{font-size:12px;color:var(--muted);margin:14px 6px 6px;font-weight:600}
.item{display:flex;align-items:center;gap:10px;padding:7px 8px;border-radius:10px;cursor:default}
.item:hover{background:color-mix(in srgb,currentColor 7%,transparent)}
.tile{width:24px;height:24px;flex:none;border-radius:7px;display:grid;place-items:center;font-size:12px;font-weight:600;
  background:color-mix(in srgb,currentColor 12%,transparent)}
.item .grow{flex:1;min-width:0}
.t{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.s{font-size:12px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.item button{padding:4px 8px;font-size:12px}
input[type=search]{width:100%;margin:0 0 6px;background:color-mix(in srgb,currentColor 7%,transparent);border:0;border-radius:10px}
.bar{height:4px;border-radius:2px;background:color-mix(in srgb,currentColor 12%,transparent);margin-top:5px;overflow:hidden}
.bar>i{display:block;height:100%;background:var(--accent)}
.space{margin:0 0 10px;padding:10px;border-radius:14px;background:color-mix(in srgb,currentColor 5%,transparent)}
.space[data-current=true]{outline:2px solid color-mix(in srgb,var(--accent) 60%,transparent)}
.space header{display:flex;align-items:center;gap:8px;padding:4px 6px 8px;font-weight:600;cursor:default}
.space header .badge{font-size:11px;font-weight:500;color:var(--muted)}
.space header:hover{color:var(--accent)}
.menu{position:absolute;right:18px;top:52px;background:var(--surface2);border-radius:10px;padding:6px;box-shadow:0 8px 24px rgba(0,0,0,.3);z-index:2}
.menu button{display:block;width:100%;text-align:left;background:transparent}
.menu button:hover{background:color-mix(in srgb,currentColor 10%,transparent)}
</style>
<div id="lib">
  <nav id="side">
    <div role="tablist" aria-orientation="vertical">
      <button class="tab" role="tab" data-s="spaces"><i class="ico">&#xE8A9;</i><span>Spaces</span></button>
      <button class="tab" role="tab" data-s="downloads"><i class="ico">&#xE896;</i><span>Downloads</span></button>
      <button class="tab" role="tab" data-s="history"><i class="ico">&#xE81C;</i><span>History</span></button>
    </div>
    <button id="close" title="Close library" aria-label="Close library"><i class="ico">&#xE8BB;</i></button>
  </nav>
  <section id="content" role="tabpanel"></section>
</div>)html";
  std::string script = R"js(
const content = document.getElementById('content');
const tabs = [...document.querySelectorAll('.tab')];
let section = new URLSearchParams(location.search).get('section') || 'history';
let poll = 0;
const el = (tag, props, ...kids) => { const e = Object.assign(document.createElement(tag), props || {}); e.append(...kids); return e; };
const hostOf = (u) => { try { return new URL(u).hostname.replace(/^www\./, ''); } catch { return u; } };
const tile = (u) => el('span', {className: 'tile', textContent: (hostOf(u) || '?').charAt(0).toUpperCase()});
const open = (url) => orbitAction('/api/open', {url});
document.getElementById('close').onclick = () => orbitAction('/api/close');

function show(name) {
  section = name;
  for (const t of tabs) t.setAttribute('aria-selected', String(t.dataset.s === name));
  clearInterval(poll);
  content.textContent = '';
  orbitAction('/api/section', {name}).catch(() => {});
  ({spaces: showSpaces, downloads: showDownloads, history: showHistory})[name]();
}
for (const t of tabs) t.onclick = () => show(t.dataset.s);

// --- Spaces (ZenLibrarySpacesSection): every space with its tabs.
async function showSpaces() {
  const list = el('div', {className: 'list'});
  content.append(el('h1', null, el('span', {className: 'grow', textContent: 'Spaces'}),
      el('button', {textContent: 'Create Space', onclick: () => orbitAction('/api/spaces/new')})), list);
  const {spaces} = await (await fetch('/api/spaces')).json();
  for (const s of spaces) {
    const card = el('div', {className: 'space'});
    card.dataset.current = String(s.current);
    const head = el('header', null, el('span', {textContent: s.icon || '•'}),
        el('span', {className: 't', textContent: s.name}),
        el('span', {className: 'badge', textContent: s.tabs.length + (s.tabs.length === 1 ? ' tab' : ' tabs')}));
    head.onclick = () => orbitAction('/api/spaces/show', {space: s.id, tab: -1});
    card.append(head);
    s.tabs.forEach((t, i) => {
      const row = el('div', {className: 'item'}, tile(t.url),
          el('div', {className: 'grow'}, el('div', {className: 't', textContent: t.title || t.url}),
              el('div', {className: 's', textContent: (t.pinned ? 'Pinned · ' : '') + hostOf(t.url)})));
      row.onclick = () => orbitAction('/api/spaces/show', {space: s.id, tab: t.index});
      card.append(row);
    });
    list.append(card);
  }
}

// --- Downloads (ZenLibraryDownloadsSection).
function showDownloads() {
  const list = el('div', {className: 'list'});
  content.append(el('h1', null, el('span', {className: 'grow', textContent: 'Downloads'})), list);
  const fmt = (n) => n >= 1 << 20 ? (n / (1 << 20)).toFixed(1) + ' MB' : n >= 1024 ? Math.round(n / 1024) + ' KB' : n + ' B';
  const act = (name, id) => orbitAction('/api/downloads/' + name, {id}).then(load);
  async function load() {
    const {items} = await (await fetch('/api/downloads/list')).json();
    list.textContent = '';
    if (!items.length) list.append(el('div', {className: 'empty', textContent: 'No downloads yet.'}));
    for (const d of items) {
      const running = d.state === 0, paused = d.state === 4, done = d.state === 1;  // DownloadState
      const status = done ? fmt(d.total || d.received) + ' · ' + hostOf(d.url)
          : running || paused ? fmt(d.received) + (d.total ? ' of ' + fmt(d.total) : '') + (paused ? ' · Paused' : '')
          : 'Failed or cancelled';
      const info = el('div', {className: 'grow'}, el('div', {className: 't', textContent: d.name}),
          el('div', {className: 's', textContent: status}));
      if ((running || paused) && d.total) info.append(el('div', {className: 'bar'}, el('i', {style: 'width:' + Math.round(100 * d.received / d.total) + '%'})));
      const row = el('div', {className: 'item'}, el('span', {className: 'tile ico', innerHTML: '&#xE896;'}), info);
      if (done && d.exists) row.append(el('button', {textContent: 'Show', onclick: (e) => { e.stopPropagation(); act('show', d.id); }}));
      if (running) row.append(el('button', {textContent: 'Pause', onclick: () => act('pause', d.id)}));
      if (paused) row.append(el('button', {textContent: 'Resume', onclick: () => act('resume', d.id)}));
      if (running || paused) row.append(el('button', {textContent: 'Cancel', onclick: () => act('cancel', d.id)}));
      if (done && d.exists) row.ondblclick = () => act('open', d.id);
      list.append(row);
    }
  }
  load();
  poll = setInterval(load, 1000);
}

// --- History (ZenLibraryHistorySection): by day, searchable, clearable.
function showHistory() {
  const search = el('input', {type: 'search', placeholder: 'Search history', ariaLabel: 'Search history'});
  const list = el('div', {className: 'list'});
  const clear = el('button', {textContent: 'Clear…'});
  content.append(el('h1', null, el('span', {className: 'grow', textContent: 'History'}), clear), search, list);
  clear.onclick = () => {
    const menu = el('div', {className: 'menu'});
    for (const [range, label] of [['hour', 'Last hour'], ['day', 'Today'], ['week', 'Last week'], ['month', 'Last month'], ['all', 'Everything']]) {
      menu.append(el('button', {textContent: label, onclick: async () => {
        menu.remove();
        if (!confirm('Clear history?\nThe pages you visited in this time frame will be forgotten. This cannot be undone.')) return;
        await orbitAction('/api/history/clear', {range}); reload();
      }}));
    }
    content.append(menu);
    setTimeout(() => addEventListener('click', () => menu.remove(), {once: true}));
  };
  let before = 0, done = false, busy = false, lastDay = '';
  async function more() {
    if (busy || done) return;
    busy = true;
    const q = new URLSearchParams({q: search.value, before: String(before)});
    const {items} = await (await fetch('/api/history/list?' + q)).json();
    busy = false;
    if (!items.length) { done = true; if (!before) list.append(el('div', {className: 'empty', textContent: search.value ? 'No matches.' : 'No history yet.'})); return; }
    const today = new Date().toDateString(), yesterday = new Date(Date.now() - 864e5).toDateString();
    for (const h of items) {
      const d = new Date(h.time), key = d.toDateString();
      if (key !== lastDay) {
        lastDay = key;
        list.append(el('div', {className: 'day', textContent: key === today ? 'Today' : key === yesterday ? 'Yesterday' :
            d.toLocaleDateString(undefined, {weekday: 'long', month: 'long', day: 'numeric'})}));
      }
      const row = el('div', {className: 'item'}, tile(h.url),
          el('div', {className: 'grow'}, el('div', {className: 't', textContent: h.title || h.url}),
              el('div', {className: 's', textContent: hostOf(h.url) + ' · ' + d.toLocaleTimeString(undefined, {hour: '2-digit', minute: '2-digit'})})));
      row.onclick = () => open(h.url);
      list.append(row);
      before = h.time;
    }
  }
  function reload() { before = 0; done = false; lastDay = ''; list.textContent = ''; more(); }
  let t = 0;
  search.oninput = () => { clearTimeout(t); t = setTimeout(reload, 150); };
  list.onscroll = () => { if (list.scrollTop + list.clientHeight > list.scrollHeight - 200) more(); };
  more();
}

window.orbitRefresh = () => show(section);
window.orbitShowSection = (name) => show(['spaces', 'downloads', 'history'].includes(name) ? name : section);
show(['spaces', 'downloads', 'history'].includes(section) ? section : 'history');
)js";
  return {200, "text/html", PageShell("Library", body, script)};
}

// orbit://sitedata/: ZenSiteDataPanel. Header buttons (copy, bookmark),
// the connection's security, the site's permissions with Allowed/Blocked,
// clearing its data, and a link to all site settings.
InternalResponse SiteDataPage(const InternalRequest&) {
  std::string body = R"html(
<style>
html,body{height:100%}
body{-webkit-user-select:none;user-select:none;font-size:13px}
.ico{font-family:"Segoe Fluent Icons","Segoe MDL2 Assets";font-style:normal;font-size:15px;line-height:1}
#panel{padding:12px;display:flex;flex-direction:column;gap:10px}
.head{display:flex;gap:8px}
.head button{flex:1;display:flex;flex-direction:column;align-items:center;gap:6px;padding:10px 4px;border-radius:10px;font-size:11px}
.head button[aria-pressed=true]{color:var(--accent)}
.site{display:flex;align-items:center;gap:10px;padding:2px 4px}
.site .t{font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.sec{display:flex;align-items:center;gap:10px;padding:10px;border-radius:10px;background:var(--surface)}
.sec[data-level=secure] .ico{color:#3fb950}
.sec[data-level=not-secure] .ico,.sec[data-level=dangerous] .ico{color:var(--danger)}
h3{font-size:11px;text-transform:uppercase;letter-spacing:.05em;color:var(--muted);margin:6px 4px 0}
.perm{display:flex;align-items:center;gap:8px;padding:6px 4px}
.perm .grow{flex:1;min-width:0}
.perm select{padding:4px 6px}
.links{display:flex;flex-direction:column;gap:6px}
.links button{text-align:left}
</style>
<div id="panel">
  <div class="head">
    <button id="copy"><i class="ico">&#xE8C8;</i>Copy URL</button>
    <button id="bookmark" aria-pressed="false"><i class="ico">&#xE734;</i>Bookmark</button>
    <button id="boost" aria-pressed="false"><i class="ico">&#xE790;</i>Boost</button>
    <button id="settings"><i class="ico">&#xE713;</i>Settings</button>
  </div>
  <div class="site"><div class="grow" style="min-width:0"><div class="t" id="host"></div><div class="muted t" id="title"></div></div></div>
  <div class="sec" id="sec"><i class="ico"></i><div><div id="secLabel" style="font-weight:600"></div><div class="muted" id="secText"></div></div></div>
  <h3 id="permsHead">Permissions</h3>
  <div id="perms"></div>
  <div class="links">
    <button id="clear" class="danger">Clear cookies and site data</button>
    <button id="all">All Site Settings</button>
  </div>
</div>)html";
  std::string script = R"js(
const $ = (id) => document.getElementById(id);
const levels = {
  secure: ['', 'Secure', 'Connection is secure. The page is served over HTTPS with a valid certificate.'],
  'not-secure': ['', 'Not Secure', 'Information you send or receive on this page could be read or changed by others.'],
  dangerous: ['', 'Not Secure', 'This page has certificate errors or runs insecure content. Do not enter passwords or payment information.'],
};
async function load() {
  const s = await (await fetch('/api/info')).json();
  let host = s.url; try { host = new URL(s.url).host || s.url; } catch {}
  $('host').textContent = host; $('title').textContent = s.title;
  $('copy').disabled = !s.canCopy;
  $('boost').disabled = !s.canBoost;  // canBoostSite
  $('boost').setAttribute('aria-pressed', String(s.boosted));
  $('bookmark').setAttribute('aria-pressed', String(s.bookmarked));
  const level = levels[s.security];
  $('sec').hidden = !level;  // internal pages have no connection to describe (_isSecureInternalUI)
  if (level) {
    $('sec').dataset.level = s.security;
    $('sec').querySelector('.ico').textContent = level[0];
    $('secLabel').textContent = level[1]; $('secText').textContent = level[2];
  }
  $('clear').hidden = !s.origin;
  const perms = $('perms');
  perms.textContent = '';
  $('permsHead').hidden = !s.permissions.length;
  for (const p of s.permissions) {
    const select = document.createElement('select');
    select.setAttribute('aria-label', p.label);
    for (const [v, t] of [['allow', 'Allowed'], ['block', 'Blocked']]) select.append(new Option(t, v, false, (v === 'allow') === p.allowed));
    select.onchange = () => orbitAction('/api/permission', {bit: p.bit, allow: select.value === 'allow'});
    const reset = document.createElement('button');
    reset.textContent = 'Reset'; reset.title = 'Ask again next time';
    reset.onclick = async () => { await orbitAction('/api/permission-reset', {bit: p.bit}); load(); };
    const label = document.createElement('div');
    label.className = 'grow'; label.textContent = p.label;
    const row = document.createElement('div');
    row.className = 'perm'; row.append(label, select, reset);
    perms.append(row);
  }
  document.title = 'Site information';
}
$('copy').onclick = () => orbitAction('/api/copy');
$('bookmark').onclick = async () => { await orbitAction('/api/bookmark'); setTimeout(load, 100); };
$('settings').onclick = $('all').onclick = () => orbitAction('/api/settings');
$('boost').onclick = () => orbitAction('/api/boost');
$('clear').onclick = async () => { $('clear').disabled = true; await orbitAction('/api/clear-site-data'); };
load();
)js";
  return {200, "text/html", PageShell("Loading", body, script)};
}

// orbit://welcome/: ZenWelcome's pages — welcome, import, default browser,
// search engine, Essentials, space colour, all set. Every step can be skipped.
InternalResponse WelcomePage(const InternalRequest&) {
  std::string body = R"html(
<style>
html,body{height:100%}
body{display:grid;place-items:center;background:radial-gradient(circle at 30% 20%,color-mix(in srgb,var(--accent) 28%,var(--bg)),var(--bg) 70%)}
#card{width:min(760px,92vw);min-height:460px;display:flex;gap:28px;padding:36px;border-radius:24px;background:var(--surface);box-shadow:0 20px 60px rgba(0,0,0,.25)}
#text{flex:1;display:flex;flex-direction:column;gap:12px}
#text h1{font-size:30px;line-height:1.15;margin:0}
#text p{color:var(--muted);margin:0}
#nav{margin-top:auto;display:flex;gap:8px}
#content{flex:1.1;display:flex;flex-direction:column;gap:10px;justify-content:center}
.opt{display:flex;align-items:center;gap:10px;padding:12px 14px;border-radius:12px;background:var(--surface2);cursor:pointer;text-align:left}
.opt[aria-checked=true]{outline:2px solid var(--accent)}
.apps{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.app{display:flex;flex-direction:column;align-items:center;gap:8px;padding:14px 8px;border-radius:14px;background:var(--surface2)}
.app span.tile{width:36px;height:36px;border-radius:10px;display:grid;place-items:center;color:#fff;font-weight:700}
.app[aria-pressed=true]{outline:2px solid var(--accent)}
.colors{display:flex;flex-wrap:wrap;gap:12px}
.colors button{width:40px;height:40px;border-radius:50%;padding:0}
.colors button[aria-pressed=true]{outline:3px solid var(--text);outline-offset:3px}
</style>
<div id="card"><div id="text"></div><div id="content"></div></div>)html";
  std::string script = R"js(
const text = document.getElementById('text'), content = document.getElementById('content');
const el = (tag, props, ...kids) => { const e = Object.assign(document.createElement(tag), props || {}); e.append(...kids); return e; };
let state, step = 0;
const choice = (label, checked, onclick) => {
  const b = el('button', {className: 'opt', role: 'radio', onclick});
  b.setAttribute('aria-checked', String(checked));
  b.innerHTML = '<span>' + label + '</span>';  // one flex item: no gap after <strong>
  return b;
};
const pages = [
  {title: 'Welcome to<br>a calmer internet', text: [], next: 'Get started', render() {}},
  {title: 'Bring your stuff along.', text: ['Import your bookmarks from another browser (an exported bookmarks HTML file) and pick up right where you left off.'],
   render() {
     const file = el('input', {type: 'file', accept: '.html,.htm', hidden: true});
     const status = el('p', {className: 'muted'});
     file.onchange = async () => {
       const html = await file.files[0].text();
       const r = await orbitAction('/api/import', {html});
       status.textContent = 'Imported ' + r.imported + ' bookmarks.';
     };
     content.append(choice('<strong>YES</strong>, import bookmarks from a file.', false, () => file.click()),
                    choice('<strong>NO</strong>, start fresh.', false, next), file, status);
   }},
  {title: 'Make Orbit your default?', text: ['Links from other apps will open in Orbit. You can always change this later.'],
   render() {
     content.append(choice('<strong>YES</strong>, make Orbit default.', false, async () => { await orbitAction('/api/default-browser'); next(); }),
                    choice('<strong>NO</strong>, still exploring my options.', false, next));
   }},
  {title: 'Pick your search engine.', text: ['Choose your default search engine. You can always change it later!'],
   render() {
     state.engines.forEach((e, i) => content.append(choice(e.name, e.current, async () => {
       await orbitAction('/api/search', {index: i}); state.engines.forEach((x, j) => x.current = j === i); show();
     })));
   }},
  {title: 'Choose the apps you use most.', text: ['Select your favorite apps to keep them handy in your sidebar as Essentials.'],
   skip: () => state.private, picked: new Set(), render() {
     const grid = el('div', {className: 'apps'});
     state.apps.forEach((a, i) => {
       const b = el('button', {className: 'app', onclick: () => { this.picked.has(i) ? this.picked.delete(i) : this.picked.add(i); show(); }},
         el('span', {className: 'tile', style: 'background:#' + a.color, textContent: a.name[0]}), el('span', {textContent: a.name}));
       b.setAttribute('aria-pressed', String(this.picked.has(i)));
       grid.append(b);
     });
     content.append(grid);
   }, async leave() { if (this.picked.size) await orbitAction('/api/essentials', {apps: [...this.picked]}); }},
  {title: 'Your space, your colors.', text: ['Personalize your browser by giving each space its own unique color identity.'],
   picked: -1, render() {
     const row = el('div', {className: 'colors'});
     state.colors.forEach((c, i) => {
       const b = el('button', {style: 'background:#' + c, ariaLabel: 'Color #' + c, onclick: async () => { this.picked = i; await orbitAction('/api/space-color', {index: i}); show(); }});
       b.setAttribute('aria-pressed', String(this.picked === i));
       row.append(b);
     });
     content.append(row);
   }},
  {title: 'All set? Let’s get rolling!', text: ['You’re all set up and ready to go. Click the button below to start browsing with Orbit.'],
   next: 'Dive in!', render() {}},
];
function next() { go(1); }
async function go(delta) {
  const page = pages[step];
  if (delta > 0 && page.leave) await page.leave();
  if (delta > 0 && step === pages.length - 1) { await orbitAction('/api/done'); return; }
  do { step += delta; } while (pages[step] && pages[step].skip && pages[step].skip());
  show();
}
function show() {
  const page = pages[step];
  text.replaceChildren(el('h1', {innerHTML: page.title}), ...page.text.map(t => el('p', {textContent: t})));
  const nav = el('div', {id: 'nav'});
  if (step > 0) nav.append(el('button', {textContent: 'Back', onclick: () => go(-1)}));
  nav.append(el('button', {className: 'primary', textContent: page.next || 'Next', onclick: next}));
  if (step > 0 && step < pages.length - 1) nav.append(el('button', {textContent: 'Skip for now', onclick: next}));
  text.append(nav);
  content.replaceChildren();
  page.render();
}
(async () => { state = await (await fetch('/api/state')).json(); document.title = 'Welcome to Orbit'; show(); })();
)js";
  return {200, "text/html", PageShell("Welcome", body, script)};
}

}  // namespace orbit
