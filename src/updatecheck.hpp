// updatecheck.hpp — boot-time update check. Ported from sabigoku
// src/updatecheck.rs. Best-effort: compare the built-in version to GitHub's
// latest release tag; every failure (offline, rate-limit, bad body, no cache
// dir) is a silent nullopt. Never blocks startup, never surfaces errors. The
// caller's config (check_for_updates) gates whether to run at all. Wall clock
// and cache dir are parameters; no clock or path resolution in here.
//
// Scope note: sabigoku's update.rs additionally ships a real self-update
// (install-method detection + install.sh spawn). shigoku drops that entirely:
// the legacy target's binary is package-owned (MacPorts) and there is no
// standalone-install story to self-update into. Updates are either the
// package manager's job or a manual rebuild; shigoku only ever reports what
// it finds.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "http.hpp"

namespace shigoku::updatecheck {

// Ambient re-check window; stays under the GitHub unauth rate limit across
// launches. 1 hour.
inline constexpr std::int64_t kCheckTtlSecs = 60 * 60;

struct CacheEntry {
  std::int64_t checked_at = 0;
  std::string latest;
  friend bool operator==(const CacheEntry&, const CacheEntry&) = default;
};

// The latest tag only when strictly newer than `current_version`; nullopt
// otherwise and on every failure. `now` is unix seconds.
[[nodiscard]] std::optional<std::string> check(const http::Client& client,
                                                const std::string& cache_dir,
                                                std::string_view current_version,
                                                std::int64_t now);

// Fresh network tag, bypassing (and refreshing) the cache: `shigoku update`
// reports live rather than acting on an hour-old cached answer.
[[nodiscard]] std::optional<std::string> latest_fresh(const http::Client& client,
                                                       const std::string& cache_dir,
                                                       std::int64_t now);

// Fresh if within TTL and not future-dated: a backward clock or a hand-edited
// cache must not wedge the check forever. Saturating semantics: checked_at is
// untrusted file input, and a plain subtraction on an extreme negative would
// read as permanently fresh.
[[nodiscard]] bool is_fresh(std::int64_t checked_at, std::int64_t now);

// Two-line body: `<checked_at>\n<tag>\n`. The tag line goes through the same
// sanitizer as a network tag: the cache file is untrusted input too.
[[nodiscard]] std::optional<CacheEntry> parse_cache(std::string_view text);

// Tag from the release JSON, through the sanitize_tag gate.
[[nodiscard]] std::optional<std::string> parse_latest_tag(std::string_view body);

}  // namespace shigoku::updatecheck
