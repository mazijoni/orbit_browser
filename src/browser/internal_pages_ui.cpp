// Markup and scripts for orbit://history, bookmarks, downloads and settings.
// Pages render from JSON fetched from their own /api/ endpoints and build the
// DOM with textContent (never innerHTML with data), so page data cannot
// inject markup.

#include "browser/internal_page_parts.h"

namespace orbit {
namespace {

// Small DOM helper shared by all pages.
constexpr const char* kDomHelpers = R"js(
function el(tag, attrs, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return e;
}
function hostOf(url) { try { return new URL(url).host || url; } catch { return url; } }
function fmtBytes(n) {
  if (n < 0) return '';
  const u = ['B', 'KB', 'MB', 'GB', 'TB']; let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return (i ? n.toFixed(1) : n) + ' ' + u[i];
}
function safeHref(url) { return /^(https?|file|ftp):/i.test(url) ? url : '#'; }
)js";

}  // namespace

InternalResponse HistoryPage(const InternalRequest& req) {
  const std::string body = R"html(
<main>
  <h1>History</h1>
  <div class="toolbar">
    <input id="q" type="search" placeholder="Search history" aria-label="Search history" style="flex:1;min-width:220px">
    <select id="range" aria-label="Time range to clear">
      <option value="hour">Last hour</option><option value="day">Last 24 hours</option>
      <option value="week">Last 7 days</option><option value="month">Last 4 weeks</option>
      <option value="all">All time</option>
    </select>
    <button id="clear" class="danger">Clear history</button>
  </div>
  <div id="list" role="list"></div>
  <div style="text-align:center;margin-top:12px"><button id="more" hidden>Load more</button></div>
</main>)html";
  std::string script = kDomHelpers;
  script += R"js(
const list = document.getElementById('list');
const more = document.getElementById('more');
let last = 0, query = '', lastDay = '';
function dayLabel(t) {
  const d = new Date(t), today = new Date();
  const y = new Date(); y.setDate(today.getDate() - 1);
  if (d.toDateString() === today.toDateString()) return 'Today';
  if (d.toDateString() === y.toDateString()) return 'Yesterday';
  return d.toLocaleDateString(undefined, {weekday: 'long', year: 'numeric', month: 'long', day: 'numeric'});
}
async function load(reset) {
  if (reset) { list.replaceChildren(); last = 0; lastDay = ''; }
  const params = new URLSearchParams({q: query});
  if (last) params.set('before', last);
  const r = await fetch('/api/list?' + params);
  if (!r.ok) { list.replaceChildren(el('p', {class: 'empty'}, (await r.json()).error)); return; }
  const {items} = await r.json();
  document.title = 'History';  // set only once data has loaded
  if (reset && !items.length) list.append(el('p', {class: 'empty'}, query ? 'No matching history.' : 'Your history is empty.'));
  for (const it of items) {
    const day = dayLabel(it.time);
    if (day !== lastDay) { list.append(el('h2', {}, day)); lastDay = day; }
    const row = el('div', {class: 'row', role: 'listitem'},
      el('span', {class: 'muted', style: 'width:56px;font-size:12px'},
         new Date(it.time).toLocaleTimeString(undefined, {hour: '2-digit', minute: '2-digit'})),
      el('div', {class: 'grow'},
        el('div', {class: 'title'}, el('a', {href: safeHref(it.url)}, it.title || it.url)),
        el('div', {class: 'sub'}, hostOf(it.url))),
      el('button', {title: 'Remove from history', 'aria-label': 'Remove from history', onclick: async () => {
        await orbitAction('/api/delete', {ids: [it.id]}); row.remove(); }}, '✕'));
    list.append(row);
    last = it.time;
  }
  more.hidden = items.length < 150;
}
let timer;
document.getElementById('q').addEventListener('input', (e) => {
  clearTimeout(timer); timer = setTimeout(() => { query = e.target.value.trim(); load(true); }, 200);
});
more.addEventListener('click', () => load(false));
document.getElementById('clear').addEventListener('click', async () => {
  const sel = document.getElementById('range');
  if (!confirm('Clear browsing history (' + sel.options[sel.selectedIndex].text + ')?')) return;
  await orbitAction('/api/clear', {range: sel.value});
  load(true);
});
load(true);
)js";
  return {200, "text/html", PageShell("Loading", body, script)};
}

InternalResponse BookmarksPage(const InternalRequest& req) {
  const std::string body = R"html(
<main>
  <h1>Bookmarks</h1>
  <div class="toolbar">
    <input id="q" type="search" placeholder="Search bookmarks" aria-label="Search bookmarks" style="flex:1;min-width:220px">
    <button id="newFolder">New folder</button>
    <label class="button" style="display:inline-block"><input id="importFile" type="file" accept=".html,.htm" hidden><span role="button" tabindex="0" id="importBtn" style="background:var(--surface2);border:1px solid var(--border);border-radius:8px;padding:7px 12px;cursor:pointer;display:inline-block">Import…</span></label>
    <button id="export">Export</button>
  </div>
  <div id="tree"></div>
</main>)html";
  std::string script = kDomHelpers;
  script += R"js(
const tree = document.getElementById('tree');
let items = [], query = '', collapsed = new Set();
async function load() {
  const r = await fetch('/api/list');
  if (!r.ok) { tree.replaceChildren(el('p', {class: 'empty'}, (await r.json()).error)); return; }
  items = (await r.json()).items;
  document.title = 'Bookmarks';
  render();
}
function folders() { return items.filter(i => i.folder); }
function moveSelect(item) {
  const sel = el('select', {'aria-label': 'Move to folder', onchange: async (e) => {
    await orbitAction('/api/move', {id: item.id, parent: Number(e.target.value)}); load(); }},
    el('option', {value: '', disabled: true, selected: true}, 'Move to…'),
    el('option', {value: '0'}, 'Top level'),
    folders().filter(f => f.id !== item.id).map(f => el('option', {value: f.id}, f.title)));
  return sel;
}
function rowFor(item, depth) {
  const actions = el('span', {style: 'display:flex;gap:6px'},
    el('button', {onclick: async () => {
      const title = prompt('Name', item.title); if (title === null) return;
      let url = item.url;
      if (!item.folder) { url = prompt('URL', item.url); if (url === null) return; }
      await orbitAction('/api/update', {id: item.id, title, url}); load(); }}, 'Edit'),
    moveSelect(item),
    el('button', {class: 'danger', onclick: async () => {
      if (item.folder && !confirm('Delete folder "' + item.title + '" and everything in it?')) return;
      await orbitAction('/api/delete', {id: item.id}); load(); }}, 'Delete'));
  if (item.folder) {
    const open = !collapsed.has(item.id);
    return el('div', {class: 'row', style: 'padding-left:' + (10 + depth * 20) + 'px'},
      el('button', {'aria-expanded': open, 'aria-label': open ? 'Collapse' : 'Expand',
                    style: 'padding:2px 8px', onclick: () => { open ? collapsed.add(item.id) : collapsed.delete(item.id); render(); }},
         open ? '▾' : '▸'),
      el('div', {class: 'grow'}, el('div', {class: 'title', style: 'font-weight:600'}, item.title)),
      actions);
  }
  return el('div', {class: 'row', style: 'padding-left:' + (10 + depth * 20) + 'px'},
    el('div', {class: 'grow'},
      el('div', {class: 'title'}, el('a', {href: safeHref(item.url)}, item.title || item.url)),
      el('div', {class: 'sub'}, item.url)),
    actions);
}
function renderLevel(parent, depth, out) {
  for (const item of items.filter(i => i.parent === parent)) {
    out.push(rowFor(item, depth));
    if (item.folder && !collapsed.has(item.id)) renderLevel(item.id, depth + 1, out);
  }
}
function render() {
  const out = [];
  if (query) {
    const q = query.toLowerCase();
    for (const i of items.filter(i => !i.folder && (i.title.toLowerCase().includes(q) || i.url.toLowerCase().includes(q))))
      out.push(rowFor(i, 0));
  } else {
    renderLevel(0, 0, out);
  }
  if (!out.length) out.push(el('p', {class: 'empty'}, query ? 'No matching bookmarks.' :
      'No bookmarks yet. Press Ctrl+D or the star in the address bar to bookmark a page.'));
  tree.replaceChildren(...out);
}
document.getElementById('q').addEventListener('input', (e) => { query = e.target.value.trim(); render(); });
document.getElementById('newFolder').addEventListener('click', async () => {
  const title = prompt('Folder name', 'New folder'); if (!title) return;
  await orbitAction('/api/add', {folder: true, title, parent: 0}); load();
});
const fileInput = document.getElementById('importFile');
document.getElementById('importBtn').addEventListener('click', () => fileInput.click());
document.getElementById('importBtn').addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') fileInput.click(); });
fileInput.addEventListener('change', async () => {
  const f = fileInput.files[0]; if (!f) return;
  const html = await f.text();
  try {
    const r = await orbitAction('/api/import', {html});
    alert('Imported ' + r.imported + ' bookmarks into the "Imported" folder.');
  } catch (e) { alert('Import failed.'); }
  fileInput.value = ''; load();
});
document.getElementById('export').addEventListener('click', async () => {
  const r = await fetch('/api/export');
  const blob = new Blob([await r.text()], {type: 'text/html'});
  const a = el('a', {href: URL.createObjectURL(blob), download: 'orbit-bookmarks.html'});
  document.body.append(a); a.click(); a.remove();
});
load();
)js";
  return {200, "text/html", PageShell("Loading", body, script)};
}

InternalResponse DownloadsPage(const InternalRequest& req) {
  const std::string body = R"html(
<main>
  <h1>Downloads</h1>
  <div class="card" id="loc" style="display:flex;gap:12px;align-items:center;flex-wrap:wrap">
    <div class="grow" style="flex:1;min-width:200px"><div class="muted" style="font-size:12px">Save files to</div><div id="dir"></div></div>
    <label><input type="checkbox" id="ask"> Ask where to save each file</label>
    <button id="change">Change…</button>
  </div>
  <div id="list"></div>
</main>)html";
  std::string script = kDomHelpers;
  script += R"js(
const STATES = ['In progress', 'Completed', 'Cancelled', 'Interrupted', 'Paused'];
const list = document.getElementById('list');
let timer = null;
function act(name, id) { return orbitAction('/api/' + name, {id}).then(load, load); }
function row(d) {
  const active = d.state === 0 || d.state === 4;
  const pct = d.total > 0 ? Math.round(d.received * 100 / d.total) : -1;
  let status = STATES[d.state];
  if (active) status = (d.total > 0 ? fmtBytes(d.received) + ' of ' + fmtBytes(d.total) : fmtBytes(d.received)) +
                       (d.state === 0 && d.speed > 0 ? ' · ' + fmtBytes(d.speed) + '/s' : (d.state === 4 ? ' · Paused' : ''));
  else if (d.state === 1) status = fmtBytes(d.total > 0 ? d.total : d.received) + (d.exists ? '' : ' · File removed');
  const buttons = [];
  if (d.state === 0) buttons.push(el('button', {onclick: () => act('pause', d.id)}, 'Pause'));
  if (d.state === 4) buttons.push(el('button', {onclick: () => act('resume', d.id)}, 'Resume'));
  if (active) buttons.push(el('button', {class: 'danger', onclick: () => act('cancel', d.id)}, 'Cancel'));
  if (d.state === 1 && d.exists) {
    buttons.push(el('button', {class: 'primary', onclick: () => act('open', d.id)}, 'Open'));
    buttons.push(el('button', {onclick: () => act('show', d.id)}, 'Show in folder'));
  }
  if (!active) buttons.push(el('button', {title: 'Remove from list', 'aria-label': 'Remove from list', onclick: () => act('remove', d.id)}, '✕'));
  const bar = active && pct >= 0
    ? el('div', {style: 'height:4px;background:var(--surface2);border-radius:2px;margin-top:6px'},
         el('div', {style: 'height:4px;border-radius:2px;background:var(--accent);width:' + pct + '%'}))
    : null;
  return el('div', {class: 'card', style: 'display:flex;gap:12px;align-items:center'},
    el('div', {class: 'grow', style: 'flex:1;min-width:0'},
      el('div', {class: 'title', style: 'font-weight:600'}, d.name || hostOf(d.url)),
      el('div', {class: 'sub'}, hostOf(d.url) + ' · ' + status), bar),
    el('div', {style: 'display:flex;gap:6px'}, buttons));
}
async function load() {
  const r = await fetch('/api/list');
  const data = await r.json();
  document.title = 'Downloads';
  document.getElementById('dir').textContent = data.dir;
  document.getElementById('ask').checked = data.ask;
  list.replaceChildren(...(data.items.length ? data.items.map(row) :
      [el('p', {class: 'empty'}, data.private ? 'Downloads in this private window are listed here until it closes.' : 'No downloads yet.')]));
  const active = data.items.some(d => d.state === 0 || d.state === 4);
  clearTimeout(timer);
  if (active) timer = setTimeout(load, 1000);  // poll only while something is downloading
}
document.getElementById('change').addEventListener('click', async () => { await orbitAction('/api/choose-dir', {}); load(); });
document.getElementById('ask').addEventListener('change', (e) =>
  orbitAction('/api/ask', {value: e.target.checked ? '1' : '0'}));
load();
)js";
  return {200, "text/html", PageShell("Loading", body, script)};
}

InternalResponse SettingsPage(const InternalRequest& req) {
  const std::string body = R"html(
<main>
  <h1>Settings</h1>
  <h2>Appearance</h2>
  <div class="card">
    <div class="row" style="padding-left:0"><div class="grow">Theme</div>
      <div role="radiogroup" aria-label="Theme" id="theme" style="display:flex;gap:6px"></div></div>
    <div class="row" style="padding-left:0"><div class="grow">Accent color</div><div id="accents" style="display:flex;gap:8px"></div></div>
    <div class="row" style="padding-left:0"><label class="grow" for="themeStrength">Space theme color</label>
      <select id="themeStrength"><option value="0">Off</option><option value="0.25">Subtle</option><option value="0.45">Normal</option><option value="0.7">Vivid</option></select></div>
    <div class="row" style="padding-left:0"><label class="grow" for="hidden">Compact mode (hide the sidebar until the pointer reaches the left edge)</label><input type="checkbox" id="hidden"></div>
    <div class="row" style="padding-left:0"><label class="grow" for="compact">Collapse the sidebar to icons</label><input type="checkbox" id="compact"></div>
  </div>
  <h2>Search</h2>
  <div class="card">
    <div class="row" style="padding-left:0"><label class="grow" for="engine">Search engine used in the address bar</label><select id="engine"></select></div>
    <div class="row" style="padding-left:0" id="customRow" hidden>
      <input id="custom" type="text" style="flex:1" placeholder="https://example.com/search?q={query}" aria-label="Custom search URL">
      <button id="saveCustom">Save</button></div>
    <div id="searchError" class="muted" style="color:var(--danger)"></div>
  </div>
  <h2>Tabs and memory</h2>
  <div class="card">
    <div class="row" style="padding-left:0"><label class="grow" for="sleep">Put inactive tabs to sleep after</label>
      <select id="sleep"><option value="0">Never</option><option value="15">15 minutes</option><option value="30">30 minutes</option>
        <option value="60">1 hour</option><option value="120">2 hours</option><option value="240">4 hours</option></select></div>
    <p class="muted" style="margin:4px 0 0">Sleeping tabs release their memory and reload when selected. Pinned tabs, tabs marked
      "Never put to sleep", tabs using the camera or microphone and tabs playing audio are never put to sleep.
      Reloading can lose unsaved form input.</p>
  </div>
  <h2>Site permissions</h2>
  <div class="card" id="perms"></div>
  <h2>Privacy</h2>
  <div class="card">
    <div style="display:flex;gap:16px;flex-wrap:wrap;margin-bottom:10px">
      <label><input type="checkbox" id="cHistory" checked> Browsing history</label>
      <label><input type="checkbox" id="cCookies"> Cookies</label>
      <label><input type="checkbox" id="cClosed" checked> Recently closed tabs</label>
    </div>
    <button id="clear" class="danger">Clear data for this profile</button> <span id="cleared" class="muted"></span>
  </div>
  <h2>About</h2>
  <div class="card" id="about"></div>
</main>)html";
  std::string script = kDomHelpers;
  script += R"js(
async function set(key, value) {
  try { await orbitAction('/api/set', {key, value}); return true; }
  catch (e) { return false; }
}
(async () => {
  const s = await (await fetch('/api/get')).json();
  if (s.error) return;
  const themes = [['system', 'System'], ['dark', 'Dark'], ['light', 'Light']];
  const themeBox = document.getElementById('theme');
  for (const [v, label] of themes) {
    themeBox.append(el('button', {role: 'radio', 'aria-checked': s.theme === v, class: s.theme === v ? 'primary' : '',
      onclick: async () => { await set('theme', v); location.reload(); }}, label));
  }
  const accents = document.getElementById('accents');
  for (const a of s.accents) {
    accents.append(el('button', {'aria-label': 'Accent #' + a, title: '#' + a,
      style: 'width:24px;height:24px;border-radius:50%;padding:0;background:#' + a + ';' +
             (a === s.accent ? 'outline:2px solid var(--text);outline-offset:2px' : ''),
      onclick: async () => { await set('accent', a); location.reload(); }}));
  }
  const compact = document.getElementById('compact');
  compact.checked = s.compact;
  compact.addEventListener('change', () => set('compact', compact.checked ? '1' : '0'));
  const hidden = document.getElementById('hidden');
  hidden.checked = s.hidden;
  hidden.addEventListener('change', () => set('hidden', hidden.checked ? '1' : '0'));
  const strength = document.getElementById('themeStrength');
  strength.value = ['0', '0.25', '0.45', '0.7'].includes(s.themeStrength) ? s.themeStrength : '0.45';
  strength.addEventListener('change', () => set('theme_strength', strength.value));

  const engine = document.getElementById('engine'), customRow = document.getElementById('customRow');
  const custom = document.getElementById('custom'), err = document.getElementById('searchError');
  let matched = false;
  for (const p of s.presets) {
    const o = el('option', {value: p.url}, p.name);
    if (p.url === s.search) { o.selected = true; matched = true; }
    engine.append(o);
  }
  engine.append(el('option', {value: 'custom', selected: !matched}, 'Custom…'));
  customRow.hidden = matched; custom.value = matched ? '' : s.search;
  engine.addEventListener('change', async () => {
    customRow.hidden = engine.value !== 'custom'; err.textContent = '';
    if (engine.value !== 'custom') await set('search', engine.value);
  });
  document.getElementById('saveCustom').addEventListener('click', async () => {
    err.textContent = (await set('search', custom.value.trim())) ? 'Saved.' :
        'The URL must start with https:// and contain {query}.';
  });

  const sleep = document.getElementById('sleep');
  sleep.value = s.sleep;
  sleep.addEventListener('change', () => set('sleep', sleep.value));

  document.getElementById('clear').addEventListener('click', async () => {
    const body = {history: document.getElementById('cHistory').checked,
                  cookies: document.getElementById('cCookies').checked,
                  closed: document.getElementById('cClosed').checked};
    if (!confirm('Clear the selected data for profile "' + s.profile + '"? This cannot be undone.')) return;
    await orbitAction('/api/clear-data', body);
    document.getElementById('cleared').textContent = 'Cleared.';
  });
  async function loadPerms() {
    const box = document.getElementById('perms');
    if (s.private) { box.replaceChildren(el('p', {class: 'muted', style: 'margin:0'},
        'Choices made in a private window are forgotten when it closes.')); return; }
    const {items} = await (await fetch('/api/permissions')).json();
    box.replaceChildren(...(items.length ? items.map(p => el('div', {class: 'row', style: 'padding-left:0'},
      el('div', {class: 'grow'}, el('div', {class: 'title'}, p.origin),
         el('div', {class: 'sub'}, p.label.replace(/\u2022 /g, '').trim() + ' \u00B7 ' + (p.allowed ? 'Allowed' : 'Blocked'))),
      el('button', {onclick: async () => { await orbitAction('/api/permission-forget', {origin: p.origin, bit: p.bit}); loadPerms(); }}, 'Reset')))
      : [el('p', {class: 'muted', style: 'margin:0'}, 'No site has asked for permissions yet. Sites must ask before using your camera, microphone, location or similar features.')]));
  }
  loadPerms();
  document.getElementById('about').append(
    el('div', {}, 'Orbit ' + s.version),
    el('div', {class: 'muted'}, 'Engine: Chromium Embedded Framework ' + s.engine),
    el('div', {class: 'muted'}, 'Profile: ' + s.profile + (s.private ? ' (private)' : '')),
    el('div', {class: 'muted', style: 'word-break:break-all'}, 'Profile data: ' + s.dataDir));
  document.title = 'Settings';
})();
)js";
  return {200, "text/html", PageShell("Loading", body, script)};
}

InternalResponse DiagnosticsPage(const InternalRequest& req) {
  const std::string body = R"html(
<main>
  <h1>Diagnostics</h1>
  <p class="muted">Live measurements of Orbit and its Chromium processes. <b>Private memory</b> is the
    process's private working set (resident memory not shared with other processes; Task Manager's
    "Memory" column). <b>Commit</b> is private committed memory, including pages not currently resident.</p>
  <div class="toolbar">
    <div class="card" style="margin:0;flex:1"><div class="muted" style="font-size:12px">Private memory</div><div id="tPws" style="font-size:20px;font-weight:600"></div></div>
    <div class="card" style="margin:0;flex:1"><div class="muted" style="font-size:12px">Commit</div><div id="tCommit" style="font-size:20px;font-weight:600"></div></div>
    <div class="card" style="margin:0;flex:1"><div class="muted" style="font-size:12px">Processes</div><div id="tProc" style="font-size:20px;font-weight:600"></div></div>
    <div class="card" style="margin:0;flex:1"><div class="muted" style="font-size:12px">Tabs (live / sleeping)</div><div id="tTabs" style="font-size:20px;font-weight:600"></div></div>
  </div>
  <div class="toolbar"><button id="sleep">Put background tabs to sleep now</button><span class="muted" id="note"></span></div>
  <table style="width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums">
    <thead><tr style="text-align:left;color:var(--muted);font-size:12px">
      <th style="padding:6px">Process</th><th>PID</th><th style="text-align:right">Private memory</th>
      <th style="text-align:right">Working set</th><th style="text-align:right">Commit</th><th style="text-align:right;padding-right:6px">CPU time</th></tr></thead>
    <tbody id="rows"></tbody>
  </table>
</main>)html";
  std::string script = kDomHelpers;
  script += R"js(
const MB = (n) => (n / 1048576).toFixed(1) + ' MB';
async function load() {
  const r = await fetch('/api/metrics');
  const d = await r.json();
  document.title = 'Diagnostics';
  let pws = 0, commit = 0;
  const rows = d.processes.map(p => {
    pws += p.privateWs; commit += p.commit;
    return el('tr', {style: 'border-top:1px solid var(--border)'},
      el('td', {style: 'padding:6px'}, p.type), el('td', {}, p.pid),
      el('td', {style: 'text-align:right'}, MB(p.privateWs)), el('td', {style: 'text-align:right'}, MB(p.ws)),
      el('td', {style: 'text-align:right'}, MB(p.commit)),
      el('td', {style: 'text-align:right;padding-right:6px'}, (p.cpuMs / 1000).toFixed(1) + ' s'));
  });
  document.getElementById('rows').replaceChildren(...rows);
  document.getElementById('tPws').textContent = MB(pws);
  document.getElementById('tCommit').textContent = MB(commit);
  document.getElementById('tProc').textContent = d.processes.length;
  document.getElementById('tTabs').textContent = d.liveTabs + ' / ' + d.sleepingTabs;
}
document.getElementById('sleep').addEventListener('click', async () => {
  await orbitAction('/api/sleep-all', {});
  document.getElementById('note').textContent = 'Background tabs that are not pinned, protected or playing media will sleep.';
  setTimeout(load, 2500);
});
load();
setInterval(() => { if (!document.hidden) load(); }, 2000);  // only while visible
)js";
  return {200, "text/html", PageShell("Loading", body, script)};
}

}  // namespace orbit
