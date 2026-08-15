// semver.cpp — ported from sabigoku src/semver.rs.

#include "semver.hpp"

#include <compare>
#include <string>
#include <tuple>

namespace shigoku::semver {

namespace {

// Parses one ASCII-digit-only segment into a u32. nullopt on an empty or
// non-digit segment (matches Rust's parse_field).
std::optional<std::uint32_t> parse_field(std::string_view seg) {
  if (seg.empty()) return std::nullopt;
  std::uint32_t value = 0;
  for (char ch : seg) {
    if (ch < '0' || ch > '9') return std::nullopt;
    value = value * 10 + static_cast<std::uint32_t>(ch - '0');
  }
  return value;
}

}  // namespace

std::optional<Version> parse_version(std::string_view text) {
  std::string_view s = text;
  if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.remove_prefix(1);

  // Split the core off any prerelease (`-`) or build-metadata (`+`) tail. A
  // `-` before a `+` starts the prerelease; a leading `+` is build only.
  const auto dash = s.find('-');
  const auto plus = s.find('+');
  std::string_view core;
  bool prerelease = false;
  if (dash != std::string_view::npos && (plus == std::string_view::npos || dash < plus)) {
    core = s.substr(0, dash);
    const std::string_view tail = s.substr(dash + 1);
    prerelease = !tail.empty() && tail.front() != '+';
  } else if (plus != std::string_view::npos) {
    core = s.substr(0, plus);
    prerelease = false;
  } else {
    core = s;
    prerelease = false;
  }

  const auto d1 = core.find('.');
  if (d1 == std::string_view::npos) return std::nullopt;
  const auto d2 = core.find('.', d1 + 1);
  if (d2 == std::string_view::npos) return std::nullopt;
  if (core.find('.', d2 + 1) != std::string_view::npos) return std::nullopt;  // >3 segments.

  const auto major = parse_field(core.substr(0, d1));
  const auto minor = parse_field(core.substr(d1 + 1, d2 - d1 - 1));
  const auto patch = parse_field(core.substr(d2 + 1));
  if (!major.has_value() || !minor.has_value() || !patch.has_value()) return std::nullopt;

  return Version{*major, *minor, *patch, prerelease};
}

std::strong_ordering Version::order(const Version& other) const {
  const auto key = [](const Version& v) {
    return std::tuple(v.major, v.minor, v.patch, !v.prerelease);
  };
  return key(*this) <=> key(other);
}

bool is_newer(std::string_view latest, std::string_view current) {
  const auto l = parse_version(latest);
  const auto c = parse_version(current);
  if (!l.has_value() || !c.has_value()) return false;
  return l->order(*c) == std::strong_ordering::greater;
}

}  // namespace shigoku::semver
