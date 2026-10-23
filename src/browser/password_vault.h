#pragma once

#include <string>

namespace orbit {

class Tab;

// Saved-password helpers. A password is kept in the store as base64 of a
// DPAPI blob (bound to this Windows user); it is turned back into cleartext
// only in the browser process, to fill a form or show it in settings.
namespace passwords {

// Cleartext <-> the opaque base64 the store holds (empty on failure).
std::wstring Protect(const std::wstring& cleartext);
std::wstring Unprotect(const std::wstring& secret);

// scheme://host[:port] of |url| (what a login is keyed on), or empty for a
// non-web URL.
std::wstring OriginOf(const std::wstring& url);

// If autofill is on and exactly one sensible saved login matches the tab's
// current origin, push it to the page (the renderer fills the form). The
// password never leaves the browser process except into that page's frame.
void FillSavedLogin(Tab* tab);

// A form with a password was submitted in |tab| (the page at |page_url|):
// shows Orbit's "Save password" / "Update password" toast unless the login is
// already stored. Nothing is stored until the user presses the button. Off in
// private windows and when "fill saved passwords" is off.
void OfferToSave(Tab* tab, const std::wstring& page_url, const std::wstring& username,
                 const std::wstring& password);
// The toast's button (and tests): stores the pending offer. False if none.
bool AcceptPendingOffer();
bool HasPendingOffer();
void ClearPendingOffer();

}  // namespace passwords
}  // namespace orbit
