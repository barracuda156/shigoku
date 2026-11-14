// crypto.hpp — the one place libcrypto is called: AES-256-GCM open/seal,
// AES-CBC open/seal, SHA-256, PBKDF2 and base64 (standard + URL-safe), for
// providers whose playlists or stream envelopes arrive encrypted (senshi,
// megaplay, reanime).
// Byte-wise in and out; nothing here packs bytes into wider ints (§3
// endianness), so it is identical big- and little-endian.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku::crypto {

inline constexpr std::size_t kAesGcmKeyLen = 32;
inline constexpr std::size_t kAesGcmIvLen = 12;
inline constexpr std::size_t kAesGcmTagLen = 16;

// Decrypt `sealed` = ciphertext ‖ 16-byte tag under a 32-byte key and a
// 12-byte iv, no AAD. nullopt on any size mismatch or a failed tag check —
// never a partial plaintext.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> aes256gcm_open(
    const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& iv,
    const std::uint8_t* sealed, std::size_t sealed_len);

// The inverse (ciphertext ‖ tag), for tests and fixtures.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> aes256gcm_seal(
    const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& iv,
    const std::uint8_t* plain, std::size_t plain_len);

// Standard base64 (RFC 4648 alphabet, padding optional, ASCII whitespace
// skipped) -> bytes; nullopt on any other byte or a dangling quantum.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view s);
[[nodiscard]] std::string base64_encode(const std::uint8_t* bytes, std::size_t len);

// The encrypted-playlist envelope: base64( iv[12] ‖ ciphertext ‖ tag[16] )
// under `key`. nullopt when the base64, the size, or the tag fails.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> open_b64_gcm(
    std::string_view b64, const std::vector<std::uint8_t>& key);

inline constexpr std::size_t kAesCbcKeyLen = 32;
inline constexpr std::size_t kAesCbcIvLen = 16;

// Decrypt `ct` (PKCS#7-padded, a non-zero multiple of 16 bytes) under a
// 32-byte key and a 16-byte iv. nullopt on any size mismatch or a bad pad —
// never a partial plaintext (megaplay's `enc` stream envelope).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> aes256cbc_open(
    const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& iv,
    const std::uint8_t* ct, std::size_t ct_len);

// The inverse (PKCS#7 pad + encrypt), for tests and fixtures.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> aes256cbc_seal(
    const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& iv,
    const std::uint8_t* plain, std::size_t plain_len);

// URL-safe base64 (RFC 4648 §5, the '-'/'_' alphabet), padding optional,
// ASCII whitespace skipped -> bytes; nullopt on any other byte (including the
// standard alphabet's '+'/'/') or a dangling quantum.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64url_decode(std::string_view s);

// base64url(ciphertext) opened under CBC with a caller-supplied iv (megaplay's
// `enc` shape: no iv prefix, the iv is the site's fixed literal — unlike
// open_b64_gcm's iv-in-blob). nullopt when the base64 or the CBC open fails.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> open_b64url_cbc(
    std::string_view b64url, const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& iv);

// AES-128-CBC open with PKCS#7, the HLS `METHOD=AES-128` segment cipher.
// Same contract as aes256cbc_open with a 16-byte key.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> aes128cbc_open(
    const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& iv,
    const std::uint8_t* ct, std::size_t ct_len);

inline constexpr std::size_t kSha256Len = 32;

// SHA-256 digest; nullopt only when libcrypto itself fails.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> sha256(const std::uint8_t* data,
                                                              std::size_t len);

// Lowercase hex SHA-256 of `s` — WebCrypto's digest as the page's JS
// renders it. Empty only when libcrypto fails.
[[nodiscard]] std::string sha256_hex(std::string_view s);

// PBKDF2-HMAC-SHA256 over a binary password. nullopt on a zero iteration
// count, an empty or oversize output, or a libcrypto failure.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> pbkdf2_sha256(
    const std::vector<std::uint8_t>& password, std::string_view salt,
    std::uint32_t iterations, std::size_t out_len);

}  // namespace shigoku::crypto
