// idmap.hpp — offline MyAnimeList <-> AniList anime id bridge.
//
// The store keys every row on anilist_id; a show found through the MAL
// catalog carries only its MAL id. This table (generated from the manami
// anime-offline-database, see third_party/manami/README) answers the join
// for every show the dataset knew at generation time, so most MAL-sourced
// rows land under their REAL AniList id and only shows newer than the
// dataset need a synthetic one. Lookups are binary searches over a sorted
// constant array; the reverse index is built once on first use.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace shigoku::idmap {

// MAL id -> AniList id, nullopt when the table has no entry.
[[nodiscard]] std::optional<std::int64_t> to_anilist(std::int64_t mal_id);

// AniList id -> MAL id, nullopt when the table has no entry.
[[nodiscard]] std::optional<std::int64_t> to_mal(std::int64_t anilist_id);

// The anilist_id a show known only by its MAL id is stored under: the
// table's real id, else -mal_id — synthetic, unique, and recognisable by its
// sign everywhere AniList would otherwise be asked (catalog.hpp's id law).
[[nodiscard]] std::int64_t anilist_or_synthetic(std::int64_t mal_id);

// Number of pairs in the table.
[[nodiscard]] std::size_t size();

// Upstream release tag the table was generated from (informational).
[[nodiscard]] const char* release();

namespace data {
// Flat (mal_id, anilist_id) pairs, 2*kCount values, sorted by mal_id.
extern const std::uint32_t kByMal[];
extern const std::size_t kCount;
extern const char* const kRelease;
}  // namespace data

}  // namespace shigoku::idmap
