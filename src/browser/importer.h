#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/bookmark_html.h"

namespace orbit {

// Importing the user's own saved data from other browsers installed on this
// machine into Orbit — the standard "import from another browser" feature.
// All of it stays on the local machine and in the current Windows user's
// context; nothing is sent anywhere. A login's password is decrypted (DPAPI
// for Chromium, the NSS key for Firefox) only so it can be re-encrypted into
// Orbit's own per-user store.
namespace importer {

// One browser profile Orbit can import from.
struct Source {
  enum class Engine { kChromium, kFirefox };
  Engine engine = Engine::kChromium;
  std::wstring name;              // "Chrome", "Edge — Profile 1", "Firefox (default)"
  std::filesystem::path profile_dir;
  std::filesystem::path user_data_dir;  // Chromium: holds "Local State" (the key)
  bool has_passwords = false;
  bool has_bookmarks = false;
  bool has_history = false;
};

struct Login {
  std::wstring origin;    // scheme://host[:port]
  std::wstring username;
  std::wstring password;  // cleartext, in memory only, until re-encrypted
};

struct HistoryVisit {
  std::wstring url;
  std::wstring title;
  int64_t visit_time = 0;  // unix ms
};

struct Data {
  std::vector<BookmarkNode> bookmarks;
  std::vector<Login> logins;
  std::vector<HistoryVisit> history;
};

// The installed browsers' profiles, newest-used first. Does not touch any of
// their files beyond checking which data exists.
std::vector<Source> DetectSources();

// Reads |source|. |want_*| selects what to read (reading passwords decrypts
// them). A file a browser is holding open is copied to a temp file first, so
// the source browser does not have to be closed. Missing or unreadable parts
// are skipped; the returned Data has whatever could be read.
Data Read(const Source& source, bool want_bookmarks, bool want_logins, bool want_history);

// A passwords file exported from a browser or password manager (CSV):
// Chrome / Edge / Brave / Opera ("name,url,username,password,note"), Firefox
// ("url","username","password",...), Bitwarden (login_uri, login_username,
// login_password), 1Password, LastPass, Safari, KeePass and the like; the
// columns are found by their names. Only web logins (http / https; a bare
// "example.com" counts as https) with a password are taken.
struct CsvLogins {
  bool recognized = false;  // a header with a URL and a password column
  std::vector<Login> logins;
  int skipped = 0;          // rows without a web address or a password
};
CsvLogins ParsePasswordCsv(std::string_view text);

// Exposed for tests: decrypt a Chromium "encrypted_value" column given the
// AES key already unwrapped from Local State (empty for the legacy DPAPI
// form, which this handles directly).
std::wstring DecryptChromiumValue(const std::string& encrypted, const std::string& aes_key);
// The AES key from a Chromium "Local State" file (os_crypt.encrypted_key),
// unwrapped with DPAPI. Empty when absent (older profiles use DPAPI per value).
std::string ChromiumKeyFromLocalState(const std::string& local_state_json);

}  // namespace importer
}  // namespace orbit
