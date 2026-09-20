// crypto.hpp — the one place libcrypto is called: AES-256-GCM open/seal and
// base64, for providers whose playlists arrive encrypted (senshi). Byte-wise
// in and out; nothing here packs bytes into wider ints (§3 endianness), so it
// is identical big- and little-endian.
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

}  // namespace shigoku::crypto
