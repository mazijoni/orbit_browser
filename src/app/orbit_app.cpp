#include "app/orbit_app.h"

#include "app/application.h"
#include "browser/default_client.h"
#include "app/self_test.h"
#include "browser/internal_pages.h"
#include "core/strings.h"
#include "core/url_fixup.h"
#include "include/cef_command_line.h"
#include "include/base/cef_callback.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_closure_task.h"

#include <algorithm>

namespace orbit {
namespace {

// ZenGlanceChild: a capturing click listener installed before any page
// script runs. The native callback reaches the listener only through the
// installer's closure, never through a global, and only trusted (real
// user) Alt+left clicks on http(s) links without other modifiers count;
// clicks that moved more than a few pixels since mousedown are selections.
constexpr char kGlanceListener[] = R"js((function (send, trigger) {
  let downX = null, downY = null;
  addEventListener('mousedown', (e) => { downX = e.clientX; downY = e.clientY; }, true);
  addEventListener('click', (e) => {
    const moved = downX !== null && (Math.abs(e.clientX - downX) > 3 || Math.abs(e.clientY - downY) > 3);
    downX = downY = null;
    // zen.glance.activation-method: exactly the trigger modifier.
    const mods = {alt: e.altKey, ctrl: e.ctrlKey, shift: e.shiftKey};
    if (!e.isTrusted || moved || e.button !== 0 || e.metaKey || e.defaultPrevented ||
        !mods[trigger] || Object.keys(mods).some((k) => k !== trigger && mods[k])) return;
    let link = null;
    for (const node of e.composedPath()) {
      if (node && node.localName === 'a' && node.href) { link = node; break; }
    }
    if (!link) return;
    const href = String(link.href);
    if (!/^https?:/i.test(href)) return;
    e.preventDefault();
    e.stopImmediatePropagation();
    // Where the link is (the glance grows out of it).
    const r = link.getBoundingClientRect();
    send(href, r.left, r.top, r.width, r.height);
  }, true);
}))js";

// ZenWindowDragChild: a left press in the top 5% of the top-level page, not
// over anything the page wants to click, select, drag or scroll, starts a
// window move once it has moved 4 px. Trusted events only.
// The Chrome Web Store only offers "Add to Chrome" to browsers whose client
// hints name Google Chrome; this Chromium build names only Chromium. Applied
// to the store's own pages only.
constexpr char kStoreBrand[] = R"js((function () {
  if (typeof NavigatorUAData === 'undefined') return;
  const brands = [{brand: 'Not A(Brand', version: '99'}, {brand: 'Chromium', version: '154'},
                  {brand: 'Google Chrome', version: '154'}];
  Object.defineProperty(NavigatorUAData.prototype, 'brands', {get() { return brands; }, configurable: true});
})())js";

bool IsChromeWebStoreUrl(const std::string& url) {
  return url.rfind("https://chromewebstore.google.com/", 0) == 0 ||
         url.rfind("https://chrome.google.com/webstore", 0) == 0;
}

// Password capture: remembers the last non-empty password a form held and
// reports it (username, password) when the form is submitted or the page is
// left. The browser decides whether to offer saving it.
constexpr char kPasswordCapture[] = R"js((function (send) {
  let last = null, sent = false;
  const userOf = (pw) => {
    const inputs = Array.from((pw.form || document).querySelectorAll('input'));
    for (let i = inputs.indexOf(pw) - 1; i >= 0; --i) {
      const t = (inputs[i].type || 'text').toLowerCase();
      if ((t === 'text' || t === 'email' || t === 'tel') && inputs[i].value) return inputs[i].value;
    }
    return '';
  };
  const remember = (scope) => {
    const root = scope && scope.querySelectorAll ? scope : document;
    const pws = Array.from(root.querySelectorAll('input[type=password]')).filter((i) => i.value);
    if (!pws.length) return;
    last = {u: userOf(pws[0]), p: pws[0].value};
    sent = false;
  };
  const flush = () => {
    if (last && !sent && last.p.length <= 256 && last.u.length <= 256) { sent = true; send(last.u, last.p); }
  };
  document.addEventListener('input', (e) => {
    const t = e.target;
    if (t && t.type === 'password') remember(t.form || document);
  }, true);
  document.addEventListener('submit', (e) => { remember(e.target); flush(); }, true);
  window.addEventListener('pagehide', flush);
}))js";

class PasswordSender : public CefV8Handler {
 public:
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>&, CefString&) override {
    if (arguments.size() != 2 || !arguments[0]->IsString() || !arguments[1]->IsString()) return true;
    const CefString user = arguments[0]->GetStringValue();
    const CefString password = arguments[1]->GetStringValue();
    if (password.empty() || user.size() > 256 || password.size() > 256) return true;
    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
    if (!frame || !frame->IsMain()) return true;
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kPasswordSubmitMessage);
    message->GetArgumentList()->SetString(0, user);
    message->GetArgumentList()->SetString(1, password);
    frame->SendProcessMessage(PID_BROWSER, message);
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(PasswordSender);
};

constexpr char kWindowDragListener[] = R"js((function (send) {
  const interactive = new Set(['a', 'button', 'input', 'select', 'textarea', 'label', 'summary', 'details',
      'option', 'audio', 'video', 'canvas', 'iframe', 'embed', 'object', 'img', 'svg']);
  const roles = new Set(['button', 'checkbox', 'combobox', 'grid', 'link', 'listbox', 'menu', 'menubar',
      'menuitem', 'option', 'radio', 'scrollbar', 'searchbox', 'slider', 'spinbutton', 'switch', 'tab',
      'textbox', 'toolbar', 'tree', 'treegrid']);
  const cursors = new Set(['pointer', 'text', 'grab', 'grabbing', 'move', 'crosshair', 'vertical-text',
      'col-resize', 'row-resize', 'ew-resize', 'ns-resize']);
  const overContent = (e) => {
    if (e.clientX >= document.documentElement.clientWidth) return true;  // the scrollbar
    for (let node = e.target; node && node.nodeType === 1; node = node.parentElement || node.getRootNode().host) {
      if (interactive.has(node.localName) || node.isContentEditable || node.draggable) return true;
      if (roles.has(node.getAttribute('role'))) return true;
      if (node.tabIndex >= 0 && node !== document.body) return true;
    }
    if (e.target && e.target.nodeType === 1 && cursors.has(getComputedStyle(e.target).cursor)) return true;
    const range = document.caretRangeFromPoint && document.caretRangeFromPoint(e.clientX, e.clientY);
    if (range && range.startContainer.nodeType === 3) {
      const r = document.createRange();
      r.selectNodeContents(range.startContainer);
      for (const b of r.getClientRects()) {
        if (e.clientX >= b.left && e.clientX <= b.right && e.clientY >= b.top && e.clientY <= b.bottom &&
            getComputedStyle(range.startContainer.parentElement).userSelect !== 'none') return true;
      }
    }
    return false;
  };
  let start = null, sent = false;
  addEventListener('mousedown', (e) => {
    start = null; sent = false;
    if (!e.isTrusted || e.button !== 0 || e.buttons !== 1 || e.detail !== 1 || e.defaultPrevented ||
        e.ctrlKey || e.altKey || e.shiftKey || e.metaKey || document.fullscreenElement ||
        document.pointerLockElement || e.clientY > innerHeight * 0.05 || overContent(e)) return;
    start = {x: e.screenX, y: e.screenY};
  }, true);
  addEventListener('mousemove', (e) => {
    if (!start || !e.isTrusted) return;
    if (!(e.buttons & 1)) { start = null; return; }
    const d = Math.hypot(e.screenX - start.x, e.screenY - start.y) * devicePixelRatio;
    if (d < 4) return;
    if (!sent) getSelection().removeAllRanges();
    sent = true;
    e.preventDefault();
    send(d);  // the browser starts the move (50 px for a maximized window)
  }, true);
  addEventListener('mouseup', () => { start = null; }, true);
  addEventListener('dragstart', () => { start = null; }, true);
}))js";

// ZenMediaController's card needs to know what plays: on media events (and
// every second while something plays) the frame reports one JSON object —
// playing / paused, the MediaSession title and artist (or the page's), and
// the position. Audible playback only (not muted, volume above 0).
constexpr char kMediaListener[] = R"js((function (send) {
  let timer = 0, last = '';
  const report = () => {
    timer = 0;
    const media = Array.from(document.querySelectorAll('video,audio'));
    const audible = (m) => !m.paused && !m.ended && !m.muted && m.volume > 0;
    const playing = media.find(audible);
    const paused = !playing && media.find((m) => m.paused && !m.ended && m.currentTime > 0 && !m.muted);
    const el = playing || paused;
    const meta = navigator.mediaSession && navigator.mediaSession.metadata;
    const state = el ? {state: playing ? 'playing' : 'paused',
                        title: String((meta && meta.title) || document.title || location.hostname).slice(0, 200),
                        artist: String((meta && meta.artist) || location.hostname).slice(0, 200),
                        time: el.currentTime || 0, duration: isFinite(el.duration) ? el.duration : 0}
                     : {state: 'none'};
    const s = JSON.stringify(state);
    if (s === last) return;
    last = s;
    send(s);
  };
  const schedule = () => { if (!timer) timer = setTimeout(report, 200); };
  for (const t of ['play', 'playing', 'pause', 'ended', 'emptied', 'volumechange', 'seeked'])
    addEventListener(t, schedule, true);
  setInterval(() => { if (last.indexOf('"playing"') >= 0) report(); }, 1000);
}))js";

class MediaStateSender : public CefV8Handler {
 public:
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>&, CefString&) override {
    if (arguments.size() != 1 || !arguments[0]->IsString()) return true;
    const std::string json = arguments[0]->GetStringValue().ToString();
    if (json.size() > 2048) return true;
    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
    if (!frame) return true;
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kMediaStateMessage);
    message->GetArgumentList()->SetString(0, json);
    frame->SendProcessMessage(PID_BROWSER, message);
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(MediaStateSender);
};

class WindowDragSender : public CefV8Handler {
 public:
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>&, CefString&) override {
    if (arguments.size() != 1 || !arguments[0]->IsDouble()) return true;
    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
    if (!frame || !frame->IsMain()) return true;
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kWindowDragMessage);
    message->GetArgumentList()->SetDouble(0, arguments[0]->GetDoubleValue());
    frame->SendProcessMessage(PID_BROWSER, message);
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(WindowDragSender);
};

// ZenZapOverlayChild / ZenSelectorComponent: while the browser has a page
// in zap or focus mode, the element under the pointer is outlined and a
// trusted click picks it (reported as a CSS selector); Escape stops.
constexpr char kBoostPicker[] = R"js((function (send) {
  let mode = '', box = null, hover = null;
  const ident = /^[A-Za-z][A-Za-z0-9_-]*$/;
  const selectorFor = (el) => {
    const parts = [];
    for (let n = el; n && n.nodeType === 1 && n !== document.documentElement; n = n.parentElement) {
      if (n.id && ident.test(n.id)) { parts.unshift('#' + n.id); break; }
      if (n === document.body) { parts.unshift('body'); break; }
      let i = 1;
      for (let s = n.previousElementSibling; s; s = s.previousElementSibling) if (s.localName === n.localName) i++;
      parts.unshift(n.localName + ':nth-of-type(' + i + ')');
    }
    return parts.join(' > ');
  };
  const outline = (el) => {
    if (!box) {
      box = document.createElement('div');
      box.style.cssText = 'position:fixed;z-index:2147483647;pointer-events:none;border-radius:4px;' +
        'outline:2px solid #ff5c5c;background:rgba(255,92,92,.15);transition:all .08s';
    }
    const r = el.getBoundingClientRect();
    Object.assign(box.style, {left: r.left + 'px', top: r.top + 'px', width: r.width + 'px', height: r.height + 'px'});
    if (!box.isConnected) document.documentElement.append(box);
  };
  addEventListener('mousemove', (e) => {
    if (!mode || !e.isTrusted || e.target === box) return;
    hover = e.target;
    outline(hover);
  }, true);
  const swallow = (e) => { if (mode && e.isTrusted) { e.preventDefault(); e.stopImmediatePropagation(); } };
  addEventListener('mousedown', swallow, true);
  addEventListener('mouseup', swallow, true);
  addEventListener('click', (e) => {
    if (!mode || !e.isTrusted || e.button !== 0) return;
    e.preventDefault();
    e.stopImmediatePropagation();
    const target = e.target === box ? hover : e.target;
    if (target && target !== document.documentElement && target !== document.body) send(mode, selectorFor(target));
  }, true);
  addEventListener('keydown', (e) => { if (mode && e.isTrusted && e.key === 'Escape') { send('off', ''); } }, true);
  return (next) => { mode = next === 'zap' || next === 'focus' ? next : ''; if (!mode && box) box.remove(); };
}))js";

// Fills a saved login into the page. Installed per main frame; the browser
// calls it with (username, password) once it has matched the page's origin.
// Only visible, enabled, same-form fields are touched, and events are fired
// so the site's own validation runs.
constexpr char kAutofillInstaller[] = R"js((function () {
  const visible = (el) => {
    if (!el || el.disabled || el.readOnly || el.type === 'hidden') return false;
    const r = el.getBoundingClientRect();
    if (r.width < 2 || r.height < 2) return false;
    const st = getComputedStyle(el);
    return st.visibility !== 'hidden' && st.display !== 'none';
  };
  const setValue = (el, value) => {
    const proto = el instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLElement.prototype;
    const setter = Object.getOwnPropertyDescriptor(proto, 'value');
    el.focus();
    if (setter && setter.set) setter.set.call(el, value); else el.value = value;
    el.dispatchEvent(new Event('input', {bubbles: true}));
    el.dispatchEvent(new Event('change', {bubbles: true}));
    el.blur();
  };
  return (username, password) => {
    const pwFields = Array.from(document.querySelectorAll('input[type=password]')).filter(visible);
    if (!pwFields.length) return;
    const pw = pwFields[0];
    setValue(pw, password);
    if (username) {
      const form = pw.form;
      const candidates = Array.from((form || document).querySelectorAll(
          'input[type=text],input[type=email],input[type=tel],input:not([type])')).filter(visible);
      // The username field is the visible text field just before the password.
      let user = null;
      for (const c of candidates) {
        if (pw.compareDocumentPosition(c) & Node.DOCUMENT_POSITION_PRECEDING) user = c; else break;
      }
      if (user) setValue(user, username);
    }
  };
}))js";

class BoostPickSender : public CefV8Handler {
 public:
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>&, CefString&) override {
    if (arguments.size() != 2 || !arguments[0]->IsString() || !arguments[1]->IsString()) return true;
    const std::wstring mode = arguments[0]->GetStringValue().ToWString();
    const std::wstring selector = arguments[1]->GetStringValue().ToWString();
    if (selector.size() > 500) return true;
    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
    if (!frame || !frame->IsMain()) return true;
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kBoostPickMessage);
    message->GetArgumentList()->SetString(0, mode);
    message->GetArgumentList()->SetString(1, selector);
    frame->SendProcessMessage(PID_BROWSER, message);
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(BoostPickSender);
};

class GlanceSender : public CefV8Handler {
 public:
  bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>&, CefString&) override {
    if (arguments.empty() || !arguments[0]->IsString()) return true;
    const std::wstring url = arguments[0]->GetStringValue().ToWString();
    if (url.size() > 8192) return true;
    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
    if (!frame) return true;
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kGlanceOpenMessage);
    message->GetArgumentList()->SetString(0, url);
    // The link's viewport rect, for the opening animation (top-level page
    // only: a frame's coordinates are not the page's).
    if (frame->IsMain() && arguments.size() == 5) {
      for (size_t i = 1; i < 5; ++i) {
        if (!arguments[i]->IsDouble() && !arguments[i]->IsInt()) return true;
        message->GetArgumentList()->SetDouble(i, arguments[i]->GetDoubleValue());
      }
    }
    frame->SendProcessMessage(PID_BROWSER, message);
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(GlanceSender);
};

}  // namespace

void OrbitApp::OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefDictionaryValue> extra_info) {
  std::string trigger = extra_info && extra_info->HasKey("glance")
                            ? extra_info->GetString("glance").ToString()
                            : std::string();
  if (trigger != "alt" && trigger != "ctrl" && trigger != "shift") trigger.clear();
  glance_trigger_[browser->GetIdentifier()] = trigger;
  window_drag_[browser->GetIdentifier()] =
      extra_info && extra_info->HasKey("drag") && extra_info->GetBool("drag");
}

void OrbitApp::OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) {
  pickers_.erase(browser->GetIdentifier());
  autofill_.erase(browser->GetIdentifier());
  glance_trigger_.erase(browser->GetIdentifier());
  window_drag_.erase(browser->GetIdentifier());
}

void OrbitApp::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefRefPtr<CefV8Context> context) {
  // The frame's URL is not always set yet (a page reached by a redirect), so
  // the page's own location is asked as well.
  std::string page_url = frame->GetURL().ToString();
  if (!IsChromeWebStoreUrl(page_url)) {
    CefRefPtr<CefV8Value> href;
    CefRefPtr<CefV8Exception> href_exception;
    if (context->Eval("location.href", CefString(), 0, href, href_exception) && href && href->IsString()) {
      page_url = href->GetStringValue().ToString();
    }
  }
  if (IsChromeWebStoreUrl(page_url)) {
    CefRefPtr<CefV8Value> ignored;
    CefRefPtr<CefV8Exception> brand_exception;
    context->Eval(kStoreBrand, CefString(), 0, ignored, brand_exception);
    context->Eval(kStoreRename, CefString(), 0, ignored, brand_exception);
  }
  // Pages only (the browser's own orbit:// pages play nothing).
  if (frame->GetURL().ToString().rfind("orbit:", 0) != 0 && frame->IsMain()) {
    // Boosts' picker, controlled from the browser (kBoostPickModeMessage).
    CefRefPtr<CefV8Value> installer;
    CefRefPtr<CefV8Exception> exception;
    if (context->Eval(kBoostPicker, CefString(), 0, installer, exception) && installer && installer->IsFunction()) {
      CefV8ValueList args{CefV8Value::CreateFunction("send", new BoostPickSender())};
      CefRefPtr<CefV8Value> control = installer->ExecuteFunction(nullptr, args);
      if (control && control->IsFunction()) pickers_[browser->GetIdentifier()] = {context, control};
    }
    // Password autofill: the fill function, called when the browser pushes a
    // matched credential (kAutofillMessage).
    if (frame->IsMain()) {
      CefRefPtr<CefV8Value> filler;
      CefRefPtr<CefV8Exception> fx;
      if (context->Eval(kAutofillInstaller, CefString(), 0, filler, fx) && filler && filler->IsFunction()) {
        CefRefPtr<CefV8Value> fill = filler->ExecuteFunction(nullptr, {});
        if (fill && fill->IsFunction()) autofill_[browser->GetIdentifier()] = {context, fill};
      }
      // New logins: offered for saving when a form with a password is submitted.
      CefRefPtr<CefV8Value> capture;
      CefRefPtr<CefV8Exception> cx;
      if (context->Eval(kPasswordCapture, CefString(), 0, capture, cx) && capture && capture->IsFunction()) {
        CefV8ValueList args{CefV8Value::CreateFunction("send", new PasswordSender())};
        capture->ExecuteFunction(nullptr, args);
      }
    }
  }
  if (frame->GetURL().ToString().rfind("orbit:", 0) != 0) {
    CefRefPtr<CefV8Value> installer;
    CefRefPtr<CefV8Exception> exception;
    if (context->Eval(kMediaListener, CefString(), 0, installer, exception) && installer &&
        installer->IsFunction()) {
      CefV8ValueList args{CefV8Value::CreateFunction("send", new MediaStateSender())};
      installer->ExecuteFunction(nullptr, args);
    }
  }
  if (const auto drag = window_drag_.find(browser->GetIdentifier());
      drag != window_drag_.end() && drag->second && frame->IsMain()) {
    CefRefPtr<CefV8Value> installer;
    CefRefPtr<CefV8Exception> exception;
    if (context->Eval(kWindowDragListener, CefString(), 0, installer, exception) && installer &&
        installer->IsFunction()) {
      CefV8ValueList args{CefV8Value::CreateFunction("send", new WindowDragSender())};
      installer->ExecuteFunction(nullptr, args);
    }
  }
  const auto it = glance_trigger_.find(browser->GetIdentifier());
  if (it == glance_trigger_.end() || it->second.empty()) return;  // glance off
  CefRefPtr<CefV8Value> installer;
  CefRefPtr<CefV8Exception> exception;
  if (!context->Eval(kGlanceListener, CefString(), 0, installer, exception) || !installer ||
      !installer->IsFunction()) {
    return;
  }
  CefV8ValueList args{CefV8Value::CreateFunction("send", new GlanceSender()),
                      CefV8Value::CreateString(it->second)};
  installer->ExecuteFunction(nullptr, args);
}

void OrbitApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterInternalScheme(registrar);
}

void OrbitApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                              CefRefPtr<CefCommandLine> command_line) {
  // Self-test runs only: let fixture pages start audio without a user gesture
  // so media detection can be tested unattended. Never set for normal use.
  if (process_type.empty() && command_line->HasSwitch("orbit-self-test")) {
    command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
  }
  // Self-test of the extension host: an unpacked fixture loaded at launch.
  if (process_type.empty() && command_line->HasSwitch("orbit-test-extension")) {
    command_line->AppendSwitchWithValue("load-extension",
                                        command_line->GetSwitchValue("orbit-test-extension"));
  }
}

bool OrbitApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame, CefProcessId source_process,
                                         CefRefPtr<CefProcessMessage> message) {
  // Renderer process: report whether any audible <audio>/<video> element is
  // playing in this frame. Only the browser process may ask. Web Audio API
  // output is not visible to this check (documented limitation).
  if (source_process == PID_BROWSER && message->GetName() == kBoostPickModeMessage) {
    const auto it = pickers_.find(browser->GetIdentifier());
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (it != pickers_.end() && args->GetSize() == 1 && args->GetType(0) == VTYPE_STRING &&
        it->second.first->IsValid() && it->second.first->Enter()) {
      CefV8ValueList call{CefV8Value::CreateString(args->GetString(0))};
      it->second.second->ExecuteFunction(nullptr, call);
      it->second.first->Exit();
    }
    return true;
  }
  if (source_process == PID_BROWSER && message->GetName() == kAutofillMessage) {
    const auto it = autofill_.find(browser->GetIdentifier());
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (it != autofill_.end() && args->GetSize() == 2 && it->second.first->IsValid() &&
        it->second.first->Enter()) {
      CefV8ValueList call{CefV8Value::CreateString(args->GetString(0)),
                          CefV8Value::CreateString(args->GetString(1))};
      it->second.second->ExecuteFunction(nullptr, call);
      it->second.first->Exit();
    }
    return true;
  }
  if (source_process != PID_BROWSER || message->GetName() != kMediaQueryMessage) return false;
  bool playing = false;
  CefRefPtr<CefV8Context> context = frame->GetV8Context();
  if (context && context->Enter()) {
    CefRefPtr<CefV8Value> result;
    CefRefPtr<CefV8Exception> exception;
    if (context->Eval("Array.prototype.some.call(document.querySelectorAll('audio,video'),"
                      "m => !m.paused && !m.ended && !m.muted && m.volume > 0)",
                      CefString(), 0, result, exception) &&
        result && result->IsBool()) {
      playing = result->GetBoolValue();
    }
    context->Exit();
  }
  CefRefPtr<CefProcessMessage> reply = CefProcessMessage::Create(kMediaReplyMessage);
  reply->GetArgumentList()->SetBool(0, playing);
  frame->SendProcessMessage(PID_BROWSER, reply);
  return true;
}

std::vector<std::wstring> OrbitApp::UrlsFromCommandLine(
    CefRefPtr<CefCommandLine> command_line) {
  std::vector<std::wstring> urls;
  CefCommandLine::ArgumentList args;
  command_line->GetArguments(args);
  for (const auto& arg : args) {
    const OmniboxResult r = FixupOmniboxInput(arg.ToWString(), L"");
    if (r.kind != OmniboxResult::Kind::kUrl) continue;
    const std::wstring lower = ToLowerAscii(r.url);
    if (lower.starts_with(L"http://") || lower.starts_with(L"https://") ||
        lower.starts_with(L"file:")) {
      urls.push_back(r.url);
    }
    if (urls.size() >= 20) break;  // bound work from a single launch
  }
  return urls;
}

CefRefPtr<CefClient> OrbitApp::GetDefaultClient() {
  static CefRefPtr<DefaultClient> client = new DefaultClient();
  return client;
}

void OrbitApp::OnContextInitialized() {
  CefRefPtr<CefCommandLine> command_line = CefCommandLine::GetGlobalCommandLine();
  Application::Get()->Start(UrlsFromCommandLine(command_line));
  // Self-tests only run against an explicitly chosen data directory.
  if (command_line->HasSwitch("orbit-self-test") && command_line->HasSwitch("user-data-dir")) {
    self_test::Start(command_line->GetSwitchValue("orbit-self-test").ToWString());
  }
  // Benchmark hook (scripts/benchmark.ps1): after N seconds, run the sleep
  // policy on every background tab regardless of idle time.
  if (command_line->HasSwitch("orbit-bench-sleep-after")) {
    const int seconds = std::clamp(
        _wtoi(command_line->GetSwitchValue("orbit-bench-sleep-after").ToWString().c_str()), 1,
        600);
    for (int pass = 0; pass < 2; ++pass) {
      CefPostDelayedTask(TID_UI, base::BindOnce([] {
                           if (Application* app = Application::Get()) app->ApplySleepPolicy(0);
                         }),
                         seconds * 1000 + pass * 1500);
    }
  }
}

bool OrbitApp::OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                            const CefString& current_directory) {
  Application::Get()->OnRelaunch(UrlsFromCommandLine(command_line));
  return true;
}

}  // namespace orbit
