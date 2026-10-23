#include "core/crypto.h"

#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <vector>

namespace orbit::crypto {
namespace {

class Algorithm {
 public:
  Algorithm(LPCWSTR id, ULONG flags = 0) {
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&handle_, id, nullptr, flags))) handle_ = nullptr;
  }
  ~Algorithm() {
    if (handle_) ::BCryptCloseAlgorithmProvider(handle_, 0);
  }
  Algorithm(const Algorithm&) = delete;
  Algorithm& operator=(const Algorithm&) = delete;
  BCRYPT_ALG_HANDLE get() const { return handle_; }
  bool SetMode(LPCWSTR mode) {
    return handle_ && BCRYPT_SUCCESS(::BCryptSetProperty(handle_, BCRYPT_CHAINING_MODE,
                                                         reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(mode)),
                                                         static_cast<ULONG>((wcslen(mode) + 1) * sizeof(wchar_t)), 0));
  }

 private:
  BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class Key {
 public:
  Key(BCRYPT_ALG_HANDLE alg, std::string_view secret) {
    if (!alg || !BCRYPT_SUCCESS(::BCryptGenerateSymmetricKey(
                    alg, &handle_, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(secret.data())),
                    static_cast<ULONG>(secret.size()), 0))) {
      handle_ = nullptr;
    }
  }
  ~Key() {
    if (handle_) ::BCryptDestroyKey(handle_);
  }
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
  BCRYPT_KEY_HANDLE get() const { return handle_; }

 private:
  BCRYPT_KEY_HANDLE handle_ = nullptr;
};

PUCHAR Bytes(std::string_view s) { return reinterpret_cast<PUCHAR>(const_cast<char*>(s.data())); }

std::optional<std::string> Gcm(bool encrypt, std::string_view key, std::string_view nonce, std::string_view data) {
  constexpr size_t kTag = 16;
  if (key.size() != 32 || nonce.empty() || (!encrypt && data.size() < kTag)) return std::nullopt;
  Algorithm alg(BCRYPT_AES_ALGORITHM);
  if (!alg.SetMode(BCRYPT_CHAIN_MODE_GCM)) return std::nullopt;
  Key k(alg.get(), key);
  if (!k.get()) return std::nullopt;
  std::string tag(kTag, '\0');
  std::string_view body = data;
  if (!encrypt) {
    tag.assign(data.substr(data.size() - kTag));
    body = data.substr(0, data.size() - kTag);
  }
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = Bytes(nonce);
  info.cbNonce = static_cast<ULONG>(nonce.size());
  info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
  info.cbTag = static_cast<ULONG>(kTag);
  std::string out(body.size(), '\0');
  ULONG written = 0;
  const NTSTATUS status =
      encrypt ? ::BCryptEncrypt(k.get(), Bytes(body), static_cast<ULONG>(body.size()), &info, nullptr, 0,
                                reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(out.size()), &written, 0)
              : ::BCryptDecrypt(k.get(), Bytes(body), static_cast<ULONG>(body.size()), &info, nullptr, 0,
                                reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(out.size()), &written, 0);
  if (!BCRYPT_SUCCESS(status)) return std::nullopt;
  out.resize(written);
  if (encrypt) out += tag;
  return out;
}

std::optional<std::string> Cbc(bool encrypt, Cipher cipher, std::string_view key, std::string_view iv,
                               std::string_view data) {
  Algorithm alg(cipher == Cipher::kAes ? BCRYPT_AES_ALGORITHM : BCRYPT_3DES_ALGORITHM);
  if (!alg.SetMode(BCRYPT_CHAIN_MODE_CBC)) return std::nullopt;
  const size_t block = cipher == Cipher::kAes ? 16 : 8;
  if (iv.size() != block || (!encrypt && (data.empty() || data.size() % block))) return std::nullopt;
  Key k(alg.get(), key);
  if (!k.get()) return std::nullopt;
  std::string chain(iv);  // updated in place by CNG
  ULONG needed = 0;
  auto call = [&](PUCHAR out, ULONG out_size, ULONG* result) {
    return encrypt ? ::BCryptEncrypt(k.get(), Bytes(data), static_cast<ULONG>(data.size()), nullptr,
                                     reinterpret_cast<PUCHAR>(chain.data()), static_cast<ULONG>(chain.size()), out,
                                     out_size, result, BCRYPT_BLOCK_PADDING)
                   : ::BCryptDecrypt(k.get(), Bytes(data), static_cast<ULONG>(data.size()), nullptr,
                                     reinterpret_cast<PUCHAR>(chain.data()), static_cast<ULONG>(chain.size()), out,
                                     out_size, result, BCRYPT_BLOCK_PADDING);
  };
  if (!BCRYPT_SUCCESS(call(nullptr, 0, &needed))) return std::nullopt;
  chain.assign(iv);
  std::string out(needed, '\0');
  ULONG written = 0;
  if (!BCRYPT_SUCCESS(call(reinterpret_cast<PUCHAR>(out.data()), needed, &written))) return std::nullopt;
  out.resize(written);
  return out;
}

}  // namespace

std::string Base64Encode(std::string_view bytes) {
  DWORD size = 0;
  if (!::CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size)) {
    return {};
  }
  std::string out(size, '\0');
  if (!::CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &size)) {
    return {};
  }
  out.resize(size);
  return out;
}

std::optional<std::string> Base64Decode(std::string_view text) {
  if (text.empty()) return std::string();
  DWORD size = 0;
  if (!::CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &size,
                              nullptr, nullptr)) {
    return std::nullopt;
  }
  std::string out(size, '\0');
  if (!::CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(out.data()), &size, nullptr, nullptr)) {
    return std::nullopt;
  }
  out.resize(size);
  return out;
}

std::optional<std::string> DpapiProtect(std::string_view plain) {
  DATA_BLOB in{static_cast<DWORD>(plain.size()), Bytes(plain)};
  DATA_BLOB out{};
  if (!::CryptProtectData(&in, L"Orbit", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return result;
}

std::optional<std::string> DpapiUnprotect(std::string_view blob) {
  DATA_BLOB in{static_cast<DWORD>(blob.size()), Bytes(blob)};
  DATA_BLOB out{};
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char*>(out.pbData), out.cbData);
  ::SecureZeroMemory(out.pbData, out.cbData);
  ::LocalFree(out.pbData);
  return result;
}

std::optional<std::string> AesGcmDecrypt(std::string_view key, std::string_view nonce, std::string_view sealed) {
  return Gcm(false, key, nonce, sealed);
}

std::optional<std::string> AesGcmEncrypt(std::string_view key, std::string_view nonce, std::string_view plain) {
  return Gcm(true, key, nonce, plain);
}

std::optional<std::string> CbcDecrypt(Cipher cipher, std::string_view key, std::string_view iv,
                                      std::string_view data) {
  return Cbc(false, cipher, key, iv, data);
}

std::optional<std::string> CbcEncrypt(Cipher cipher, std::string_view key, std::string_view iv,
                                      std::string_view data) {
  return Cbc(true, cipher, key, iv, data);
}

std::string Sha1(std::string_view data) {
  Algorithm alg(BCRYPT_SHA1_ALGORITHM);
  std::string out(20, '\0');
  if (!alg.get() || !BCRYPT_SUCCESS(::BCryptHash(alg.get(), nullptr, 0, Bytes(data), static_cast<ULONG>(data.size()),
                                                 reinterpret_cast<PUCHAR>(out.data()), 20))) {
    return {};
  }
  return out;
}

std::optional<std::string> Pbkdf2Sha256(std::string_view password, std::string_view salt, unsigned iterations,
                                        size_t length) {
  Algorithm alg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
  if (!alg.get() || iterations == 0 || length == 0 || length > 1024) return std::nullopt;
  std::string out(length, '\0');
  if (!BCRYPT_SUCCESS(::BCryptDeriveKeyPBKDF2(alg.get(), Bytes(password), static_cast<ULONG>(password.size()),
                                              Bytes(salt), static_cast<ULONG>(salt.size()), iterations,
                                              reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(length), 0))) {
    return std::nullopt;
  }
  return out;
}

std::string RandomBytes(size_t count) {
  std::string out(count, '\0');
  ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(count),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return out;
}

}  // namespace orbit::crypto
