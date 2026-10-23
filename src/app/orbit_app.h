#pragma once

#include <map>
#include <string>
#include <vector>

#include "include/cef_app.h"

namespace orbit {

// CefApp shared by all processes. Browser-process behaviour is delegated to
// Application; sub-processes (renderer, GPU, utility) only need the scheme
// registration, which must be identical in every process.
inline constexpr char kMediaQueryMessage[] = "orbit.media-query";
inline constexpr char kMediaReplyMessage[] = "orbit.media-reply";
// Renderer -> browser: a trusted Alt+click on a web link (Zen's glance).
inline constexpr char kGlanceOpenMessage[] = "orbit.glance-open";
// Renderer -> browser: a trusted drag from the page's top strip (Zen's
// zen.view.drag-window-from-content); one number, the distance moved.
inline constexpr char kWindowDragMessage[] = "orbit.window-drag";
// Boosts' zap / focus picker: the browser turns it on in a page's main frame
// (mode "zap" / "focus" / "off"); the page reports picked selectors.
inline constexpr char kBoostPickModeMessage[] = "orbit.boost-pick-mode";
inline constexpr char kBoostPickMessage[] = "orbit.boost-pick";
// Renderer -> browser: this frame's media state as JSON (Zen's media card).
inline constexpr char kMediaStateMessage[] = "orbit.media-state";
// Browser -> renderer: fill a saved login into the page (two strings,
// username then password). The password lives in the browser process and is
// pushed only to the main frame of the matching origin, as browsers do.
inline constexpr char kAutofillMessage[] = "orbit.autofill";
// Renderer -> browser: a form with a password was submitted (username, password).
inline constexpr char kPasswordSubmitMessage[] = "orbit.password-submit";

// The Chrome Web Store names the browser in its buttons. Orbit installs from
// it, so the store's own wording is rewritten in the page ("Add to Chrome" ->
// "Add to Orbit"); the store's banner that sends people to install Chrome is
// hidden. Runs on the store's own pages only, and again as the page changes.
inline constexpr char kStoreRename[] = R"js((function () {
  const phrases = [[/\b(Add|Added|Remove|Removed) (to|from) Chrome\b/g, '$1 $2 Orbit'],
                   [/\bAdding to Chrome\b/g, 'Adding to Orbit'],
                   [/Switch to Chrome to install extensions and themes/g, 'Extensions you add here are installed in Orbit']];
  const rewrite = (root) => {
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    const buttons = [], nags = [];
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
      let t = n.nodeValue;
      if (!t || t.length > 200) continue;
      for (const [re, to] of phrases) t = t.replace(re, to);
      if (t !== n.nodeValue) n.nodeValue = t;
      if (t.trim() === 'Install Chrome') buttons.push(n);
      if (/recommends using Chrome/.test(t)) nags.push(n);
    }
    // Only the "Install Chrome" button itself is hidden.
    for (const n of buttons) {
      const b = n.parentElement && n.parentElement.closest('button, a');
      if (b) b.style.display = 'none';
    }
    // The pop-up that suggests Chrome is dismissed with its own "No thanks".
    for (const n of nags) {
      let el = n.parentElement;
      for (let i = 0; el && i < 6; ++i, el = el.parentElement) {
        const no = Array.from(el.querySelectorAll('button')).find((x) => /^No thanks$/.test((x.innerText || '').trim()));
        if (no) { no.click(); break; }
      }
    }
  };
  let pending = false;
  const run = () => { pending = false; if (document.body) rewrite(document.body); };
  const schedule = () => { if (!pending) { pending = true; setTimeout(run, 50); } };
  new MutationObserver(schedule).observe(document, {childList: true, subtree: true, characterData: true});
  if (document.readyState !== 'loading') schedule();
})())js";

class OrbitApp : public CefApp,
                  public CefBrowserProcessHandler,
                  public CefRenderProcessHandler {
 public:
  OrbitApp() = default;

  // Extracts navigable URLs from command-line arguments (as passed by the
  // shell when Orbit is the default browser). Anything that does not fix up
  // to an http(s)/file URL is ignored.
  static std::vector<std::wstring> UrlsFromCommandLine(CefRefPtr<CefCommandLine> command_line);

  // CefApp
  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override;
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

  // CefRenderProcessHandler (runs in sandboxed renderer processes)
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;
  void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue> extra_info) override;
  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override;

  // CefBrowserProcessHandler
  void OnContextInitialized() override;
  // Browsers Chromium creates itself (an extension opening a tab) land here.
  CefRefPtr<CefClient> GetDefaultClient() override;
  bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                   const CefString& current_directory) override;

 private:
  // Renderer process: each browser's glance trigger key ("" = off).
  std::map<int, std::string> glance_trigger_;
  std::map<int, bool> window_drag_;
  // The picker control installed in each main frame's context (by browser id).
  std::map<int, std::pair<CefRefPtr<CefV8Context>, CefRefPtr<CefV8Value>>> pickers_;
  // The autofill function installed in each main frame's context.
  std::map<int, std::pair<CefRefPtr<CefV8Context>, CefRefPtr<CefV8Value>>> autofill_;
  IMPLEMENT_REFCOUNTING(OrbitApp);
};

}  // namespace orbit
