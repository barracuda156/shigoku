// store.hpp — SQLite persistence, PK'd by AniList key from day one (P9, A7).
//
// Ported from sabigoku src/store.rs, scoped to the P9 subset: the two v1 tables
// (`show` identity + user state, `episode_progress` resume/watched) on the 02
// §3.3 key shapes, so P14's growth toward the full schema is additive
// migrations, never a table rewrite. The full store.rs is 4.8k lines; this port
// is the 02 schema + resume/progress laws (§1.1 rule 4: contracts, not lines).
//
// Ownership (A7 / 04 model): the UI thread owns the Store. Workers NEVER hold
// the connection — they post events and tick() writes. There is no locking
// here and the type is deliberately non-copyable, single-owner. The link graph
// keeps the render core (shigoku_tui_render) off this library entirely, so
// draw()/views provably cannot reach the store (02 §5 / PORT_CPP.md A2).
//
// One documented exception (P20, 06 §5): the AniList sync worker opens its OWN
// short-lived Store::open(db_path) connection inside its detached-thread
// lambda (app.cpp's spawn_sync/spawn_quit_flush_worker), closed when the run
// ends. This is safe, not a hole in the rule above: WAL mode (this file, since
// P9) supports a second connection to the same file, and every write the sync
// worker makes goes through Store::reconcile_pull's CAS-guarded UPDATE or
// Store::mark_synced's unconditional advance — both built specifically so a
// concurrent UI-thread writer either loses the race cleanly (contended,
// retried next run) or cannot corrupt state (mark_synced records "what the
// server now holds", true regardless of a racing local edit). store_tests.cpp
// and sync_tests.cpp's two-connection tests exercise exactly this race. No
// other worker gets this exception — the whole point of the UI-thread-owns-
// Store rule is that sync's CAS machinery is NOT free to reuse elsewhere
// without its own review.
//
// Independent store (02 L3): this file never opens, migrates, or imports a
// zigoku DB. Its ladder starts at 1 for the 02 §3.3 shape.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "result.hpp"

struct sqlite3;  // opaque; the C handle lives only in the .cpp.

namespace shigoku {

// --- StoreError (store.rs error subset, mapped to the A2 closed-enum idiom) --
//
// A `Kind` enum class (the closed set the -Werror=switch-enum fence guards)
// plus the payload fields a kind uses (error.hpp idiom). SQLite failures fold
// into Kind::Sqlite with the driver message in `detail`; the schema/version
// guards (02 §5) get named kinds so open can refuse a too-new or half-applied
// DB loud, before any write.
struct StoreError {
  enum class Kind {
    Sqlite,             // any sqlite3_* failure   (detail = driver message)
    SchemaTooNew,       // DB newer than we support (found/expected)
    SchemaMissing,      // version stamp over absent DDL (foreign/partial file)
    SchemaInvalid,      // negative user_version    (found)
    MigrationIncomplete,// ladder stopped mid-way   (found/expected)
    ReadOnlyDb,         // file denies READWRITE    (detail = path)
    NonFinitePosition,  // NaN/Inf position/duration reached a write (detail)
  };

  Kind kind;
  std::string detail;            // driver message / path / context.
  std::uint32_t found = 0;       // schema kinds: the version we read.
  std::uint32_t expected = 0;    // schema kinds: the version we wanted.

  static StoreError sqlite(std::string why) { return {Kind::Sqlite, std::move(why)}; }
  static StoreError schema_too_new(std::uint32_t f, std::uint32_t e) {
    return {Kind::SchemaTooNew, {}, f, e};
  }
  static StoreError schema_missing(std::uint32_t v) { return {Kind::SchemaMissing, {}, v, v}; }
  static StoreError schema_invalid(std::uint32_t f) { return {Kind::SchemaInvalid, {}, f}; }
  static StoreError migration_incomplete(std::uint32_t at, std::uint32_t exp) {
    return {Kind::MigrationIncomplete, {}, at, exp};
  }
  static StoreError read_only(std::string path) { return {Kind::ReadOnlyDb, std::move(path)}; }
  static StoreError non_finite(std::string why) { return {Kind::NonFinitePosition, std::move(why)}; }
};

// The 02 §3.3 schema version this build writes. P9 shipped v1 (show +
// episode_progress, show carrying the FULL 02 column set). P14 adds v2: the
// seven remaining 02 tables (provider_binding, episode_cache, provider_pin,
// provider_absence, provider_route, catalog_cache, app_meta). The v2 end state
// is the same table set as sabigoku's own v2, so a sabigoku DB opens here and
// vice versa (02 §3.6 shared key shapes). P31 slice 4 adds v3: two columns on
// `show` for the MAL mirror's own snapshot (mal_synced_status/progress) — no
// sabigoku equivalent exists (MAL mirror is shigoku-only, §9 P31), so v3 has
// no cross-store compat claim past v2. v4 (P34 slice 1) adds user_score/
// synced_score; v5 (P34 slice 2) adds mal_synced_score, the mirror's own
// score snapshot. v6 (P37 slice 3) adds notice_last_episode/notice_pending,
// the schedule new-episode notice's dedup high-water mark + History NEW
// marker — shigoku-only (no sabigoku Schedule view exists), no cross-store
// compat claim past v2. Bumping this is a migration event, never a silent
// DDL add (02 §5).
inline constexpr std::uint32_t kSchemaVersion = 6;

// The 03 §6.3.1 resume offset default: back up this many seconds from the saved
// position so playback re-establishes context (config-overridable in P18).
inline constexpr std::uint32_t kResumeOffsetSecs = 5;

// Which enrichment column set a row was filled under (02 §5 CLONE idea,
// renumbered for this schema). Bump when columns are added so older rows heal
// on view instead of waiting out the full TTL. Mirrors store.rs
// ENRICHMENT_FIELDSET_VERSION.
inline constexpr std::uint32_t kEnrichmentFieldsetVersion = 1;

// Status-aware enrichment heal TTL (02 §5). Statuses are AniList media status
// only; legacy provider spellings do not exist in this store (store.rs).
inline constexpr std::int64_t kEnrichTtlFinishedSecs = 30 * 24 * 60 * 60;
inline constexpr std::int64_t kEnrichTtlReleasingSecs = 24 * 60 * 60;
inline constexpr std::int64_t kEnrichTtlDefaultSecs = 7 * 24 * 60 * 60;

[[nodiscard]] std::int64_t enrichment_ttl_secs(std::optional<std::string_view> status);

// "Not stocked" negative-cache TTL (ROD-347): a stale absence reads as
// unchecked so resolve re-probes (store.rs ABSENCE_TTL_SECS).
inline constexpr std::int64_t kAbsenceTtlSecs = 7 * 24 * 60 * 60;

// Episode-list cache TTL by airing status (ROD-68 shape, AniList statuses
// only, like enrichment_ttl_secs; store.rs EP_CACHE_TTL_*).
inline constexpr std::int64_t kEpCacheTtlFinishedSecs = 7 * 24 * 60 * 60;
inline constexpr std::int64_t kEpCacheTtlReleasingSecs = 6 * 60 * 60;
inline constexpr std::int64_t kEpCacheTtlDefaultSecs = 24 * 60 * 60;

[[nodiscard]] std::int64_t episode_cache_ttl_secs(std::optional<std::string_view> status);

// A durable Browse/Discover/detail hit (02 §3.5). No user state, ever.
struct CatalogHit {
  Enrichment enrichment;
  std::uint32_t fieldset_version = 0;
  std::int64_t fetched_at = 0;
  std::optional<std::int64_t> expires_at;

  friend bool operator==(const CatalogHit&, const CatalogHit&) = default;
};

// One provider offering edge for a show (store.rs Binding).
struct Binding {
  std::string provider;
  std::string provider_id;
  std::int64_t bound_at = 0;

  friend bool operator==(const Binding&, const Binding&) = default;
};

// Detail-rail availability (ROD-348): bound wins even if a negative coexists
// (external edit); bind clears negatives on mint (store.rs ProviderAvailability).
enum class ProviderAvailability {
  Unchecked,
  Bound,
  Absent,
};

// A resume point read back from episode_progress (store.rs Resume).
struct Resume {
  double position_secs = 0.0;
  double duration_secs = 0.0;
  bool fully_watched = false;

  // The 03 §6.3.1 resume start rule: restart at 0 when fully watched, past the
  // natural end (0.80), or the saved position is unusable (non-finite / <= 0);
  // else back up by resume_offset_secs, saturating at 0. An unknown (0)
  // duration still resumes — only a real ratio restarts.
  [[nodiscard]] double start_secs(std::uint32_t resume_offset_secs = kResumeOffsetSecs) const;

  friend bool operator==(const Resume&, const Resume&) = default;
};

// Per-episode watched flag for the grid (05 §10.5 watched marks). Parallel to a
// list of raw labels the caller passes in; true = a fully_watched row exists for
// that label under the active translation.
using WatchedMarks = std::vector<bool>;

// --- AniList sync (P20, 06 §5; score widened P34 slice 2) -------------------
//
// (status, progress, score) triple carried through the sync guard/reconcile
// path (RemoteEntry, SyncRow.synced, SyncPlanRow's three pairs, reconcile()'s
// params, AniListSync::fetch_entry/save_entry). Named fields rather than a
// bare std::tuple since every call site reads/writes by meaning, not
// position. MAL mirror (P31, deliberately separate machinery) is NOT widened
// to this shape — its guard pair stays (ListStatus, progress) with score
// threaded as its own sibling field, matching the "not a generalization of
// the 06 §5 sync law" ruling (mal_mirror.hpp).
struct ListEntry {
  ListStatus status = ListStatus::Planning;
  std::uint32_t progress = 0;
  // Raw 0..=100 (AniList's own POINT_100 canonical storage), same scale as
  // Show::user_score; conversion to/from the account's scoreFormat happens
  // only at anilist.cpp's wire boundary.
  std::uint32_t score = 0;

  friend bool operator==(const ListEntry&, const ListEntry&) = default;
};

// One remote MediaListCollection entry, mapped to the domain (06 §5.4).
// Defined here (not anilist.hpp) so store.hpp — sqlite-only, curl-free — stays
// the leaf; anilist.cpp depends on store.hpp to build these, not the reverse.
struct RemoteEntry {
  std::int64_t anilist_id = 0;
  ListStatus status = ListStatus::Planning;
  std::uint32_t progress = 0;
  std::uint32_t score = 0;  // raw 0..=100, already converted from the wire scale.
  // Server edit time, the tiebreak when one media appears in several custom
  // lists (06 §5.4). 0 when the wire omitted it.
  std::int64_t updated_at = 0;
  // Title + episodes for an unmatched WATCHING/REPEATING entry's auto-import
  // (06 O3). nullopt when the wire omitted the media node.
  std::optional<Enrichment> import_seed;

  friend bool operator==(const RemoteEntry&, const RemoteEntry&) = default;
};

// Push work-list row (store.rs SyncRow): a library show's live pair plus the
// last server-accepted snapshot, or nullopt if never synced.
struct SyncRow {
  std::int64_t anilist_id = 0;
  std::string title_romaji;
  ListStatus list_status = ListStatus::Planning;
  std::uint32_t progress = 0;
  std::uint32_t score = 0;
  std::optional<ListEntry> synced;

  friend bool operator==(const SyncRow&, const SyncRow&) = default;
};

// Tally from one pull reconcile (06 §5.4, O3). `imported`: unmatched
// WATCHING/REPEATING entries minted into the library from their seed.
// `unmatched`: remaining remote ids with no library row, counted not imported.
//
// `contended` carries ids, not a count: the push gates on them (06 §5.3). A row
// whose merge failed to land holds a pre-merge pair against a snapshot that
// never advanced, so pushing it in the same run would overwrite the very
// remote change the pull was carrying (ROD-500).
struct PullOutcome {
  std::uint32_t reconciled = 0;
  std::uint32_t conflicts = 0;
  std::vector<std::int64_t> contended;
  std::uint32_t imported = 0;
  std::vector<std::int64_t> unmatched;

  friend bool operator==(const PullOutcome&, const PullOutcome&) = default;
};

// One planned reconcile write, guarded on the pre-merge local pair (store.rs
// PlanRow). Exposed only so tests can drive reconcile_plan/apply_reconcile
// as two steps, with a concurrent writer landing between them (the CAS race
// reconcile_pull's own read-then-write only exercises implicitly).
struct SyncPlanRow {
  std::int64_t id = 0;
  ListEntry guard;
  ListEntry merged;
  ListEntry snapshot;
  bool conflict = false;
};

// One planned auto-import (store.rs ImportRow).
struct SyncImportRow {
  Enrichment seed;
  ListStatus status = ListStatus::Planning;
  std::uint32_t progress = 0;
  std::uint32_t score = 0;
};

struct SyncPlan {
  std::vector<SyncPlanRow> plan;
  std::vector<SyncImportRow> imports;
  std::vector<std::int64_t> unmatched;
};

// --- MAL mirror (P31 §9.1 slice 4) -------------------------------------------
//
// Push work-list row for the MAL mirror, parallel to SyncRow but keyed to a
// SEPARATE snapshot column pair (mal_synced_status/mal_synced_progress, v3):
// the two mirrors are independent client relationships against the same live
// (list_status, progress) pair, so one snapshot must never gate the other
// (PORT_PARITY.md P31 — "not a generalization of the 06 §5 sync law"). Only
// library rows with a non-NULL mal_id are ever produced here; the NULL-mal_id
// skip is the SQL query's WHERE clause, not a runtime branch.
struct MalMirrorRow {
  std::int64_t anilist_id = 0;
  std::int64_t mal_id = 0;
  ListStatus list_status = ListStatus::Planning;
  std::uint32_t progress = 0;
  // MAL's own native 0..=10 int scale (P34 slice 2), converted from the
  // store's raw 0..=100 at the SQL read (store.cpp's list_dirty_for_mal_
  // mirror), NOT at mal_mirror.cpp — matching how progress/status are
  // already read pre-converted to the domain shape here.
  std::uint32_t score = 0;
  // The mirror's own last-accepted (status, progress) snapshot (mal_synced_
  // status/progress), or nullopt if this row was never mirrored. The
  // per-row guard compares MAL's live entry against THIS, not against
  // (list_status, progress) — the guard asks "did MAL's value move since we
  // last pushed", the same question sync.cpp's push_dirty asks of
  // `row.synced` (ROD-498). Score stays a sibling field with its OWN
  // snapshot (synced_score below), not folded into this pair — MAL mirror
  // is deliberately separate machinery from the AniList sync ListEntry
  // triple (mal_mirror.hpp's own header comment).
  std::optional<std::pair<ListStatus, std::uint32_t>> synced;
  // The mirror's own last-accepted score snapshot, or nullopt if never
  // mirrored. Independent of `synced` above: a status/progress-only push
  // must not silently declare a stale score snapshot "current".
  std::optional<std::uint32_t> synced_score;

  friend bool operator==(const MalMirrorRow&, const MalMirrorRow&) = default;
};

class Store {
 public:
  // Open (creating if absent), flip WAL, enable FKs, migrate to kSchemaVersion,
  // prove writability. The path is shigoku's own DB file (paths.data); there is
  // no zigoku compatibility mode.
  [[nodiscard]] static Result<Store, StoreError> open(std::string_view path);

  // Isolated blank in-memory DB for tests/doctests.
  [[nodiscard]] static Result<Store, StoreError> open_memory();

  ~Store();
  Store(Store&&) noexcept;
  Store& operator=(Store&&) noexcept;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Mint an identity row if absent (02 §3.7: rows are cheap, carry no UI
  // meaning until library_added_at is stamped). Idempotent: an existing row's
  // enrichment/user-state is untouched (ON CONFLICT DO NOTHING). The P9 subset
  // fills only the columns resume/progress need (title, mal_id, total, status);
  // P14's enrichment upsert is a superset write on the same key.
  [[nodiscard]] Result<Unit, StoreError> ensure_show(
      std::int64_t anilist_id, std::string_view title_romaji,
      std::optional<std::int64_t> mal_id, std::optional<std::uint32_t> total_episodes,
      std::optional<std::string_view> media_status);

  // --- Catalog cache + enrichment (02 §3.5, 04 §10 L2) ---------------------

  // Upsert a Browse/Discover/search hit (02 §3.5 write path). Runs on every
  // successful AniList page; freshness columns always restamp. Incoming NULL
  // never wipes a stored value; a blank title is treated as absence; an
  // absolute cover URL never downgrades to a relative one (ROD-267).
  [[nodiscard]] Result<Unit, StoreError> upsert_catalog_cache(
      const Enrichment& e, std::int64_t now, std::optional<std::int64_t> expires_at);

  // The cached hit by anilist_id, or nullopt.
  [[nodiscard]] Result<std::optional<CatalogHit>, StoreError> get_catalog(
      std::int64_t anilist_id) const;

  // The watchlist-add writer (02 §3.7): mints or merges the show row and stamps
  // membership set-once. User-state columns are absent from the conflict SET
  // entirely; INSERT defaults cover them on mint (02 §5 — NOT COALESCE).
  [[nodiscard]] Result<Unit, StoreError> add_to_library(const Enrichment& e, std::int64_t now);

  // Enrichment-only patch, UPDATE-only: enrichment never mints a show row (the
  // 02 §3.7 mint list is binding/absence/route/library-add). Returns whether a
  // row was patched. `stamp_fresh` is the full-fieldset contract: only a
  // confirmed complete answer may stamp freshness or clear a stale total
  // (ROD-419). Partial feeds pass false.
  [[nodiscard]] Result<bool, StoreError> patch_show_enrichment(
      const Enrichment& e, bool stamp_fresh, std::int64_t now);

  // Refresh-on-view staleness (04 §10): a show row judges by its own stamp
  // (NULL = never enriched), else the catalog_cache row by its expiry; no row
  // anywhere is a miss. Fieldset drift re-heals without waiting out the TTL.
  [[nodiscard]] Result<bool, StoreError> enrichment_stale(
      std::int64_t anilist_id, std::int64_t now) const;

  // A confirmed-null enrich answer stamps freshness with no fields (05 §8: a
  // true negative is an answer, never re-queried forever). UPDATE-only, like
  // the patch. Returns whether any row (show or cache) was stamped.
  [[nodiscard]] Result<bool, StoreError> stamp_enrichment_checked(
      std::int64_t anilist_id, std::int64_t now);

  // Promote a cached card into the library (02 §3.5): straight enrichment copy
  // plus the membership stamp; the cache row remains. False if no cache row.
  [[nodiscard]] Result<bool, StoreError> promote_catalog_to_show(
      std::int64_t anilist_id, std::int64_t now);

  // --- Provider bindings / pins / absences / routes ------------------------

  // Bind a provider offering to a show. Mints the identity row when absent
  // (no membership, 02 §3.7). Re-bind of a provider id that pointed at another
  // show is delete+insert (02 O5). Clears any absence so bound and absent never
  // coexist (02 §5). One transaction: a racing peer never surfaces the raw
  // UNIQUE(provider, provider_id) failure.
  [[nodiscard]] Result<Unit, StoreError> bind_provider(
      const Enrichment& e, std::string_view provider, std::string_view provider_id,
      std::int64_t now);

  // Drop the (show, provider) binding. Returns whether a row was deleted.
  [[nodiscard]] Result<bool, StoreError> unbind_provider(
      std::int64_t anilist_id, std::string_view provider);

  // Every binding for a show, ordered by provider name.
  [[nodiscard]] Result<std::vector<Binding>, StoreError> bindings_for(
      std::int64_t anilist_id) const;

  // The show a (provider, provider_id) edge points at, or nullopt.
  [[nodiscard]] Result<std::optional<std::int64_t>, StoreError> show_id_for_binding(
      std::string_view provider, std::string_view provider_id) const;

  // Persist a definitive not-stocked answer (ROD-347; callers pass only a clean
  // miss, never a transport error). Mints the identity row when absent (no
  // membership, 02 §3.7).
  [[nodiscard]] Result<Unit, StoreError> mark_provider_absent(
      const Enrichment& e, std::string_view provider, std::int64_t now);

  // Fresh (within-TTL) absence. Stale reads as unchecked.
  [[nodiscard]] Result<bool, StoreError> provider_absent_fresh(
      std::int64_t anilist_id, std::string_view provider, std::int64_t now) const;

  // Detail-rail availability (ROD-348): Bound wins over a fresh Absence over
  // Unchecked.
  [[nodiscard]] Result<ProviderAvailability, StoreError> provider_availability(
      std::int64_t anilist_id, std::string_view provider, std::int64_t now) const;

  // Per-show forced provider (ROD-345), stored verbatim; unknown names degrade
  // at the registry, not here. nullopt clears the pin.
  [[nodiscard]] Result<Unit, StoreError> set_provider_pin(
      std::int64_t anilist_id, std::optional<std::string_view> provider);

  // The pinned provider, or nullopt.
  [[nodiscard]] Result<std::optional<std::string>, StoreError> get_provider_pin(
      std::int64_t anilist_id) const;

  // The preferred_provider this show last settled under (ROD-398), or nullopt.
  [[nodiscard]] Result<std::optional<std::string>, StoreError> get_route_pref(
      std::int64_t anilist_id) const;

  // Stamp the settled preferred provider (ROD-398). Mints the identity row when
  // absent (stamp-before-fetch, 03 §5.3; no membership, 02 §3.7).
  [[nodiscard]] Result<Unit, StoreError> set_route_pref(
      const Enrichment& e, std::string_view pref);

  // --- Episode-list cache + app_meta ---------------------------------------

  // Cache a provider's episode-label list for (show, provider, translation)
  // with a status-aware TTL. The list is stored as a JSON string array.
  [[nodiscard]] Result<Unit, StoreError> set_episode_cache(
      std::int64_t anilist_id, std::string_view provider, Translation translation,
      const std::vector<std::string>& episodes, std::optional<std::string_view> airing_status,
      std::int64_t now);

  // Cached episode list if unexpired; stale or a corrupt blob is a miss (not an
  // error). nullopt = miss; an empty vector = a cached empty listing.
  [[nodiscard]] Result<std::optional<std::vector<std::string>>, StoreError> get_cached_episodes(
      std::int64_t anilist_id, std::string_view provider, Translation translation,
      std::int64_t now) const;

  // One-shot flag store (app_meta). nullopt = key absent.
  [[nodiscard]] Result<std::optional<std::string>, StoreError> meta_get(
      std::string_view key) const;
  [[nodiscard]] Result<Unit, StoreError> meta_set(std::string_view key, std::string_view value);

  // --- Show reads / delete -------------------------------------------------

  // The full library row for a show, or nullopt.
  [[nodiscard]] Result<std::optional<Show>, StoreError> get_show(std::int64_t anilist_id) const;

  // History = library rows only (library_added_at IS NOT NULL), watched-first
  // then by add recency (02 §3.7). One card per show by construction.
  [[nodiscard]] Result<std::vector<Show>, StoreError> list_history() const;

  // Show-wide cascade from the PK: FKs take progress, caches, bindings, pins,
  // absences, routes (02 §5 FIX-IN-RUST). Returns whether a row was deleted.
  [[nodiscard]] Result<bool, StoreError> delete_show(std::int64_t anilist_id);

  // --- Manual status (History P16, ROD-139/189/193) -------------------------

  // Manual status write: no play_count / last_watched_at (never an engagement).
  // Completed snaps progress to total only when total > 0 (a null or zero total
  // must not zero progress). Stamps membership set-once. Unknown show: no-op.
  // One BEGIN IMMEDIATE envelope (store.rs set_list_status) so the snap never
  // writes through a stale read.
  [[nodiscard]] Result<Unit, StoreError> set_list_status(
      std::int64_t anilist_id, ListStatus status, std::int64_t now);

  // Undo restore of the exact prior (status, progress) pair (ROD-193): no
  // force-complete snap, no read-modify — a verbatim write of the caller's
  // captured pair. Same progress_stamped_at / library_added_at set-once
  // semantics as set_list_status. Unknown show: silent no-op.
  [[nodiscard]] Result<Unit, StoreError> restore_list_status(
      std::int64_t anilist_id, ListStatus status, std::uint32_t progress, std::int64_t now);

  // P34 slice 1: raw 0..=100 write, no read-modify (unlike set_list_status,
  // there is no progress-snap side effect). std::nullopt clears to unset
  // (NULL), matching the History `s` prompt's empty-input-clears rule. Leaves
  // synced_score untouched — dirtiness against the AniList push (P34 slice 2)
  // is exactly "user_score differs from synced_score", the same shape as the
  // mal-mirror snapshot pair. Unknown show: silent no-op (set_list_status law).
  [[nodiscard]] Result<Unit, StoreError> set_user_score(
      std::int64_t anilist_id, std::optional<std::uint32_t> score);

  // P37 slice 3: raise the schedule notice's dedup high-water mark and set
  // the History NEW marker together, one write per detected notice
  // (check_schedule_notices, app.cpp). UPDATE-only, unknown show: silent
  // no-op (set_list_status law). No read-modify: the caller (a pure
  // detect_schedule_notices result) already knows the new episode number is
  // higher than the show's current mark.
  [[nodiscard]] Result<Unit, StoreError> set_schedule_notice(
      std::int64_t anilist_id, std::uint32_t episode);

  // P37 review: seed the dedup high-water mark QUIETLY — no NEW marker. The
  // first-ever look at a row (fresh v6 migration, a new library add) is the
  // plan's "last-run stamp" duty: it establishes belief without raising, so
  // a mature library's long-stale next_airing_* rows (ROD-261 keeps them
  // around) never storm the first boot with months-old notices. Unknown
  // show: silent no-op.
  [[nodiscard]] Result<Unit, StoreError> seed_schedule_notice(
      std::int64_t anilist_id, std::uint32_t episode);

  // P37 slice 3: clear the History NEW marker (open_detail, app.cpp) without
  // touching notice_last_episode — the marker and the dedup mark are
  // independent (clearing the marker must never allow the same episode to
  // re-raise). Unknown show: silent no-op.
  [[nodiscard]] Result<Unit, StoreError> clear_notice_pending(std::int64_t anilist_id);

  // The 30s-checkpoint / partial-save primitive (02 §4b): upsert the resume row
  // for (show, translation, label). fully_watched derives from kWatchedRatio
  // here and only here. Rejects non-finite floats loud — SQLite turns NaN into
  // NULL and +Inf sails past the ratio into a permanent fully_watched
  // (store.rs ROD-434 red-team finding). No play_count / status side effects.
  [[nodiscard]] Result<Unit, StoreError> save_progress(
      std::int64_t anilist_id, Translation translation, std::string_view episode,
      double position_secs, double duration_secs,
      std::optional<std::string_view> last_provider, std::int64_t now);

  // Atomic play-completion writer (02 §4b): the resume row AND the
  // engagement/ratchet land under ONE immediate transaction, so a mid-write
  // failure never leaves a saved resume point with the play uncounted. progress
  // ratchets (never lowers) only on a natural end (>= 0.80); play_count and
  // last_watched_at always bump; library_added_at stamps set-once. Unknown show
  // or a zero episode_index: no-op (membership must not ride an unknown ep).
  [[nodiscard]] Result<Unit, StoreError> record_finish(
      std::int64_t anilist_id, Translation translation, std::string_view episode,
      std::uint32_t episode_index, double position_secs, double duration_secs,
      std::optional<std::string_view> last_provider, std::int64_t now);

  // Resume point for one (show, translation, label), or nullopt (02 §3.4
  // keying — one row per key, cross-provider union automatic).
  [[nodiscard]] Result<std::optional<Resume>, StoreError> get_resume(
      std::int64_t anilist_id, Translation translation, std::string_view episode) const;

  // The DESIGN §4.6 resume-cell seed (05 §10.7): the freshest partial watch,
  // fully_watched excluded, position > 0. P9 has no progress_stamped_at gating
  // yet (that frontier-retire rule is P15's walk machinery — 08 §10 ROD-477);
  // v0 returns the freshest usable partial by updated_at.
  [[nodiscard]] Result<std::optional<std::pair<std::string, Resume>>, StoreError>
  latest_resume(std::int64_t anilist_id, Translation translation) const;

  // Recompute contract (store.rs recompute_progress, 02 §4b): 1-based ordinal of
  // the last fully_watched row among present rows in episode_label_cmp order —
  // positional, not the label value, not a count; gap-watch under-counts on
  // purpose. Translation-scoped; overwrites show.progress unconditionally.
  // Returns the new progress. No-op returning 0 for an unknown/absent show.
  [[nodiscard]] Result<std::uint32_t, StoreError> recompute_progress(
      std::int64_t anilist_id, Translation translation, std::int64_t now);

  // Read show.progress (the one translation-blind counter). nullopt = no row.
  [[nodiscard]] Result<std::optional<std::uint32_t>, StoreError> get_progress(
      std::int64_t anilist_id) const;

  // Per-grid-cell watched marks (05 §10.5): for each label in `episodes`, true
  // iff a fully_watched episode_progress row exists for it under `translation`.
  // The result is index-parallel to `episodes` (the grid's own order).
  [[nodiscard]] Result<WatchedMarks, StoreError> watched_marks(
      std::int64_t anilist_id, Translation translation,
      const std::vector<std::string>& episodes) const;

  // --- AniList sync (P20, 06 §5) --------------------------------------------

  // Push work-list (ROD-284): library rows whose live (status, progress,
  // score) triple differs from the sync snapshot, or that were never synced.
  // Library-only so identity rows never flood AniList planning.
  [[nodiscard]] Result<std::vector<SyncRow>, StoreError> list_dirty_for_sync() const;

  // Advance the sync snapshot after AniList accepts the triple. Unconditional:
  // the snapshot means "what the server holds", and after an accepted write
  // the server holds exactly what was sent (06 §5.3 — not a CAS target).
  //
  // Score caveat (P34): under a lossy scoreFormat the server stores the
  // QUANTIZED value, not the raw local one — `score` here must be the
  // round-tripped from(to(raw)) the server actually holds. When
  // `adopt_from_raw` is set, the local user_score is snapped to `score` too,
  // CAS-guarded on it still equalling the raw value that was pushed (a
  // mid-push edit keeps the fresher local value dirty instead) — otherwise a
  // raw off-grid score would stay eternally dirty against its own snapshot.
  [[nodiscard]] Result<Unit, StoreError> mark_synced(
      std::int64_t anilist_id, ListStatus status, std::uint32_t progress, std::uint32_t score,
      std::optional<std::uint32_t> adopt_from_raw = std::nullopt);

  // Pull reconcile (06 §5.4): merge the remote list into matching library rows
  // by anilist_id, clean rows included. Read-then-write per row (no wrapping
  // transaction) so the CAS guard can catch a concurrent local edit.
  [[nodiscard]] Result<PullOutcome, StoreError> reconcile_pull(
      const std::vector<RemoteEntry>& remote, std::int64_t now);

  // reconcile_pull split into its two steps, for tests that need a concurrent
  // writer to land between the plan read and the apply (the CAS guard's
  // reason to exist). Production code always calls reconcile_pull.
  [[nodiscard]] Result<SyncPlan, StoreError> reconcile_plan(
      const std::vector<RemoteEntry>& remote) const;
  [[nodiscard]] Result<PullOutcome, StoreError> apply_reconcile(
      const std::vector<SyncPlanRow>& plan, const std::vector<SyncImportRow>& imports,
      std::vector<std::int64_t> unmatched, std::int64_t now);

  // --- MAL mirror (P31 §9.1 slice 4) ----------------------------------------

  // Push work-list: library rows with a non-NULL mal_id whose live (status,
  // progress) pair OR score differs from the MAL-mirror's own snapshots
  // (mal_synced_status/progress, v3; mal_synced_score, v5), or that were
  // never mirrored. Rows with NULL mal_id never appear (the NULL-mal_id
  // skip, PORT_PARITY.md P31). `score` is already converted to MAL's native
  // 0..=10 scale (round(raw/10)).
  [[nodiscard]] Result<std::vector<MalMirrorRow>, StoreError> list_dirty_for_mal_mirror() const;

  // Advance the MAL-mirror snapshots after MAL accepts the push. Unconditional,
  // same rationale as mark_synced: the snapshot means "what MAL now holds".
  // `score` is MAL's own 0..=10 scale (the same value push_mirror sent);
  // nullopt = the push WITHHELD the field (locally unscored — mal.hpp's
  // score=0-erases rule), so the snapshot stays NULL: the mirror records no
  // belief about MAL's score and the guard's score arm keeps waiving it.
  [[nodiscard]] Result<Unit, StoreError> mark_mal_synced(
      std::int64_t anilist_id, ListStatus status, std::uint32_t progress,
      std::optional<std::uint32_t> score);

 private:
  explicit Store(sqlite3* conn) : conn_(conn) {}
  sqlite3* conn_ = nullptr;
};

// Internals exposed for direct unit tests of the pure reconcile matrix (06
// §5.4), mirroring anilist.hpp's `detail` exposure pattern.
namespace store_detail {

struct Reconciled {
  ListStatus status = ListStatus::Planning;
  std::uint32_t progress = 0;
  // Score's own three-way merge cell (P34 slice 2): same local-moved/
  // remote-moved matrix as status, not progress's rise-biased merge — a
  // lowered score is as intentional an edit as a raised one, so there is no
  // "correction" case to protect against (unlike merge_progress's own doc
  // comment on why status/progress special-case a fall).
  std::uint32_t score = 0;
  ListStatus snapshot_status = ListStatus::Planning;
  std::uint32_t snapshot_progress = 0;
  std::uint32_t snapshot_score = 0;
  bool conflict = false;

  friend bool operator==(const Reconciled&, const Reconciled&) = default;
};

// The reconcile matrix (06 §5.4, score folded in P34 slice 2). `base` is the
// snapshot, nullopt on first contact (Planning / 0 / 0). Status, progress,
// and score each run the same three-way matrix against their own slice of
// base; the snapshot re-baselines to the raw remote triple whenever any of
// the three differs from base.
[[nodiscard]] Reconciled reconcile(std::optional<ListEntry> base, ListEntry local,
                                    ListEntry remote);

// Progress half of the 06 §5.4 matrix. `max` only in the both-moved cell:
// anywhere else it would re-raise a correction.
[[nodiscard]] std::uint32_t merge_progress(std::uint32_t base, std::uint32_t local,
                                            std::uint32_t remote);

}  // namespace store_detail

}  // namespace shigoku
