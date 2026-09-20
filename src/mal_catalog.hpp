// mal_catalog.hpp — MyAnimeList API v2 as a CATALOG (search, Discover feed,
// by-id detail, recommendations). The second catalog next to anilist.hpp; no
// sabigoku precedent. Same house shape as mal.hpp (plain REST
// over http::Client) and anilibria.hpp (pure parsers in detail::, transport
// thin): URL builders and JSON -> Enrichment mappers are free functions that
// golden tests exercise offline; the four network calls just glue them to a
// fetch.
//
// Everything is mapped INTO the AniList-shaped Enrichment the whole UI reads,
// so a MAL-sourced row renders, persists, plays and syncs like any other. The
// one thing MAL cannot give is an AniList id: the mapper asks an IdBridge
// (default: the offline idmap table, else the synthetic negative -mal_id) and
// never invents one itself.
//
// Auth: every request carries X-MAL-CLIENT-ID. The id is the app's own
// registration (kClientId, baked like login::kClientId) unless the config
// override is set.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "http.hpp"
#include "result.hpp"

namespace shigoku::mal_catalog {

inline constexpr const char* kApiBase = "https://api.myanimelist.net/v2";

// shigoku's own MAL app registration (app type "other": a public client, no
// secret; redirect http://127.0.0.1:8767/mal/callback for the mirror's
// Connect). With an empty id every call refuses cleanly (Forbidden) rather
// than sending a request MAL would reject.
inline constexpr const char* kClientId = "f9b42c82ab17491f3abb3e16a9e9c230";

// Page sizes match the AniList catalog so the grid/list paging is identical
// whichever source answers.
inline constexpr std::uint32_t kSearchPageSize = 26;
inline constexpr std::uint32_t kDiscoverPageSize = 20;
// When Discover filters are active the ranking endpoints can't narrow server-side,
// so a page is fetched at MAL's maximum and filtered client-side; the larger
// window keeps filtered pages from coming back empty most of the time.
inline constexpr std::uint32_t kFilteredPageSize = 100;

// MAL answers 400 to a search shorter than this; the catalog answers such a
// query with an empty page instead, without a request.
inline constexpr std::size_t kMinQueryChars = 3;

// The id the requests carry: the config override when non-empty, else the
// baked one. Empty result = MAL catalog unavailable.
[[nodiscard]] std::string effective_client_id(std::string_view config_override);

struct Page {
  std::vector<Enrichment> entries;
  bool has_next = false;
  friend bool operator==(const Page&, const Page&) = default;
};

// By-id answer: the show plus its recommendation nodes (sparse Enrichments:
// title + cover + ids), which MAL returns on the same request.
struct Detail {
  Enrichment show;
  std::vector<Enrichment> recommendations;
  friend bool operator==(const Detail&, const Detail&) = default;
};

// mal_id -> the anilist_id a row is stored under. See default_bridge.
using IdBridge = std::function<std::int64_t(std::int64_t mal_id)>;

// idmap table hit -> the real AniList id; miss -> -mal_id (synthetic, unique,
// recognisable by sign everywhere AniList would otherwise be asked).
[[nodiscard]] std::int64_t default_bridge(std::int64_t mal_id);

// Browse search, kSearchPageSize per page, 1-indexed. A query shorter than
// kMinQueryChars returns an empty page without a request.
[[nodiscard]] Result<Page, ProviderError> search(const http::Client& client,
                                                 std::string_view client_id,
                                                 std::string_view query, std::uint32_t page,
                                                 const IdBridge& bridge = default_bridge);

// Discover feed for an axis, 1-indexed. `unix_secs` anchors This Season (the
// same current_cour the AniList body uses). Filters apply client-side per
// page: a filtered page may be short or even empty while
// has_next is still true.
[[nodiscard]] Result<Page, ProviderError> discover(const http::Client& client,
                                                   std::string_view client_id, DiscoverAxis axis,
                                                   std::uint32_t page, std::int64_t unix_secs,
                                                   const DiscoverFilters& filters = DiscoverFilters{},
                                                   const IdBridge& bridge = default_bridge);

// By-id detail. Three-state like anilist::enrich: Ok(some) = present,
// Ok(nullopt) = MAL says no such anime (404, a true negative), Err = no
// answer. `keep_anilist_id` != 0 pins the row's existing key (an AniList-keyed
// row refreshed from MAL keeps its id); 0 asks the bridge.
[[nodiscard]] Result<std::optional<Detail>, ProviderError> by_id(
    const http::Client& client, std::string_view client_id, std::int64_t mal_id,
    std::int64_t keep_anilist_id = 0, const IdBridge& bridge = default_bridge);

// One row of the signed-in account's own anime list (GET /users/@me/animelist):
// the show as this catalog maps it (anilist_id via the bridge, mal_id set)
// plus the list_status triple. `score` is MAL's native 0..=10 — the caller
// converts to the store's raw scale; `updated_at` is list_status.updated_at
// as unix seconds (0 when absent or unparseable).
struct UserListEntry {
  Enrichment seed;
  ListStatus status = ListStatus::Planning;
  std::uint32_t progress = 0;
  std::uint32_t score = 0;
  std::int64_t updated_at = 0;
  friend bool operator==(const UserListEntry&, const UserListEntry&) = default;
};

// The account's whole list, every status, following `paging.next` for up
// to kUserListPages pages of kUserListPageSize (MAL's per-page maximum).
// Bearer-authenticated with the account's token — this is the one catalog
// call that is not keyed by the app's client id. A 401 surfaces as
// Http/401, a 429 as RateLimited; a continuation link off MAL's own host
// is refused (the walk stops there rather than fetch a foreign URL).
inline constexpr std::uint32_t kUserListPageSize = 1000;
inline constexpr std::uint32_t kUserListPages = 20;
[[nodiscard]] Result<std::vector<UserListEntry>, ProviderError> user_list(
    const http::Client& client, std::string_view token, const IdBridge& bridge = default_bridge);

// The genre vocabulary for the Discover filter overlay: MAL v2 has no genre
// endpoint, so this is the AniList vocabulary (what the overlay already
// speaks) and passes_filters folds MAL's own spellings onto it.
[[nodiscard]] std::vector<std::string> genre_vocabulary();

// --- Internals exposed for the golden tests --------------------------------
namespace detail {

// The `fields` query value every anime-bearing request asks for.
[[nodiscard]] std::string fields_param();

// Percent-encode one query-string component (RFC 3986 unreserved passes raw).
[[nodiscard]] std::string percent_encode(std::string_view s);

[[nodiscard]] bool query_too_short(std::string_view query);

[[nodiscard]] std::string search_url(std::string_view query, std::uint32_t page);
// ranking_type for the three ranking-backed axes; This Season has none.
[[nodiscard]] std::optional<std::string_view> ranking_type(DiscoverAxis axis);
[[nodiscard]] std::string discover_url(DiscoverAxis axis, std::uint32_t page, std::int64_t unix_secs,
                                       std::uint32_t limit);
[[nodiscard]] std::string by_id_url(std::int64_t mal_id);
[[nodiscard]] std::string user_list_url();  // page 1; later pages ride paging.next verbatim.

// One page of the user list: the mapped rows plus the continuation link.
struct UserListPage {
  std::vector<UserListEntry> entries;
  std::optional<std::string> next;
  friend bool operator==(const UserListPage&, const UserListPage&) = default;
};
[[nodiscard]] Result<UserListPage, ProviderError> parse_user_list_page(std::string_view raw_json,
                                                                       const IdBridge& bridge);
// "YYYY-MM-DDTHH:MM:SS" + "Z" | "+HH:MM" | "-HH:MM" (MAL emits +00:00) -> unix
// seconds; 0 on any shape or range failure.
[[nodiscard]] std::int64_t parse_iso8601_utc(std::string_view s);

// A list response ({"data":[{"node":{…}},…],"paging":{"next":…}}): search,
// ranking (each item also carries "ranking") and season all share it.
[[nodiscard]] Result<Page, ProviderError> parse_page(std::string_view raw_json,
                                                     const IdBridge& bridge);

// A by-id response (the anime object at top level, recommendations inline).
[[nodiscard]] Result<Detail, ProviderError> parse_detail(std::string_view raw_json,
                                                         std::int64_t keep_anilist_id,
                                                         const IdBridge& bridge);

// Discover filters applied to one mapped row: genres = contains ALL selected
// (Thriller/Suspense fold), year on the row's year, status on the mapped
// AniList spelling, min_score = strictly greater (averageScore_greater).
[[nodiscard]] bool passes_filters(const Enrichment& e, const DiscoverFilters& f);

// Field mappers.
[[nodiscard]] std::optional<Date> parse_date(std::string_view s);  // "Y", "Y-M", "Y-M-D"
[[nodiscard]] std::optional<std::string> map_status(std::string_view mal_status);
[[nodiscard]] std::optional<std::string> map_media_type(std::string_view media_type);
[[nodiscard]] std::optional<std::string> map_source(std::string_view source);
[[nodiscard]] std::optional<std::uint32_t> score_from_mean(double mean);  // ×10, rounded
[[nodiscard]] std::optional<std::uint32_t> minutes_from_seconds(std::int64_t secs);
// Drops the "[Written by MAL Rewrite]" trailer, collapses whitespace runs,
// trims; empty result -> nullopt.
[[nodiscard]] std::optional<std::string> clean_synopsis(std::string_view raw);

}  // namespace detail

}  // namespace shigoku::mal_catalog
