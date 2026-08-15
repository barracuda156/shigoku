// nonce.cpp — 128-bit hex from /dev/urandom (P12). Ported 1:1 from nonce.rs.

#include "nonce.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace shigoku::nonce {

Result<std::string, std::string> mint() {
  unsigned char bytes[16];
  std::FILE* f = std::fopen("/dev/urandom", "rb");
  if (f == nullptr) {
    return err(std::string("open /dev/urandom: ") + std::strerror(errno));
  }
  const std::size_t got = std::fread(bytes, 1, sizeof(bytes), f);
  std::fclose(f);
  if (got != sizeof(bytes)) {
    return err(std::string("short read from /dev/urandom"));
  }
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (unsigned char b : bytes) {
    out.push_back(kHex[(b >> 4) & 0x0f]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

}  // namespace shigoku::nonce
