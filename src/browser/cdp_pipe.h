// A private Chrome DevTools Protocol connection to the browser itself, over
// an anonymous pipe pair that only this process holds (Chromium's
// --remote-debugging-pipe, as Puppeteer and Playwright use it; no TCP port,
// nothing another program can reach). Pipe clients may use the browser-level
// domains a page's DevTools session may not, which Orbit needs for one thing:
// clicking an extension's toolbar action (Extensions.triggerAction), so
// extensions without a popup (Picture-in-Picture, ...) get their
// action.onClicked with activeTab, as from Chrome's toolbar.
#pragma once

#include <functional>
#include <string>

#include "include/cef_command_line.h"

namespace orbit::cdp_pipe {

// Browser process, OnBeforeCommandLineProcessing: creates the pipes and adds
// the switches. Without it (other processes) nothing here is active.
void Setup(CefRefPtr<CefCommandLine> command_line);
bool available();

// Sends |method| with |params_json| (a JSON object, "{}" for none). |done|
// runs on the UI thread with the whole answer's JSON (its "result"), or
// ok=false and the error message.
using Done = std::function<void(bool ok, const std::string& json)>;
// |session_id|: a target session attached with flatten:true, or empty for
// the browser.
void Send(const std::string& method, const std::string& params_json, Done done,
          const std::string& session_id = std::string());

// Protocol events (no id): |method|, the session they came from (empty for
// the browser) and the whole message's JSON. UI thread. One handler.
using EventHandler = std::function<void(const std::string& method, const std::string& session_id,
                                        const std::string& message)>;
void SetEventHandler(EventHandler handler);

}  // namespace orbit::cdp_pipe
