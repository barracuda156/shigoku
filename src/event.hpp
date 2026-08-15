// event.hpp — the Event variant (P1, 04 §4 subset for v0).
//
// A1: one event queue; workers post ONLY these; the UI thread owns all state.
// A2: Event is a std::variant dispatched via std::visit + overloaded{} with NO
// catch-all lambda — adding an alternative must fail compilation until every
// visitor handles it. Payloads are OWNED (moved std::string/std::vector inside
// the event) so a worker never shares memory with App (A1).
//
// Staleness (04 §6): each async result carries the generation token captured at
// spawn; tick() drops mismatches. Key/cover carry the anilist_id they were
// spawned for; search carries the query string. The token type is uint32_t
// (stays lock-free on PPC32; §3).

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "provider.hpp"  // SearchHit (ProviderSearchDone payload).

namespace shigoku {

using Generation = std::uint32_t;

// A catalog row as surfaced to the UI: the AniList search hit (P3 fills it).
// Kept here (not domain.hpp) because it is the *event* payload shape; the
// underlying metadata is Enrichment.
struct CatalogRow {
  Enrichment meta;
  friend bool operator==(const CatalogRow&, const CatalogRow&) = default;
};

// Decoded RGBA8 cover, ready for the kitty emitter (P8). Bytes are in memory
// order (§3: RGBA = 4 bytes, never a packed uint32).
struct CoverPixels {
  std::vector<unsigned char> rgba;  // w*h*4, row-major, R,G,B,A per pixel.
  std::uint32_t w = 0;
  std::uint32_t h = 0;
};

// --- 4.1 Terminal / time ---------------------------------------------------

// A decoded key press. The full KeyEvent (modifiers, special keys) lands with
// the input decoder in P6; P1 pins only the shape so the variant compiles.
struct KeyEvent {
  // Unicode codepoint of a printable key, or 0 when `special` names the key.
  char32_t codepoint = 0;
  enum class Special {
    None,
    Enter,
    Escape,
    Backspace,
    Tab,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Delete,
  } special = Special::None;
  bool ctrl = false;
  friend bool operator==(const KeyEvent&, const KeyEvent&) = default;
};

// A decoded mouse report (P32, §9): a button press/release or a wheel notch
// at a cell, normalized to 0-based coordinates from either encoding (SGR
// ?1006 or the legacy X10 3-byte form). Motion and wheel-tilt reports never
// surface — the decoder consumes them silently. Wheel kinds carry
// Button::None (the wheel is not a button the views can hold). X10 releases
// don't say which button went up, hence Button::None there too.
struct MouseEvent {
  enum class Kind {
    Press,
    Release,
    WheelUp,
    WheelDown,
  } kind = Kind::Press;
  enum class Button {
    Left,
    Middle,
    Right,
    None,
  } button = Button::None;
  int x = 0;  // 0-based cell column.
  int y = 0;  // 0-based cell row.
  friend bool operator==(const MouseEvent&, const MouseEvent&) = default;
};

struct Resize {
  std::uint16_t cols = 0;
  std::uint16_t rows = 0;
  std::uint16_t xpixel = 0;  // for kitty cover sizing (0 = unknown).
  std::uint16_t ypixel = 0;
  friend bool operator==(const Resize&, const Resize&) = default;
};

// ~100ms timeout tick: spinner, search debounce (300ms), cover settle/cooldown.
struct Tick {
  friend bool operator==(const Tick&, const Tick&) = default;
};

// --- 4.2 Catalog / search --------------------------------------------------

struct SearchDone {
  std::vector<CatalogRow> results;  // owned.
  std::string query;                // stale if != current query.
  std::uint32_t page = 1;
  bool has_next = false;  // AniList hasNextPage: drives Browse load-more (05 §16).
  friend bool operator==(const SearchDone&, const SearchDone&) = default;
};

struct SearchError {
  ProviderError cause;
  std::string query;
  friend bool operator==(const SearchError&, const SearchError&) = default;
};

// --- 4.2b Discover feed (P17, DESIGN §3.8) ---------------------------------
//
// The AniList discover feed, per axis (Trending/Popular/TopRated/ThisSeason).
// Staleness is POSITIONAL within one filter set: a page is filed only when it
// is exactly `slot.page + 1` (DiscoverState::on_feed discards otherwise).
// Across filter sets position is not identity (P38 review) — `gen` carries
// the filter generation the fetch was spawned under, and on_feed/
// on_feed_error discard a mismatched generation whole.

struct DiscoverFeedDone {
  DiscoverAxis axis = DiscoverAxis::Trending;
  std::uint32_t page = 1;
  std::vector<CatalogRow> rows;  // owned.
  bool has_next = false;
  std::uint32_t gen = 0;  // filter generation at spawn (P38).
  friend bool operator==(const DiscoverFeedDone&, const DiscoverFeedDone&) = default;
};

struct DiscoverFeedError {
  DiscoverAxis axis = DiscoverAxis::Trending;
  std::string cause;  // human-facing message (the slot's sticky failed state).
  std::uint32_t gen = 0;  // filter generation at spawn (P38).
  friend bool operator==(const DiscoverFeedError&, const DiscoverFeedError&) = default;
};

// --- 4.2c Discover grid covers (P17b, CoverPool + composition) -------------
//
// The multi-cover grid keys on URL (not anilist_id): a result for a scrolled-
// away or evicted url is still adopted; its slot is recreated (04 §4.4). No
// generation token — the url IS the identity.

struct GridCoverDone {
  std::string url;         // the url the pixels belong to (identity, A4 §4.4).
  CoverPixels pixels;      // owned decoded RGBA.
  friend bool operator==(const GridCoverDone&, const GridCoverDone&) = default;
};

struct GridCoverError {
  std::string url;         // per-url cooldown key.
  friend bool operator==(const GridCoverError&, const GridCoverError&) = default;
};

// --- 4.3 Resolve / episodes ------------------------------------------------

struct EpisodesDone {
  std::vector<std::string> episodes;  // sorted raw labels, owned.
  std::int64_t for_id = 0;            // anilist_id; keep-check vs current.
  Generation gen = 0;
  // The provider NAME the fetch ran on (P15): the transport needs it to
  // bind_provider / mark_provider_absent / set_episode_cache and to caption
  // the serving provider (05 §15). v0 carried only the id.
  std::string provider;
  // Provider-side id the episodes were fetched under (Tier-A canonical key,
  // or the Tier-C search hit). Play MUST reuse this: a Tier-C show has no
  // canonical_key and its anilist_id is NOT a provider key (senshi keys by
  // MAL id), so re-deriving at play time resolves the wrong show.
  std::string provider_id;
  friend bool operator==(const EpisodesDone&, const EpisodesDone&) = default;
};

struct EpisodesError {
  ProviderError cause;
  std::int64_t for_id = 0;
  Generation gen = 0;
  std::string provider;  // P15: which provider failed (fail_over / Fail toast).
  friend bool operator==(const EpisodesError&, const EpisodesError&) = default;
};

// --- 4.3b Provider search (P15, Tier-C walk) -------------------------------
//
// A provider-side title search fired by the walk when no binding/key exists.
// Its hits are scored (best_id_match → best_provider_match) on the UI thread;
// a winner fires a mint-fetch, a miss re-drives the walk. Distinct from the
// AniList catalog SearchDone (that fills the browse list; this feeds resolve).

struct ProviderSearchDone {
  std::vector<SearchHit> hits;  // owned provider candidates.
  std::int64_t for_id = 0;      // anilist_id; keep-check vs current.
  Generation gen = 0;
  std::string provider;         // the provider searched (fetch runs on it).
  friend bool operator==(const ProviderSearchDone&, const ProviderSearchDone&) = default;
};

struct ProviderSearchError {
  ProviderError cause;
  std::int64_t for_id = 0;
  Generation gen = 0;
  std::string provider;
  friend bool operator==(const ProviderSearchError&, const ProviderSearchError&) = default;
};

// --- 4.4 Covers ------------------------------------------------------------

struct CoverDone {
  CoverPixels pixels;       // owned decoded RGBA.
  std::int64_t for_id = 0;  // anilist_id; drop if stale.
  Generation gen = 0;
  friend bool operator==(const CoverDone&, const CoverDone&) = default;
};

struct CoverError {
  std::int64_t for_id = 0;  // cooldown path (A4: 10s per id+url).
  Generation gen = 0;
  friend bool operator==(const CoverError&, const CoverError&) = default;
};

// --- 4.5 Playback ----------------------------------------------------------

struct PositionUpdate {
  double position = 0.0;   // seconds; "meaningful" = finite and > 0 (A6).
  double duration = 0.0;   // seconds; 0 if unknown yet.
  std::int64_t for_id = 0;
  std::string episode;     // raw label being played.
  friend bool operator==(const PositionUpdate&, const PositionUpdate&) = default;
};

struct PlayDone {
  double final_position = 0.0;  // seconds; persisted only if meaningful.
  std::int64_t for_id = 0;
  std::string episode;
  friend bool operator==(const PlayDone&, const PlayDone&) = default;
};

struct PlayErrorEvent {
  PlayError cause;
  bool final = true;  // false while a retry/hop may still recover.
  std::int64_t for_id = 0;
  std::string episode;
  friend bool operator==(const PlayErrorEvent&, const PlayErrorEvent&) = default;
};

struct PlayRetry {
  std::uint32_t attempt = 0;
  std::uint32_t max = 0;
  friend bool operator==(const PlayRetry&, const PlayRetry&) = default;
};

// --- 4.5b AniList connect (P19, 06 §4.2) -----------------------------------
//
// Mirrors login::ConnectResult::Kind (login.hpp) field-for-field, but stays
// dependency-free of login.hpp/anilist.hpp/http.hpp: event.hpp is included by
// the curl-free render core (settings.hpp, queue.hpp), and login.hpp pulls in
// http.hpp transitively. app.cpp (the one curl-gated TU that spawns the
// connect worker) converts login::ConnectResult -> ConnectOutcome at the
// worker boundary.
enum class ConnectOutcome {
  Ok,
  NoToken,
  Rejected,
  NetworkError,
  SaveFailed,
  BadState,
  Canceled,
};

struct ConnectDone {
  ConnectOutcome outcome = ConnectOutcome::NoToken;
  std::string user_name;  // Ok only.
  friend bool operator==(const ConnectDone&, const ConnectDone&) = default;
};

// --- 4.5b* MAL connect (P31 §9.1 slice 2) -----------------------------------
//
// Mirrors mal_login::MalConnectResult::Kind (mal_login.hpp) field-for-field,
// same dependency-free-of-the-curl-touching-module reason as ConnectOutcome
// above (mal_login.hpp pulls in http.hpp transitively). A separate enum/
// struct rather than reusing ConnectOutcome/ConnectDone: MAL's NoClientId
// has no AniList analog (shigoku's own AniList app registration is baked
// in, so that failure mode cannot happen on that path), and folding it into
// the shared enum would force every ConnectOutcome switch (AniList-only
// today) to grow a dead arm.
enum class MalConnectOutcome {
  Ok,
  NoClientId,
  NoCode,
  Rejected,
  NetworkError,
  SaveFailed,
  BadState,
  Canceled,
};

struct MalConnectDone {
  MalConnectOutcome outcome = MalConnectOutcome::NoCode;
  friend bool operator==(const MalConnectDone&, const MalConnectDone&) = default;
};

// --- 4.5c AniList sync (P20, 06 §5) -----------------------------------------
//
// Mirrors sync::SyncOutcome/SyncSummary field-for-field, but stays dependency-
// free of sync.hpp/store.hpp: event.hpp is included by the curl-free render
// core; sync.hpp pulls in store.hpp (RemoteEntry/PullOutcome/SyncRow) and
// http.hpp transitively via anilist.hpp. app.cpp (the one curl+sqlite-gated TU
// that spawns the sync worker) converts sync::SyncSummary -> SyncFlushed at
// the worker boundary, flattening PullOutcome's counts into plain fields (the
// only three on_sync_flushed's toast logic reads: imported/reconciled/pushed).
enum class SyncOutcome {
  Completed,
  Disabled,
  NoToken,
  Expired,
  NoUserId,
  PullUnauthorized,
  PullRateLimited,
  Unauthorized,
  RateLimited,
  Failed,
};

struct SyncFlushed {
  SyncOutcome outcome = SyncOutcome::Disabled;
  std::uint32_t imported = 0;    // flattened from PullOutcome (store.hpp stays out).
  std::uint32_t reconciled = 0;  // flattened from PullOutcome.
  std::uint32_t pushed = 0;
  // P31 §9.1 slice 4: the MAL mirror push rides this same worker/event (no
  // drain rights of its own, PORT_PARITY.md P31) — its one flattened count,
  // same dependency-free rationale as the three AniList fields above
  // (mal_mirror.hpp stays out of event.hpp's link graph too).
  std::uint32_t mal_pushed = 0;
  friend bool operator==(const SyncFlushed&, const SyncFlushed&) = default;
};

// --- 4.5d Update check (P28, 06 §6.1) ---------------------------------------
//
// The boot-time worker's one possible result: a strictly-newer release tag was
// found. No-newer and every failure (offline, rate-limited, no cache dir) post
// nothing at all — a silent worker exit, same shape as the sync worker's
// silent no-op branches. Posted at most once, from the one boot-time spawn in
// run() (05 §14 "update_available… low-key whisper", fires once per session).
struct UpdateAvailable {
  std::string latest;  // sanitized tag (updatecheck::sanitize_tag), e.g. "v0.5.0".
  friend bool operator==(const UpdateAvailable&, const UpdateAvailable&) = default;
};

// --- 4.6 Prewarm (P15, 04 §4.3 / 05 §10.4) ---------------------------------
//
// The eager sibling-provider warm walk (prewarm.rs): after an add/first-play,
// background-probe the providers a show is not yet bound to and MINT the result
// into the store (a hidden binding or a fresh absence) so a later flip/fallback
// is tier-0. The verdict is the probe outcome the UI-thread on_result consumes.

// PrewarmVerdict (event.rs:61-66): a 3-way outcome of one candidate probe.
struct PrewarmVerdict {
  enum class Kind {
    Found,    // non-empty listing → bind provider_id.
    Absent,   // authoritative empty listing → mark absence.
    Nothing,  // transport error / clean search miss → write nothing.
  } kind = Kind::Nothing;
  std::string provider_id;  // Found only.
  friend bool operator==(const PrewarmVerdict&, const PrewarmVerdict&) = default;
};

// One prewarm probe result (event.rs:155-160). `token` is the prewarm
// subsystem's own generation (its own counter, distinct from search/episode/
// cover), checked by PrewarmState::on_result before any store mint (04 §6).
struct PrewarmResult {
  std::int64_t anilist_id = 0;
  std::string provider;         // the probed provider's name.
  PrewarmVerdict verdict;
  Generation token = 0;
  friend bool operator==(const PrewarmResult&, const PrewarmResult&) = default;
};

// --- 4.7 Enrichment refresh (P21, 05 §8 / 04 §10) --------------------------
//
// Refresh-on-view: while a detail surface is up, spend the network only when a
// show's metadata is stale (store.enrichment_stale). The worker posts one of
// the three below by `for_id`, checked against the shown show in tick(). The
// three-state contract (05 §8, ROD-278) rides these variants — a boolean
// "answered" flag would lose the middle (confirmed-null) case.

// The healed answer (event.rs EnrichmentRefreshed, :184). Held by value (the
// variant already carries Enrichment via CatalogRow); the app overwrites drift
// fields, stamps freshness, preserves user state.
struct EnrichmentRefreshed {
  std::int64_t for_id = 0;
  Enrichment enrichment;
  friend bool operator==(const EnrichmentRefreshed&, const EnrichmentRefreshed&) = default;
};

// Confirmed no-match (event.rs EnrichmentNull, :189): a true negative is an
// answer, not a failure — stamp freshness so it is never re-queried forever.
struct EnrichmentNull {
  std::int64_t for_id = 0;
  friend bool operator==(const EnrichmentNull&, const EnrichmentNull&) = default;
};

// No answer (event.rs EnrichmentFailed, :192): transport error / timeout /
// malformed. Never stamps freshness (never freeze a never-answered row); raises
// the ANILIST_TOPIC persistent toast.
struct EnrichmentFailed {
  std::int64_t for_id = 0;
  friend bool operator==(const EnrichmentFailed&, const EnrichmentFailed&) = default;
};

// --- 4.7a Detail zoom characters+recommendations (P36) ---------------------
//
// The `c`-toggle worker posts one of the three below by `for_id`, same
// three-state contract as Enrichment* above (a confirmed no-match is still
// an answer; only the transport-error arm carries nothing forward).

struct CharactersRecsDone {
  std::int64_t for_id = 0;
  CharactersAndRecommendations data;
  friend bool operator==(const CharactersRecsDone&, const CharactersRecsDone&) = default;
};

// Confirmed no-match (Media:null) — rare for an id already on screen, but
// kept distinct from Failed for the same reason EnrichmentNull is: a true
// negative is an answer, not "never asked".
struct CharactersRecsNull {
  std::int64_t for_id = 0;
  friend bool operator==(const CharactersRecsNull&, const CharactersRecsNull&) = default;
};

struct CharactersRecsFailed {
  std::int64_t for_id = 0;
  friend bool operator==(const CharactersRecsFailed&, const CharactersRecsFailed&) = default;
};

// --- 4.7b Discover filter overlay genre picker (P38, §9) --------------------
//
// The `f` overlay's single-flight genre fetch, session-cached (no for_id: the
// vocabulary is global, not per-show). Two-state, not three (see anilist.hpp
// genre_collection): no confirmed-absent shape for a global collection.
struct GenreCollectionDone {
  std::vector<std::string> genres;
  friend bool operator==(const GenreCollectionDone&, const GenreCollectionDone&) = default;
};

struct GenreCollectionFailed {
  friend bool operator==(const GenreCollectionFailed&, const GenreCollectionFailed&) = default;
};

// --- 4.8 Downloads (P35 slice 3) --------------------------------------------
//
// One transfer at a time (the app's DownloadState is the gate); no generation
// token — for_id + episode identify the transfer, and the active flag guards
// adoption. DownloadOutcome mirrors download::DownloadError::Kind
// dependency-free (download.hpp pulls http.hpp; event.hpp is included by the
// curl-free render core — the ConnectOutcome pattern). app.cpp converts at
// the worker boundary.

enum class DownloadOutcome {
  Fetch,
  Io,
  Cancelled,
  UnsafeUrl,
  UnsafeArg,
  FfmpegNotFound,
  Ffmpeg,
};

// Throttled progress tick (the worker posts at most ~2/sec, so a slow pty is
// never flooded — plus the final tick). total 0 = unknown (the ffmpeg arm posts no
// byte counts in v1; the statusline shows an indeterminate transfer).
struct DownloadProgress {
  std::uint64_t bytes = 0;
  std::uint64_t total = 0;
  std::int64_t for_id = 0;
  std::string episode;
  friend bool operator==(const DownloadProgress&, const DownloadProgress&) = default;
};

struct DownloadDone {
  std::int64_t for_id = 0;
  std::string episode;
  std::string dest;  // the final published path (toast copy).
  friend bool operator==(const DownloadDone&, const DownloadDone&) = default;
};

struct DownloadFailed {
  DownloadOutcome outcome = DownloadOutcome::Fetch;
  std::string detail;  // short human fragment (provider copy / errno text).
  std::int64_t for_id = 0;
  std::string episode;
  friend bool operator==(const DownloadFailed&, const DownloadFailed&) = default;
};

// --- The variant -----------------------------------------------------------
//
// v0 subset (PORT_CPP.md A2) + P15's PrewarmResult + P17's Discover* / GridCover*
// + P19's ConnectDone + P20's SyncFlushed + P21's Enrichment* + P28's
// UpdateAvailable + P32's MouseEvent + P35's Download* (§9). Remaining deferred events
// (History*) are NOT in the
// variant yet — adding one is additive, and until then no visitor is
// obligated to handle it. The A2 fence turns each addition into a compile
// error at every visitor until handled — never add a catch-all to shut it up.
using Event = std::variant<KeyEvent,
                           MouseEvent,
                           Resize,
                           Tick,
                           SearchDone,
                           SearchError,
                           DiscoverFeedDone,
                           DiscoverFeedError,
                           GridCoverDone,
                           GridCoverError,
                           EpisodesDone,
                           EpisodesError,
                           ProviderSearchDone,
                           ProviderSearchError,
                           CoverDone,
                           CoverError,
                           PositionUpdate,
                           PlayDone,
                           PlayErrorEvent,
                           PlayRetry,
                           PrewarmResult,
                           ConnectDone,
                           MalConnectDone,
                           SyncFlushed,
                           EnrichmentRefreshed,
                           EnrichmentNull,
                           EnrichmentFailed,
                           CharactersRecsDone,
                           CharactersRecsNull,
                           CharactersRecsFailed,
                           GenreCollectionDone,
                           GenreCollectionFailed,
                           UpdateAvailable,
                           DownloadProgress,
                           DownloadDone,
                           DownloadFailed>;

// overloaded{} — the visitor helper. Compose per-alternative lambdas; a
// visitor that omits an alternative is a COMPILE ERROR (no generic catch-all
// lambda is provided, by design — A2).
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace shigoku
