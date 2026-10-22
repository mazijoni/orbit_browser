// orbit://boost/: the boost editor (ZenBoostsEditor, zen-boost-editor.inc.
// xhtml): name and shuffle, the colour disc with the accent and its
// complementary dot, invert / advanced colour controls / disable, font,
// size, case, Zap and Focus (picking on the page), custom CSS, delete.
#include "browser/internal_page_parts.h"

namespace orbit {

InternalResponse BoostPage(const InternalRequest&) {
  std::string body = R"html(
<style>
html,body{height:100%;overflow:hidden}
body{-webkit-user-select:none;user-select:none;font-size:13px}
.ico{font-family:"Segoe Fluent Icons","Segoe MDL2 Assets";font-style:normal;font-size:15px;line-height:1}
#editor,#code{padding:10px;display:flex;flex-direction:column;gap:8px;height:100%;box-sizing:border-box}
#code{display:none}
body.code #editor{display:none} body.code #code{display:flex}
.head{display:flex;align-items:center;gap:6px}
.head .name{flex:1;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;cursor:text;padding:4px 6px;border-radius:6px}
.head .name:hover{background:var(--surface)}
.sq{width:30px;height:30px;min-width:30px;padding:0;display:flex;align-items:center;justify-content:center;border-radius:8px}
#disc{position:relative;flex:none;width:200px;height:200px;margin:4px auto;border-radius:50%;
  background:conic-gradient(from 90deg,hsl(0 90% 60%),hsl(60 90% 60%),hsl(120 90% 60%),hsl(180 90% 60%),hsl(240 90% 60%),hsl(300 90% 60%),hsl(360 90% 60%));
  cursor:crosshair}
#disc::after{content:"";position:absolute;inset:0;border-radius:50%;background:radial-gradient(circle,rgba(128,128,128,.9),transparent 70%);pointer-events:none}
#disc[data-off=true]{filter:grayscale(1);opacity:.45}
.dot{position:absolute;width:22px;height:22px;border-radius:50%;border:3px solid #fff;transform:translate(-50%,-50%);box-shadow:0 1px 4px rgba(0,0,0,.4);z-index:2}
.dot.primary{width:30px;height:30px;cursor:grab}
#magic{position:absolute;right:-4px;top:-4px;z-index:3}
.row{display:flex;gap:6px}
.row>button{flex:1}
.big{display:flex;flex-direction:column;align-items:flex-start;gap:2px;padding:10px 12px;border-radius:10px;text-align:left}
.big .v{color:var(--muted);font-size:12px}
.big[aria-pressed=true]{outline:2px solid var(--accent)}
#advanced{display:none;flex-direction:column;gap:4px;padding:8px;border-radius:10px;background:var(--surface)}
#advanced.open{display:flex}
#advanced label{display:flex;justify-content:space-between;gap:8px;align-items:center}
select{width:100%}
textarea{flex:1;width:100%;box-sizing:border-box;resize:none;font:12px Consolas,monospace;background:var(--surface);color:var(--text);border:1px solid var(--border);border-radius:8px;padding:8px}
.foot{display:flex;gap:6px;margin-top:auto}
.foot>button{flex:1}
</style>
<div id="editor">
  <div class="head">
    <button class="sq" id="close" title="Close" aria-label="Close"><i class="ico">&#xE711;</i></button>
    <div class="name" id="name" title="Rename Boost" tabindex="0"></div>
    <button class="sq" id="shuffle" title="Shuffle Boost Settings" aria-label="Shuffle Boost Settings"><i class="ico">&#xE8B1;</i></button>
  </div>
  <div id="disc" role="application" aria-label="Boost colors">
    <div class="dot" id="dot2"></div>
    <div class="dot primary" id="dot1"></div>
  </div>
  <div class="row">
    <button id="invert" class="sq" title="Smart Invert Colors" aria-label="Smart Invert Colors" aria-pressed="false"><i class="ico">&#xE793;</i></button>
    <button id="controls" class="sq" title="Advanced Color Controls" aria-label="Advanced Color Controls"><i class="ico">&#xE9E9;</i></button>
    <button id="disable" class="sq" title="Disable Color Adjustments" aria-label="Disable Color Adjustments" aria-pressed="false"><i class="ico">&#xE7BA;</i></button>
  </div>
  <div id="advanced">
    <label>Contrast <input type="range" id="contrast" min="0.05" max="0.9" step="0.01"></label>
    <label>Brightness <input type="range" id="brightness" min="0" max="1" step="0.01"></label>
    <label>Original Saturation <input type="range" id="saturation" min="0" max="1" step="0.01"></label>
  </div>
  <select id="font" aria-label="Font">
    <option value="">Page font</option><option value="sans-serif">Sans serif</option><option value="serif">Serif</option>
    <option value="monospace">Monospace</option><option value="'Segoe UI'">Segoe UI</option><option value="Georgia">Georgia</option>
    <option value="Verdana">Verdana</option><option value="'Times New Roman'">Times New Roman</option>
    <option value="'Courier New'">Courier New</option><option value="'Comic Sans MS'">Comic Sans MS</option>
  </select>
  <div class="row">
    <button class="big" id="size"><span>Size</span><span class="v" id="sizeV"></span></button>
    <button class="big" id="case" title="Toggle Text Case"><span>Case</span><span class="v" id="caseV"></span></button>
  </div>
  <div class="row">
    <button class="big" id="zap" aria-pressed="false"><span>Zap</span><span class="v" id="zapV"></span></button>
    <button class="big" id="focus" aria-pressed="false"><span>Focus</span><span class="v" id="focusV"></span></button>
  </div>
  <div class="row"><button id="unzap">Unzap all</button><button id="unfocus">Clear focus</button></div>
  <div class="foot">
    <button id="codeBtn"><i class="ico">&#xE943;</i> Code</button>
    <button id="delete" class="danger">Delete Boost</button>
  </div>
</div>
<div id="code">
  <div class="head"><button class="sq" id="back" aria-label="Back"><i class="ico">&#xE72B;</i></button><div class="name">Custom CSS</div></div>
  <textarea id="css" spellcheck="false" aria-label="Custom CSS" placeholder="/* CSS for this site */"></textarea>
</div>)html";
  std::string script = R"js(
const $ = (id) => document.getElementById(id);
let b = null, saveTimer = 0;
const SIZES = [0.8, 0.9, 1, 1.1, 1.25, 1.5];
const CASES = ['none', 'uppercase', 'lowercase', 'capitalize'];
const CASE_LABELS = {none: 'Default', uppercase: 'UPPERCASE', lowercase: 'lowercase', capitalize: 'Capitalize'};
function save(now) {
  clearTimeout(saveTimer);
  const go = () => orbitAction('/api/set', {name: b.name, enabled: true, color: b.color, dotAngle: b.dotAngle,
    dotDistance: b.dotDistance, secondaryDelta: b.secondaryDelta, brightness: b.brightness, saturation: b.saturation,
    contrast: b.contrast, smartInvert: b.smartInvert, font: b.font, textCase: b.textCase, size: b.size, css: b.css})
    .then(() => { b.exists = true; });
  if (now) go(); else saveTimer = setTimeout(go, 150);
}
// The disc: the angle is the hue, the distance the dot's place (Zen's
// dotAngleDeg / dotDistance); the second dot follows at the delta.
function place() {
  const r = 100, d = b.dotDistance * (r - 12);
  const a1 = b.dotAngle * Math.PI / 180, a2 = (b.dotAngle + b.secondaryDelta) * Math.PI / 180;
  const set = (el, a, hue) => {
    el.style.left = (r + Math.cos(a) * d) + 'px'; el.style.top = (r + Math.sin(a) * d) + 'px';
    el.style.background = `hsl(${hue} ${Math.round((1 - b.saturation) * 100)}% ${Math.round((0.1 + 0.9 * b.brightness) * 100)}%)`;
  };
  set($('dot1'), a1, b.dotAngle); set($('dot2'), a2, b.dotAngle + b.secondaryDelta);
  $('disc').dataset.off = String(!b.color);
}
function render() {
  $('name').textContent = b.name;
  document.title = 'Boost: ' + b.domain;
  place();
  $('invert').setAttribute('aria-pressed', String(b.smartInvert));
  $('disable').setAttribute('aria-pressed', String(!b.color));
  $('contrast').value = b.contrast; $('brightness').value = b.brightness; $('saturation').value = b.saturation;
  $('font').value = b.font;
  $('sizeV').textContent = Math.round(b.size * 100) + '%';
  $('caseV').textContent = CASE_LABELS[b.textCase] || 'Default';
  $('zapV').textContent = b.pick === 'zap' ? 'Click elements on the page to Zap them' : b.zaps + ' zapped';
  $('focusV').textContent = b.pick === 'focus' ? 'Click the part to keep' : (b.focus ? 'On' : 'Off');
  $('zap').setAttribute('aria-pressed', String(b.pick === 'zap'));
  $('focus').setAttribute('aria-pressed', String(b.pick === 'focus'));
  if (document.activeElement !== $('css')) $('css').value = b.css;
}
async function load() { b = await (await fetch('/api/state')).json(); render(); }
window.orbitReload = load;
let dragging = false;
function fromPointer(e) {
  const r = $('disc').getBoundingClientRect();
  const x = e.clientX - r.left - r.width / 2, y = e.clientY - r.top - r.height / 2;
  b.dotAngle = ((Math.atan2(y, x) * 180 / Math.PI) + 360) % 360;
  b.dotDistance = Math.min(1, Math.hypot(x, y) / (r.width / 2 - 12));
  b.color = true;
  place(); save(false);
}
$('disc').addEventListener('mousedown', (e) => { dragging = true; fromPointer(e); });
document.addEventListener('mousemove', (e) => { if (dragging) fromPointer(e); });
document.addEventListener('mouseup', () => { if (dragging) { dragging = false; save(true); render(); } });
$('invert').onclick = () => { b.smartInvert = !b.smartInvert; render(); save(true); };
$('disable').onclick = () => { b.color = !b.color; render(); save(true); };
$('controls').onclick = () => $('advanced').classList.toggle('open');
for (const id of ['contrast', 'brightness', 'saturation']) {
  $(id).addEventListener('input', () => { b[id] = parseFloat($(id).value); b.color = true; place(); save(false); });
}
$('font').onchange = () => { b.font = $('font').value; save(true); };
$('size').onclick = () => { const i = SIZES.indexOf(b.size); b.size = SIZES[(i + 1) % SIZES.length]; render(); save(true); };
$('case').onclick = () => { const i = CASES.indexOf(b.textCase); b.textCase = CASES[(i + 1) % CASES.length]; render(); save(true); };
$('zap').onclick = async () => { await orbitAction('/api/pick', {mode: b.pick === 'zap' ? 'off' : 'zap'}); load(); };
$('focus').onclick = async () => { await orbitAction('/api/pick', {mode: b.pick === 'focus' ? 'off' : 'focus'}); load(); };
$('unzap').onclick = async () => { await orbitAction('/api/clear', {list: 'zap'}); load(); };
$('unfocus').onclick = async () => { await orbitAction('/api/clear', {list: 'focus'}); load(); };
// cmd_zenBoostShuffle: a random accent, delta and tone.
$('shuffle').onclick = () => {
  b.color = true; b.dotAngle = Math.random() * 360; b.dotDistance = 0.5 + Math.random() * 0.5;
  b.secondaryDelta = [30, 55, 90, 150, 180][Math.floor(Math.random() * 5)];
  b.brightness = 0.3 + Math.random() * 0.4; b.saturation = Math.random() * 0.6; b.contrast = 0.3 + Math.random() * 0.5;
  render(); save(true);
};
// cmd_zenBoostEditName: renamed in place (Enter keeps, Escape cancels).
$('name').onclick = () => {
  const el = $('name');
  el.contentEditable = 'plaintext-only';
  el.focus();
  document.getSelection().selectAllChildren(el);
};
$('name').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { e.preventDefault(); $('name').blur(); }
  if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); $('name').textContent = b.name; $('name').blur(); }
});
$('name').addEventListener('blur', () => {
  const el = $('name');
  el.contentEditable = 'false';
  const next = el.textContent.trim().slice(0, 100);
  if (next && next !== b.name) { b.name = next; save(true); }
  render();
});
$('delete').onclick = async () => { await orbitAction('/api/delete'); await orbitAction('/api/close'); };
$('close').onclick = () => orbitAction('/api/close');
$('codeBtn').onclick = () => { document.body.classList.add('code'); $('css').focus(); };
$('back').onclick = () => document.body.classList.remove('code');
$('css').addEventListener('input', () => { b.css = $('css').value; save(false); });
load();
)js";
  return {200, "text/html", PageShell("Boost", body, script)};
}

}  // namespace orbit
