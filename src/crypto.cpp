// crypto.cpp — see crypto.hpp.

#include "crypto.hpp"

#include <openssl/evp.h>

#include <limits>

namespace shigoku::crypto {

namespace {

struct CtxGuard {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  ~CtxGuard() {
    if (ctx != nullptr) EVP_CIPHER_CTX_free(ctx);
  }
};

// A GCM payload beyond this is not a playlist; keeps every length an int.
constexpr std::size_t kMaxGcmPayload = std::size_t{1} << 30;

}  // namespace

std::optional<std::vector<std::uint8_t>> aes256gcm_open(const std::vector<std::uint8_t>& key,
                                                        const std::vector<std::uint8_t>& iv,
                                                        const std::uint8_t* sealed,
                                                        std::size_t sealed_len) {
  if (key.size() != kAesGcmKeyLen || iv.size() != kAesGcmIvLen || sealed_len < kAesGcmTagLen ||
      sealed_len > kMaxGcmPayload) {
    return std::nullopt;
  }
  CtxGuard g;
  if (g.ctx == nullptr) return std::nullopt;
  if (EVP_DecryptInit_ex(g.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) return std::nullopt;
  if (EVP_CIPHER_CTX_ctrl(g.ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kAesGcmIvLen), nullptr) != 1) {
    return std::nullopt;
  }
  if (EVP_DecryptInit_ex(g.ctx, nullptr, nullptr, key.data(), iv.data()) != 1) return std::nullopt;
  const std::size_t ct_len = sealed_len - kAesGcmTagLen;
  std::vector<std::uint8_t> out(ct_len + 16);
  int len = 0;
  int total = 0;
  if (ct_len > 0) {
    if (EVP_DecryptUpdate(g.ctx, out.data(), &len, sealed, static_cast<int>(ct_len)) != 1) {
      return std::nullopt;
    }
    total = len;
  }
  std::vector<std::uint8_t> tag(sealed + ct_len, sealed + sealed_len);
  if (EVP_CIPHER_CTX_ctrl(g.ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kAesGcmTagLen), tag.data()) != 1) {
    return std::nullopt;
  }
  if (EVP_DecryptFinal_ex(g.ctx, out.data() + total, &len) != 1) return std::nullopt;  // tag mismatch.
  total += len;
  out.resize(static_cast<std::size_t>(total));
  return out;
}

std::optional<std::vector<std::uint8_t>> aes256gcm_seal(const std::vector<std::uint8_t>& key,
                                                        const std::vector<std::uint8_t>& iv,
                                                        const std::uint8_t* plain,
                                                        std::size_t plain_len) {
  if (key.size() != kAesGcmKeyLen || iv.size() != kAesGcmIvLen || plain_len > kMaxGcmPayload) {
    return std::nullopt;
  }
  CtxGuard g;
  if (g.ctx == nullptr) return std::nullopt;
  if (EVP_EncryptInit_ex(g.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) return std::nullopt;
  if (EVP_CIPHER_CTX_ctrl(g.ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kAesGcmIvLen), nullptr) != 1) {
    return std::nullopt;
  }
  if (EVP_EncryptInit_ex(g.ctx, nullptr, nullptr, key.data(), iv.data()) != 1) return std::nullopt;
  std::vector<std::uint8_t> out(plain_len + 16 + kAesGcmTagLen);
  int len = 0;
  int total = 0;
  if (plain_len > 0) {
    if (EVP_EncryptUpdate(g.ctx, out.data(), &len, plain, static_cast<int>(plain_len)) != 1) {
      return std::nullopt;
    }
    total = len;
  }
  if (EVP_EncryptFinal_ex(g.ctx, out.data() + total, &len) != 1) return std::nullopt;
  total += len;
  if (EVP_CIPHER_CTX_ctrl(g.ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kAesGcmTagLen),
                          out.data() + total) != 1) {
    return std::nullopt;
  }
  total += static_cast<int>(kAesGcmTagLen);
  out.resize(static_cast<std::size_t>(total));
  return out;
}

namespace {

int b64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

constexpr const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view s) {
  std::vector<std::uint8_t> out;
  out.reserve(s.size() / 4 * 3 + 3);
  std::uint32_t acc = 0;
  int bits = 0;
  bool padded = false;
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    if (c == '=') {
      padded = true;
      continue;
    }
    if (padded) return std::nullopt;  // data after padding.
    const int v = b64_value(c);
    if (v < 0) return std::nullopt;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
    }
  }
  // A single leftover sextet cannot encode a byte: a truncated quantum.
  if (bits == 6) return std::nullopt;
  return out;
}

std::string base64_encode(const std::uint8_t* bytes, std::size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  std::size_t i = 0;
  while (i + 3 <= len) {
    const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                            (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                            static_cast<std::uint32_t>(bytes[i + 2]);
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(kB64[(n >> 6) & 63]);
    out.push_back(kB64[n & 63]);
    i += 3;
  }
  if (i + 1 == len) {
    const std::uint32_t n = static_cast<std::uint32_t>(bytes[i]) << 16;
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out += "==";
  } else if (i + 2 == len) {
    const std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                            (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(kB64[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> open_b64_gcm(std::string_view b64,
                                                      const std::vector<std::uint8_t>& key) {
  auto blob = base64_decode(b64);
  if (!blob.has_value() || blob->size() < kAesGcmIvLen + kAesGcmTagLen) return std::nullopt;
  const std::vector<std::uint8_t> iv(blob->begin(), blob->begin() + kAesGcmIvLen);
  return aes256gcm_open(key, iv, blob->data() + kAesGcmIvLen, blob->size() - kAesGcmIvLen);
}

}  // namespace shigoku::crypto
