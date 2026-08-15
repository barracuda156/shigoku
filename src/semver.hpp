// semver.hpp — version comparison for the update check (06 §6.1). Ported
// from sabigoku src/semver.rs. A plain string compare gets `0.10.0` <
// `0.9.0` wrong; this parses the `major.minor.patch` core numerically.
// Prerelease handling is coarse on purpose: a `-suffix` ranks below the same
// core without one, but two prereleases are not ordered against each other by
// identifier; the nag logic never needs it. Build metadata (`+sha`) is
// ignored entirely.

#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace shigoku::semver {

// A parsed `major.minor.patch` plus prerelease presence (not identifier).
struct Version {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
  bool prerelease = false;

  friend bool operator==(const Version&, const Version&) = default;

  // Core numerically, then a prerelease ranks below the same released core.
  [[nodiscard]] std::strong_ordering order(const Version& other) const;
};

// Parse `[v]MAJOR.MINOR.PATCH[-prerelease][+build]`. Rejects anything without
// exactly three numeric core segments so a garbage remote tag cannot
// masquerade as a version; the caller treats nullopt as "no update".
[[nodiscard]] std::optional<Version> parse_version(std::string_view text);

// True when `latest` is strictly newer than `current`: the one question the
// update check asks. A parse failure on either side yields false; a malformed
// remote tag stays silent instead of nagging.
[[nodiscard]] bool is_newer(std::string_view latest, std::string_view current);

}  // namespace shigoku::semver
