// resolver.hpp — Tier-B/C candidate scoring (P15). Pure functions, no I/O.
//
// Ported 1:1 from sabigoku src/resolver.rs + the title_score/normalize_title/
// canon_season region of src/anilist.rs. The whole module prefers "no match"
// over guessing (resolver.rs:1-7): id agreement is tried first (best_id_match),
// then fuzzy title match (best_provider_match), and both return an INDEX into
// the candidate slice, or nullopt = refuse to bind. Below the floor / near-tie,
// nothing binds — an empty grid never carries a wrong id (03 §4.2).
//
// This replaces P7's Tier-C first-hit (debt #4). Everything here is a pure
// function over Enrichment + SearchHit, so the whole surface unit-tests offline
// (§8 pure-function bias) — no store, no registry, no network.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "provider.hpp"

namespace shigoku::resolver {

// --- Title scoring (anilist.rs:242-311) ------------------------------------

// normalize_title (anilist.rs:299-311): byte-level. ASCII alphanumerics kept,
// lowercased; every other ASCII byte (space, punctuation, colon) dropped;
// non-ASCII bytes (UTF-8 lead/continuation) passed through verbatim. So
// "Re:Zero 2nd Season" -> "rezero2ndseason"; "葬送のフリーレン" unchanged.
[[nodiscard]] std::string normalize_title(std::string_view s);

// canon_season (anilist.rs:267-295): runs AFTER normalize_title. Folds the
// "Season N" / "Nth Season" forms to a canonical "sN" marker so both spellings
// score identical. No "season" substring -> input unchanged. Only ONE ordinal
// (st/nd/rd/th, first match) is stripped. Form A (digits after "season") is
// checked before Form B (digits before).
[[nodiscard]] std::string canon_season(std::string_view s);

// title_score (anilist.rs:242-262): one of exactly five values.
//   nullopt / empty / no overlap -> -5000
//   normalized exact equality     -> 1600
//   one is a prefix of the other  -> 1250
//   one is a substring of the other -> 900
// No tie-breaking here; the caller breaks ties via eps/year deltas.
[[nodiscard]] std::int32_t title_score(std::string_view a,
                                       std::optional<std::string_view> b);

// --- Candidate helpers (resolver.rs:164-177) -------------------------------

// candidate_episodes (resolver.rs:164-167): catalog total_episodes if present,
// else max(eps_sub, eps_dub). 0 = unknown (disables the episode signal).
[[nodiscard]] std::uint32_t candidate_episodes(const SearchHit& cand);

// total_is_authoritative (resolver.rs:174-177): a null status is authoritative
// (a doubtful total should still veto a wild gap); RELEASING/HIATUS (case-
// insensitive) are NOT authoritative. Gates the eps hard-reject in
// candidate_score AND the eps veto in id_match_contradicted.
[[nodiscard]] bool total_is_authoritative(std::optional<std::string_view> status);

// --- Scoring (resolver.rs:102-160) -----------------------------------------

// candidate_score (resolver.rs:102-160): best title agreement over the 3x3
// title cross product, then eps/year bonuses; a movie/series episode gap under
// an authoritative total is a hard reject (-4000). Exposed for tests.
[[nodiscard]] std::int32_t candidate_score(const Enrichment& canonical,
                                           const SearchHit& cand);

// --- Public entry points (resolver.rs:25, :74) -----------------------------

// best_id_match (resolver.rs:25-70) — Tier-B, tried FIRST. Returns the index of
// the candidate whose AniList/MAL id agrees with the canonical, preferring a
// corroborated survivor over the first bare id; contradicted candidates are
// skipped entirely. nullopt = no id agreement anywhere. title_score is NEVER
// consulted here (id agreement fully short-circuits fuzzy scoring).
[[nodiscard]] std::optional<std::size_t> best_id_match(
    const Enrichment& canonical, const std::vector<SearchHit>& candidates);

// best_provider_match (resolver.rs:74-98) — Tier-C fuzzy, when id match refuses.
// Picks the top candidate_score, but refuses (nullopt) when the best is below
// the floor (1200) or within the margin (250) of a non-negative runner-up.
[[nodiscard]] std::optional<std::size_t> best_provider_match(
    const Enrichment& canonical, const std::vector<SearchHit>& candidates);

// The two thresholds (resolver.rs:13-16), exposed for the tests' documentation.
inline constexpr std::int32_t kBestFloor = 1200;
inline constexpr std::int32_t kMatchMargin = 250;

}  // namespace shigoku::resolver
