// mal.hpp — MAL API v2 REST client (P31 §9.1 slice 3). No sabigoku reference
// (MAL never existed there — mal_login.hpp's header comment). This is the
// second genuinely new mechanism in the phase: a plain-REST provider client
// alongside the GraphQL shape anilist.hpp uses for AniList. House style fits
// either way (A5's http::Client, ProviderError taxonomy), so this module
// reuses both without adaptation — only the wire format (query-string GET,
// form-encoded PUT, flat JSON responses) differs from anilist.cpp's POST-only
// GraphQL body.
//
// Scope (status-map + get/put only): the mirror push itself (dirty-row walk,
// NULL-mal_id skip, synced stamp) is slice 4 — this module is the transport
// + mapping layer slice 4 calls through, mirroring how anilist.hpp's
// pull_entry/push_entry predate sync.cpp's orchestration (P3 before P20).
//
// Status map (PORT_PARITY.md §9 P31): AniList's five reachable statuses
// (Planning/Watching/Paused/Completed/Dropped — REPEATING already folds to
// Watching at AniList ingest, anilist.cpp:213) against MAL's five
// (plan_to_watch/watching/on_hold/completed/dropped) is a clean bijection;
// no lossy corner survives into this store's domain because ListStatus
// itself carries no rewatch bit (grep confirms: nothing in the codebase
// tracks is_rewatching). MAL's API *does* accept `is_rewatching` as a
// separate field on the PUT — this client always sends `false`, matching
// what the store can actually assert.
//
// Auth: Bearer, same shape as AniList's Authorization header (anilist.cpp),
// via MalAuth::bearer() (auth.hpp) rather than AniListAuth::bearer().

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "domain.hpp"
#include "error.hpp"
#include "http.hpp"
#include "result.hpp"

namespace shigoku::mal {

// v2 REST base (developers.myanimelist.net). No GraphQL endpoint constant
// (contrast anilist::kEndpoint) — each call builds its own path off this.
inline constexpr const char* kApiBase = "https://api.myanimelist.net/v2";

// Get the caller's current (status, progress, score) for one anime, or
// nullopt if the title has no list entry yet (MAL's `my_list_status` field
// is simply absent from the response — not an error, same "absence is not
// failure" rule as 03 §4.3). `mal_id` is the store's mal_id column (0 rows
// never reach this call — slice 4's NULL-mal_id skip happens before the
// client is invoked, per the plan). Score is MAL's own native 0..=10 int
// scale (`my_list_status.score`) — a DIFFERENT scale from the store's raw
// 0..=100 (P34 slice 1) and from AniList's scoreFormat-dependent wire value
// (P34 slice 2); mal_mirror.cpp converts at this module's boundary, the same
// pattern anilist.cpp uses for its own wire scale.
[[nodiscard]] Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>,
                     ProviderError>
get_list_entry(const http::Client& client, std::string_view token, std::int64_t mal_id);

// Push (status, progress, score) for one anime (PUT .../my_list_status,
// form-encoded). `score` is MAL's own 0..=10 int scale; nullopt OMITS the
// field entirely — MAL treats `score=0` as "remove the user's score", so a
// locally-unscored row must withhold the field rather than send 0 and wipe
// whatever the user rated on the MAL site (the mirror never destroys what
// it never held — the same doctrine as the deleted-entry branch). MAL's PUT is
// idempotent upsert — no separate create/update path, unlike AniList's
// SaveMediaListEntry which needs no anime-id lookup either. is_rewatching is
// always sent false (see header comment). Returns Ok(Unit) on any 2xx; MAL's
// response body on success just echoes the fields, which this client does
// not need back (the push work-list's local state is already authoritative —
// mirrors anilist.cpp's push_entry needing only the new remote id, not a
// full echo).
[[nodiscard]] Result<Unit, ProviderError> update_list_entry(
    const http::Client& client, std::string_view token, std::int64_t mal_id, ListStatus status,
    std::uint32_t progress, std::optional<std::uint32_t> score);

// --- Internals exposed for the golden tests --------------------------------
namespace detail {

// Domain -> MAL's five list_status wire values (push direction, 06 §5.3-
// equivalent). Total over the closed ListStatus enum, unlike
// list_status_from_mal below, which must also tolerate unknown/absent.
[[nodiscard]] const char* list_status_to_mal(ListStatus s);

// MAL's five list_status wire values -> domain (pull direction). Unknown or
// absent maps to Planning, matching anilist.cpp's list_status_from_anilist
// default-safe behavior (06 §5.4 "never invent an active state").
[[nodiscard]] ListStatus list_status_from_mal(std::optional<std::string_view> s);

// Percent-encode one query-string component (RFC 3986 unreserved set passes
// through raw). Reused from nowhere else in this module's own translation
// unit — mal_login.cpp's detail::form_encode is the same algorithm applied
// to a POST body rather than a URL, so this is a second small copy rather
// than a cross-module dependency on mal_login (this module intentionally has
// no link edge to it — see mal.cpp's CMakeLists comment).
[[nodiscard]] std::string percent_encode(std::string_view s);

// Decode a GET .../my_list_status response body. Ok(nullopt) when the
// `my_list_status` field is absent/null (no entry yet); Ok(some) with the
// mapped triple when present; Err(Decode) only for unparseable JSON — a
// response missing fields inside my_list_status degrades field-by-field
// (status absent -> Planning, num_episodes_watched absent -> 0, score
// absent -> 0), same tolerant-defaulting spirit as anilist.cpp's
// classify_entry.
[[nodiscard]] Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>,
                     ProviderError>
parse_get_response(std::string_view json_body);

// Build the form-encoded PUT body: status, num_watched_episodes, score
// (omitted entirely when nullopt — score=0 would ERASE the user's MAL-side
// score, see update_list_entry), is_rewatching=false.
[[nodiscard]] std::string put_body(ListStatus status, std::uint32_t progress,
                                   std::optional<std::uint32_t> score);

}  // namespace detail

}  // namespace shigoku::mal
