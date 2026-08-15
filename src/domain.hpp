// domain.hpp — core type vocabulary (P1). Ported from sabigoku src/domain.rs.
//
// Closed enums are `enum class` with NO default: arm anywhere (A2,
// -Werror=switch-enum). StreamLink carries the demuxer/proxy/sub bits with
// their doc-comment meaning preserved (P1 DoD). Pure helpers (title pick,
// control stripping, episode sort/remap) are free functions over string_view
// so later phases test them offline (§8 pure-function bias).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku {

// --- Resume thresholds (domain.rs:9, 02 §4b) -------------------------------
// The 02 §4b table is the single authority. fully_watched past 0.95; natural
// end (0.80) ratchets progress but does NOT mark fully_watched. Kept here (not
// the store) so the pure resume/ratchet logic tests offline (§8) and the store
// and player agree on one constant.
inline constexpr double kWatchedRatio = 0.95;
inline constexpr double kNaturalEndRatio = 0.80;

// Upper bound on an episode count hint (domain.rs MAX_EPISODE_HINT). A
// listing-less provider (megaplay) mints a 1..N grid off a caller hint; this
// caps the alloc so a hostile/absurd total can never size it. The full
// episode_count_hint clamp helper (domain.rs:141) lands with its owning phase;
// megaplay only needs the ceiling.
inline constexpr std::uint32_t kMaxEpisodeHint = 10000;

// The 02 §4b natural-end tier: ratchets progress, never marks fully_watched.
// Duration 0 (unknown) can never be a natural end; mirror the store's watched
// derivation so a zero never divides into a spurious restart.
inline bool natural_end(double position_secs, double duration_secs) {
  return duration_secs > 0.0 && position_secs / duration_secs >= kNaturalEndRatio;
}

// --- Translation (domain.rs:30) --------------------------------------------
// Sub/dub track. Rides search, episode lists, progress rows, resolve.
enum class Translation {
  Sub,
  Dub,
};

inline std::string_view to_string(Translation t) {
  switch (t) {
    case Translation::Sub: return "sub";
    case Translation::Dub: return "dub";
  }
  return "";  // unreachable (closed enum).
}

inline std::optional<Translation> parse_translation(std::string_view s) {
  if (s == "sub") return Translation::Sub;
  if (s == "dub") return Translation::Dub;
  return std::nullopt;
}

// --- Quality (domain.rs:165) -----------------------------------------------
// Stream quality pref. best/worst pick the extremum; rungs are a pixel cap.
// Providers with no adaptive master (megaplay, anibd — Rust never caps these
// either) accept-and-ignore this (A8). Providers that DO advertise a master
// ladder (senshi) cap via hls::parse_master_playlist/select_variant (P23);
// Best still always hands mpv the master untouched.
enum class Quality {
  Best,
  P1080,
  P720,
  P480,
  Worst,
};

// Config string form; unknown falls to Best (safe default).
inline Quality parse_quality(std::string_view s) {
  if (s == "worst") return Quality::Worst;
  if (s == "480") return Quality::P480;
  if (s == "720") return Quality::P720;
  if (s == "1080") return Quality::P1080;
  return Quality::Best;
}

// Pixel ceiling, or nullopt for best/worst.
inline std::optional<std::uint32_t> quality_cap(Quality q) {
  switch (q) {
    case Quality::P1080: return 1080u;
    case Quality::P720:  return 720u;
    case Quality::P480:  return 480u;
    case Quality::Best:
    case Quality::Worst: return std::nullopt;
  }
  return std::nullopt;  // unreachable.
}

// --- Season (domain.rs:198) ------------------------------------------------
// Broadcast cour. AniList spellings fold here; render maps to kanji.
enum class Season {
  Winter,
  Spring,
  Summer,
  Fall,
};

// Case-insensitive; "autumn" folds to Fall. Unknown/empty is nullopt.
std::optional<Season> parse_season(std::string_view s);

// Canonical lowercase spelling for the SQLite `season` column (domain.rs
// Season::as_str). Round-trips through parse_season; the store persists this.
inline std::string_view season_as_str(Season s) {
  switch (s) {
    case Season::Winter: return "winter";
    case Season::Spring: return "spring";
    case Season::Summer: return "summer";
    case Season::Fall:   return "fall";
  }
  return "winter";  // unreachable (closed enum).
}

// Month 1..=12 → AniList cour (domain.rs Season::from_month, ROD-186). December
// is next-year Winter; the year roll is the caller's (current_cour). An
// out-of-range month (0, ≥12) folds to Winter.
inline Season season_from_month(std::uint32_t month) {
  if (month >= 3 && month <= 5) return Season::Spring;
  if (month >= 6 && month <= 8) return Season::Summer;
  if (month >= 9 && month <= 11) return Season::Fall;
  return Season::Winter;
}

// --- DiscoverAxis (providers.rs:253) ---------------------------------------
// Discover ranking axes; variant order is the freeze enum order AND the UI tab
// order (04 §7.5, DESIGN §3.8). Rank is positional per axis, never one shared
// list re-sorted. Shigoku has no CatalogProvider trait — AniList is the only
// catalog (01 §5) — so this lives in domain next to Season, not on a trait.
enum class DiscoverAxis {
  Trending,
  Popular,
  TopRated,
  ThisSeason,
};

// --- DiscoverFilters (P38, §9 — no Rust precedent) --------------------------
// Session-scoped narrowing over the active Discover feed, composed into
// discover_body's query variables (anilist.hpp). Global across all four axis
// slots (not per-axis): the axis picks the ranking, the filters narrow it.
// `status` is the raw AniList MediaStatus string ("RELEASING", "FINISHED",
// …), the same convention Enrichment::status already uses — no local enum.
// `min_score` is 0..=100 (AniList averageScore's own scale, matching
// Enrichment::score).
struct DiscoverFilters {
  std::vector<std::string> genres;
  std::optional<std::uint32_t> year;
  std::optional<std::string> status;
  std::optional<std::uint32_t> min_score;

  [[nodiscard]] bool empty() const {
    return genres.empty() && !year.has_value() && !status.has_value() &&
           !min_score.has_value();
  }

  friend bool operator==(const DiscoverFilters&, const DiscoverFilters&) = default;
};

// --- Cour (domain.rs:261) --------------------------------------------------
// Broadcast cour at a wall-clock instant; anchors the This Season discover
// axis. current_cour rolls December into next-year Winter (ROD-186).
struct Cour {
  Season season = Season::Winter;
  std::uint32_t year = 0;
  friend bool operator==(const Cour&, const Cour&) = default;
};

// Cour for a unix timestamp (domain.rs current_cour, ROD-186): a pre-epoch
// instant clamps to 1970 Winter; December carries the month's year roll into
// next-year Winter.
[[nodiscard]] Cour current_cour(std::int64_t unix_secs);

// --- TitleLanguage (domain.rs:295) -----------------------------------------
// Primary title form. No Auto: english already falls back.
enum class TitleLanguage {
  Romaji,
  English,
  Native,
};

// Config string form; unknown falls to Romaji (the universal backstop).
inline TitleLanguage parse_title_language(std::string_view s) {
  if (s == "english") return TitleLanguage::English;
  if (s == "native") return TitleLanguage::Native;
  return TitleLanguage::Romaji;
}

// --- ListStatus (domain.rs:55) ---------------------------------------------
// Watchlist state per show. Planning is the default; auto-status after a play
// goes through after_status (ROD-139/296). The store persists as_string();
// manual pause/drop/force is a later phase (P16 History), so only Planning →
// Watching → Completed transitions are reachable in v0.
enum class ListStatus {
  Planning,
  Watching,
  Paused,
  Completed,
  Dropped,
};

inline std::string_view to_string(ListStatus s) {
  switch (s) {
    case ListStatus::Planning:  return "planning";
    case ListStatus::Watching:  return "watching";
    case ListStatus::Paused:    return "paused";
    case ListStatus::Completed: return "completed";
    case ListStatus::Dropped:   return "dropped";
  }
  return "planning";  // unreachable (closed enum).
}

// Unknown or empty maps to planning: never invent an active state.
inline ListStatus parse_list_status(std::string_view s) {
  if (s == "watching")  return ListStatus::Watching;
  if (s == "paused")    return ListStatus::Paused;
  if (s == "completed") return ListStatus::Completed;
  if (s == "dropped")   return ListStatus::Dropped;
  return ListStatus::Planning;
}

// --- ScoreFormat (P34 slice 2, 06 §4.2 Viewer.mediaListOptions.scoreFormat) -
// The account's chosen AniList scale, fetched at connect and persisted in
// auth.json (AniListAuth::score_format) so push/pull can convert the store's
// canonical raw 0..=100 to/from whatever SaveMediaListEntry(score:) expects
// for this account. Point100 is the default: an account never connected
// under this phase (or whose Viewer read omitted the field) pushes/reads
// raw values unconverted, matching the store's own storage format.
enum class ScoreFormat {
  Point100,
  Point10Decimal,
  Point10,
  Point5,
  Point3,
};

inline std::string_view to_string(ScoreFormat f) {
  switch (f) {
    case ScoreFormat::Point100:      return "POINT_100";
    case ScoreFormat::Point10Decimal: return "POINT_10_DECIMAL";
    case ScoreFormat::Point10:       return "POINT_10";
    case ScoreFormat::Point5:        return "POINT_5";
    case ScoreFormat::Point3:        return "POINT_3";
  }
  return "POINT_100";  // unreachable (closed enum).
}

// Unknown/empty maps to Point100: never guess a lossier scale than the wire
// said (an unrecognized string is far more likely a future AniList format
// this build doesn't know yet than a lossy one).
inline ScoreFormat parse_score_format(std::string_view s) {
  if (s == "POINT_10_DECIMAL") return ScoreFormat::Point10Decimal;
  if (s == "POINT_10")         return ScoreFormat::Point10;
  if (s == "POINT_5")          return ScoreFormat::Point5;
  if (s == "POINT_3")          return ScoreFormat::Point3;
  return ScoreFormat::Point100;
}

// Auto-status after a play (domain.rs:after_play, ROD-139/296): Completed is
// sticky; a still-airing show never auto-completes (total may be aired-so-far,
// ROD-296); otherwise complete only at a known positive total reached. Manual
// pause/drop/force goes through the History setListStatus path (P16), not here.
inline ListStatus after_play_status(ListStatus cur, std::uint32_t progress,
                                    std::optional<std::uint32_t> total,
                                    bool still_airing) {
  if (cur == ListStatus::Completed) return ListStatus::Completed;
  if (still_airing) return ListStatus::Watching;
  if (total.has_value() && *total > 0 && progress >= *total)
    return ListStatus::Completed;
  return ListStatus::Watching;
}

// --- StreamLink (domain.rs:442) --------------------------------------------
// Playable stream for mpv. EVERY field is provider-derived and untrusted;
// referer/UA/sub_url are sanitized/guarded before they touch argv (A5/A6).
struct StreamLink {
  std::string url;
  std::optional<std::uint32_t> resolution;
  std::optional<std::string> referer;
  // Browser-shaped UA for CDN bot scoring; nullopt = player default.
  std::optional<std::string> user_agent;
  // HLS segments cloaked as .jpg: player relaxes its demuxer gate via
  // --demuxer-lavf-o=allowed_extensions=ALL (A6). senshi sets this true.
  bool cloaked_segments = false;
  // Segments carry a decoy image-header PREFIX before the real TS; no ffmpeg
  // flag reaches the inner demuxer, so playback would route through the local
  // stripping proxy instead of handing mpv the url. senshi sets this FALSE and
  // v0 ships no proxy (PORT_CPP.md §2.3) — the field exists so the M2 megaplay
  // path is additive, not a rewrite.
  bool decloak_segments = false;
  // External WebVTT softsub; nullopt if hardsub or none. Reaches mpv --sub-file
  // UNPROXIED, so its URL must pass the fetch guard at resolve time (A5).
  std::optional<std::string> sub_url;

  friend bool operator==(const StreamLink&, const StreamLink&) = default;
};

// --- Date (domain.rs:254) --------------------------------------------------
// AniList fuzzy start date: a known year, month/day may be absent. Feeds the
// `start_year/month/day` enrichment columns.
struct Date {
  std::uint32_t year = 0;
  std::optional<std::uint32_t> month;
  std::optional<std::uint32_t> day;

  friend bool operator==(const Date&, const Date&) = default;
};

// --- Enrichment (domain.rs, catalog row) -----------------------------------
// AniList-shaped metadata: the catalog row the detail pane renders and the
// resolve tier-B/C scorers read. No user state ever lives here. The full 02
// fieldset ships now (P14): the enrichment merge writers and catalog_cache
// round-trip every column, so a partial init (P3 search) leaves the rest
// default and the store's incoming-NULL-never-wipes merge keeps prior values.
struct Enrichment {
  std::int64_t anilist_id = 0;
  // Secondary bridge id (senshi/megaplay tier-A key, AniSkip). Non-unique in
  // the wild, NEVER a key. nullopt = no MAL id -> tier-C search path (A8).
  std::optional<std::int64_t> mal_id;
  std::string title_romaji;
  std::optional<std::string> title_english;
  std::optional<std::string> title_native;
  std::optional<std::string> cover_url;
  std::optional<std::uint32_t> total_episodes;
  std::optional<std::uint32_t> duration_minutes;
  std::optional<std::uint32_t> year;
  std::optional<Season> season;
  // Raw AniList media status ("RELEASING", "FINISHED", …); consumed
  // un-normalized by is_still_airing.
  std::optional<std::string> status;
  std::optional<std::string> description;
  std::optional<std::uint32_t> score;  // AniList averageScore 0..=100.
  std::optional<std::string> kind;     // format ("TV", "MOVIE", …).
  std::optional<Date> start_date;
  std::vector<std::string> genres;
  std::vector<std::string> studios;
  std::optional<std::string> source_material;  // AniList `source`.
  std::optional<std::uint32_t> rank;
  std::optional<std::string> rank_type;
  std::optional<std::uint32_t> rank_year;
  std::optional<std::int64_t> next_airing_at;        // unix secs of next ep.
  std::optional<std::uint32_t> next_airing_episode;  // 1-based ordinal.
  std::optional<std::string> country;

  friend bool operator==(const Enrichment&, const Enrichment&) = default;
};

// --- Show (domain.rs, library row) -----------------------------------------
// A library `show` row: identity + enrichment (above) plus user state and
// enrichment-freshness stamps. Read by get_show/list_history; the store's
// user-state writers own each field, never the enrichment merge (02 §5).
struct Show {
  Enrichment enrichment;
  std::optional<std::int64_t> enrichment_fetched_at;
  std::optional<std::uint32_t> enrichment_fieldset_version;
  ListStatus list_status = ListStatus::Planning;
  std::optional<double> user_rating;  // user's own 0..=10 (≠ AniList score).
  std::optional<std::string> notes;
  std::uint32_t play_count = 0;
  // Unclamped on purpose: overshoot past total still counts as completed; the
  // 14/2 clamp is render-time only (02 §4b).
  std::uint32_t progress = 0;
  // NULL = identity row only (bindable/probeable), NOT in the library (02 §3.7).
  std::optional<std::int64_t> library_added_at;
  std::optional<std::int64_t> last_watched_at;
  std::optional<ListStatus> synced_status;
  std::optional<std::uint32_t> synced_progress;
  // P34: raw 0..=100, AniList's POINT_100 canonical storage; 0/nullopt = unset
  // (≠ user_rating above, which is the dead zimport-only field, P29 waived).
  std::optional<std::uint32_t> user_score;
  std::optional<std::uint32_t> synced_score;
  // P37 slice 3: the highest episode number ever notified as newly-aired for
  // this show (nullopt = never notified). A high-water mark, not a set — an
  // episode number once notified is never re-raised, and airing episode
  // numbers only increase, so a scalar suffices (same shape as
  // synced_progress). Persists across restarts.
  std::optional<std::uint32_t> notice_last_episode;
  // P37 slice 3: the History NEW marker (cleared when the show's detail is
  // opened, from any origin view — the marker is a "you have an unseen
  // update" flag, not itself the dedup guard; notice_last_episode is).
  bool notice_pending = false;

  friend bool operator==(const Show&, const Show&) = default;
};

// --- CharacterEntry (P36) ---------------------------------------------------
// One row of the detail zoom's Characters section: name/role/VA, text-only
// (no portraits in v1 — the cover pool stays show-keyed).
struct CharacterEntry {
  std::string name;
  std::string role;                      // AniList CharacterRole ("MAIN", …).
  std::optional<std::string> va_name;    // first Japanese VA, if staffed.

  friend bool operator==(const CharacterEntry&, const CharacterEntry&) = default;
};

// --- CharactersAndRecommendations (P36) -------------------------------------
// One show's on-demand `c`-section answer. Recommendations reuse Enrichment
// (the catalog_cache row shape) so Enter-to-promote is a direct
// upsert_catalog_cache call — no separate id/title/score struct to map from.
struct CharactersAndRecommendations {
  std::vector<CharacterEntry> characters;
  std::vector<Enrichment> recommendations;

  friend bool operator==(const CharactersAndRecommendations&,
                         const CharactersAndRecommendations&) = default;
};

// ---------------------------------------------------------------------------
// Pure helpers (title/episode). Ported 1:1 from domain.rs; unit-tested offline.
// ---------------------------------------------------------------------------

// strip_controls (domain.rs:strip_controls, ROD-247/435/439): drop
// terminal-hostile codepoints from untrusted free text before it reaches a
// cell or an argv — C0 + DEL, C1, bidi overrides/isolates, zero-width, BOM.
// Operates on a UTF-8 string; decodes codepoints byte-wise (no locale, §3).
std::string strip_controls(std::string_view s);

// is_absolute_url (domain.rs:24): scheme check only, http(s):// prefix.
// Callers still run guard_fetch_url before any fetch — this alone is not a
// safety boundary (used to reject scheme-less URLs before an argv splice).
bool is_absolute_url(std::string_view s);

// preferred_title (domain.rs:preferred_title, ROD-205, DESIGN §9.1a): primary
// title under pref; romaji is the universal backstop. A blank string never
// wins the chain. Returns a view into one of the arguments (or empty romaji).
std::string_view preferred_title(std::string_view romaji,
                                 std::optional<std::string_view> english,
                                 std::optional<std::string_view> native,
                                 TitleLanguage pref);

// episode_sort_key (domain.rs:episode_sort_key): leading digits+dots parse as
// double; non-numeric labels ("SP1") return +inf so specials sort after the
// numbered run. Grid ordering and store recompute both key on this.
double episode_sort_key(std::string_view label);

// episode_label_cmp: total order over labels via the sort key. Ties (all
// specials are +inf) compare equal; a stable sort keeps incoming order.
// Returns <0, 0, >0.
int episode_label_cmp(std::string_view a, std::string_view b);

// parse_user_score (P34, History `s` prompt): free text -> raw 0..=100 store
// value. Accepts 0-10 with up to one decimal point, ×10 and rounds to the
// nearest int. Tri-state result: a blank/whitespace-only string is Clear
// (caller maps it to nullopt, i.e. unset); a full-string, in-range parse is
// Value; anything else (junk, out-of-range, partial parse) is Invalid.
enum class ScoreParse { Clear, Value, Invalid };
struct ParsedScore {
  ScoreParse kind = ScoreParse::Invalid;
  std::uint32_t value = 0;  // meaningful only when kind == Value.
};
ParsedScore parse_user_score(std::string_view input);

// to_anilist_score / from_anilist_score (P34 slice 2, 06 §4.2): convert
// between the store's canonical raw 0..=100 (AniList's own POINT_100 scale)
// and the wire value SaveMediaListEntry(score:)/mediaList.score expect under
// the account's scoreFormat. 0 (unset) maps to 0.0 under every format — a
// genuine zero score is indistinguishable from unset in AniList's own model,
// so this mirrors AniList's own lossiness rather than inventing a sentinel.
// Point10Decimal is float-valued (7.5); every other format is integral, so
// to_anilist_score rounds to the nearest representable step (round-half-up,
// matching parse_user_score's own rounding) and from_anilist_score scales
// back up assuming the wire value already sits on that step.
[[nodiscard]] double to_anilist_score(std::uint32_t raw, ScoreFormat format);
[[nodiscard]] std::uint32_t from_anilist_score(double wire, ScoreFormat format);

// map_episode_index (domain.rs:map_episode_index, 03 §6.6): map an episode
// onto another provider's grid — exact raw label first, else 1-based ordinal
// into the sorted list if in range. String equality is identity; ordinal is
// best-effort hop UX, never a second progress key (02 L1). nullopt = no map.
std::optional<std::size_t> map_episode_index(const std::vector<std::string>& episodes,
                                             std::string_view raw,
                                             std::uint32_t ordinal);

// is_still_airing (domain.rs:is_still_airing): a show settles only on FINISHED
// or CANCELLED (case-insensitive per the Rust tests); everything else —
// RELEASING, HIATUS, NOT_YET_RELEASED, unknown, or absent — is still airing.
bool is_still_airing(std::optional<std::string_view> status);

// --- Schedule (P37 slice 1) -------------------------------------------------
// Pure weekday-grouping + countdown lens over the watchlist's next_airing_*
// columns (already stored per-show, P21). No wall clock in here: `now_secs`
// is a caller-supplied stamp (boot/tick, never sampled inside this file).

// ISO-ish weekday of a `next_airing_at` instant, in UTC (the store's
// next_airing_at is UTC unix secs). KNOWN LIMITATION, not a plan mandate: no
// local-tz conversion in v1, so a JST-evening airing can group under the
// wrong local weekday for non-UTC users — localtime_r plumbing is a welcome
// follow-up. Monday-first to match ISO 8601; `weekday_of` is exposed for
// tests.
enum class Weekday {
  Monday,
  Tuesday,
  Wednesday,
  Thursday,
  Friday,
  Saturday,
  Sunday,
};
[[nodiscard]] Weekday weekday_of(std::int64_t unix_secs);

// One watchlist row with a future airing: the show's identity/title (via the
// Show's own enrichment — this struct just carries the derived fields) plus
// the countdown split at `now_secs`. `seconds_until` is always > 0 (schedule()
// filters out past/absent airings); render formats it into e.g. "2d 4h".
struct ScheduleEntry {
  std::size_t show_index = 0;  // index into the input `shows` vector.
  Weekday weekday = Weekday::Monday;
  std::int64_t airing_at = 0;
  std::uint32_t episode = 0;
  std::int64_t seconds_until = 0;
  friend bool operator==(const ScheduleEntry&, const ScheduleEntry&) = default;
};

// One weekday's schedule rows, airing-time ordered.
struct ScheduleGroup {
  Weekday weekday = Weekday::Monday;
  std::vector<ScheduleEntry> entries;
};

// schedule() (P37 slice 1): watchlist rows with a future next_airing_at,
// grouped by weekday (Monday..Sunday, only non-empty weekdays present),
// entries within a group airing-time ordered. Rows with no next_airing_at, or
// with next_airing_at <= now_secs, are excluded (already aired / unknown).
// Pure and total; `shows` is read-only (indices into it feed ScheduleEntry).
[[nodiscard]] std::vector<ScheduleGroup> schedule(const std::vector<Show>& shows,
                                                  std::int64_t now_secs);

// --- Schedule notices (P37 slice 3) -----------------------------------------
// Pure detection of "an episode aired since we last looked": the stored pair
// means "episode next_airing_episode airs AT next_airing_at" (AniList's
// nextAiringEpisode {episode airingAt} — the same reading the Schedule view
// renders), so once next_airing_at drops to or below `now_secs` that episode
// ITSELF has aired. (Not episode−1 — that misreading missed every series
// premiere and marked all notices one episode low; P37 review.) Compared
// against the show's own notice_last_episode high-water mark so a re-check
// (another boot, another sync pull) never re-raises the same episode twice.

// One detected notice: the show and the episode number that just aired.
struct ScheduleNotice {
  std::size_t show_index = 0;  // index into the input `shows` vector.
  std::uint32_t episode = 0;   // the episode that aired (== next_airing_episode).
};

// Scan `shows` for rows whose currently-known next_airing_at has passed
// (<= now_secs) and whose implied aired episode is newer than
// notice_last_episode. Pure and total; does not mutate `shows` or persist
// anything — the caller (app.cpp) owns writing notice_last_episode/
// notice_pending back through the store and raising the aggregate toast.
[[nodiscard]] std::vector<ScheduleNotice> detect_schedule_notices(
    const std::vector<Show>& shows, std::int64_t now_secs);

}  // namespace shigoku
