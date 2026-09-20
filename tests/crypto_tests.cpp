// crypto_tests.cpp — base64 vectors, AES-256-GCM seal/open, the envelope.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../src/crypto.hpp"

using namespace shigoku::crypto;

namespace {

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string text(const std::vector<std::uint8_t>& v) { return {v.begin(), v.end()}; }
std::vector<std::uint8_t> key32() { return std::vector<std::uint8_t>(32, 0x42); }
std::vector<std::uint8_t> iv12() { return std::vector<std::uint8_t>(12, 0x01); }

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
