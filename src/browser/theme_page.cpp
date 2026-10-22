// orbit://theme/: the space theme picker (Zen's "Edit Theme…" panel,
// #PanelUI-zen-gradient-generator). The picking logic (positions on the
// disc, colour harmonies, preset pages, the opacity wave and the grain
// dial) is ported from zen-browser/desktop src/zen/spaces/
// ZenGradientGenerator.mjs and zen-gradient-generator.css, which are
// subject to the Mozilla Public License 2.0 (http://mozilla.org/MPL/2.0/).
// The browser side (storage, painting the window) is orbit's own.
#include "browser/internal_page_parts.h"

namespace orbit {

InternalResponse ThemePage(const InternalRequest&) {
  std::string body = R"html(
<style>
html,body{height:100%;overflow:hidden}
body{-webkit-user-select:none;user-select:none;font-size:13px}
.ico{font-family:"Segoe Fluent Icons","Segoe MDL2 Assets";font-style:normal;font-size:15px;line-height:1}
#panel{padding:10px;display:flex;flex-direction:column;gap:10px}
#pad{position:relative;width:358px;height:358px;border-radius:12px;overflow:hidden;
  background:color-mix(in srgb,var(--text) 4%,transparent);
  background-image:radial-gradient(color-mix(in srgb,var(--text) 8%,transparent) 1px,transparent 0);
  background-position:-23px -23px;background-size:6px 6px;cursor:crosshair}
.dot{position:absolute;width:20px;height:20px;border-radius:50%;background:var(--c);border:3px solid #fff;
  transform:translate(-50%,-50%);pointer-events:none;box-shadow:0 0 0 2px rgba(0,0,0,.1);z-index:2;
  transition:left .3s cubic-bezier(.3,1.4,.6,1),top .3s cubic-bezier(.3,1.4,.6,1)}
.dot.primary{width:38px;height:38px;border-width:6px;pointer-events:all;cursor:grab;z-index:3}
.dot.primary:hover{transform:translate(-50%,-50%) scale(1.05)}
.dot.dragging{transform:translate(-50%,-50%) scale(1.2);transition:none;cursor:grabbing}
.bar{position:absolute;left:50%;transform:translateX(-50%);display:flex;gap:5px;z-index:4}
#scheme{top:15px}#actions{bottom:12px}
.bar button{width:30px;height:30px;min-width:30px;padding:0;border-radius:8px;background:transparent;
  display:flex;align-items:center;justify-content:center;color:color-mix(in srgb,var(--text) 85%,transparent)}
.bar button:not([disabled]):hover{background:color-mix(in srgb,var(--text) 10%,transparent)}
.bar button[disabled]{opacity:.5;cursor:not-allowed}
.bar button[aria-pressed=true]{background:color-mix(in srgb,var(--text) 14%,transparent)}
#clickToAdd{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-weight:600;font-size:small;pointer-events:none;white-space:nowrap}
#pagesRow{display:flex;align-items:center;gap:4px}
#pagesRow>button{width:28px;height:28px;padding:0;background:transparent}
#pages{display:flex;overflow:hidden;flex:1;scroll-behavior:smooth;margin:0 6px}
#pages>div{display:flex;justify-content:space-between;min-width:100%;padding:0 1px}
.swatch{width:26px;height:26px;border-radius:50%;box-shadow:0 0 1px 1px rgba(0,0,0,.1);cursor:pointer;transition:transform .1s;padding:0;border:0}
.swatch:hover{transform:scale(1.05)}.swatch:active{transform:scale(.95)}
.swatch[data-num-dots="3"]{background:radial-gradient(circle at 0% 0%,var(--c1),transparent 100%),radial-gradient(circle at 100% 0%,var(--c2),transparent 100%),linear-gradient(to top,var(--c3) 0%,transparent 60%)}
#controls{display:flex;align-items:center;justify-content:space-between;gap:1.5rem;padding:0 10px 4px}
#opacityWrap{position:relative;flex:1;height:60px;display:flex;align-items:center}
#wave{position:absolute;left:-5px;width:100%;height:100%;pointer-events:none;display:flex;align-items:center}
#wave::before{content:"";position:absolute;width:calc(100% - 8px);height:24px;left:8px;top:50%;transform:translateY(-50%);border-radius:999px;background:color-mix(in srgb,var(--text) 10%,transparent)}
#wave svg{overflow:visible;min-width:110%;scale:1.2;margin-left:4px}
#opacity{-webkit-appearance:none;appearance:none;width:100%;margin:0;background:transparent;z-index:2;padding:0 5px}
#opacity::-webkit-slider-runnable-track{height:18px;border-radius:999px;background:transparent}
#opacity::-webkit-slider-thumb{-webkit-appearance:none;background:var(--text);border-radius:999px;height:var(--th,40px);width:var(--tw,10px);margin-top:calc((18px - var(--th,40px)) / 2);cursor:pointer;transition:transform .5s}
#opacity:hover::-webkit-slider-thumb{transform:scale(1.1)}
#texture{width:5rem;height:5rem;position:relative;flex:none}
#texture::after{content:"";position:absolute;width:60%;height:60%;border:1px solid color-mix(in srgb,var(--border) 50%,transparent);border-radius:50%;
  background:linear-gradient(-45deg,transparent -10%,color-mix(in srgb,var(--text) 10%,transparent) 110%);top:50%;left:50%;transform:translate(-50%,-50%);pointer-events:none}
.tdot{position:absolute;width:4px;height:4px;border-radius:50%;background:color-mix(in srgb,var(--text) 30%,transparent);transform:translate(-50%,-50%);transition:opacity .2s;pointer-events:none}
.tdot:not(.active){opacity:.4}
#handle{position:absolute;width:6px;height:10px;border-radius:2px;background:color-mix(in srgb,var(--text) 70%,transparent);cursor:pointer;z-index:2;transition:height .1s}
#handle:hover{height:14px}
</style>
<div id="panel">
  <div id="pad" role="application" aria-label="Theme colors">
    <div class="bar" id="scheme" role="radiogroup" aria-label="Color scheme">
      <button id="scheme-auto" data-scheme="system" title="Automatic" aria-label="Automatic"><i class="ico">&#xE7F4;</i></button>
      <button id="scheme-light" data-scheme="light" title="Light" aria-label="Light"><i class="ico">&#xE706;</i></button>
      <button id="scheme-dark" data-scheme="dark" title="Dark" aria-label="Dark"><i class="ico">&#xE708;</i></button>
    </div>
    <div class="bar" id="actions">
      <button id="add" title="Add color" aria-label="Add color"><i class="ico">&#xE710;</i></button>
      <button id="remove" title="Remove color" aria-label="Remove color"><i class="ico">&#xE738;</i></button>
      <button id="algo" title="Color harmony" aria-label="Color harmony"><i class="ico">&#xE8B1;</i></button>
    </div>
    <div id="clickToAdd">Click to add a color</div>
  </div>
  <div id="pagesRow">
    <button id="left" aria-label="Previous colors" disabled><i class="ico">&#xE76B;</i></button>
    <div id="pages"></div>
    <button id="right" aria-label="Next colors"><i class="ico">&#xE76C;</i></button>
  </div>
  <div id="controls">
    <div id="opacityWrap">
      <div id="wave"><svg viewBox="0 -7.605 455 70" preserveAspectRatio="xMinYMid meet"><path fill="none" stroke-linecap="round" stroke-linejoin="round" style="stroke-width:8px"/>
        <defs><linearGradient id="waveGrad" x1="0%" y1="0%" x2="100%" y2="0%"><stop offset="0%"/><stop offset="0%"/><stop offset="100%"/></linearGradient></defs></svg></div>
      <input type="range" id="opacity" min="0.25" max="0.8" step="0.001" value="0.5" aria-label="Opacity">
    </div>
    <div id="texture" role="slider" aria-label="Grain" aria-valuemin="0" aria-valuemax="1" tabindex="0"></div>
  </div>
</div>)html";
  std::string script = R"js(
// --- ported from ZenGradientGenerator.mjs (MPL 2.0) ----------------------
const PAD = 358, MAX_DOTS = 3;
const EXPLICIT_LIGHTNESS = 'explicit-lightness', EXPLICIT_BW = 'explicit-black-white';
const HARMONIES = [
  {type: 'complementary', angles: [180]}, {type: 'singleAnalogous', angles: [310]},
  {type: 'splitComplementary', angles: [150, 210]}, {type: 'analogous', angles: [50, 310]},
  {type: 'triadic', angles: [120, 240]}, {type: 'floating', angles: []}];
const ALGO_LABELS = {complementary: 'Complementary', splitComplementary: 'Split', analogous: 'Analogous',
  triadic: 'Triadic', floating: 'Floating', singleAnalogous: 'Analogous', float: 'Floating'};
const PAGES = [
  [[90,'float',1,'240,240','#f4efdf'],[80,'float',1,'233,157','#f0b8cd'],[80,'float',1,'236,111','#e9c3e3'],[70,'float',1,'234,173','#da7682'],
   [70,'float',1,'220,187','#eb8570'],[60,'float',1,'225,237','#dcce7f'],[60,'float',1,'147,195','#5becad'],[50,'float',1,'81,84','#919bb5']],
  [[90,'analogous',3,'240,240',['rgb(245,237,214)','rgb(221,243,216)','rgb(243,216,225)']],[85,'analogous',3,'233,157',['rgb(243,190,222)','rgb(247,222,186)','rgb(223,195,238)']],
   [80,'analogous',3,'236,111',['rgb(229,179,228)','rgb(236,172,178)','rgb(197,185,223)']],[70,'analogous',3,'234,173',['rgb(235,122,159)','rgb(239,239,118)','rgb(210,133,224)']],
   [70,'analogous',3,'220,187',['rgb(242,115,123)','rgb(175,242,115)','rgb(230,125,232)']],[60,'analogous',3,'225,237',['rgb(221,205,85)','rgb(97,212,94)','rgb(215,91,124)']],
   [60,'analogous',3,'147,195',['rgb(75,231,210)','rgb(84,175,222)','rgb(62,244,112)']],[55,'analogous',3,'81,84',['rgb(122,132,158)','rgb(137,117,164)','rgb(116,162,164)']]],
  [[10,'float',1,'171,72','rgb(93,86,106)'],[40,'float',1,'265,79','#997096'],[35,'float',1,'301,176','#956066'],[30,'float',1,'237,210','#9c6645'],
   [30,'float',1,'91,228','#517b6c'],[25,'float',1,'67,159','#576e75'],[20,'float',1,'314,235','rgb(131,109,95)'],[20,'float',1,'118,215','#447464']],
  [[10,'analogous',3,'171,72',['rgb(23,17,34)','rgb(37,14,35)','rgb(18,22,33)']],[40,'analogous',3,'265,79',['rgb(128,76,124)','rgb(141,63,66)','rgb(97,88,116)']],
   [35,'analogous',3,'301,176',['rgb(122,56,64)','rgb(126,121,52)','rgb(111,68,110)']],[30,'analogous',3,'237,210',['rgb(131,65,22)','rgb(64,128,25)','rgb(122,31,91)']],
   [30,'analogous',3,'91,228',['rgb(45,108,85)','rgb(52,85,101)','rgb(52,118,35)']],[25,'analogous',3,'67,159',['rgb(45,74,83)','rgb(46,50,81)','rgb(38,90,65)']],
   [20,'analogous',3,'314,235',['rgb(64,47,38)','rgb(55,64,38)','rgb(59,43,52)']],[20,'analogous',3,'118,215',['rgb(22,80,61)','rgb(26,60,76)','rgb(27,87,15)']]],
  [[null,'float',1,'340,180','rgb(224,224,224)',EXPLICIT_BW],[null,'float',1,'337.5,180','rgb(224,224,224)',EXPLICIT_BW],[null,'float',1,'315,180','rgb(192,192,192)',EXPLICIT_BW],
   [null,'float',1,'292.5,180','rgb(160,160,160)',EXPLICIT_BW],[null,'float',1,'270,180','rgb(128,128,128)',EXPLICIT_BW],[null,'float',1,'247.5,180','rgb(96,96,96)',EXPLICIT_BW],
   [null,'float',1,'225,180','rgb(64,64,64)',EXPLICIT_BW],[null,'float',1,'202.5,180','rgb(32,32,32)',EXPLICIT_BW],[null,'float',1,'180,180','rgb(0,0,0)',EXPLICIT_BW]]];

let dots = [];       // {ID, x, y, type, c: [r,g,b], el}
let useAlgo = '', currentLightness = 50, opacity = 0.5, texture = 0, space = 0;
let dragging = null, recentlyDragged = false, colorPage = 0;
const pad = document.getElementById('pad');

function hueToRgb(p, q, t) {
  if (t < 0) t += 1; if (t > 1) t -= 1;
  if (t < 1 / 6) return p + (q - p) * 6 * t;
  if (t < 1 / 2) return q;
  if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
  return p;
}
function hslToRgb(h, s, l) {
  let r, g, b;
  if (s === 0) { r = g = b = l; } else {
    const q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hueToRgb(p, q, h + 1 / 3); g = hueToRgb(p, q, h); b = hueToRgb(p, q, h - 1 / 3);
  }
  return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
}
// getColorFromPosition: hue from the angle, saturation (or lightness) from
// the distance to the centre.
function colorFromPosition(x, y, type) {
  const padding = 30, half = 29;
  x += half; y += half;
  const w = PAD + padding * 2, cx = w / 2, cy = w / 2, radius = (w - padding) / 2;
  const distance = Math.sqrt((x - cx) ** 2 + (y - cy) ** 2) - 0.2;
  let angle = Math.atan2(y - cy, x - cx) * 180 / Math.PI;
  if (angle < 0) angle += 360;
  const nd = 1 - Math.min(distance / radius, 1);
  let saturation = nd * 100;
  if (type !== EXPLICIT_LIGHTNESS) {
    saturation = 90 + (1 - nd) * 10;
    currentLightness = Math.round((1 - nd) * 100);
  }
  let lightness = currentLightness;
  if (type === EXPLICIT_BW) { saturation = 0; lightness = Math.round((1 - nd) * 100); }
  return hslToRgb(angle / 360, saturation / 100, lightness / 100).map(v => Math.min(255, Math.max(0, v)));
}
)js";
  script += R"js(function harmonyFor(numDots, action, harmony) {
  if (harmony) {
    const selected = HARMONIES.find(h => h.type === harmony);
    if (selected) {
      if (action === 'remove') return dots.length ? HARMONIES.find(h => h.angles.length === selected.angles.length - 1) : HARMONIES[5];
      if (action === 'add') return HARMONIES.find(h => h.angles.length === selected.angles.length + 1);
      if (action === 'update') return selected;
    }
  }
  if (action === 'remove') {
    let h = HARMONIES.find(x => x.angles.length === numDots);
    if (h && h.type === 'analogous' && numDots === 1) h = HARMONIES.find(x => x.type === 'singleAnalogous');
    return h;
  }
  return HARMONIES.find(x => x.angles.length + 1 === numDots);
}
// calculateCompliments: the other dots follow the primary at the harmony's angles.
function calculateCompliments(list, action, harmony) {
  if (!list.length) return [];
  const center = {x: PAD / 2, y: PAD / 2};
  const h = harmonyFor(list.length + (action === 'add' ? 1 : action === 'remove' ? -1 : 0), action, harmony);
  useAlgo = h ? h.type : '';
  if (!h || !h.angles.length) return list;
  const primary = list.find(d => d.ID === 0);
  if (!primary) return [];
  const base = (Math.atan2(primary.y - center.y, primary.x - center.x) * 180 / Math.PI + 360) % 360;
  const distance = Math.min(Math.hypot(primary.x - center.x, primary.y - center.y), PAD / 2);
  const out = [{ID: 0, x: primary.x, y: primary.y, type: primary.type}];
  h.angles.forEach((offset, i) => {
    const r = ((base + offset) % 360) * Math.PI / 180;
    out.push({ID: i + 1, x: center.x + distance * Math.cos(r), y: center.y + distance * Math.sin(r), type: primary.type});
  });
  return out;
}
function placeDot(d) {
  if (!d.el) {
    d.el = document.createElement('div');
    d.el.className = 'dot';
    d.el.addEventListener('mousedown', startDrag);
    d.el.addEventListener('contextmenu', removePrimary);
    pad.append(d.el);
  }
  d.el.classList.toggle('primary', d.ID === 0);
  d.el.style.left = d.x + 'px'; d.el.style.top = d.y + 'px';
  d.el.style.setProperty('--c', `rgb(${d.c[0]}, ${d.c[1]}, ${d.c[2]})`);
}
// handleColorPositions
function applyPositions(list) {
  list.sort((a, b) => a.ID - b.ID);
  for (const p of list) {
    let d = dots.find(x => x.ID === p.ID);
    if (!d) { d = {ID: p.ID}; dots.push(d); }
    d.x = p.x; d.y = p.y; d.type = p.type;
    d.c = colorFromPosition(p.x, p.y, p.type);
    placeDot(d);
  }
  for (const d of dots.filter(x => !list.some(p => p.ID === x.ID))) d.el && d.el.remove();
  dots = dots.filter(x => list.some(p => p.ID === x.ID));
  refreshControls();
}
// updateCurrentWorkspace
let pending = null;
function send(save) {
  const body = {space, opacity, texture, algorithm: useAlgo, lightness: currentLightness, save,
    dots: [...dots].sort((a, b) => a.ID - b.ID).map(d => ({r: d.c[0], g: d.c[1], b: d.c[2], x: d.x, y: d.y, type: d.type || '', primary: d.ID === 0}))};
  if (save) { pending = null; return orbitAction('/api/set', body).catch(() => {}); }
  // Live updates while dragging: at most one in flight.
  if (pending) { pending.next = body; return; }
  pending = {};
  orbitAction('/api/set', body).catch(() => {}).finally(() => {
    const next = pending && pending.next; pending = null;
    if (next) { pending = {}; orbitAction('/api/set', next).catch(() => {}).finally(() => { pending = null; }); }
  });
}
function refreshControls() {
  document.getElementById('add').disabled = !dots.length || dots.length >= MAX_DOTS;
  document.getElementById('remove').disabled = !dots.length;
  document.getElementById('algo').disabled = dots.length < 2;
  document.getElementById('algo').title = ALGO_LABELS[useAlgo] || 'Color harmony';
  document.getElementById('clickToAdd').hidden = dots.length > 0;
}
function padPoint(e) {
  const r = pad.getBoundingClientRect();
  let x = e.clientX - r.left, y = e.clientY - r.top;
  const c = PAD / 2, d = Math.hypot(x - c, y - c);
  if (d > c) { const a = Math.atan2(y - c, x - c); x = c + Math.cos(a) * c; y = c + Math.sin(a) * c; }
  return {x, y};
}
pad.addEventListener('click', (e) => {
  if (e.target.closest('.bar') || dragging || recentlyDragged || e.button !== 0) return;
  const p = padPoint(e);
  if (!dots.length) {
    applyPositions([{ID: 0, x: p.x, y: p.y, type: EXPLICIT_LIGHTNESS}]);
  } else {
    const primary = dots.find(d => d.ID === 0);
    primary.x = p.x; primary.y = p.y;
    applyPositions(calculateCompliments(dots.map(d => ({...d})), 'update', useAlgo));
  }
  send(true);
});
function startDrag(e) {
  if (e.button !== 0 || !e.target.classList.contains('primary')) return;
  e.preventDefault();
  dragging = e.target; dragging.classList.add('dragging');
}
document.addEventListener('mousemove', (e) => {
  if (!dragging) return;
  const p = padPoint(e), primary = dots.find(d => d.ID === 0);
  primary.x = p.x; primary.y = p.y;
  applyPositions(calculateCompliments(dots.map(d => ({...d})), 'update', useAlgo));
  send(false);
});
document.addEventListener('mouseup', () => {
  if (!dragging) return;
  dragging.classList.remove('dragging'); dragging = null;
  recentlyDragged = true; setTimeout(() => recentlyDragged = false, 100);
  send(true);
});
function renumber() { dots.sort((a, b) => a.ID - b.ID).forEach((d, i) => d.ID = i); }
function removePrimary(e) {
  e.preventDefault();
  const d = dots.find(x => x.el === e.target);
  if (!d) return;
  d.el.remove(); dots = dots.filter(x => x !== d); renumber();
  applyPositions(calculateCompliments(dots.map(x => ({...x})), 'remove'));
  send(true);
}
document.getElementById('add').onclick = (e) => {
  e.stopPropagation();
  if (dots.length >= MAX_DOTS) return;
  applyPositions(calculateCompliments(dots.map(d => ({...d})), 'add', useAlgo));
  send(true);
};
document.getElementById('remove').onclick = (e) => {
  e.stopPropagation();
  if (!dots.length) return;
  renumber();
  const last = dots.pop(); last.el.remove();
  applyPositions(calculateCompliments(dots.map(d => ({...d})), 'remove'));
  send(true);
};
document.getElementById('algo').onclick = (e) => {
  e.stopPropagation();
  const options = HARMONIES.filter(h => h.angles.length + 1 === dots.length);
  if (!options.length) return;
  const i = options.findIndex(h => h.type === useAlgo);
  useAlgo = options[i === -1 ? 0 : (i + 1) % options.length].type;
  applyPositions(calculateCompliments(dots.map(d => ({...d})), 'update', useAlgo));
  send(true);
};
for (const b of document.querySelectorAll('#scheme button')) {
  b.onclick = async (e) => {
    e.stopPropagation();
    await orbitAction('/api/scheme', {scheme: b.dataset.scheme});
    load();
  };
}
)js";
  script += R"js(// Preset pages (initPredefinedColors / initColorPages).
const pages = document.getElementById('pages');
for (const page of PAGES) {
  const row = document.createElement('div');
  for (const [lightness, algo, numDots, position, color, type] of page) {
    const s = document.createElement('button');
    s.className = 'swatch'; s.dataset.numDots = numDots;
    if (Array.isArray(color)) { s.style.setProperty('--c1', color[0]); s.style.setProperty('--c2', color[1]); s.style.setProperty('--c3', color[2]); }
    else s.style.background = color;
    s.setAttribute('aria-label', 'Preset');
    s.onclick = () => {
      for (const d of dots.slice(numDots)) d.el.remove();
      dots = dots.slice(0, numDots);
      const [x, y] = position.split(',').map(v => parseInt(v));
      const t = type || EXPLICIT_LIGHTNESS;
      const list = [{ID: 0, x, y, type: t}];
      for (let i = 1; i < numDots; i++) list.push({ID: i, x: 0, y: 0, type: t});
      useAlgo = algo;
      if (lightness !== null) currentLightness = lightness;
      applyPositions(calculateCompliments(list, 'update', useAlgo));
      send(true);
    };
    row.append(s);
  }
  pages.append(row);
}
const left = document.getElementById('left'), right = document.getElementById('right');
function showPage() {
  pages.scrollLeft = colorPage * pages.scrollWidth / PAGES.length;
  left.disabled = colorPage === 0; right.disabled = colorPage === PAGES.length - 1;
}
left.onclick = () => { colorPage = (colorPage - 1 + PAGES.length) % PAGES.length; showPage(); };
right.onclick = () => { colorPage = (colorPage + 1) % PAGES.length; showPage(); };
)js";
  script += R"js(// Opacity: the wave grows with the value (#interpolateWavePath).
const LINE = 'M 51.373 27.395 L 367.037 27.395';
const SINE = 'M 51.373 27.395 C 60.14 -8.503 68.906 -8.503 77.671 27.395 C 86.438 63.293 95.205 63.293 103.971 27.395 C 112.738 -8.503 121.504 -8.503 130.271 27.395 C 139.037 63.293 147.803 63.293 156.57 27.395 C 165.335 -8.503 174.101 -8.503 182.868 27.395 C 191.634 63.293 200.4 63.293 209.167 27.395 C 217.933 -8.503 226.7 -8.503 235.467 27.395 C 244.233 63.293 252.999 63.293 261.765 27.395 C 270.531 -8.503 279.297 -8.503 288.064 27.395 C 296.83 63.293 305.596 63.293 314.363 27.395 C 323.13 -8.503 331.896 -8.503 340.662 27.395 M 314.438 27.395 C 323.204 -8.503 331.97 -8.503 340.737 27.395 C 349.503 63.293 358.27 63.293 367.037 27.395';
function wavePath(t) {
  if (t <= 0.001) return LINE;
  if (t >= 0.999) return SINE;
  const ref = 27.3;
  return SINE.replace(/(-?\d+(?:\.\d+)?) (-?\d+(?:\.\d+)?)/g, (m, x, y) => `${x} ${ref + (parseFloat(y) - ref) * t}`);
}
const slider = document.getElementById('opacity');
const path = document.querySelector('#wave path');
const stops = document.querySelectorAll('#waveGrad stop');
function drawWave() {
  const dark = matchMedia('(prefers-color-scheme: dark)').matches;
  const strong = dark ? 'rgb(161,161,161)' : 'rgb(90,90,90)', faint = dark ? 'rgba(161,161,161,.5)' : 'rgba(77,77,77,.5)';
  let t = (opacity - 0.25) / (0.8 - 0.25);
  if (!dots.length) t = 1;
  t = Math.max(0, Math.min(1, t));
  stops[0].setAttribute('stop-color', strong); stops[1].setAttribute('stop-color', strong); stops[2].setAttribute('stop-color', faint);
  stops[1].setAttribute('offset', `${t * 100}%`); stops[2].setAttribute('offset', `${t * 100}%`);
  const d = wavePath(t);
  path.setAttribute('d', d);
  path.style.stroke = d === LINE ? faint : 'url(#waveGrad)';
  slider.style.setProperty('--th', `${40 + t * 15}px`); slider.style.setProperty('--tw', `${10 + t * 15}px`);
}
slider.addEventListener('input', () => { opacity = parseFloat(slider.value); drawWave(); send(false); });
slider.addEventListener('change', () => send(true));
// Grain dial: 16 steps around a circle (initTextureInput).
const dial = document.getElementById('texture');
const tdots = [];
for (let i = 0; i < 16; i++) {
  const d = document.createElement('div');
  d.className = 'tdot';
  const a = (i / 16) * Math.PI * 2 + 80;
  d.style.left = `${Math.cos(a) * 50 + 50}%`; d.style.top = `${Math.sin(a) * 50 + 50}%`;
  dial.append(d); tdots.push(d);
}
const handle = document.createElement('div');
handle.id = 'handle';
dial.append(handle);
function drawDial() {
  const w = dial.getBoundingClientRect().width || 80;
  const rotation = texture * 360 - 90;
  handle.style.transform = `rotate(${rotation + 90}deg)`;
  handle.style.top = `${w / 2 + Math.sin(rotation * Math.PI / 180) * (w / 2) - 6}px`;
  handle.style.left = `${w / 2 + Math.cos(rotation * Math.PI / 180) * (w / 2) - 3}px`;
  let i = 4;
  for (const d of tdots) { d.classList.toggle('active', i / 16 <= texture); i++; if (i === 16) i = 0; }
  dial.setAttribute('aria-valuenow', String(texture));
}
let turning = false;
function turn(e) {
  const r = dial.getBoundingClientRect();
  let t = (Math.atan2(e.clientY - r.top - r.height / 2, e.clientX - r.left - r.width / 2) * 180 / Math.PI + 90);
  if (t < 0) t += 360;
  t = Math.round(t / 360 * 16) / 16;
  if (t === 1) t = 0;
  if (t !== texture) { texture = t; drawDial(); send(false); }
}
handle.addEventListener('mousedown', (e) => { e.preventDefault(); turning = true; });
document.addEventListener('mousemove', (e) => { if (turning) turn(e); });
document.addEventListener('mouseup', () => { if (turning) { turning = false; send(true); } });
dial.addEventListener('keydown', (e) => {
  const step = e.key === 'ArrowRight' || e.key === 'ArrowUp' ? 1 : e.key === 'ArrowLeft' || e.key === 'ArrowDown' ? -1 : 0;
  if (!step) return;
  texture = ((Math.round(texture * 16) + step + 16) % 16) / 16; drawDial(); send(true);
});
async function load() {
  const s = await (await fetch('/api/state')).json();
  space = s.space; opacity = s.opacity; texture = s.texture; useAlgo = s.algorithm; currentLightness = s.lightness;
  for (const d of dots) d.el && d.el.remove();
  dots = s.dots.map((d, i) => ({ID: i, x: d.x, y: d.y, type: d.type, c: [d.r, d.g, d.b]}));
  const p = dots.findIndex((d, i) => s.dots[i].primary);
  if (p > 0) { const [d] = dots.splice(p, 1); dots.unshift(d); dots.forEach((x, i) => x.ID = i); }
  dots.forEach(placeDot);
  slider.value = opacity;
  for (const b of document.querySelectorAll('#scheme button')) b.setAttribute('aria-pressed', String(b.dataset.scheme === s.scheme));
  document.title = 'Edit Theme: ' + s.name;
  refreshControls(); drawWave(); drawDial();
}
window.orbitReload = load;
load();
)js";
  return {200, "text/html", PageShell("Edit Theme", body, script)};
}

}  // namespace orbit
