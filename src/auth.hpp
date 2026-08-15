// auth.hpp — AniList auth token per 06 §3 (P19), plus the MAL mirror's
// `[mal]` block (P31 §9.1 slice 1). AniList's shape ported from sabigoku
// src/auth.rs; MAL never existed there ("later MAL/Kitsu" — sabigoku's own
// doc comment), so MalAuth is new but follows the same nesting idiom. Kept
// out of Config: the bearer must not ride Settings round-trips (06 §3.1) —
// this is its own file, its own load/save.
//
// Invariant: read a token through its *Auth::bearer(), never access_token
// directly. That accessor is the single place the control-byte rule (06
// §3.3) is enforced; a raw read bypasses it. MalAuth::bearer() applies the
// same rule to MAL's access_token (refresh_token is never read as a bearer,
// so it carries no such accessor — P31 slice 2 owns refresh rollover).

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "domain.hpp"
#include "result.hpp"

namespace shigoku {

// 06 §3.1: oversized auth file loads as the empty signed-out record.
inline constexpr std::uint64_t kMaxAuthBytes = 16 * 1024;

// AniList returns bearer tokens; the default keeps a hand-written file usable.
inline constexpr const char* kDefaultTokenType = "Bearer";

// 06 §3.2 record shape. Empty access_token = signed out.
struct AniListAuth {
  std::string access_token;
  std::string token_type = kDefaultTokenType;
  // Unix seconds; 0 = undated, which stays live until a 401 (06 §3.3): an
  // undated token must not be refused locally.
  std::int64_t expires_at = 0;
  // AniList user id. The sync pull (MediaListCollection) needs > 0.
  std::int64_t user_id = 0;
  std::string user_name;
  // The account's score scale (P34 slice 2, Viewer.mediaListOptions.
  // scoreFormat), fetched alongside id/name at connect. Point100 default
  // covers a pre-P34 auth.json and an account never (re)connected since.
  ScoreFormat score_format = ScoreFormat::Point100;

  friend bool operator==(const AniListAuth&, const AniListAuth&) = default;

  // The usable bearer, or nullopt when signed out. A control byte (< 0x20)
  // anywhere in the token forces signed-out: a bare LF injects headers, and
  // CR/LF trips strict HTTP clients' line asserts (06 §3.3). This is the
  // only sanctioned way to reach the token.
  [[nodiscard]] std::optional<std::string_view> bearer() const;

  // Expired only when dated and reached: a 0 expiry never expires locally
  // (06 §3.3). Independent of bearer(); a present-but-expired token is the
  // "reconnect" account state.
  [[nodiscard]] bool is_expired(std::int64_t now) const;
};

// P31: MAL's block. Separate struct (not a reuse of AniListAuth) because
// MAL adds a genuinely new field (refresh_token — MAL tokens live ~31 days
// and nothing before this phase handles refresh) and drops user_name (MAL's
// v2 API does not hand back a display name on the token exchange).
struct MalAuth {
  std::string access_token;
  std::string refresh_token;
  std::string token_type = kDefaultTokenType;
  // Unix seconds; 0 = undated, same "never expires locally" rule as AniList
  // (06 §3.3) — a dated-but-unreached token still authenticates.
  std::int64_t expires_at = 0;
  // MAL numeric user id, from the token exchange's user info.
  std::int64_t user_id = 0;

  friend bool operator==(const MalAuth&, const MalAuth&) = default;

  // Same control-byte rule as AniListAuth::bearer() (06 §3.3), applied to
  // MAL's access_token. The sole sanctioned read path.
  [[nodiscard]] std::optional<std::string_view> bearer() const;

  [[nodiscard]] bool is_expired(std::int64_t now) const;
};

// Top-level auth record. Nested provider blocks (.anilist, .mal) so a
// second provider does not reshape the file (06 §3.2) — preserve the
// nesting, don't flatten it (PORT_PARITY.md P19, extended P31).
struct Auth {
  AniListAuth anilist;
  MalAuth mal;

  friend bool operator==(const Auth&, const Auth&) = default;

  // Total load (06 §3.1): missing, unreadable, oversized, or corrupt files
  // all yield the empty signed-out record. Auth never wedges startup.
  [[nodiscard]] static Auth load(const std::string& path);

  // Write the bearer atomically at mode 0600 (06 §3.1). The temp is created
  // 0600 directly (O_EXCL|O_CREAT, not chmod-after): a plain create honours
  // the ambient umask, opening a window where the secret is group/world-
  // readable on disk before a chmod would land. rename then swaps it in
  // atomically.
  [[nodiscard]] Result<Unit, std::string> save(const std::string& path) const;
};

}  // namespace shigoku
