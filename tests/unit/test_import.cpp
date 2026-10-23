#include <string>

#include "browser/importer.h"
#include "core/crypto.h"
#include "core/json.h"
#include "core/strings.h"
#include "persistence/database.h"
#include "persistence/schema.h"
#include "persistence/store.h"
#include "test.h"

using namespace orbit;

TEST(Json_ParsesNestedValuesAndEscapes) {
  const auto v = ParseJson(R"({"a":[1,2,{"b":"xé😀\n"}],"t":true,"n":null,"s":"hi"})");
  EXPECT_TRUE(v.has_value());
  if (!v) return;
  EXPECT_TRUE(v->is_object());
  EXPECT_EQ(v->Str("s"), std::string("hi"));
  const JsonValue* a = v->Get("a");
  EXPECT_TRUE(a && a->is_array() && a->array.size() == 3);
  if (a && a->array.size() == 3) EXPECT_EQ(a->array[2].Str("b"), std::string("x\xC3\xA9\xF0\x9F\x98\x80\n"));
}

TEST(Json_RejectsMalformedAndTooDeep) {
  EXPECT_FALSE(ParseJson("{\"a\":}").has_value());
  EXPECT_FALSE(ParseJson("[1,2").has_value());
  EXPECT_FALSE(ParseJson("{} trailing").has_value());
  EXPECT_FALSE(ParseJson(std::string(200, '[') + std::string(200, ']')).has_value());
  EXPECT_TRUE(ParseJson("\xEF\xBB\xBF[1]").has_value());  // a BOM is tolerated
}

TEST(Crypto_Base64AndDpapiRoundTrip) {
  const std::string bytes("\x00\x01\xFE\xFFhello", 9);
  const auto decoded = crypto::Base64Decode(crypto::Base64Encode(bytes));
  EXPECT_TRUE(decoded.has_value() && *decoded == bytes);
  const auto sealed = crypto::DpapiProtect("s3cret");
  EXPECT_TRUE(sealed.has_value() && *sealed != "s3cret");
  if (sealed) EXPECT_EQ(crypto::DpapiUnprotect(*sealed).value_or(""), std::string("s3cret"));
}

TEST(Crypto_AesGcmRoundTripAndTamper) {
  const std::string key(32, 'k'), nonce(12, 'n');
  const auto sealed = crypto::AesGcmEncrypt(key, nonce, "password");
  EXPECT_TRUE(sealed.has_value());
  if (!sealed) return;
  EXPECT_EQ(crypto::AesGcmDecrypt(key, nonce, *sealed).value_or(""), std::string("password"));
  std::string bad = *sealed;
  bad[0] ^= 1;
  EXPECT_FALSE(crypto::AesGcmDecrypt(key, nonce, bad).has_value());  // the tag catches it
}

TEST(Crypto_CbcRoundTripsForAesAndTripleDes) {
  const std::string aes_key(32, 'a'), aes_iv(16, 'i'), des_key(24, 'd'), des_iv(8, 'v');
  const auto a = crypto::CbcEncrypt(crypto::Cipher::kAes, aes_key, aes_iv, "twelve bytes");
  EXPECT_TRUE(a.has_value());
  if (a) EXPECT_EQ(crypto::CbcDecrypt(crypto::Cipher::kAes, aes_key, aes_iv, *a).value_or(""), std::string("twelve bytes"));
  const auto d = crypto::CbcEncrypt(crypto::Cipher::kTripleDes, des_key, des_iv, "legacy login");
  EXPECT_TRUE(d.has_value());
  if (d) EXPECT_EQ(crypto::CbcDecrypt(crypto::Cipher::kTripleDes, des_key, des_iv, *d).value_or(""), std::string("legacy login"));
}

TEST(Import_DecryptsChromiumV10AndLegacyValues) {
  const std::string key(32, 'K'), nonce(12, 'N');
  const auto sealed = crypto::AesGcmEncrypt(key, nonce, "hunter2");
  EXPECT_TRUE(sealed.has_value());
  if (sealed) EXPECT_WEQ(importer::DecryptChromiumValue("v10" + nonce + *sealed, key), L"hunter2");
  // Older profiles: the value is a DPAPI blob itself.
  const auto legacy = crypto::DpapiProtect("oldpass");
  if (legacy) EXPECT_WEQ(importer::DecryptChromiumValue(*legacy, ""), L"oldpass");
  EXPECT_WEQ(importer::DecryptChromiumValue("v10" + nonce + "garbage-garbage-garbage", key), L"");
}

TEST(Import_UnwrapsTheChromiumKeyFromLocalState) {
  const std::string aes(32, 'Z');
  const auto wrapped = crypto::DpapiProtect(aes);
  EXPECT_TRUE(wrapped.has_value());
  if (!wrapped) return;
  const std::string json = R"({"os_crypt":{"encrypted_key":")" + crypto::Base64Encode("DPAPI" + *wrapped) + R"("}})";
  EXPECT_TRUE(importer::ChromiumKeyFromLocalState(json) == aes);
  EXPECT_TRUE(importer::ChromiumKeyFromLocalState("{}").empty());
}

TEST(Store_PasswordsUpsertFindAndCascade) {
  Database db;
  EXPECT_TRUE(db.Open({}));
  EXPECT_TRUE(MigrateSchema(db));
  Store store(db);
  auto p = store.CreateProfile(L"u", L"P", 0, L"");
  PasswordRecord r;
  r.profile_id = p->id;
  r.origin = L"https://example.com";
  r.username = L"me";
  r.secret = L"c2VjcmV0MQ==";
  const int64_t id = store.UpsertPassword(r);
  EXPECT_TRUE(id != 0);
  r.secret = L"c2VjcmV0Mg==";  // same (origin, username): replaced, not duplicated
  EXPECT_EQ(store.UpsertPassword(r), id);
  EXPECT_EQ(store.PasswordCount(p->id), 1);
  const auto found = store.FindPasswords(p->id, L"https://example.com");
  EXPECT_EQ(found.size(), size_t{1});
  if (!found.empty()) EXPECT_WEQ(found[0].secret, L"c2VjcmV0Mg==");
  EXPECT_TRUE(store.FindPasswords(p->id, L"https://other.example").empty());
  EXPECT_TRUE(store.DeleteProfile(p->id));  // the profile's logins go with it
  EXPECT_EQ(store.PasswordCount(p->id), 0);
}
