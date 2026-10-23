#include "browser/importer.h"

#include <windows.h>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <optional>

#include "core/crypto.h"
#include "core/json.h"
#include "core/log.h"
#include "core/strings.h"

namespace orbit::importer {
namespace {

std::optional<std::string> ReadFileBytes(const std::filesystem::path& path, size_t max_bytes = 256u << 20) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) > max_bytes) {
    ::CloseHandle(file);
    return std::nullopt;
  }
  std::string out(static_cast<size_t>(size.QuadPart), '\0');
  size_t done = 0;
  bool ok = true;
  while (done < out.size()) {
    DWORD read = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20));
    if (!::ReadFile(file, out.data() + done, chunk, &read, nullptr) || read == 0) {
      ok = read == 0 && done == out.size();
      break;
    }
    done += read;
  }
  ::CloseHandle(file);
  if (done != out.size() && !ok) out.resize(done);
  return out;
}

// A source database may be held open by the running browser, so work on a copy.
class TempDb {
 public:
  explicit TempDb(const std::filesystem::path& source) {
    wchar_t dir[MAX_PATH];
    wchar_t name[MAX_PATH];
    if (::GetTempPathW(MAX_PATH, dir) && ::GetTempFileNameW(dir, L"orb", 0, name)) {
      path_ = name;
      if (!::CopyFileW(source.c_str(), path_.c_str(), FALSE)) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        path_.clear();
      }
    }
    if (!path_.empty()) {
      // Read-only, treat the copy as immutable (ignore a stale -wal).
      const std::string uri = "file:" + ToUtf8(path_.wstring()) + "?immutable=1&mode=ro";
      if (sqlite3_open_v2(uri.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
      }
    }
  }
  ~TempDb() {
    if (db_) sqlite3_close(db_);
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  sqlite3* get() const { return db_; }

 private:
  std::filesystem::path path_;
  sqlite3* db_ = nullptr;
};

std::string ColBlob(sqlite3_stmt* stmt, int col) {
  const void* data = sqlite3_column_blob(stmt, col);
  const int bytes = sqlite3_column_bytes(stmt, col);
  return data && bytes > 0 ? std::string(static_cast<const char*>(data), static_cast<size_t>(bytes)) : std::string();
}

std::wstring ColText(sqlite3_stmt* stmt, int col) {
  const unsigned char* text = sqlite3_column_text(stmt, col);
  return text ? FromUtf8(reinterpret_cast<const char*>(text)) : std::wstring();
}

std::filesystem::path EnvPath(const wchar_t* name, const wchar_t* tail) {
  wchar_t buffer[MAX_PATH];
  const DWORD n = ::GetEnvironmentVariableW(name, buffer, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::filesystem::path p(buffer);
  if (tail) p /= tail;
  return p;
}

bool Exists(const std::filesystem::path& p) {
  std::error_code ec;
  return !p.empty() && std::filesystem::exists(p, ec);
}

int64_t WriteTimeMs(const std::filesystem::path& p) {
  std::error_code ec;
  const auto t = std::filesystem::last_write_time(p, ec);
  if (ec) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

// --- Chromium ---------------------------------------------------------------

std::wstring OriginString(std::wstring_view url) {
  // scheme://host[:port] — what a saved login is keyed on.
  const size_t scheme_end = url.find(L"://");
  if (scheme_end == std::wstring::npos) return std::wstring(url);
  const size_t host_start = scheme_end + 3;
  size_t host_end = url.find(L'/', host_start);
  if (host_end == std::wstring::npos) host_end = url.size();
  return std::wstring(url.substr(0, host_end));
}

void ReadChromiumBookmarks(const std::filesystem::path& file, std::vector<BookmarkNode>& out);

void WalkChromiumBookmarkNode(const JsonValue& node, std::vector<BookmarkNode>& out) {
  const std::string& type = node.Str("type");
  if (type == "url") {
    const std::wstring url = FromUtf8(node.Str("url"));
    if (StartsWithNoCase(url, L"http://") || StartsWithNoCase(url, L"https://") ||
        StartsWithNoCase(url, L"ftp://") || StartsWithNoCase(url, L"file://")) {
      out.push_back({FromUtf8(node.Str("name")), url, false, {}});
    }
  } else if (type == "folder") {
    BookmarkNode folder{FromUtf8(node.Str("name")), L"", true, {}};
    if (const JsonValue* children = node.Get("children"); children && children->is_array()) {
      for (const JsonValue& child : children->array) WalkChromiumBookmarkNode(child, folder.children);
    }
    if (!folder.children.empty()) out.push_back(std::move(folder));
  }
}

void ReadChromiumBookmarks(const std::filesystem::path& file, std::vector<BookmarkNode>& out) {
  const auto text = ReadFileBytes(file, 64u << 20);
  if (!text) return;
  const auto json = ParseJson(*text);
  const JsonValue* roots = json ? json->Get("roots") : nullptr;
  if (!roots || !roots->is_object()) return;
  for (const char* which : {"bookmark_bar", "other", "synced"}) {
    const JsonValue* root = roots->Get(which);
    if (root && root->is_object()) WalkChromiumBookmarkNode(*root, out);
  }
}

void ReadChromiumHistory(const std::filesystem::path& file, std::vector<HistoryVisit>& out) {
  TempDb db(file);
  if (!db.get()) return;
  sqlite3_stmt* stmt = nullptr;
  // Chromium time: microseconds since 1601-01-01; convert to unix ms.
  if (sqlite3_prepare_v2(db.get(),
                         "SELECT u.url, u.title, v.visit_time FROM visits v JOIN urls u ON u.id = v.url "
                         "ORDER BY v.visit_time DESC LIMIT 20000",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::wstring url = ColText(stmt, 0);
    if (!StartsWithNoCase(url, L"http")) continue;
    const int64_t chrome_us = sqlite3_column_int64(stmt, 2);
    const int64_t unix_ms = chrome_us > 0 ? chrome_us / 1000 - 11644473600000LL : 0;
    out.push_back({url, ColText(stmt, 1), unix_ms});
  }
  sqlite3_finalize(stmt);
}

void ReadChromiumLogins(const std::filesystem::path& file, const std::string& aes_key,
                        std::vector<Login>& out) {
  TempDb db(file);
  if (!db.get()) return;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.get(),
                         "SELECT origin_url, username_value, password_value FROM logins", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::wstring origin = OriginString(ColText(stmt, 0));
    const std::wstring username = ColText(stmt, 1);
    const std::string encrypted = ColBlob(stmt, 2);
    if (origin.empty() || encrypted.empty()) continue;
    const std::wstring password = DecryptChromiumValue(encrypted, aes_key);
    if (!password.empty()) out.push_back({origin, username, password});
  }
  sqlite3_finalize(stmt);
}

// --- Firefox ----------------------------------------------------------------

// A minimal DER reader for the few structures in key4.db / logins.json.
struct Der {
  std::string_view data;
  size_t pos = 0;
  bool ReadTag(uint8_t& tag, std::string_view& content) {
    if (pos + 2 > data.size()) return false;
    tag = static_cast<uint8_t>(data[pos++]);
    size_t len = static_cast<uint8_t>(data[pos++]);
    if (len & 0x80) {
      const size_t bytes = len & 0x7F;
      if (bytes == 0 || bytes > 4 || pos + bytes > data.size()) return false;
      len = 0;
      for (size_t i = 0; i < bytes; ++i) len = (len << 8) | static_cast<uint8_t>(data[pos++]);
    }
    if (pos + len > data.size()) return false;
    content = data.substr(pos, len);
    pos += len;
    return true;
  }
};

// Firefox key4.db + logins.json. Modern Firefox (NSS, no master password)
// wraps the key with PBES2: PBKDF2-HMAC-SHA256 then AES-256-CBC. Returns the
// 3DES/AES key that decrypts each login's SecretDecoderRing blob, or empty.
std::optional<std::string> FirefoxKey(const std::filesystem::path& key4) {
  TempDb db(key4);
  if (!db.get()) return std::nullopt;
  std::string global_salt, item2;
  {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT item1, item2 FROM metadata WHERE id = 'password'", -1, &stmt,
                           nullptr) != SQLITE_OK) {
      return std::nullopt;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      global_salt = ColBlob(stmt, 0);
      item2 = ColBlob(stmt, 1);
    }
    sqlite3_finalize(stmt);
  }
  if (global_salt.empty() || item2.empty()) return std::nullopt;

  // Decrypt a SecretDecoderRing blob (ASN.1): SEQUENCE { keyid, algid, data }.
  // Here it verifies the empty master password; the same routine decrypts the
  // stored key. Returns the plaintext or empty.
  auto sdr_decrypt = [&](std::string_view blob) -> std::string {
    Der top{blob};
    uint8_t tag = 0;
    std::string_view seq;
    if (!top.ReadTag(tag, seq) || tag != 0x30) return {};
    Der inner{seq};
    std::string_view keyid, algid_seq, data;
    if (!inner.ReadTag(tag, keyid) || !inner.ReadTag(tag, algid_seq) || tag != 0x30 ||
        !inner.ReadTag(tag, data) || tag != 0x04) {
      return {};
    }
    // algid: SEQUENCE { OID pkcs5 PBES2, SEQUENCE { kdf, enc } }
    Der alg{algid_seq};
    std::string_view oid, params;
    if (!alg.ReadTag(tag, oid) || tag != 0x06 || !alg.ReadTag(tag, params) || tag != 0x30) return {};
    // PBES2 OID 1.2.840.113549.1.5.13
    static const std::array<uint8_t, 9> kPbes2 = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x05, 0x0D};
    if (oid.size() != kPbes2.size() || memcmp(oid.data(), kPbes2.data(), oid.size()) != 0) return {};
    Der pbes2{params};
    std::string_view kdf_seq, enc_seq;
    if (!pbes2.ReadTag(tag, kdf_seq) || tag != 0x30 || !pbes2.ReadTag(tag, enc_seq) || tag != 0x30) return {};
    // kdf: SEQUENCE { OID pbkdf2, SEQUENCE { salt(OCTET), iters(INT), keylen(INT), prf } }
    Der kdf{kdf_seq};
    std::string_view kdf_oid, kdf_params;
    if (!kdf.ReadTag(tag, kdf_oid) || tag != 0x06 || !kdf.ReadTag(tag, kdf_params) || tag != 0x30) return {};
    Der kp{kdf_params};
    std::string_view salt, iter_bytes, keylen_bytes;
    if (!kp.ReadTag(tag, salt) || tag != 0x04 || !kp.ReadTag(tag, iter_bytes) || tag != 0x02) return {};
    uint32_t iterations = 0;
    for (char c : iter_bytes) iterations = (iterations << 8) | static_cast<uint8_t>(c);
    size_t key_len = 32;
    if (kp.ReadTag(tag, keylen_bytes) && tag == 0x02) {
      key_len = 0;
      for (char c : keylen_bytes) key_len = (key_len << 8) | static_cast<uint8_t>(c);
    }
    if (iterations == 0 || iterations > 10'000'000 || key_len == 0 || key_len > 64) return {};
    // enc: SEQUENCE { OID aes-256-cbc, OCTET iv }
    Der enc{enc_seq};
    std::string_view enc_oid, iv;
    if (!enc.ReadTag(tag, enc_oid) || tag != 0x06 || !enc.ReadTag(tag, iv) || tag != 0x04) return {};
    // NSS prepends the global salt (SHA-1) to form the PBKDF2 password here:
    // entry salt is empty, the "password" is sha1(globalSalt + masterPw).
    const std::string pw = crypto::Sha1(global_salt);  // master password is empty
    auto derived = crypto::Pbkdf2Sha256(pw, std::string(salt), iterations, key_len);
    if (!derived) return {};
    auto plain = crypto::CbcDecrypt(crypto::Cipher::kAes, *derived, std::string(iv), std::string(data));
    return plain.value_or(std::string());
  };

  // item2 must decrypt to "password-check\x02\x02" when the master password
  // is empty; if so, the stored key (nssPrivate a11) decrypts the same way.
  const std::string check = sdr_decrypt(item2);
  if (check.rfind("password-check", 0) != 0) return std::nullopt;

  std::string a11;
  {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT a11 FROM nssPrivate WHERE a11 IS NOT NULL LIMIT 1", -1, &stmt,
                           nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) a11 = ColBlob(stmt, 0);
      sqlite3_finalize(stmt);
    }
  }
  if (a11.empty()) return std::nullopt;
  std::string key = sdr_decrypt(a11);
  // The ASN.1 wrapper yields the 24-byte 3DES key padded; NSS uses the first
  // 24 bytes for login values.
  if (key.size() >= 24) key.resize(24);
  return key.empty() ? std::nullopt : std::optional<std::string>(key);
}

// Decrypts one logins.json "encryptedUsername"/"encryptedPassword" value: a
// base64 SEQUENCE { keyid, SEQUENCE{OID des-ede3-cbc, OCTET iv}, OCTET data }.
std::wstring FirefoxDecrypt(const std::string& key, std::string_view base64) {
  auto blob = crypto::Base64Decode(base64);
  if (!blob) return {};
  Der top{*blob};
  uint8_t tag = 0;
  std::string_view seq;
  if (!top.ReadTag(tag, seq) || tag != 0x30) return {};
  Der inner{seq};
  std::string_view keyid, algid, data;
  if (!inner.ReadTag(tag, keyid) || !inner.ReadTag(tag, algid) || tag != 0x30 ||
      !inner.ReadTag(tag, data) || tag != 0x04) {
    return {};
  }
  Der alg{algid};
  std::string_view oid, iv;
  if (!alg.ReadTag(tag, oid) || tag != 0x06 || !alg.ReadTag(tag, iv) || tag != 0x04) return {};
  auto plain = crypto::CbcDecrypt(crypto::Cipher::kTripleDes, key, std::string(iv), std::string(data));
  return plain ? FromUtf8(*plain) : std::wstring();
}

void ReadFirefoxLogins(const std::filesystem::path& profile_dir, std::vector<Login>& out) {
  const auto key = FirefoxKey(profile_dir / L"key4.db");
  if (!key) return;
  const auto text = ReadFileBytes(profile_dir / L"logins.json", 64u << 20);
  if (!text) return;
  const auto json = ParseJson(*text);
  const JsonValue* logins = json ? json->Get("logins") : nullptr;
  if (!logins || !logins->is_array()) return;
  for (const JsonValue& entry : logins->array) {
    const std::wstring origin = OriginString(FromUtf8(entry.Str("hostname")));
    const std::wstring user = FirefoxDecrypt(*key, entry.Str("encryptedUsername"));
    const std::wstring pass = FirefoxDecrypt(*key, entry.Str("encryptedPassword"));
    if (!origin.empty() && !pass.empty()) out.push_back({origin, user, pass});
  }
}

void ReadFirefoxPlaces(const std::filesystem::path& profile_dir, bool want_bookmarks, bool want_history,
                       Data& data) {
  TempDb db(profile_dir / L"places.sqlite");
  if (!db.get()) return;
  if (want_history) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(),
                           "SELECT p.url, p.title, h.visit_date FROM moz_historyvisits h "
                           "JOIN moz_places p ON p.id = h.place_id ORDER BY h.visit_date DESC LIMIT 20000",
                           -1, &stmt, nullptr) == SQLITE_OK) {
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::wstring url = ColText(stmt, 0);
        if (!StartsWithNoCase(url, L"http")) continue;
        data.history.push_back({url, ColText(stmt, 1), sqlite3_column_int64(stmt, 2) / 1000});  // us → ms
      }
      sqlite3_finalize(stmt);
    }
  }
  if (want_bookmarks) {
    // moz_bookmarks: type 1 = url (fk → moz_places), type 2 = folder; parent
    // links the tree. Build it under each top-level folder.
    struct Row {
      int64_t id = 0, parent = 0;
      int type = 0;
      std::wstring title, url;
    };
    std::vector<Row> rows;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(),
                           "SELECT b.id, b.parent, b.type, b.title, COALESCE(p.url, '') "
                           "FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id = b.fk ORDER BY b.position",
                           -1, &stmt, nullptr) == SQLITE_OK) {
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back({sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1),
                        sqlite3_column_int(stmt, 2), ColText(stmt, 3), ColText(stmt, 4)});
      }
      sqlite3_finalize(stmt);
    }
    std::function<std::vector<BookmarkNode>(int64_t, int)> build = [&](int64_t parent, int depth) {
      std::vector<BookmarkNode> out;
      if (depth > 32) return out;
      for (const Row& r : rows) {
        if (r.parent != parent) continue;
        if (r.type == 1 && (StartsWithNoCase(r.url, L"http") || StartsWithNoCase(r.url, L"ftp"))) {
          out.push_back({r.title, r.url, false, {}});
        } else if (r.type == 2) {
          BookmarkNode folder{r.title, L"", true, build(r.id, depth + 1)};
          if (!folder.children.empty()) out.push_back(std::move(folder));
        }
      }
      return out;
    };
    // Roots: toolbar (id 3), menu (2), unfiled (5). Skip the synthetic root 1.
    for (int64_t root : {3, 2, 5}) {
      auto children = build(root, 0);
      for (auto& c : children) data.bookmarks.push_back(std::move(c));
    }
  }
}

void DetectChromium(const std::wstring& name, const std::filesystem::path& user_data,
                    std::vector<Source>& out) {
  if (!Exists(user_data / L"Local State")) return;
  std::error_code ec;
  for (auto& entry : std::filesystem::directory_iterator(user_data, ec)) {
    if (!entry.is_directory()) continue;
    const std::wstring dir = entry.path().filename().wstring();
    if (dir != L"Default" && !StartsWithNoCase(dir, L"Profile ")) continue;
    Source s;
    s.engine = Source::Engine::kChromium;
    s.profile_dir = entry.path();
    s.user_data_dir = user_data;
    s.name = name + (dir == L"Default" ? L"" : L" — " + dir);
    s.has_bookmarks = Exists(entry.path() / L"Bookmarks");
    s.has_passwords = Exists(entry.path() / L"Login Data");
    s.has_history = Exists(entry.path() / L"History");
    if (s.has_bookmarks || s.has_passwords || s.has_history) out.push_back(std::move(s));
  }
}

void DetectFirefox(std::vector<Source>& out) {
  const std::filesystem::path root = EnvPath(L"APPDATA", L"Mozilla\\Firefox\\Profiles");
  std::error_code ec;
  if (!Exists(root)) return;
  for (auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_directory()) continue;
    const bool places = Exists(entry.path() / L"places.sqlite");
    const bool logins = Exists(entry.path() / L"logins.json") && Exists(entry.path() / L"key4.db");
    if (!places && !logins) continue;
    Source s;
    s.engine = Source::Engine::kFirefox;
    s.profile_dir = entry.path();
    std::wstring dir = entry.path().filename().wstring();
    const size_t dot = dir.find(L'.');
    s.name = L"Firefox (" + (dot == std::wstring::npos ? dir : dir.substr(dot + 1)) + L")";
    s.has_bookmarks = places;
    s.has_history = places;
    s.has_passwords = logins;
    out.push_back(std::move(s));
  }
}

}  // namespace

std::string ChromiumKeyFromLocalState(const std::string& local_state_json) {
  const auto json = ParseJson(local_state_json);
  const JsonValue* os_crypt = json ? json->Get("os_crypt") : nullptr;
  if (!os_crypt) return {};
  const std::string& encoded = os_crypt->Str("encrypted_key");
  if (encoded.empty()) return {};
  auto blob = crypto::Base64Decode(encoded);
  if (!blob || blob->rfind("DPAPI", 0) != 0) return {};
  auto key = crypto::DpapiUnprotect(blob->substr(5));
  return key.value_or(std::string());
}

std::wstring DecryptChromiumValue(const std::string& encrypted, const std::string& aes_key) {
  // v10/v11: AES-256-GCM, 3-byte prefix + 12-byte nonce + ciphertext+tag.
  if ((encrypted.rfind("v10", 0) == 0 || encrypted.rfind("v11", 0) == 0) && !aes_key.empty() &&
      encrypted.size() > 3 + 12) {
    const std::string nonce = encrypted.substr(3, 12);
    const std::string sealed = encrypted.substr(3 + 12);
    if (auto plain = crypto::AesGcmDecrypt(aes_key, nonce, sealed)) return FromUtf8(*plain);
    return {};
  }
  // Legacy: the value is a DPAPI blob directly.
  if (auto plain = crypto::DpapiUnprotect(encrypted)) return FromUtf8(*plain);
  return {};
}

std::vector<Source> DetectSources() {
  std::vector<Source> out;
  const std::filesystem::path local = EnvPath(L"LOCALAPPDATA", nullptr);
  const std::filesystem::path roaming = EnvPath(L"APPDATA", nullptr);
  struct Known {
    const wchar_t* name;
    std::filesystem::path base;
  };
  const Known chromium[] = {
      {L"Chrome", local / L"Google\\Chrome\\User Data"},
      {L"Edge", local / L"Microsoft\\Edge\\User Data"},
      {L"Brave", local / L"BraveSoftware\\Brave-Browser\\User Data"},
      {L"Vivaldi", local / L"Vivaldi\\User Data"},
      {L"Opera", roaming / L"Opera Software\\Opera Stable"},
      {L"Chromium", local / L"Chromium\\User Data"},
  };
  for (const Known& k : chromium) {
    if (Exists(k.base)) DetectChromium(k.name, k.base, out);
  }
  DetectFirefox(out);
  // Most-recently-used first (by the profile's last write).
  std::stable_sort(out.begin(), out.end(), [](const Source& a, const Source& b) {
    return WriteTimeMs(a.profile_dir) > WriteTimeMs(b.profile_dir);
  });
  return out;
}

Data Read(const Source& source, bool want_bookmarks, bool want_logins, bool want_history) {
  Data data;
  if (source.engine == Source::Engine::kChromium) {
    if (want_bookmarks && source.has_bookmarks) {
      ReadChromiumBookmarks(source.profile_dir / L"Bookmarks", data.bookmarks);
    }
    if (want_history && source.has_history) {
      ReadChromiumHistory(source.profile_dir / L"History", data.history);
    }
    if (want_logins && source.has_passwords) {
      std::string aes_key;
      if (auto local_state = ReadFileBytes(source.user_data_dir / L"Local State", 32u << 20)) {
        aes_key = ChromiumKeyFromLocalState(*local_state);
      }
      ReadChromiumLogins(source.profile_dir / L"Login Data", aes_key, data.logins);
    }
  } else {
    ReadFirefoxPlaces(source.profile_dir, want_bookmarks && source.has_bookmarks,
                      want_history && source.has_history, data);
    if (want_logins && source.has_passwords) ReadFirefoxLogins(source.profile_dir, data.logins);
  }
  ORBIT_LOG_INFO("import: read %zu bookmarks, %zu logins, %zu history from a source",
                 data.bookmarks.size(), data.logins.size(), data.history.size());
  return data;
}

}  // namespace orbit::importer
