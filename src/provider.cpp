// provider.cpp — shared provider guards (P1). Ported from providers.rs.

#include "provider.hpp"

#include <algorithm>

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

std::vector<std::unique_ptr<StreamProvider>> drop_disabled(
    std::vector<std::unique_ptr<StreamProvider>> providers,
    const std::vector<std::string>& disabled) {
  auto off = [&](const std::unique_ptr<StreamProvider>& p) {
    return std::find(disabled.begin(), disabled.end(), p->name()) != disabled.end();
  };
  if (std::all_of(providers.begin(), providers.end(), off)) return providers;
  providers.erase(std::remove_if(providers.begin(), providers.end(), off), providers.end());
  return providers;
}

}  // namespace shigoku
