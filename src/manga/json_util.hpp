// json_util.hpp — JSON field reads the manga sources share. Header-only, for
// the source .cpp files that already parse with nlohmann: no header includes
// it, so nothing else pays for the JSON header.

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace shigoku::manga::detail {

// Absent/null/wrong-typed string field degrades to "".
[[nodiscard]] inline std::string str_or_empty(const nlohmann::json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || j.at(key).is_null()) return {};
  const nlohmann::json& v = j.at(key);
  if (!v.is_string()) return {};
  return v.get<std::string>();
}

}  // namespace shigoku::manga::detail
