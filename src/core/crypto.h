#pragma once

#include <optional>
#include <string>
#include <string_view>

// Small wrappers over Windows CNG (bcrypt) and DPAPI, as used by the
// password store and the importers. Every byte string is a std::string.
namespace orbit::crypto {

std::string Base64Encode(std::string_view bytes);
// Whitespace is ignored; nullopt on anything else that is not Base64.
std::optional<std::string> Base64Decode(std::string_view text);

// DPAPI, bound to the current Windows user (what Chromium and Orbit use).
std::optional<std::string> DpapiProtect(std::string_view plain);
std::optional<std::string> DpapiUnprotect(std::string_view blob);

// AES-256-GCM; |sealed| is the ciphertext followed by its 16-byte tag.
std::optional<std::string> AesGcmDecrypt(std::string_view key, std::string_view nonce, std::string_view sealed);
std::optional<std::string> AesGcmEncrypt(std::string_view key, std::string_view nonce, std::string_view plain);

// CBC with PKCS#7 padding: AES (16/24/32-byte keys) or 3DES (24-byte key).
enum class Cipher { kAes, kTripleDes };
std::optional<std::string> CbcDecrypt(Cipher cipher, std::string_view key, std::string_view iv,
                                      std::string_view data);
std::optional<std::string> CbcEncrypt(Cipher cipher, std::string_view key, std::string_view iv,
                                      std::string_view data);

std::string Sha1(std::string_view data);
std::optional<std::string> Pbkdf2Sha256(std::string_view password, std::string_view salt, unsigned iterations,
                                        size_t length);
std::string RandomBytes(size_t count);

}  // namespace orbit::crypto
