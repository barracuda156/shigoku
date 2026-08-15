// provider.cpp — shared provider guards (P1). Ported from providers.rs.

#include "provider.hpp"

namespace shigoku {

Result<Unit, ProviderError> guard_show_id(std::string_view show_id) {
  if (show_id.empty()) return err(ProviderError::decode("invalid show id"));
  for (const char c : show_id) {
    if (c < '0' || c > '9') return err(ProviderError::decode("invalid show id"));
  }
  return Unit{};
}

bool clean_arg(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x21 || u > 0x7e) return false;
  }
  return true;
}

}  // namespace shigoku
