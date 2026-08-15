// updatecheck.cpp — ported from sabigoku src/updatecheck.rs.

#include "updatecheck.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>

#include <nlohmann/json.hpp>

#include "debug_log.hpp"
#include "semver.hpp"

namespace shigoku::updatecheck {

namespace {

using json = nlohmann::json;

// GitHub rejects requests without a User-Agent (403).
constexpr const char* kUserAgent = "shigoku-update-check";

// Cap on the one GET. Without it a silent host hangs a teardown drain.
constexpr long kFetchTimeoutSecs = 3;

// `/releases/latest` skips prereleases and drafts by definition. shigoku's
// own repo, not zigoku's/sabigoku's (06 §6.1: "sabigoku points at its GitHub
// repo" — same rule applies one level down).
constexpr const char* kLatestUrl =
    "https://api.github.com/repos/vantroy/shigoku/releases/latest";

constexpr const char* kCacheFile = "update_check";

// Cap on the response body. The release JSON is a few KB; a host that can
// answer as api.github.com (DNS/proxy tampering) must not be able to force an
// arbitrarily large allocation inside the fetch deadline.
constexpr std::size_t kBodyCap = 64 * 1024;

// Bounded like the network body (4096, matching updatecheck.rs's read cap); a
// bloated file truncates into a parse that fails or a tag the cap trims.
constexpr std::size_t kCacheReadCap = 4096;

std::string cache_path(const std::string& cache_dir) { return cache_dir + "/" + kCacheFile; }

// Printable ASCII, capped at 64; nullopt when nothing survives. The one gate
// both tag sources (release JSON, cache file) pass through, so no consumer
// can be fed terminal escapes or an unbounded string.
std::optional<std::string> sanitize_tag(std::string_view raw) {
  std::string clean;
  for (char c : raw) {
    if (c >= ' ' && c <= '~') clean.push_back(c);
    if (clean.size() == 64) break;
  }
  if (clean.empty()) return std::nullopt;
  return clean;
}

// nullopt on any problem: a bad cache is no cache, so the check re-fetches.
std::optional<CacheEntry> read_cache(const std::string& cache_dir) {
  std::ifstream f(cache_path(cache_dir), std::ios::binary);
  if (!f.is_open()) return std::nullopt;
  std::string text(kCacheReadCap, '\0');
  f.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(f.gcount()));
  return parse_cache(text);
}

// Best-effort; a failure means the next launch re-checks. Write-then-rename:
// a concurrent boot must never read a truncated half-write, and teardown
// abandoning this worker mid-write must leave the old cache, not a torn one.
void write_cache(const std::string& cache_dir, std::int64_t now, std::string_view latest) {
  std::error_code ec;
  std::filesystem::create_directories(cache_dir, ec);  // best-effort, matches Rust's create_dir_all.
  const std::string tmp = cache_dir + "/" + kCacheFile + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
      debug_log("update check: cache write failed (open)");
      return;
    }
    f << now << "\n" << latest << "\n";
    if (!f.good()) {
      debug_log("update check: cache write failed (write)");
      return;
    }
  }
  if (std::rename(tmp.c_str(), cache_path(cache_dir).c_str()) != 0) {
    debug_log("update check: cache write failed (rename)");
  }
}

// Test seam: lets the suite redirect the fetch at a local fixture without a
// real GitHub hit. Read once per call (no caching) — this is boot-path code,
// not a hot loop.
std::optional<std::string> url_override(const char* var) {
  const char* v = std::getenv(var);
  if (v == nullptr || v[0] == '\0') return std::nullopt;
  return std::string(v);
}

std::optional<std::string> fetch_latest(const http::Client& client) {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url_override("SHIGOKU_UPDATE_URL").value_or(kLatestUrl);
  req.user_agent = kUserAgent;
  req.accept = http::Accept::Any2xx;
  req.timeout_secs = kFetchTimeoutSecs;

  auto resp = client.fetch(req);
  if (!resp.has_value()) {
    debug_log("update check: fetch failed");
    return std::nullopt;
  }
  if (resp->size() > kBodyCap) return std::nullopt;
  const std::string_view body(reinterpret_cast<const char*>(resp->data()), resp->size());
  return parse_latest_tag(body);
}

std::optional<std::string> resolve_latest(const http::Client& client, const std::string& cache_dir,
                                           std::int64_t now) {
  if (auto entry = read_cache(cache_dir); entry.has_value() && is_fresh(entry->checked_at, now)) {
    return entry->latest;
  }
  auto latest = fetch_latest(client);
  if (latest.has_value()) write_cache(cache_dir, now, *latest);
  return latest;
}

}  // namespace

bool is_fresh(std::int64_t checked_at, std::int64_t now) {
  if (checked_at > now) return false;  // future-dated: always re-check.
  // Saturating subtraction (updatecheck.rs's now.saturating_sub(checked_at)):
  // an adversarial/corrupt checked_at near INT64_MIN must not wrap a plain
  // signed subtraction into a small/negative delta that reads as fresh.
  // Unsigned math is well-defined on overflow, so compute the gap there and
  // clamp back down before comparing against the (small, positive) TTL.
  const auto gap = static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(checked_at);
  const std::int64_t delta = gap > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                                  ? std::numeric_limits<std::int64_t>::max()
                                  : static_cast<std::int64_t>(gap);
  return delta < kCheckTtlSecs;
}

std::optional<CacheEntry> parse_cache(std::string_view text) {
  const auto nl1 = text.find('\n');
  if (nl1 == std::string_view::npos) return std::nullopt;
  std::string_view first = text.substr(0, nl1);
  if (!first.empty() && first.back() == '\r') first.remove_suffix(1);

  std::string_view rest = text.substr(nl1 + 1);
  const auto nl2 = rest.find('\n');
  std::string_view second = (nl2 == std::string_view::npos) ? rest : rest.substr(0, nl2);
  if (!second.empty() && second.back() == '\r') second.remove_suffix(1);
  if (second.empty()) return std::nullopt;

  // Parse checked_at as a plain i64 (ASCII digits, optional leading '-').
  std::int64_t checked_at = 0;
  {
    std::size_t i = 0;
    bool neg = false;
    if (i < first.size() && first[i] == '-') {
      neg = true;
      ++i;
    }
    if (i >= first.size()) return std::nullopt;
    for (; i < first.size(); ++i) {
      if (first[i] < '0' || first[i] > '9') return std::nullopt;
      checked_at = checked_at * 10 + (first[i] - '0');
    }
    if (neg) checked_at = -checked_at;
  }

  auto latest = sanitize_tag(second);
  if (!latest.has_value()) return std::nullopt;
  return CacheEntry{checked_at, *latest};
}

std::optional<std::string> parse_latest_tag(std::string_view body) {
  json parsed;
  try {
    parsed = json::parse(body);
  } catch (const json::exception&) {
    return std::nullopt;
  }
  if (!parsed.is_object()) return std::nullopt;
  const auto it = parsed.find("tag_name");
  if (it == parsed.end() || !it->is_string()) return std::nullopt;
  return sanitize_tag(it->get<std::string>());
}

std::optional<std::string> check(const http::Client& client, const std::string& cache_dir,
                                  std::string_view current_version, std::int64_t now) {
  auto latest = resolve_latest(client, cache_dir, now);
  if (!latest.has_value()) return std::nullopt;
  if (!semver::is_newer(*latest, current_version)) return std::nullopt;
  return latest;
}

std::optional<std::string> latest_fresh(const http::Client& client, const std::string& cache_dir,
                                         std::int64_t now) {
  auto tag = fetch_latest(client);
  if (tag.has_value()) write_cache(cache_dir, now, *tag);
  return tag;
}

}  // namespace shigoku::updatecheck
