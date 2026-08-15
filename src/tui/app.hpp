// app.hpp — App state + tick() + run() (P6/P7, A1/A2/A3). The event-loop
// skeleton: one UI thread owns App and the terminal; tick(Event) mutates;
// draw(App) is PURE (reads App, writes cells, no I/O beyond the terminal
// write, no store / network). views.hpp (P7) fills the browse/detail/episode
// render.
//
// P6 rev checklist (carried forward): "views may not include http or store" —
// enforced by the CMake link graph (shigoku_tui links ONLY domain + threads,
// never curl/sqlite). AppDeps below holds ONLY forward-declared pointers to
// the provider registry / http client (never their headers) so this file, and
// everything that includes it (views.hpp, the whole render path), stays off
// that link graph. app.cpp — the .cpp, not exposed to views.cpp — includes
// the real headers to implement the worker spawns.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../auth.hpp"
#include "../config.hpp"
#include "../domain.hpp"
#include "../event.hpp"
#include "../result.hpp"
#include "cells.hpp"
#include "connect.hpp"
#include "cover_geom.hpp"
#include "cover_pool.hpp"
#include "cover_probe.hpp"  // CoverBackend — the probe's protocol verdict.
#include "covers.hpp"
#include "discover.hpp"
#include "discover_filters.hpp"
#include "queue.hpp"
#include "settings.hpp"
#include "term.hpp"
#include "theme.hpp"
#include "toast.hpp"
#include "workers.hpp"

namespace shigoku {
// Forward declarations only (see the file comment): AppDeps holds pointers
// and classify_tier/etc. only take references, so the full definitions
// (provider.hpp) are not needed in this header.
class ProviderRegistry;
class StreamProvider;
class Store;  // P9: AppDeps holds a pointer; store.hpp is included only by
              // app.cpp, so the render core stays off the sqlite link graph.
enum class Tier;
namespace http {
class Client;
}  // namespace http
}  // namespace shigoku

namespace shigoku::tui {

// Render-side availability marker (P15, 05 §15). A pure-header mirror of the
// store's ProviderAvailability so app.hpp (the render core) never includes
// store.hpp; app.cpp maps store→this when it mirrors the transport state.
enum class AvailMark {
  Unchecked,  // never probed.
  Bound,      // a binding exists.
  Absent,     // a fresh not-stocked absence.
};

// The P15 resolve-walk transport (resolve_transport.hpp) + the prewarm state
// (prewarm.hpp). Forward-declared and held by App via unique_ptr so app.hpp
// (and the whole render core that includes it) stays off store.hpp's link
// graph — both read the Store + registry and thus live only in app.cpp's TU,
// exactly like the Store pointer in AppDeps.
class EpisodeSession;
class PrewarmState;

// v0 view set. History/Discover/Settings compile in (the top-bar strip names
// them, A2 fence) but only Browse + Detail render content in P7. Detail is
// the full-screen zoom (DESIGN §5.3) — a standalone active_view, not a pane.
// Schedule (P37) joined later: a read-only weekday-grouped countdown list,
// keybind `C` (Calendar; B/H/D/S were already taken).
enum class View {
  Browse,
  History,
  Discover,
  Settings,
  Detail,
  Schedule,
};

// Which pane holds keyboard focus in a two-pane view (DESIGN §7.3). Detail
// zoom is a P7 concern; the field exists so the `·` indicator has a source.
enum class Pane {
  List,
  Detail,
};

// Bottom-bar mode (DESIGN §3.5). P6 renders Idle + Search chrome; Command is a
// P7 concern but the enum arm exists so the fence covers it. Score (P34): the
// History `s` prompt, a free-text numeric buffer distinct from Search's
// filter-as-you-type semantics (it commits once, on Enter, not per keystroke).
enum class BarMode {
  Idle,
  Search,
  Command,
  Score,
};

// Episode grid + resolve state for the one show currently open in the detail
// pane (04 §7.1 EpisodeState, collapsed: no LRU/SQLite cache in v0 — one show
// open at a time is fine). `for_id` is the anilist_id it was fetched for; a
// mismatch with the currently-selected row means "stale, about to be
// replaced" (kept only until the in-flight worker's result lands or is
// dropped).
struct EpisodeState {
  std::vector<std::string> episodes;  // sorted raw labels; empty + !loading +
                                      // fetched means authoritative "no eps".
  std::int64_t for_id = 0;
  // Provider-side id the grid was fetched under (Tier-A canonical key or the
  // Tier-C first search hit — EpisodesDone carries it back). Play MUST use
  // this, never re-derive from anilist_id: a Tier-C show has no canonical_key
  // and anilist_id is not a provider key. Empty until fetched (or no_source).
  std::string provider_id;
  bool loading = false;
  bool fetched = false;   // false = never fetched yet (grid renders nothing).
  bool no_source = false; // true = tier walk exhausted, nothing landed.
  int cursor = 0;         // index into `episodes`; grid cell focus.
  // Provider caption (P15, 05 §15): the provider currently serving the grid
  // ("" = none yet), the per-show pin (empty = unpinned), and the per-provider
  // availability snapshot (name → unchecked|bound|absent). Mirrored from the
  // resolve transport after each step; the pure detail chrome renders them
  // (serving leads, then markers) and never reads the store.
  std::string serving;
  std::string pinned;
  std::vector<std::pair<std::string, AvailMark>> avail;
  // Watched marks (P9, 05 §10.5), index-parallel to `episodes`: true iff a
  // fully_watched store row exists for that label under the active
  // translation. Computed in tick() from the store when episodes land and
  // after each PlayDone; the pure grid render reads it (never the store).
  // Empty (or shorter than episodes) means "unknown" — rendered unmarked.
  std::vector<bool> watched;
};

// Detail zoom's `c`-toggled Characters/Recommendations section (P36).
// Zoom-only (Browse/History's narrow two-pane detail column never shows
// this — the section needs the zoom's full canvas), session-cached per
// show: switching shows resets `for_id`/`data` but not `visible` (the
// toggle is a standing preference for the session, same idiom as
// app.pane). Single-flight like enrich (`loading` gates a second spawn).
struct CharactersRecsState {
  bool visible = false;
  std::int64_t for_id = 0;         // the show `data` (or the in-flight fetch) answers for.
  bool loading = false;
  bool fetched = false;            // false = never resolved for `for_id` yet.
  bool failed = false;             // last fetch for `for_id` was a transport error.
  CharactersAndRecommendations data;
  std::size_t scroll = 0;          // line offset into the rendered section body.
  std::size_t cursor = 0;          // index into data.recommendations (P36 slice 3 Enter target).
  // Tab toggles focus between the episode grid and this section while
  // visible (the zoom has no finer-grained Pane concept than that); j/k and
  // Enter route to the section only while focused. Reset to false whenever
  // the section is hidden, so re-opening always lands back on the grid.
  bool focused = false;
};

// One display line of the grouped History list (05 §2 geometry contract): a
// group header, a hairline rule, a packed title+bar pair per entry, and one
// blank row between groups (never inside one). `layout()` is the single
// source both nav (scroll_into_view) and draw (draw_history_list) consume so
// they can never disagree.
struct HistoryLine {
  enum class Kind { Header, Rule, Title, Bar, Blank } kind = Kind::Blank;
  ListStatus status = ListStatus::Planning;  // Header only.
  std::size_t count = 0;                     // Header only.
  std::size_t ord = 0;                       // Title/Bar: index into `order`.
  friend bool operator==(const HistoryLine&, const HistoryLine&) = default;
};

// History/Watchlist nav + filter state (05 §2, ROD-439 chunk 5a). Loaded
// synchronously from the store (a local read, no worker — deliberate
// deviation from the async-event pattern the rest of the app uses; History
// never touches the network). Domain-only (Show, from domain.hpp), so this
// stays in the pure render core alongside EpisodeState.
struct HistoryState {
  std::string filter;
  std::vector<Show> rows;               // store order, as loaded.
  std::vector<std::optional<std::uint32_t>> resume;  // parallel to rows; ◐ marker ep.
  std::vector<std::size_t> order;       // indices into rows: filtered, group-ordered.
  std::size_t cursor = 0;
  std::size_t scroll = 0;
  bool load_failed = false;

  [[nodiscard]] bool empty() const { return rows.empty(); }
  [[nodiscard]] std::size_t count() const { return order.size(); }
  [[nodiscard]] const Show* selected() const {
    if (cursor >= order.size()) return nullptr;
    return &rows[order[cursor]];
  }
  // The resume-landing target (05 §10.6): the most-recently-watched row (the
  // load sorts played rows first), or nullopt.
  [[nodiscard]] std::optional<std::int64_t> first_played() const {
    if (rows.empty() || !rows.front().last_watched_at.has_value()) return std::nullopt;
    return rows.front().enrichment.anilist_id;
  }

  // Rebuild `order` (filtered + group-ordered) and re-anchor the cursor onto
  // `anchor`'s identity, clamping if it fell out (05 §2 setHistory contract).
  void rebuild(std::optional<std::int64_t> anchor);
  // Move the cursor onto a show by identity; false if not in the current nav
  // order (e.g. filtered out).
  bool select_aid(std::int64_t anilist_id, int visible);
  void on_filter_edited();
  void on_filter_cleared();
  void nav(int dy, int visible);
  void jump(bool top, int visible);
  // The grouped line layout (05 §2 geometry). Exposed for tests.
  [[nodiscard]] std::vector<HistoryLine> layout() const;

 private:
  [[nodiscard]] std::optional<std::int64_t> anchor_aid() const;
  [[nodiscard]] bool matches_filter(const Show& s) const;
  void scroll_into_view(int visible);
};

// Schedule nav state (P37): a read-only weekday-grouped countdown list over
// `rows` (the same library Show set History loads — Schedule keeps its own
// copy rather than sharing History's live one, since the two views filter
// independently). `groups` is `shigoku::schedule(rows, now_secs)` rebuilt
// whenever `rows` reloads or the boot/tick clock advances a weekday boundary;
// `now_secs` is threaded in from App's own clock stamp (draw stays pure, no
// wall clock in this file or views.cpp — same discipline as HistoryState).
struct ScheduleState {
  std::vector<Show> rows;          // store order, as loaded (load_schedule).
  std::vector<ScheduleGroup> groups;  // rebuilt by rebuild(); airing-time order.
  std::size_t cursor = 0;          // index into the flattened entry walk.
  std::size_t scroll = 0;          // first visible flattened line.
  bool load_failed = false;

  [[nodiscard]] bool empty() const { return groups.empty(); }
  // Total entries across all groups (the flattened cursor's range).
  [[nodiscard]] std::size_t count() const {
    std::size_t n = 0;
    for (const ScheduleGroup& g : groups) n += g.entries.size();
    return n;
  }
  // The Show for the entry under the cursor, or nullptr (empty schedule).
  [[nodiscard]] const Show* selected() const;

  // Recompute `groups` from `rows` at `now_secs`, clamping the cursor into
  // range (no identity re-anchor: Schedule's cursor is a flattened ordinal,
  // not tied to a specific show surviving reload — a show dropping off the
  // schedule shifts the cursor like any other list shrink).
  void rebuild(std::int64_t now_secs);
  void nav(int dy, int visible);
  void jump(bool top, int visible);

  // The flattened line layout: one Header + Rule per non-empty weekday, one
  // Entry line per schedule row, one Blank between groups (never inside one)
  // — same discipline as HistoryState::layout(), single source for nav
  // (scroll_into_view) and draw so they can never disagree.
  struct Line {
    enum class Kind { Header, Rule, Entry, Blank } kind = Kind::Blank;
    Weekday weekday = Weekday::Monday;  // Header only.
    std::size_t count = 0;              // Header only.
    std::size_t group_ix = 0;           // Entry: index into groups.
    std::size_t entry_ix = 0;           // Entry: index into groups[group_ix].entries.
    std::size_t ord = 0;                // Entry: flattened ordinal (== cursor space).
  };
  [[nodiscard]] std::vector<Line> layout() const;

 private:
  void scroll_into_view(int visible);
};

// Playback tracking (04 §7.7: ONE play in flight at a time). App-global and
// NOT part of EpisodeState: it must survive the EpisodeState reset when the
// user browses to another show while mpv is still up — were the flag inside
// EpisodeState, that reset would erase it, a second Enter could spawn a
// concurrent second mpv, and the old play's terminal PlayDone/PlayError toast
// would be dropped on the for_id mismatch.
struct PlayState {
  bool active = false;         // a play worker (mpv) is in flight.
  std::int64_t for_id = 0;     // anilist_id the play was launched for.
  std::string episode;         // raw label in flight.
  // The provider NAME the grid is served by (P15): resolve + the progress/finish
  // rows use THIS, never registry.primary() — a pin-flip/fallback hop can land
  // the grid on a non-primary provider, and its provider_id only resolves on its
  // own provider (provider.hpp: callers MUST NOT fall back to primary() for
  // bound rows). Empty only on the degraded P7 path (then primary() is correct).
  std::string provider;
  double position = 0.0;       // last observed PositionUpdate.
  double duration = 0.0;       // 0 = unknown yet.
  TickCount started_tick = 0;  // launching-cell slow-color clock.
  // Last store checkpoint (04 §7.7: every 30s during playback). A PositionUpdate
  // saves progress when tick_count - this >= kCheckpointTicks; the final save
  // is record_finish on PlayDone, independent of this clock.
  TickCount last_checkpoint_tick = 0;
  Translation translation = Translation::Sub;  // track in flight (resume/save key).
  std::uint32_t episode_index = 0;  // 1-based grid ordinal (record_finish ratchet).
  // Show meta captured at fire time (P30): the play-continuation relaunch
  // (03 §6.4) must not read the CURRENT selection — the user may have scrolled.
  std::optional<std::int64_t> mal_id;
  std::string title_romaji;
};

// The one-at-a-time episode download in flight (P35 slice 3, PlayState's
// shape). The worker owns the transfer; App owns this mirror for the
// statusline/grid and the quit path. cancel/child_pid are heap-shared with
// the worker: quit sets cancel and SIGTERMs the published ffmpeg child
// directly — downloads have no drain rights (A1), so quit never waits for
// the worker to notice.
struct DownloadState {
  bool active = false;
  std::int64_t for_id = 0;      // anilist_id the download was fired for.
  std::string episode;          // raw label in flight.
  std::uint64_t bytes = 0;      // last DownloadProgress.
  std::uint64_t total = 0;      // 0 = unknown (ffmpeg arm: indeterminate).
  TickCount started_tick = 0;   // spinner clock.
  std::shared_ptr<std::atomic<bool>> cancel;
  std::shared_ptr<std::atomic<long>> child_pid;  // live ffmpeg pid, 0 = none.
};

// The live detail cover as the terminal sees it (P8, A4). draw() computes the
// placement RECT (image-owned; the diff excludes it); run() owns the actual
// APC emission (delete outgoing / transmit incoming) because draw() is pure.
// This is the bridge: draw() fills `rect`/`want_id`, run() reconciles what is
// actually on the terminal against it and splices the deltas into the flush.
struct CoverRender {
  // Decoded RGBA cropped/resized to the placement box, ready to transmit.
  // have_pixels==false means "no cover held" (placeholder box drawn). The
  // placement RECT is not stored — it is derived from the live geometry by
  // detail::cover_rect(app), so draw() (const) and run() agree without a shared
  // mutable field and a resize can never leave a stale rect behind.
  CoverPixels pixels;
  std::int64_t for_id = 0;   // anilist_id the pixels belong to (staleness).
  bool have_pixels = false;  // pixels are valid and belong to the open show.
};

// Startup cover-support probe result (P8): which image protocol the terminal
// speaks (kitty / OSC 1337 / none) and its per-cell pixel geometry (for the
// poster box sizing). Set once in run() before the loop; draw()/reconcile
// read it.
struct CoverCaps {
  CoverBackend backend = CoverBackend::None;  // None -> placeholder mode.
  CellPx cell;  // {0,0} -> unknown geometry (fixed floors/caps).
  // Any image backend at all — the fetch/exclusion/grid gates key on this;
  // the emit sites switch on `backend` itself.
  [[nodiscard]] bool images() const { return backend != CoverBackend::None; }
};

// Provider + HTTP deps a running app needs to actually fetch (P7). Held by
// pointer/reference in App (never owned) so tick()'s signature stays free of
// ownership questions; run() owns the real objects and a FixtureProvider-only
// test can construct a registry with no http::Client at all if its provider
// never touches one.
// Browse search callback: query -> page of catalog rows. Defaults to unset in
// AppDeps; main.cpp wires it to a thin wrapper over the real anilist::search
// (AniList stays the ONLY production catalog client, 01 §5 — this seam exists
// so an offline test can inject canned rows, the same role Senshi::with_
// endpoint plays for the provider side; nothing about shipped behavior
// changes). Runs on the search worker thread, so it must be safe to call off
// the UI thread (a bound http::Client + a free function satisfies that).
// {rows, has_next}: hasNextPage rides along so Browse load-more (05 §16) can
// gate the next-page fire — the same shape DiscoverPage uses below.
using SearchPage = std::pair<std::vector<CatalogRow>, bool>;
using SearchFn = std::function<Result<SearchPage, ProviderError>(
    std::string_view query, std::uint32_t page)>;

// Discover feed callback (P17): (axis, page) -> a page of catalog rows plus its
// has_next flag. Mirrors SearchFn exactly (main.cpp wraps anilist::discover,
// offline tests inject canned pages); the has_next is returned alongside the
// rows because on_feed needs it to set the slot's exhausted flag. Returned as a
// pair (not anilist::CatalogPage) so app.hpp stays off anilist.hpp's http link
// graph — the render-core purity rule. Runs on the feed worker thread.
using DiscoverPage = std::pair<std::vector<CatalogRow>, bool>;  // {rows, has_next}.
// `filters` (P38, §9): the active DiscoverFilters riding along on every
// fetch, default-constructed (empty) when no filter is set — composes the
// same body as the pre-P38 unfiltered call.
using DiscoverFn = std::function<Result<DiscoverPage, ProviderError>(
    DiscoverAxis axis, std::uint32_t page, const DiscoverFilters& filters)>;

// GenreCollection callback (P38): the full AniList genre vocabulary, fetched
// once per session for the filter overlay's genre picker. Mirrors EnrichFn's
// shape but two-state (no confirmed-absent arm — see anilist.hpp). Runs on
// its own worker thread.
using GenreCollectionFn =
    std::function<Result<std::vector<std::string>, ProviderError>()>;

// By-id enrichment callback (P21, 05 §8): anilist_id -> three-state answer.
// Mirrors SearchFn/DiscoverFn (main.cpp wraps anilist::enrich, offline tests
// inject a scripted answer). Ok(some) = fresh metadata, Ok(nullopt) = confirmed
// no-match, Err = no answer — the refresh law branches on all three. Runs on
// the enrich worker thread, so it must be safe off the UI thread.
using EnrichFn = std::function<Result<std::optional<Enrichment>, ProviderError>(
    std::int64_t anilist_id)>;

// Characters+recommendations callback (P36): anilist_id -> three-state
// answer, same shape as EnrichFn. Runs on its own worker thread (the `c`
// toggle spawns it), so it must be safe off the UI thread.
using CharRecsFn = std::function<Result<std::optional<CharactersAndRecommendations>, ProviderError>(
    std::int64_t anilist_id)>;

struct AppDeps {
  const ProviderRegistry* registry = nullptr;
  const http::Client* http = nullptr;
  SearchFn search;  // must be set for `/` search to do anything (else no-op).
  DiscoverFn discover;  // must be set for the Discover feed to fetch (else no-op).
  EnrichFn enrich;  // must be set for refresh-on-view to fetch (else no-op).
  CharRecsFn char_recs;  // must be set for the `c` section to fetch (else no-op).
  GenreCollectionFn genre_collection;  // must be set for the filter overlay's genre picker to fetch (else no-op).
  std::string mpv_path = "mpv";
  std::string socket_dir;  // paths.runtime; play() derives the socket here.
  // AniSkip's skip.lua cache dir (P22, paths.cache + "/aniskip"). "" disables
  // aniskip entirely (demo_mode / tests that pass no paths) since ensure_script
  // has nowhere to write.
  std::string aniskip_cache_dir;
  // The Settings "cover art cache" row's display string (P18, DESIGN §5.5):
  // paths.cache + "/covers", $HOME-collapsed. "" renders blank (demo_mode /
  // tests that pass no paths).
  std::string covers_dir_display;
  // Cover pipeline (P8): the decoded-LRU + guarded fetch/decode/resize. Owned
  // by run()'s stack; the cover worker calls covers->load() off the UI thread
  // and posts CoverDone/CoverError. Null in demo_mode and in FixtureProvider
  // tests that don't exercise covers (the reconcile then never spawns).
  Covers* covers = nullptr;
  // Persistence (P9, A7): resume start on play, 30s checkpoints from
  // PositionUpdate, record_finish on PlayDone, watched marks into the grid.
  // ALL access is on the UI thread — workers never touch it. Owned by run()'s
  // stack; null in demo_mode and in pty/FixtureProvider tests that pass no DB
  // (the app degrades to the P7 no-persistence behavior: start at 0, no marks).
  Store* store = nullptr;
  // P28: paths.cache, where updatecheck.hpp reads/writes its update_check
  // cache file. "" (demo_mode / no-persist tests) skips the boot check
  // entirely — same absent-dep degrade as aniskip_cache_dir.
  std::string update_cache_dir;
  // P35 slice 3: the RESOLVED download root (config.download_dir, or the
  // <data>/downloads default when that is blank — main.cpp resolves). "" =
  // downloads disabled (demo_mode / tests): `d` toasts instead of firing.
  std::string download_dir;
  // P35 slice 3: the HLS muxer binary (config.ffmpeg_path). Runtime-only dep:
  // absence surfaces at spawn as the "install ffmpeg" toast.
  std::string ffmpeg_path = "ffmpeg";
};

// App state. The subsystem controllers (EpisodeState here; ResolveTransport
// folds into it for v0; CoverState — 04 §7) land with P7/P8/P9 as embedded
// members, not a total rewrite of the P6 flat shape.
struct App {
  // --- geometry / lifecycle ---
  WinSize win;         // last known terminal size (cells + pixels).
  bool quit = false;   // set by q / Ctrl-C; run() exits the loop.
  bool dirty = true;   // a frame is owed (any state change sets this).

  // --- navigation / focus ---
  View view = View::Browse;
  Pane pane = Pane::List;
  View detail_origin = View::Browse;  // DESIGN §5.3: where `Space` zoomed from.
  // One-slot "previous page" for Esc: armed by
  // every top-level view switch (letters, tab clicks, the `/` jump), popped
  // by Esc once the zoom/pane/bar layers have all had their say. One-shot —
  // popping clears it, so Esc only ever walks BACK, never forward again.
  std::optional<View> view_back;

  // --- bottom bar / input ---
  BarMode bar = BarMode::Idle;
  std::string search_query;  // live query buffer while bar == Search.
  std::string score_input;   // live digit/'.' buffer while bar == Score (P34).

  // --- catalog (P7: AniList search results, DESIGN §4.1) ---
  std::vector<CatalogRow> catalog;  // current list contents (search results).
  int list_cursor = 0;              // selected row index into `catalog`.
  int browse_scroll = 0;            // first visible row (browse.rs scroll).
  // Browse pagination (05 §16, ROD-156): j/Down onto the last result fires the
  // next page when AniList said hasNextPage; a later page appends and keeps the
  // cursor, out-of-order pages drop. `search_answered` is the query the current
  // results answer (browse.rs `answered`) — load-more and appends require it to
  // still match the live buffer.
  std::uint32_t search_page = 1;
  bool search_has_next = false;
  std::string search_answered;

  // --- Discover card grid (P17, 04 §7.5) ---
  DiscoverState discover;  // four per-axis slots; the active axis renders.
  // The `f` filter overlay (P38, §9): a captured modal over Discover, same
  // gate tier as `connect` below (checked before BarMode::Search/Score).
  DiscoverFilterOverlay discover_filters;
  // Current broadcast cour for the NEW-badge test (discover.rs env.cour). draw()
  // is pure and has no clock; run() stamps this from now_epoch_secs() before the
  // loop (the cour changes at most once per season — no per-frame refresh).
  Cour discover_cour;
  // In-flight grid cover fetches (the Rust Drain::inflight() equivalent; A1 has
  // no drain machinery), the CoverPool pump's `busy` budget input.
  int grid_covers_inflight = 0;
  // P17b: the reusable url-keyed multi-cover coordinator (cover_pool.hpp) +
  // its render store (decoded pixels + Kitty image id per adopted url).
  // run() owns the actual placement diff (compose_grid_apc); draw() reads
  // the store to mark exclusions and the placeholder/spinner fallback.
  CoverPool cover_pool;
  CoverRenderStore cover_render_store;

  // --- History / watchlist (P16, 05 §2-§5) ---
  HistoryState history;
  // Armed hard-delete, keyed by show identity, never a row/cursor index
  // (DESIGN §6.5: a reload cancels it rather than trusting a stale row).
  std::optional<std::int64_t> confirm_delete;
  // Single-level status undo: (show, prior status, prior progress) captured
  // before the mutation (05 §4). Keyed off the captured id, not the cursor.
  std::optional<std::tuple<std::int64_t, ListStatus, std::uint32_t>> undo;

  // --- Schedule (P37) ---
  ScheduleState schedule;
  // Wall-clock stamp schedule::rebuild() consumes (draw stays pure — same
  // discipline as discover_cour above). Unlike discover_cour, this re-stamps
  // every Tick, not just at boot: the countdown column visibly counts down,
  // so schedule.rebuild(clock_epoch_secs) is re-run on the same Tick edge
  // (app.cpp's tick() Tick branch, alongside advance_spinner).
  std::int64_t clock_epoch_secs = 0;

  // --- episode grid / playback (P7, 04 §7.1/§7.7) ---
  EpisodeState episode;
  PlayState play;
  DownloadState download;  // P35 slice 3: the one transfer in flight.
  // The armed play continuation (03 §6.4, playback.rs Continuation): survives
  // across the fallback walk's hops and relaunches, remembering every provider
  // that already failed a play so the successor walk never revisits one. A
  // user-initiated fire clears it (the user's pick outranks the walk's); a
  // relaunch keeps it so a further failure grows `tried`.
  struct PlayContinuation {
    std::int64_t for_id = 0;
    std::string label;          // raw episode label in flight.
    std::uint32_t ordinal = 0;  // 1-based grid ordinal at the original fire.
    Translation translation = Translation::Sub;
    std::optional<std::int64_t> mal_id;  // meta captured at fire (aniskip).
    std::string title_romaji;            // meta captured at fire.
    std::vector<std::string> tried;
  };
  std::optional<PlayContinuation> play_continuation;

  // --- last_watched auto-open (05 §10.6, app.rs resume_pending/resume_demote) --
  // Armed once at boot when landing == "last_watched" and a played row exists;
  // the first real geometry opens the landing (pane-vs-zoom is only decidable
  // then). resume_demote holds the aid whose walk exhaust may demote back to
  // the History list — only the auto-open's own walk, never a user-driven one.
  bool resume_pending = false;
  std::optional<std::int64_t> resume_demote;

  // --- resolve walk transport (P15, 04 §7.1) ---
  // The EpisodeSession that owns the resolve walk (tiers/pins/absences/routes/
  // scoring + store writes). Held by unique_ptr so app.hpp stays off store.hpp
  // (see the forward-decl above). Constructed in run(); null in demo_mode and in
  // the P6 flat-shape tests that never resolve. tick() mirrors its grid state
  // into `episode` after each transport step so draw() (pure) never sees it.
  std::unique_ptr<EpisodeSession> episode_session;
  // Prewarm: the eager sibling-provider warm walk (05 §10.4). Held by unique_ptr
  // for the same off-store-header reason. Null when there is no store to mint
  // into (demo / no-persist tests).
  std::unique_ptr<PrewarmState> prewarm;

  // --- detail cover (P8, 05 §12 / A4) ---
  CoverState cover;         // the 05 §12 decision machine (fetch/suppress/…).
  CoverRender cover_render; // decoded pixels + placement rect for run()'s APC.
  CoverCaps cover_caps;     // terminal kitty support + cell geometry (startup).

  // --- spinner / debounce clocks (Tick-derived, 04 §8) ---
  TickCount tick_count = 0;        // monotonic ~100ms ticks since start.
  int spinner_phase = 0;           // 0..9, advanced each tick while loading.
  // DERIVED by recompute_loading() from search_inflight_query +
  // episode.loading (a stale worker result must not kill the spinner another
  // in-flight worker still owns). demo_mode sets it directly (no workers).
  bool any_loading = false;        // drives the bottom-bar spinner.
  // Query of the in-flight search worker ("" = none). SearchDone/SearchError
  // clear it only when their stamped query matches, so a stale result for an
  // edited-away query can't mark a still-running fresh worker as done.
  std::string search_inflight_query;
  // Tick at the last false->true edge of any_loading; the bottom-bar "slow"
  // spinner color measures against THIS, not app uptime.
  TickCount loading_since_tick = 0;
  // Search debounce: fire the search worker when tick_count >= this (300ms =
  // 3 ticks after the last edited keystroke). 0 = disarmed.
  TickCount search_debounce_deadline = 0;

  // --- staleness generations (04 §6) ---
  GenCounter search_gen;
  GenCounter episode_gen;
  GenCounter cover_gen;

  // --- refresh-on-view enrichment (P21, 05 §8 / 04 §10) ---------------------
  // Single-flight: enrich_checked holds the id we've already decided about this
  // selection cycle (fetched, or judged fresh) so the tick pass doesn't re-hit
  // the store or spend the network per frame; reset when the detail surface
  // moves to a different show. enrich_inflight is true while a worker is out
  // (one fetch at a time; a selection storm defers to the next reconcile).
  std::optional<std::int64_t> enrich_checked;
  bool enrich_inflight = false;

  // --- detail zoom `c` section (P36) -----------------------------------------
  CharactersRecsState char_recs;

  // --- toasts ---
  ToastQueue toasts;

  // --- Settings + config (P18, 06 §2 / DESIGN §5.5) ---------------------
  // Owned (not AppDeps): Settings mutates it live, and the pure title-chain
  // render call sites (views.cpp) need read access from draw(), which only
  // sees const App&. Loaded in main.cpp, copy-constructed into run() via the
  // `config` parameter (nullptr -> defaults, e.g. demo_mode/tests).
  Config config;
  SettingsState settings;
  std::string config_file;  // resolved path; persist_settings writes here.
  // Pre-formatted display strings for the Settings Inert rows (§5.5): no
  // update-check/theme subsystem exists yet to source these live, so
  // main.cpp fills covers_dir once at startup and version stays fixed (the
  // build version) until P20/P28 land. account is P19-live: main.cpp/
  // on_connect_result derive it from `auth` each time it changes.
  std::string settings_covers_dir;
  std::string settings_account = "not connected";
  std::string settings_version;
  // MyAnimeList's account row (P31 §9.1 slice 2), same "not connected" ->
  // live-derived-from-auth pattern as settings_account.
  std::string settings_mal_account = "not connected";

  // --- AniList auth + connect (P19, 06 §3-§4) ----------------------------
  // The signed-in-or-not token record, reloaded from disk after every
  // terminal connect outcome (06 §9 "token survives reloadAuth") so the
  // account row / future sync gate always reflect disk truth. app.hpp stays
  // off auth.hpp's network-adjacent neighbors (login.hpp/loopback.hpp
  // pull in http.hpp transitively) — auth.hpp itself is a leaf (no curl), so
  // it's safe for the render core to hold a real Auth by value.
  Auth auth;
  std::string auth_file;  // resolved path; on_connect_result reloads here.
  // The live "waiting for the OAuth callback" modal session (DESIGN 5.5a).
  // nullopt = not connecting. `cancel` is type-erased (std::function) rather
  // than a loopback::Canceler by value: app.hpp/the render core must stay off
  // loopback.hpp's link graph (it pulls in http.hpp transitively), the same
  // reason AppDeps::search is a std::function instead of a concrete type.
  struct ConnectSession {
    std::string url;               // authorize URL, for display/copy.
    TickCount started = 0;         // spinner + 10s paste-hint clock.
    bool copied = false;           // "copy link" affordance state.
    std::function<void()> cancel;  // wakes the blocked loopback accept().
  };
  std::optional<ConnectSession> connect;

  // --- AniList sync (P20, 06 §5) ------------------------------------------
  // A sync worker is inflight; gates overlap AND quitFlush (04 §11). Only one
  // worker at a time by construction (flush_sync checks this before spawning).
  bool syncing = false;
  // Action-flush debounce (ROD-291): a status/play edit arms this tick_count
  // deadline; 0 = disarmed. Same idiom as search_debounce_deadline (no
  // reusable Debounce type exists or is needed for one caller).
  TickCount sync_flush_deadline = 0;
  // The resolved db file path (P20): threaded from main.cpp so the sync
  // worker can Store::open its OWN short-lived connection (store.hpp's
  // documented exception). Empty skips every sync call site (demo_mode /
  // no-persist tests).
  std::string db_file;

  // --- mouse (P32, §9) ---
  // Double-click memory: tick_count+1 at the last left press that landed on a
  // selectable target (0 = disarmed) and its cell. A second press on the SAME
  // cell within kDoubleClickTicks is the view's Enter. Armed only by
  // selectable hits — a double-click on a tab or dead space never fires a
  // phantom Enter.
  TickCount last_click_tick = 0;
  int last_click_x = -1;
  int last_click_y = -1;

  // --- debug echo (P6 DoD: keys echo into a debug line) ---
  std::string last_key_debug;

  // Demo dressing (drive-tui DoD): render the key-echo line + keep the spinner
  // animating without a live worker. Off in the real app (P7+).
  bool demo = false;

  // Deps for spawning real workers (P7). Null in P6's demo_mode: no key fires
  // a worker so the app stays a pure chrome/skeleton demo.
  const AppDeps* deps = nullptr;
  // The event queue workers post results to. Set by run() before the loop
  // starts; tick() never touches it (workers are spawned from on_key/tick,
  // which need it to hand off to spawn_detached lambdas).
  EventQueue* queue = nullptr;
};

// Debounce / clock periods in ticks (100ms each, 04 §8). The cover cooldown
// (10s) now lives in covers.hpp (kCoverCooldownTicks) — CoverState owns the
// per-id+url failure record, superseding P6's single cover_cooldown_until
// scalar.
inline constexpr TickCount kSearchDebounceTicks = 3;    // 300ms
inline constexpr TickCount kSlowSpinnerTicks = 30;      // 3s -> "slow" color
inline constexpr TickCount kCheckpointTicks = 300;      // 30s store checkpoint (04 §7.7)
inline constexpr TickCount kSyncFlushTicks = 30;        // 3s action-flush debounce (ROD-291)
inline constexpr TickCount kDoubleClickTicks = 4;       // 400ms double-click window (P32)

// Kitty image id for the single detail cover (A4: i=<id> stable per slot; the
// one v0 cover reuses one id so a replace deletes-then-transmits the same slot).
inline constexpr std::uint32_t kDetailCoverImageId = 1;

// DESIGN §3.2: below this width Browse collapses to a single-column list (no
// detail pane); the Detail zoom demotes to `.detail` (not `.list`) at/above it.
inline constexpr int kPaneSplitMin = 60;

// tick(App, Event): mutate state for one event. Dispatches via std::visit +
// overloaded{} with NO catch-all (A2): a new Event alternative won't compile
// until handled here. Returns nothing; sets app.dirty as needed.
void tick(App& app, const Event& ev);

// draw(App) -> CellBuffer: PURE render of the current state into `buf` (sized
// to app.win). No I/O, no store, no network — views.cpp (P7) fills the
// browse/detail/episode content but stays under the same purity rule (it may
// not include http/store).
void draw(const App& app, CellBuffer& buf);

// run(): the full loop — enter terminal, spawn the stdin reader thread, wait on
// the queue (tick period = Tick), tick + draw until quit, restore. Returns the
// process exit code. `demo_mode` makes q/Ctrl-C the only bindings and skips any
// bootstrap workers (used by the drive-tui DoD binary). `deps` is null in
// demo_mode; main.cpp (P7) passes the real registry + http client so `/`
// search, `Enter` resolve/episodes, and `Enter` play actually fire workers.
// `config` (P18) is copied into App::config; null (demo_mode / most tests)
// gets Config{} defaults. `config_file` is the resolved save path (empty =
// persist_settings warns and skips the write, same as a missing config dir).
// `auth_file` (P19) is the resolved auth.json path; empty skips the startup
// Auth::load (demo_mode / most tests stay signed-out) and connect fails with
// a toast (no path to save to) rather than silently dropping the token.
// `db_file` (P20) is the resolved store db path; empty skips every sync call
// site (demo_mode / no-persist tests) since the sync worker has nothing to
// Store::open — it never reuses AppDeps::store (the UI thread's own
// connection; store.hpp's ownership comment covers why the worker needs its
// own).
// `current_version` (P28) is the running build's version (main.cpp's
// SHIGOKU_VERSION); empty skips the boot-time update check the same way an
// empty auth_file/db_file skips their subsystems (demo_mode / most tests).
int run(bool demo_mode = false, const AppDeps* deps = nullptr,
        const Config* config = nullptr, std::string config_file = {},
        std::string auth_file = {}, std::string db_file = {},
        std::string current_version = {});

// --- internals exposed for tests -------------------------------------------
namespace detail {
// The idle-help string for a view+pane (DESIGN §7.5), plain text (styling is a
// render concern). Exposed so a test can assert the subset.
[[nodiscard]] std::string idle_help(View v, Pane p);

// P32: the view whose top-bar tab covers column `x` on row 0, or nullopt for
// the gaps/name/chip. Replays draw_top_bar's span walk off the same label +
// key sources, so the hit-test can never disagree with the render. Exposed
// for tests.
[[nodiscard]] std::optional<View> top_bar_tab_at(int x);

// --- P7 pure helpers, exposed for tests + views.cpp -------------------------

// A8 degenerate classifier: mal_id present -> Tier-A canonical_key via the
// provider; else Tier-C (caller does the search(); this only picks Tier).
[[nodiscard]] Tier classify_tier(const Enrichment& show,
                                 const StreamProvider& provider);

// Toast (kind, copy) for a ProviderError, per DESIGN §4.10. `provider` is the
// display_name() spliced into "{provider} blocked us" etc.
[[nodiscard]] std::pair<ToastKind, std::string> provider_error_toast(
    const ProviderError& e, std::string_view provider);

// Toast (kind, copy) for a terminal PlayError, per DESIGN §4.10. mpv-not-found
// is its own Kind::MpvNotFound (P22, debt #5) — no detail-text sniffing.
[[nodiscard]] std::pair<ToastKind, std::string> play_error_toast(
    const PlayError& e, std::string_view provider);

// --- P8 cover helpers, exposed for tests -----------------------------------

// The cover placement rectangle for the current frame (P8, DESIGN §3.3): the
// detail cover's origin cell + cols×rows box, or an empty Rect when no cover is
// on screen (list-only width, no selection, placeholder mode, or a Detail zoom
// with no room). Pure over App + the terminal geometry; draw() uses it to mark
// the exclusion, run() to size the transmit. Cover width is the DESIGN §3.8
// tier (cover_tier) clamped to the detail column; height is detail_cover_rows.
[[nodiscard]] Rect cover_rect(const App& app);

// The (id, url) the cover SHOULD show for the current selection, or nullopt
// when the selected row has no cover / no selection. The reconcile passes this
// to CoverState::decide. Pure over App.
struct CoverTarget {
  std::int64_t id = 0;
  std::string url;
};
[[nodiscard]] std::optional<CoverTarget> cover_target(const App& app);

// One grid cover's placement: the id the terminal holds it under + the rect
// it was last transmitted into (P17b, §6).
struct Placed {
  std::uint32_t id = 0;
  Rect rect;
};

// The detail cover's placement: id + which show's pixels + the rect they were
// transmitted into. run()-owned across frames so compose_cover_apc can tell
// "already placed, emit nothing" from a real change — without it the composer
// re-transmitted the full ~180KB APC EVERY frame (25-40/s), which flooded
// the PPC terminal into an mpv-quit freeze (term.write blocks
// once the pty backs up).
struct PlacedCover {
  std::uint32_t id = 0;       // 0 = nothing placed.
  std::int64_t for_id = 0;    // whose pixels the terminal holds.
  Rect rect;                  // where they were placed.
};

// Purge every terminal-side placement after a geometry change: under the
// kitty backend fills `purge` with a delete APC per held id; under iterm no
// bytes exist to send (an OSC 1337 image dies with the cells the resize ED
// clear wipes). Either way both placement records empty, so the next
// compose_* pass re-transmits from scratch. A resize breaks the steady-state
// premise ("the terminal still holds this placement at this rect") —
// terminals reflow, re-anchor, or drop images on resize, and the placement
// model must not survive what the terminal did not. Returns whether anything
// was held (the caller forces a full repaint then, covering the case where
// the new layout wants no images at all). Exposed for the resize-purge test.
bool purge_placements(CoverBackend backend, PlacedCover& placed_cover,
                      std::map<std::string, Placed>& placed_grid,
                      std::string& purge);

// Compose the A4 cover APC deltas for one frame given the placement the
// terminal currently holds (`placed`, run()-owned across frames) and the app's
// cover_render. Fills `pre_diff` (delete the outgoing placement — kitty only;
// under iterm the forced repaint IS the erase) and `post_diff` (CUP to the
// cover origin + transmit the incoming one), updates `placed`, and returns
// whether the placement changed (the caller forces a full repaint then — the
// exclusion made prev skip those cells). Steady state (same show, same rect,
// already placed) emits NOTHING. Emits nothing in placeholder mode.
bool compose_cover_apc(const App& app, PlacedCover& placed,
                       std::string& pre_diff, std::string& post_diff);

// Generalizes compose_cover_apc for the P17 card grid (§6, R1/R3). `placed`
// is run()-owned across frames (url -> Placed); fills `pre_diff`/`post_diff`
// the same way, and returns whether ANY placement changed (appear/disappear/
// move) so the caller can force mark_all_dirty. Emits nothing outside
// Discover or in placeholder mode.
bool compose_grid_apc(const App& app, std::map<std::string, Placed>& placed,
                      std::string& pre_diff, std::string& post_diff);

// Poll `done` at a 20ms granularity until it flips true or `timeout` elapses
// (P20, 04 §11). Returns whether it finished (false = the deadline hit
// first). Exposed so the quitFlush contract ("a hung endpoint cannot wedge
// quit") is directly unit-testable without driving the whole terminal
// lifecycle.
bool bounded_wait(std::atomic<bool>& done, std::chrono::milliseconds timeout);

}  // namespace detail

}  // namespace shigoku::tui
