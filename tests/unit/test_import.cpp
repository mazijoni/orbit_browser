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

// Passwords files (CSV) exported from browsers and password managers.
TEST(PasswordCsv_ChromeFormat) {
  const auto r = importer::ParsePasswordCsv(
      "name,url,username,password,note\r\n"
      "example.com,https://example.com/login,alice,s3cret,\r\n"
      "\"Quoted, name\",https://Shop.Example.org:8443/a?b=c,\"bob \"\"the\"\" builder\",\"p,a\"\"ss\",\"multi\nline note\"\r\n"
      "app,android://AAAA@com.example.app/,carol,x,\r\n"
      "empty,https://nopass.example/,dave,,\r\n");
  EXPECT_TRUE(r.recognized);
  EXPECT_EQ(r.logins.size(), static_cast<size_t>(2));
  EXPECT_EQ(r.skipped, 2);
  if (r.logins.size() == 2) {
    EXPECT_EQ(r.logins[0].origin, std::wstring(L"https://example.com"));
    EXPECT_EQ(r.logins[0].username, std::wstring(L"alice"));
    EXPECT_EQ(r.logins[0].password, std::wstring(L"s3cret"));
    EXPECT_EQ(r.logins[1].origin, std::wstring(L"https://shop.example.org:8443"));
    EXPECT_EQ(r.logins[1].username, std::wstring(L"bob \"the\" builder"));
    EXPECT_EQ(r.logins[1].password, std::wstring(L"p,a\"ss"));
  }
}

TEST(PasswordCsv_FirefoxBitwardenAndOthers) {
  const auto firefox = importer::ParsePasswordCsv(
      "\xEF\xBB\xBF\"url\",\"username\",\"password\",\"httpRealm\",\"formActionOrigin\",\"guid\",\"timeCreated\"\n"
      "\"https://www.mozilla.org\",\"fox\",\"r3d\",,\"https://www.mozilla.org\",\"{x}\",\"1\"\n");
  EXPECT_TRUE(firefox.recognized && firefox.logins.size() == 1);
  if (!firefox.logins.empty()) EXPECT_EQ(firefox.logins[0].username, std::wstring(L"fox"));

  const auto bitwarden = importer::ParsePasswordCsv(
      "folder,favorite,type,name,notes,fields,reprompt,login_uri,login_username,login_password,login_totp\n"
      ",,login,Site,,,0,https://bw.example.com/,warden,b1t,\n"
      ",,note,A note,secret text,,0,,,,\n");
  EXPECT_TRUE(bitwarden.recognized && bitwarden.logins.size() == 1 && bitwarden.skipped == 1);
  if (!bitwarden.logins.empty()) EXPECT_EQ(bitwarden.logins[0].password, std::wstring(L"b1t"));

  // KeePass-style: other column names, a bare domain, semicolons.
  const auto keepass = importer::ParsePasswordCsv(
      "\"Account\";\"Login Name\";\"Password\";\"Web Site\";\"Comments\"\n"
      "\"Mail\";\"kee\";\"p4ss\";\"mail.example.net\";\"\"\n");
  EXPECT_TRUE(keepass.recognized && keepass.logins.size() == 1);
  if (!keepass.logins.empty()) {
    EXPECT_EQ(keepass.logins[0].origin, std::wstring(L"https://mail.example.net"));
    EXPECT_EQ(keepass.logins[0].username, std::wstring(L"kee"));
  }

  EXPECT_FALSE(importer::ParsePasswordCsv("title,notes\nx,y\n").recognized);
  EXPECT_FALSE(importer::ParsePasswordCsv("").recognized);
}
