// crypto_tests.cpp — base64 vectors, AES-256-GCM seal/open, the envelope.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../src/crypto.hpp"

using namespace shigoku::crypto;

namespace {

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string text(const std::vector<std::uint8_t>& v) { return {v.begin(), v.end()}; }
std::vector<std::uint8_t> key32() { return std::vector<std::uint8_t>(32, 0x42); }
std::vector<std::uint8_t> iv12() { return std::vector<std::uint8_t>(12, 0x01); }
std::vector<std::uint8_t> iv16() { return std::vector<std::uint8_t>(16, 0x02); }

// megaplay's baked envelope pair: the 16-byte key
// literal zero-padded to 32, and the 16-byte iv literal as-is.
std::vector<std::uint8_t> megaplay_key() {
  const std::string_view lit = "i?LMTAx0Q6,:}50U";
  std::vector<std::uint8_t> out(32, 0);
  std::copy(lit.begin(), lit.end(), out.begin());
  return out;
}
std::vector<std::uint8_t> megaplay_iv() {
  const std::string_view lit = "W0;27ToaUpl_P%'c";
  return {lit.begin(), lit.end()};
}

}  // namespace

TEST_CASE("base64 round-trips the RFC 4648 vectors") {
  const std::pair<const char*, const char*> cases[] = {
      {"", ""},           {"f", "Zg=="},       {"fo", "Zm8="},        {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
  };
  for (const auto& [plain, enc] : cases) {
    const auto b = bytes(plain);
    CHECK(base64_encode(b.data(), b.size()) == enc);
    auto d = base64_decode(enc);
    REQUIRE(d.has_value());
    CHECK(text(*d) == plain);
  }
}

TEST_CASE("base64 decode tolerates missing padding and whitespace, rejects junk") {
  auto unpadded = base64_decode("Zm9vYg");
  REQUIRE(unpadded.has_value());
  CHECK(text(*unpadded) == "foob");
  auto spaced = base64_decode("Zm9v\nYmFy ");
  REQUIRE(spaced.has_value());
  CHECK(text(*spaced) == "foobar");
  CHECK(!base64_decode("Zm9v*").has_value());
  CHECK(!base64_decode("Z").has_value());       // a lone sextet.
  CHECK(!base64_decode("Zg==Zg").has_value());  // data after padding.
}

TEST_CASE("aes256gcm seal/open round-trips and the tag catches tampering") {
  const std::string plain = "#EXTM3U\n#EXT-X-VERSION:3\nvideo/1080/playlist.txt\n";
  auto sealed = aes256gcm_seal(key32(), iv12(), reinterpret_cast<const std::uint8_t*>(plain.data()),
                               plain.size());
  REQUIRE(sealed.has_value());
  CHECK(sealed->size() == plain.size() + kAesGcmTagLen);
  auto opened = aes256gcm_open(key32(), iv12(), sealed->data(), sealed->size());
  REQUIRE(opened.has_value());
  CHECK(text(*opened) == plain);

  auto tampered = *sealed;
  tampered[3] ^= 0x80;
  CHECK(!aes256gcm_open(key32(), iv12(), tampered.data(), tampered.size()).has_value());
  auto wrong_key = key32();
  wrong_key[0] ^= 1;
  CHECK(!aes256gcm_open(wrong_key, iv12(), sealed->data(), sealed->size()).has_value());
  // Empty plaintext still authenticates.
  auto empty = aes256gcm_seal(key32(), iv12(), nullptr, 0);
  REQUIRE(empty.has_value());
  CHECK(empty->size() == kAesGcmTagLen);
  auto opened_empty = aes256gcm_open(key32(), iv12(), empty->data(), empty->size());
  REQUIRE(opened_empty.has_value());
  CHECK(opened_empty->empty());
}

TEST_CASE("aes256gcm rejects wrong key/iv/blob sizes outright") {
  const std::uint8_t blob[16] = {};
  CHECK(!aes256gcm_open(std::vector<std::uint8_t>(16, 0), iv12(), blob, sizeof(blob)).has_value());
  CHECK(!aes256gcm_open(key32(), std::vector<std::uint8_t>(16, 0), blob, sizeof(blob)).has_value());
  CHECK(!aes256gcm_open(key32(), iv12(), blob, 15).has_value());
  CHECK(!aes256gcm_seal(std::vector<std::uint8_t>(31, 0), iv12(), blob, 4).has_value());
}

TEST_CASE("open_b64_gcm opens iv-prefixed envelopes and refuses short or bad ones") {
  const std::string plain = "#EXTM3U\n";
  auto sealed = aes256gcm_seal(key32(), iv12(), reinterpret_cast<const std::uint8_t*>(plain.data()),
                               plain.size());
  REQUIRE(sealed.has_value());
  std::vector<std::uint8_t> env = iv12();
  env.insert(env.end(), sealed->begin(), sealed->end());
  const std::string b64 = base64_encode(env.data(), env.size());
  auto opened = open_b64_gcm(b64, key32());
  REQUIRE(opened.has_value());
  CHECK(text(*opened) == plain);
  CHECK(!open_b64_gcm("AAAA", key32()).has_value());        // 3 bytes: no iv+tag.
  CHECK(!open_b64_gcm("not base64!", key32()).has_value());
}

TEST_CASE("aes256cbc seal/open round-trips and a tampered last block breaks the pad") {
  const std::string plain = R"({"file":"https://cdn.example/x/master.m3u8"})";
  auto sealed = aes256cbc_seal(key32(), iv16(), reinterpret_cast<const std::uint8_t*>(plain.data()),
                               plain.size());
  REQUIRE(sealed.has_value());
  CHECK(sealed->size() % 16 == 0);
  CHECK(sealed->size() > plain.size());  // PKCS#7 always adds a full pad block on an exact fit.
  auto opened = aes256cbc_open(key32(), iv16(), sealed->data(), sealed->size());
  REQUIRE(opened.has_value());
  CHECK(text(*opened) == plain);

  auto tampered = *sealed;
  tampered.back() ^= 0x01;  // corrupts the PKCS#7 pad byte.
  CHECK(!aes256cbc_open(key32(), iv16(), tampered.data(), tampered.size()).has_value());

  // A short plaintext still round-trips (one full pad block).
  const std::string tiny = "x";
  auto sealed_tiny = aes256cbc_seal(key32(), iv16(), reinterpret_cast<const std::uint8_t*>(tiny.data()),
                                    tiny.size());
  REQUIRE(sealed_tiny.has_value());
  CHECK(sealed_tiny->size() == 16);
  auto opened_tiny = aes256cbc_open(key32(), iv16(), sealed_tiny->data(), sealed_tiny->size());
  REQUIRE(opened_tiny.has_value());
  CHECK(text(*opened_tiny) == tiny);
}

TEST_CASE("aes256cbc rejects wrong key/iv sizes and a ct that is not a non-zero multiple of 16") {
  const std::uint8_t block[16] = {};
  CHECK(!aes256cbc_open(std::vector<std::uint8_t>(16, 0), iv16(), block, sizeof(block)).has_value());
  CHECK(!aes256cbc_open(key32(), std::vector<std::uint8_t>(12, 0), block, sizeof(block)).has_value());
  CHECK(!aes256cbc_open(key32(), iv16(), block, 0).has_value());   // an empty ct.
  CHECK(!aes256cbc_open(key32(), iv16(), block, 15).has_value());  // not a block multiple.
  CHECK(!aes256cbc_open(key32(), iv16(), block, 17).has_value());
  CHECK(!aes256cbc_seal(std::vector<std::uint8_t>(31, 0), iv16(), block, 4).has_value());
}

TEST_CASE("base64url decodes the -_ alphabet padding-optional, rejects the standard +/ alphabet") {
  auto d = base64url_decode("Zm9v");
  REQUIRE(d.has_value());
  CHECK(text(*d) == "foo");

  // Bytes chosen so the standard alphabet emits '+' and '/'; base64url must
  // use '-'/'_' instead, and must not accept the standard-alphabet string.
  const std::vector<std::uint8_t> src = {0xff, 0xef, 0xbe, 0xff, 0xff};
  CHECK(base64_encode(src.data(), src.size()) == "/+++//8=");
  auto url = base64url_decode("_---__8=");
  REQUIRE(url.has_value());
  CHECK(*url == src);
  auto url_unpadded = base64url_decode("_---__8");
  REQUIRE(url_unpadded.has_value());
  CHECK(*url_unpadded == src);
  CHECK(!base64url_decode("/+++//8=").has_value());  // the standard alphabet is refused.
  CHECK(!base64url_decode("_---__8=x").has_value());  // a lone trailing sextet.
}

TEST_CASE("open_b64url_cbc opens the megaplay enc envelope over a captured live blob") {
  // Live-captured `enc` value (One Piece 21, ep1 sub), decrypted under the
  // site's baked key/iv literal; verified against the openssl CLI.
  const std::string enc =
      "wdeBruh3qqn_i5wUNnyaPcXqidp1UWP84FfPHzGyKXA2hDZBfMCmZ4FLvs7_pQuH549Eptax8UOjAJyRIZfrRhUGUKy9O"
      "BeGh2yB_-m_JLAlLnWTLzYZC3_C5I4ltveYoiaU66Do9RgI9bCetmk_o87-sd66brXnWV1MbjhLnjw=";
  auto opened = open_b64url_cbc(enc, megaplay_key(), megaplay_iv());
  REQUIRE(opened.has_value());
  CHECK(text(*opened) ==
        R"({"file":"https://fetch.nexabloom.top/anime/f899139df5e1059396431415e770c6dd/)"
        R"(61b87186ab260d05003427e16ccf5657/master.m3u8"})");

  CHECK(!open_b64url_cbc("", megaplay_key(), megaplay_iv()).has_value());
  CHECK(!open_b64url_cbc("not base64url!!", megaplay_key(), megaplay_iv()).has_value());
  auto wrong_key = megaplay_key();
  wrong_key[0] ^= 1;
  CHECK(!open_b64url_cbc(enc, wrong_key, megaplay_iv()).has_value());
}
