// anilist.hpp — AniList public GraphQL search (P3) + list sync (P20, 06 §5).
// Ported from sabigoku src/anilist.rs: the search/discover/viewer query
// builders and response mapping, plus the sync surface (pull_list/pull_entry/
// push_entry) sync.cpp calls through the SyncClient seam.
//
// AniList is the only user-facing catalog client (01 §5) — stream providers
// (senshi, P4) never power Browse. This module has no dependency on
// provider.hpp; it depends on domain (Enrichment), http (Client), and store
// (RemoteEntry's wire shape, so reconcile_pull's signature can be built here).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "http.hpp"
#include "result.hpp"
#include "store.hpp"

namespace shigoku::anilist {

// Browse search page size (anilist.rs SEARCH_PAGE_SIZE / providers.rs).
inline constexpr std::uint32_t kSearchPageSize = 26;

// Discover feed page size (anilist.rs DISCOVER_PAGE_SIZE / providers.rs:20).
inline constexpr std::uint32_t kDiscoverPageSize = 20;

inline constexpr const char* kEndpoint = "https://graphql.anilist.co";

// Which AniList MediaType the query documents scope themselves to. DEFAULTED
// TO Anime at every site so the anime app's call sites — and the bytes they
// put on the wire — are unchanged; the manga app passes Manga. Manga also
// widens the shared fieldset with `chapters volumes`, the two counts that
// simply do not exist for an anime.
enum class MediaKind { Anime, Manga };

struct CatalogPage {
  std::vector<Enrichment> entries;
  bool has_next = false;
  friend bool operator==(const CatalogPage&, const CatalogPage&) = default;
};

// Browse search, kSearchPageSize per page, 1-indexed (anilist.rs:290).
[[nodiscard]] Result<CatalogPage, ProviderError> search(
    const http::Client& client, std::string_view query, std::uint32_t page,
    MediaKind kind = MediaKind::Anime);

// Discover feed for an axis, kDiscoverPageSize per page, 1-indexed
// (anilist.rs:851). AniList is the only catalog client (01 §5). `filters`
// narrows the feed (P38, §9 — no Rust precedent); a default-constructed
// DiscoverFilters composes the same body as the pre-P38 unfiltered call.
[[nodiscard]] Result<CatalogPage, ProviderError> discover(
    const http::Client& client, DiscoverAxis axis, std::uint32_t page,
    const DiscoverFilters& filters = DiscoverFilters{},
    MediaKind kind = MediaKind::Anime);

// The full AniList genre vocabulary (P38, GenreCollection), fetched once and
// cached session-side by the caller (app.hpp) — this call itself never
// caches. No natural "confirmed absent" state for a global collection, so
// this is a plain two-state fetch: Err = no answer, Ok = the list (possibly
// empty only if AniList's schema itself ever changed shape).
[[nodiscard]] Result<std::vector<std::string>, ProviderError> genre_collection(
    const http::Client& client);

// By-id enrichment fetch (P21, 05 §8 / anilist.rs enrich, :858). Deterministic
// join on a known AniList id (ROD-181). Three-state: Ok(some) = fresh metadata,
// Ok(nullopt) = confirmed no-match (a true negative, still an answer), Err = no
// answer (network/decode). The refresh law (05 §8) branches on all three.
[[nodiscard]] Result<std::optional<Enrichment>, ProviderError> enrich(
    const http::Client& client, std::int64_t anilist_id,
    MediaKind kind = MediaKind::Anime);

// Detail zoom's on-demand `c` section (P36): characters page 1 (~12: name,
// role, VA name) + recommendations (~10, Enrichment-shaped so a promote is a
// direct catalog_cache upsert). Same three-state contract as enrich: Err = no
// answer, Ok(nullopt) = confirmed no-match, Ok(some) = present (its two lists
// may individually be empty — that's not a negative answer, just a quiet show).
[[nodiscard]] Result<std::optional<CharactersAndRecommendations>, ProviderError>
characters_and_recommendations(const http::Client& client, std::int64_t anilist_id,
                               MediaKind kind = MediaKind::Anime);

// AniList account identity from the Viewer query (06 §4.2, P19). The sync
// pull needs id > 0.
struct Viewer {
  std::int64_t id = 0;
  std::string name;
  // Point100 default covers a wire response that omitted mediaListOptions
  // (e.g. a never-configured account) — same fallback parse_score_format
  // itself uses for an unrecognized string.
  ScoreFormat score_format = ScoreFormat::Point100;
  friend bool operator==(const Viewer&, const Viewer&) = default;
};

// Verify a token and read the account identity (06 §4.2). Ok(nullopt) =
// confirmed rejection (200, Viewer:null); Err = no answer (network/decode).
// Persist only on Ok(some) — the login core enforces that ordering, not this
// call.
[[nodiscard]] Result<std::optional<Viewer>, ProviderError> viewer(
    const http::Client& client, std::string_view token);

// --- AniList sync (P20, 06 §5) ----------------------------------------------

// The full remote list in one unpaginated POST (06 §5.4); a huge list fails
// the whole pull (2 MiB response cap, http::Client) rather than truncating.
// `format` converts each entry's wire score back to the store's raw 0..=100.
[[nodiscard]] Result<std::vector<RemoteEntry>, ProviderError> pull_list(
    const http::Client& client, std::string_view token, std::int64_t user_id, ScoreFormat format,
    MediaKind kind = MediaKind::Anime);

// One entry's live triple, read immediately before overwriting it (06 §5.3).
// nullopt = the account has no entry for that media. `format` converts the
// wire's score (whatever scale the account uses) back to the store's raw
// 0..=100 (P34 slice 2).
[[nodiscard]] Result<std::optional<ListEntry>, ProviderError> pull_entry(
    const http::Client& client, std::string_view token, std::int64_t user_id,
    std::int64_t media_id, ScoreFormat format, MediaKind kind = MediaKind::Anime);

// Push one row (06 §5.3); returns the server row id (never null, see
// detail::classify_save). `score` is raw 0..=100 (the store's own scale);
// `format` converts it to the account's scoreFormat before it hits the wire.
[[nodiscard]] Result<std::int64_t, ProviderError> push_entry(
    const http::Client& client, std::string_view token, std::int64_t media_id,
    ListStatus status, std::uint32_t progress, std::uint32_t score, ScoreFormat format);

// --- Internals exposed for the golden tests (anilist.rs tests mod) --------
namespace detail {

// The shared selection set for `kind` (anilist.rs MEDIA_FIELDS, :26). Anime
// returns it verbatim; Manga appends `chapters volumes`.
[[nodiscard]] std::string media_fields(MediaKind kind = MediaKind::Anime);

// The GraphQL query document (anilist.rs search_query, :28).
[[nodiscard]] std::string search_query(MediaKind kind = MediaKind::Anime);

// The POST body: {"query": ..., "variables": {"search", "perPage", "page"}}
// (anilist.rs search_body, :66). Returned pre-serialized (JSON text) so
// callers/tests don't need to link nlohmann to inspect it structurally.
[[nodiscard]] std::string search_body(std::string_view query_text, std::uint32_t page,
                                      MediaKind kind = MediaKind::Anime);

// The discover GraphQL query document (anilist.rs discover_query, :37).
[[nodiscard]] std::string discover_query(MediaKind kind = MediaKind::Anime);

// The discover POST body (anilist.rs discover_body, :73). `unix_secs` anchors
// the This Season cour; the other axes OMIT season/seasonYear entirely (an
// explicit null would filter season==null — the serde-parity trap, 06 §8b).
// `filters` (P38) rides the same omission discipline: an inactive filter
// field (empty genres, nullopt year/status/min_score) is OMITTED from the
// variables object, never sent as an explicit null.
[[nodiscard]] std::string discover_body(DiscoverAxis axis, std::uint32_t page,
                                        std::int64_t unix_secs,
                                        const DiscoverFilters& filters = DiscoverFilters{},
                                        MediaKind kind = MediaKind::Anime);

// The GenreCollection GraphQL query document (P38): a bare, argument-free
// query returning the full genre vocabulary as a flat string array.
[[nodiscard]] std::string genre_collection_query();

// The GenreCollection POST body (P38): {"query": ...}, no variables key —
// mirrors viewer_body's no-argument shape.
[[nodiscard]] std::string genre_collection_body();

// GenreCollection body -> the flat genre list (P38). data:null/garbage =
// Err (no answer); a present (possibly empty) array = Ok. No three-state
// branch — there is no "confirmed absent" shape for a global collection.
[[nodiscard]] Result<std::vector<std::string>, ProviderError> classify_genre_collection(
    std::string_view raw_json);

// Response body -> page. Unparseable / data:null = Err(Decode) ("no
// answer"); missing Page/pageInfo = exhausted (empty entries, has_next
// false), NOT an error (anilist.rs classify_page, :433).
[[nodiscard]] Result<CatalogPage, ProviderError> classify_page(std::string_view raw_json);

// The by-id GraphQL query document (anilist.rs by_id_query, :44).
[[nodiscard]] std::string by_id_query(MediaKind kind = MediaKind::Anime);

// The by-id POST body: {"query": ..., "variables": {"id"}} (anilist.rs
// by_id_body, :89).
[[nodiscard]] std::string by_id_body(std::int64_t anilist_id,
                                     MediaKind kind = MediaKind::Anime);

// By-id body -> three-state (05 §8, anilist.rs classify_by_id, :454):
// data:null/garbage = no answer (Err); Media:null = confirmed no-match
// (Ok(nullopt)); a present Media = Ok(some). Persist the null answer's
// freshness stamp anyway (a true negative is an answer); never stamp on Err.
[[nodiscard]] Result<std::optional<Enrichment>, ProviderError> classify_by_id(
    std::string_view raw_json);

// The characters+recommendations GraphQL query document (P36).
[[nodiscard]] std::string characters_recs_query(MediaKind kind = MediaKind::Anime);

// The characters+recommendations POST body: {"query": ..., "variables":
// {"id"}} (P36, mirrors by_id_body).
[[nodiscard]] std::string characters_recs_body(std::int64_t anilist_id,
                                               MediaKind kind = MediaKind::Anime);

// Characters+recommendations body -> three-state (P36, mirrors
// classify_by_id): data:null/garbage = no answer (Err); Media:null =
// confirmed no-match (Ok(nullopt)); a present Media = Ok(some), its
// characters/recommendations arrays taken as-is (possibly empty — a quiet
// show, not a negative answer).
[[nodiscard]] Result<std::optional<CharactersAndRecommendations>, ProviderError>
classify_characters_recs(std::string_view raw_json, MediaKind kind = MediaKind::Anime);

// The Viewer query body (anilist.rs viewer_body, :512).
[[nodiscard]] std::string viewer_body();

// Viewer body -> three-state (06 §4.2, anilist.rs classify_viewer, :649):
// data:null/garbage = no answer (Err); Viewer:null = confirmed rejection
// (Ok(nullopt)). Persist only on Ok(some).
[[nodiscard]] Result<std::optional<Viewer>, ProviderError> classify_viewer(
    std::string_view raw_json);

// --- AniList sync bodies/classifiers (P20, 06 §5) --------------------------

// `score` requested is the wire's raw `score: Float`, NOT `scoreFormat`
// dependent — AniList's `score` field itself always returns/accepts a plain
// number already on the account's chosen scale, so this body carries no
// format hint; classify_list/classify_entry take ScoreFormat to interpret
// what they read back, and save_entry_body's caller converts before calling.
[[nodiscard]] std::string list_collection_body(std::int64_t user_id,
                                               MediaKind kind = MediaKind::Anime);

// Page(perPage:1){mediaList(...)}, not the MediaList root query: root returns
// a GraphQL error for a media the account has no entry for, which the push
// guard would have to read as absence rather than failure; the Page form
// returns an empty list instead (06 §5.3).
[[nodiscard]] std::string list_entry_body(std::int64_t user_id, std::int64_t media_id,
                                          MediaKind kind = MediaKind::Anime);

// `score` is the wire value already on the account's scoreFormat scale (the
// caller — push_entry — converts from the store's raw 0..=100 first).
[[nodiscard]] std::string save_entry_body(std::int64_t media_id, ListStatus status,
                                          std::uint32_t progress, double score);

// MediaListCollection body -> flat remote entries (anilist.rs classify_list).
// Duplicate ids across custom lists are possible; collapsing them is the
// reconcile join's job (06 §5.4), not this classifier's. A null updatedAt
// maps to 0 so it loses every tiebreak against a stamped sibling. `format`
// converts each entry's wire score back to the store's raw 0..=100.
[[nodiscard]] Result<std::vector<RemoteEntry>, ProviderError> classify_list(
    std::string_view raw_json, ScoreFormat format);

// Page.mediaList body -> the one entry's triple, nullopt when the account has
// no entry for that media. `format` converts the wire score to raw 0..=100.
//
// INVARIANT the push guard rests on: Ok(nullopt) is reserved for exactly ONE
// wire shape, a present-and-empty mediaList. Every other null on the path is
// an Err. A nulled Page is NOT absence: GraphQL propagates a failed
// resolver's null up to the nearest nullable ancestor, so Page:null is the
// shape a server-side error takes, and the empty list is the shape a genuine
// miss takes. Reading the first as the second lets a transient AniList fault
// present as "nothing to overwrite" and wave a stale write through — the
// overwrite ROD-498 exists to stop. Widen this back to a bare default-on-null
// and the guard silently stops guarding.
[[nodiscard]] Result<std::optional<ListEntry>, ProviderError> classify_entry(
    std::string_view raw_json, ScoreFormat format);

// Save body -> the server row id. A 200 without a non-null id is a failure,
// not a silent success: advancing the snapshot on a phantom save loses the
// row (06 §5.3).
[[nodiscard]] Result<std::int64_t, ProviderError> classify_save(std::string_view raw_json);

}  // namespace detail

}  // namespace shigoku::anilist
