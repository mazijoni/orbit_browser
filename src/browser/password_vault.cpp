#include "browser/password_vault.h"

#include "app/application.h"
#include "app/orbit_app.h"
#include "browser/profile.h"
#include "browser/tab.h"
#include "core/crypto.h"
#include "core/strings.h"
#include "ui/browser_window.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "include/cef_process_message.h"

namespace orbit::passwords {

std::wstring Protect(const std::wstring& cleartext) {
  auto blob = crypto::DpapiProtect(ToUtf8(cleartext));
  return blob ? FromUtf8(crypto::Base64Encode(*blob)) : std::wstring();
}

std::wstring Unprotect(const std::wstring& secret) {
  auto blob = crypto::Base64Decode(ToUtf8(secret));
  if (!blob) return {};
  auto plain = crypto::DpapiUnprotect(*blob);
  return plain ? FromUtf8(*plain) : std::wstring();
}

std::wstring OriginOf(const std::wstring& url) {
  if (!StartsWithNoCase(url, L"http://") && !StartsWithNoCase(url, L"https://")) return {};
  const size_t scheme_end = url.find(L"://");
  const size_t host_start = scheme_end + 3;
  size_t host_end = url.find(L'/', host_start);
  if (host_end == std::wstring::npos) host_end = url.size();
  return host_end > host_start ? url.substr(0, host_end) : std::wstring();
}

void FillSavedLogin(Tab* tab) {
  if (!tab || !tab->browser()) return;
  Profile* profile = tab->profile();
  if (!profile || profile->is_private()) return;
  Application* app = Application::Get();
  if (app->store().GetPref(Store::kGlobalScope, L"autofill_passwords").value_or(L"1") != L"1") return;
  const std::wstring origin = OriginOf(tab->url());
  if (origin.empty()) return;
  auto matches = app->store().FindPasswords(profile->id(), origin);
  if (matches.empty()) return;
  // FindPasswords is ordered by last use; fill the most recently used one.
  const PasswordRecord& best = matches.front();
  const std::wstring password = Unprotect(best.secret);
  if (password.empty()) return;
  CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kAutofillMessage);
  message->GetArgumentList()->SetString(0, best.username);
  message->GetArgumentList()->SetString(1, password);
  tab->browser()->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
  app->store().TouchPassword(profile->id(), best.id, NowUnixMillis());
}

namespace {
struct Offer {
  bool valid = false;
  int64_t profile_id = 0;
  std::wstring origin;
  std::wstring username;
  std::wstring secret;  // DPAPI-protected, like the store's
  bool update = false;
};
Offer& Pending() {
  static Offer offer;
  return offer;
}
}  // namespace

bool HasPendingOffer() { return Pending().valid; }
void ClearPendingOffer() { Pending() = Offer{}; }

bool AcceptPendingOffer() {
  Offer offer = Pending();
  Pending() = Offer{};
  if (!offer.valid) return false;
  PasswordRecord record;
  record.profile_id = offer.profile_id;
  record.origin = offer.origin;
  record.username = offer.username;
  record.secret = offer.secret;
  record.created_at = NowUnixMillis();
  record.used_at = NowUnixMillis();
  return Application::Get()->store().UpsertPassword(record) != 0;
}

void OfferToSave(Tab* tab, const std::wstring& page_url, const std::wstring& username,
                 const std::wstring& password) {
  if (!tab || password.empty() || password.size() > 256 || username.size() > 256) return;
  Profile* profile = tab->profile();
  if (!profile || profile->is_private()) return;
  Application* app = Application::Get();
  if (app->store().GetPref(Store::kGlobalScope, L"autofill_passwords").value_or(L"1") != L"1") return;
  const std::wstring origin = OriginOf(page_url);
  if (origin.empty()) return;
  bool update = false;
  for (const PasswordRecord& existing : app->store().FindPasswords(profile->id(), origin)) {
    if (existing.username != username) continue;
    if (Unprotect(existing.secret) == password) {
      app->store().TouchPassword(profile->id(), existing.id, NowUnixMillis());
      return;  // already saved
    }
    update = true;
  }
  BrowserWindow* window = tab->HostWindow();
  if (!window) return;
  const std::wstring secret = Protect(password);
  if (secret.empty()) return;
  Pending() = Offer{true, profile->id(), origin, username, secret, update};

  const size_t host_at = origin.find(L"://") + 3;
  const std::wstring host = origin.substr(host_at);
  ui::Toast::Content content;
  content.id = L"save-password";
  content.text = std::wstring(update ? L"Update the saved password for " : L"Save the password for ") +
                 (username.empty() ? host : username + L" on " + host) + L"?";
  content.timeout_ms = 15000;
  content.button_glyph = ui::glyph::kCheck;
  content.button_label = update ? L"Update" : L"Save";
  content.on_button = [window_id = window->hwnd()]() {
    const bool saved = AcceptPendingOffer();
    for (const auto& w : Application::Get()->windows()) {
      if (w->hwnd() != window_id || w->closing()) continue;
      ui::Toast::Content done;
      done.id = L"password-saved";
      done.text = saved ? L"Password saved" : L"Could not save the password";
      w->ShowToast(done);
    }
  };
  window->ShowToast(content);
}

}  // namespace orbit::passwords
