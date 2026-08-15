// discover.hpp — the Discover card-grid state (P17a, DESIGN §3.8 / 04 §7.5).
// Ported 1:1 from sabigoku src/tui/view/discover.rs (the STATE half, lines
// 1-364): four independent per-axis slots, pure nav/scroll, the feed-fetch
// decision (wanted_fetch), and page filing (on_feed). Rendering lives in
// views.cpp (draw_discover); this header owns only state + the geometry and
// glyph vocabulary that draw, nav, and the fetch pump must share so they can
// never disagree (the zigoku ROD-243 single-geometry-source shape).
//
// Clock: Rust's `loading: Option<AsyncStart(Instant)>` becomes a `bool
// loading` + a `TickCount loading_since` (04 §8 wall-clock-free discipline —
// the same swap CoverState already made). The spinner reads loading_since
// against tick_count; the state machine only needs the bool.
//
// Store seam: on_feed upserts catalog_cache best-effort. Store is forward-
// declared (app.hpp's rule — the render core never includes store.hpp); the
// definition lands only in discover.cpp (a shigoku_tui TU, like app.cpp).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../domain.hpp"
#include "../event.hpp"  // CatalogRow.
#include "cells.hpp"     // Rect.
#include "cover_geom.hpp"
#include "toast.hpp"  // TickCount.

namespace shigoku {
class Store;  // on_feed writes catalog_cache; store.hpp is a discover.cpp-only include.
}

namespace shigoku::tui {

// Freeze enum order; UI tab order matches (04 §7.5).
inline constexpr std::array<DiscoverAxis, 4> kDiscoverAxes = {
    DiscoverAxis::Trending, DiscoverAxis::Popular, DiscoverAxis::TopRated,
    DiscoverAxis::ThisSeason};

// Force-exhaustion rail (discover.rs MAX_FEED_ROWS, ROD-339).
inline constexpr std::size_t kMaxFeedRows = 300;
// Prefetch when the cursor comes within this many card-rows of the last
// loaded entry (discover.rs PREFETCH_ROWS, DESIGN §8.6).
inline constexpr std::size_t kPrefetchRows = 2;
// Peek band renders only when at least this tall (discover.rs MIN_PEEK_ROWS).
inline constexpr int kMinPeekRows = 3;

// Axis tab label (discover.rs axis_label).
[[nodiscard]] const char* axis_label(DiscoverAxis axis);

// Index of an axis in kDiscoverAxes (discover.rs axis_idx); unknown → 0.
[[nodiscard]] std::size_t axis_index(DiscoverAxis axis);

// The §3.8a genre glyph vocabulary (discover.rs GENRE_GLYPHS): 18 entries, must
// match DESIGN §3.8a exactly — edit both together (drift is rot). Unmapped
// genres are silently skipped.
[[nodiscard]] const std::array<std::pair<const char*, const char*>, 18>& genre_glyphs_table();

// First two mappable genres, in AniList's returned order (discover.rs
// genre_glyphs). Returns UTF-8 glyph strings.
[[nodiscard]] std::vector<const char*> genre_glyphs(const std::vector<std::string>& genres);

// The compact card format label (render.rs format_label): the AniList `format`
// mapped to a short display string, or nullopt for an unknown/absent format.
[[nodiscard]] std::optional<const char*> format_label(std::optional<std::string_view> kind);

// Grid geometry shared by draw, nav, and the fetch pump so they can never
// disagree on what is visible (discover.rs GridGeo). `content_h` is the content
// band; the axis bar and its spacer (2 rows) are subtracted here.
struct GridGeo {
  CoverTier tier;
  int cover_h = 0;
  int slot_h = 0;       // cover_h + 4.
  std::size_t cols = 1;
  std::size_t rows_visible = 1;
};

// grid_geo(w, content_h, cell) (discover.rs grid_geo): the tier from the raw
// canvas width (full-canvas view), the adaptive card cover height, and the
// visible-rows count for the content band minus the axis bar.
[[nodiscard]] GridGeo grid_geo(int w, int content_h, CellPx cell);

// The on-screen cover-cell rect for card index `i` in a grid whose first
// column sits at `grid_x` and first row at `grid_y`, with `scroll_row` rows
// scrolled past — or an empty Rect when the card's row is scrolled out of
// view, or the card would clip the canvas right edge (`canvas_w`) or the
// grid's bottom edge (`grid_bottom`). Matches draw_discover's card loop
// exactly (P17b, R3: THE single geometry source both draw_discover and
// compose_grid_apc's placement diff call, so they can never disagree — the
// invariant the single-cover detail rect already keeps).
[[nodiscard]] Rect card_cover_rect(const GridGeo& geo, std::size_t i,
                                   std::size_t scroll_row, int grid_x, int grid_y,
                                   int canvas_w, int grid_bottom);

// One axis slot (discover.rs AxisSlot, 04 §7.5): independent results, cursor,
// scroll, and flags. Rank is positional per axis; slots never share or re-sort
// one list. Public so draw_discover (views.cpp) can read the rendered feed
// without touching store — the mutators stay on DiscoverState.
struct AxisSlot {
  std::vector<CatalogRow> entries;
  std::uint32_t page = 0;
  bool loading = false;
  TickCount loading_since = 0;  // tick of the loading false→true edge (spinner).
  // Sticky until an axis key retries (DESIGN §3.8: auto-refetch on tick would
  // storm the API). nullopt = armed; a value = the failure cause.
  std::optional<std::string> failed;
  bool exhausted = false;
  std::size_t cursor = 0;
  std::size_t scroll_row = 0;
};

// The Discover view's whole state: the active axis + four slots (discover.rs
// DiscoverState, state half only — covers/drains are P17b and never land here).
class DiscoverState {
 public:
  [[nodiscard]] DiscoverAxis axis() const { return axis_; }
  [[nodiscard]] const AxisSlot& slot() const { return slots_[axis_index(axis_)]; }
  [[nodiscard]] const AxisSlot& slot_for(DiscoverAxis a) const { return slots_[axis_index(a)]; }

  // The entry under the active slot's cursor, or nullptr (discover.rs
  // selected_entry). Feeds the Enter→Detail zoom and the P-key add.
  [[nodiscard]] const Enrichment* selected_entry() const;

  // Mutable variant: the P21 refresh-on-view writes a healed Enrichment back
  // into the active feed row in place (the shared detail pane renders live off
  // this row, so a heal must land here to be visible).
  [[nodiscard]] Enrichment* selected_entry_mut();

  // `[` / `]` cycle with wraparound; switching preserves every slot's state
  // (DESIGN §8.6) and re-arms a failed slot for retry (discover.rs cycle_axis).
  void cycle_axis(std::int64_t delta);

  // `1`-`4` direct select; out-of-range keys are ignored (discover.rs
  // select_axis). `index` is 0-based into kDiscoverAxes.
  void select_axis(std::size_t index);

  // hjkl over the grid: left/right wrap within a row, up/down move card-rows
  // and clamp (discover.rs nav, DESIGN §7.5).
  void nav(std::int64_t dx, std::int64_t dy, const GridGeo& geo);

  // Click-to-select (P32, §9 — no Rust precedent): put the cursor on card
  // `index` (clamped to the loaded entries), keeping the scroll lawful via
  // the same clamp nav uses.
  void select(std::size_t index, const GridGeo& geo);

  // The feed decision, pure: which page (if any) the active axis wants
  // (discover.rs wanted_fetch). Empty slot → page 1; cursor within
  // kPrefetchRows of the last loaded entry → page+1; loading/failed/exhausted
  // all hold. nullopt = no fetch wanted.
  [[nodiscard]] std::optional<std::uint32_t> wanted_fetch(const GridGeo& geo) const;

  // Mark the active slot loading at `now` (spawn-side; the tick pump arms this
  // before spawning the feed worker, and clears it on a spawn failure so the
  // spinner can never strand — discover.rs fire_fetch).
  void arm_loading(TickCount now);
  void disarm_loading();

  // File a page into its axis slot (discover.rs on_feed, 04 §4.2/§6): an
  // out-of-order page is discarded, and every applied row upserts catalog_cache
  // best-effort (a cache write failure never drops a rendered feed);
  // exhausted = !has_next || len >= kMaxFeedRows. `store` is nullable — no
  // store (demo / no-persist) simply skips the cache write and files the feed
  // unchanged (the same "never drop a rendered feed" contract, at its limit).
  //
  // `gen` is the filter generation the fetch was SPAWNED under (P38 review):
  // the positional page check only orders pages within one filter set — an
  // answer spawned under the old filters that lands after set_filters reset
  // the slots would otherwise file as a perfectly-positioned page 1 of the
  // NEW filters. A mismatched gen discards whole, before even touching
  // `loading` (which now belongs to the new generation's own fetch).
  void on_feed(DiscoverAxis axis, std::uint32_t page, std::vector<CatalogRow> entries,
               bool has_next, Store* store, std::int64_t now_unix, std::uint32_t gen);

  // A feed fetch failed on `axis`: clear loading, set the sticky failed cause
  // (discover.rs on_feed_error). Same gen discipline as on_feed: a stale-
  // generation failure must not poison a freshly re-armed slot.
  void on_feed_error(DiscoverAxis axis, std::string cause, std::uint32_t gen);

  // The active filter set (P38, §9 — no Rust precedent).
  [[nodiscard]] const DiscoverFilters& filters() const { return filters_; }

  // Replace the filter set and re-arm EVERY slot (not just the active one):
  // filters compose into discover_body regardless of which axis is on
  // screen, so a filter changed while looking at Trending must also
  // invalidate Popular/Top Rated/This Season's already-fetched pages — they
  // were fetched under the old filters and would otherwise render stale
  // rows under the new ones. Re-arm mirrors cycle_axis/select_axis exactly
  // (entries/page/exhausted reset to a fresh slot, failed cleared) so the
  // very next wanted_fetch reports page 1. The positional page check alone
  // is NOT enough across a filter change — an old-filter answer still in
  // flight would file as the new set's page 1 — so set_filters also bumps
  // filter_gen(), which on_feed/on_feed_error check (P38 review). A no-op
  // commit (identical filters) leaves everything untouched.
  void set_filters(DiscoverFilters filters);

  // The current filter generation (P38 review): bumped by set_filters, stamped
  // into every spawned fetch, checked by on_feed/on_feed_error.
  [[nodiscard]] std::uint32_t filter_gen() const { return filter_gen_; }

 private:
  [[nodiscard]] AxisSlot& active();
  void clamp_scroll(const GridGeo& geo);

  DiscoverAxis axis_ = DiscoverAxis::Trending;
  std::array<AxisSlot, 4> slots_;
  DiscoverFilters filters_;
  std::uint32_t filter_gen_ = 0;
};

// `NEW` marks a current-cour release, suppressed on This Season where every
// card qualifies by construction (discover.rs is_new_this_cour); exclusive with
// `TOP`. Exposed for draw_discover and the tests.
[[nodiscard]] bool is_new_this_cour(const Enrichment& e, DiscoverAxis axis, Cour cour);

}  // namespace shigoku::tui
