// views.hpp — Browse two-pane (list + detail) + episode grid render (P7,
// DESIGN §3.1-3.7/§4.1-4.6/§5.3). PURE: reads App + writes cells, no I/O, no
// store, no network — same rule as app.cpp's draw() (enforced by not
// #including http/store here; the CMake link graph enforces it further, A2).
//
// app.cpp's draw() calls these for the content region (rows kContentY0..
// height-2); the chrome (top/bottom bar, toasts, degraded frame) stays in
// app.cpp.

#pragma once

#include "app.hpp"
#include "cells.hpp"

namespace shigoku::tui {

// Frame chrome rows (layout.rs frame_rows): row 0 = top bar, row 1 = spacer,
// content [kContentY0, rows-1), row rows-1 = bottom bar. The spacer row is
// part of the design — draw sites and hit-tests must share this origin, never
// a hand-typed 1.
inline constexpr int kContentY0 = 2;

// Rows the content region holds at `win_rows` total — the scroll budgets'
// shared height (frame_rows' content_h: minus top bar, spacer, bottom bar).
[[nodiscard]] constexpr int content_height(int win_rows) {
  return win_rows - 1 - kContentY0 > 0 ? win_rows - 1 - kContentY0 : 0;
}

// pane_split(w) (DESIGN §3.2, single source of truth): the list column width
// and the detail column's origin/width. Below kPaneSplitMin the list takes
// the full content width and there is no detail column (list_w == w case is
// signaled by the caller checking width, not by this function).
struct PaneSplit {
  int list_w = 0;
  int detail_x = 0;
  int detail_w = 0;
};
[[nodiscard]] PaneSplit pane_split(int w);

// Render the Browse view's content region (list + detail panes, or the
// full-width list alone below kPaneSplitMin) into `buf` rows [y0, y1).
void draw_browse(const App& app, CellBuffer& buf, int y0, int y1);

// Render the Detail zoom's content region (full-canvas detail + grid).
void draw_detail_zoom(const App& app, CellBuffer& buf, int y0, int y1);

// Render the History view's content region: the grouped list (DESIGN §5.4)
// full-width below kPaneSplitMin or with no focused record, or the list +
// shared detail pane two-pane split (§5.4a) otherwise — the same pane grammar
// draw_browse uses, reusing the same detail column renderer.
void draw_history(const App& app, CellBuffer& buf, int y0, int y1);

// Render the Schedule view's content region (P37): the weekday-grouped
// countdown list, full-width, no detail pane — a read-only lens over the
// same library rows History shows, so it needs no two-pane/cover geometry.
void draw_schedule(const App& app, CellBuffer& buf, int y0, int y1);

// Render the Discover view's content region (P17, DESIGN §3.8): the axis bar
// + the full-canvas card grid over the active axis slot. Each cover cell is
// image-owned (an exclusion rect, compose_grid_apc transmits after the diff)
// when the CoverPool render store holds Ready pixels for that url; otherwise
// a placeholder `#N` on surface fill, with a spinner overlay while that url
// is Loading (placeholder mode / kitty-off: every cell stays the placeholder
// path, P17a's whole story). Sets buf's exclusions to exactly the placed grid
// covers this frame. Empty-grid states (loading/error/no-entries) center on
// the band.
void draw_discover(const App& app, CellBuffer& buf, int y0, int y1);

// Render the Settings view's content region (P18, DESIGN §5.5): five sections
// (Player/Catalog/Interface/AniList Sync/Updates), each a header + hairline
// rule + rows, one blank line between sections. Full width, no cover column,
// no detail pane. Stateless scroll: keeps the focused row on screen,
// top-anchored otherwise (settings.rs draw's own rule, ported verbatim).
void draw_settings(const App& app, CellBuffer& buf, int y0, int y1);

// Season chips (DESIGN §2.3/§3.4, render.rs season_kanji/cour_chip/
// season_chip, P30). `kanji` renders the 冬/春/夏/秋 glyph form; off renders
// the English word (detail.rs season_chip_text's fallback — parity-floor
// improvement: the top bar honors the kanji_chips toggle too).
[[nodiscard]] std::string_view season_name(Season s, bool kanji);
// "冬 2026"-style chip for the ambient cour fallback; never empty.
[[nodiscard]] std::string cour_chip(const Cour& cour, bool kanji);
// Chip for a show's own season; absent unless BOTH parts are known (never an
// empty chip).
[[nodiscard]] std::optional<std::string> season_chip(std::optional<Season> season,
                                                     std::optional<std::uint32_t> year,
                                                     bool kanji);

namespace detail {
// The `c` section's row budget (P36): 0 when too narrow to split legibly.
// Single source for both the render split and app.cpp's scroll-into-view
// math. Exposed for tests.
[[nodiscard]] int char_recs_section_height(int content_rows);

// The Schedule countdown column's display string (P37): "<1m", "37m",
// "4h 12m", "2d 4h". Pure; exposed for the DoD table test.
[[nodiscard]] std::string countdown_text(std::int64_t secs);

// The flattened line offset of recommendations[rec_idx] in the `c` section's
// rendered line list (P36 slice 3), for app.cpp's scroll-into-view math.
// Exposed for tests.
[[nodiscard]] std::size_t char_recs_rec_line_index(const CharactersAndRecommendations& data,
                                                    std::size_t rec_idx);

// The provider caption (05 §15): "▸ {serving} · {other}[+/-/?]" from the grid's
// serving/pin/availability state. Pure; "" when nothing serves yet. Exposed for
// tests.
[[nodiscard]] std::string provider_caption(const EpisodeState& es);

// Word-wrap a sanitized description (a flat single line — the enrichment
// mapper collapses newline runs) into lines of at most `max_cols` display
// columns, breaking on spaces; a word wider than the box hard-breaks on a
// codepoint boundary. Space runs collapse. Exposed for tests.
[[nodiscard]] std::vector<std::string> wrap_text(std::string_view text, int max_cols);

// The Enrichment the shared detail pane/zoom should render for the current
// selection (05 §15's "one detail renderer" pattern extended to History,
// P16): Browse reads app.catalog[list_cursor], History reads
// app.history.selected(). nullptr when nothing is selected. Exposed for the
// cover geometry (cover_target/cover_rect) and tests.
[[nodiscard]] const Enrichment* selected_enrichment(const App& app);

// The library Show for the current selection (P34), or nullptr — non-null
// ONLY when the selection resolves through History (a library row); Browse/
// Discover selections are catalog rows with no user_score. Exposed for tests.
[[nodiscard]] const Show* selected_show(const App& app);

// --- P32 mouse hit-testing (§9) --------------------------------------------
// Each helper maps a 0-based cell (x, y) to the thing rendered there, by
// walking the SAME geometry source its draw function walks (pane_split /
// HistoryState::layout / card_cover_rect / settings_layout / the detail
// header replay) — never a re-derivation, so click targets can never
// disagree with the render. All return nullopt when (x, y) misses or the
// surface isn't showing. Exposed for tests.

// Browse list column: the catalog index at (x, y).
[[nodiscard]] std::optional<int> browse_row_at(const App& app, int x, int y);

// History list column: the nav-order ord (HistoryLine::ord) whose Title or
// Bar row covers (x, y).
[[nodiscard]] std::optional<std::size_t> history_ord_at(const App& app, int x, int y);

// True when (x, y) falls inside the detail surface: the two-pane detail
// column (Browse/History at/above the split, with a selection rendered) or
// anywhere in the Detail zoom's content region.
[[nodiscard]] bool detail_column_contains(const App& app, int x, int y);

// The episode-grid cell index at (x, y), on whichever surface currently
// renders the grid (two-pane detail column or the zoom). Replays the header
// geometry into a scratch buffer to find the grid origin — the header height
// is content-dependent and draw_detail_header is its single source.
[[nodiscard]] std::optional<std::size_t> episode_cell_at(const App& app, int x, int y);

// P33: cells per visual row of the episode grid on whichever surface
// currently renders it (the same x0/cw branch episode_cell_at uses) — the
// single source for h/j/k/l's per-row hop. Always >= 1.
[[nodiscard]] std::size_t episode_grid_per_row(const App& app);

// Discover: the card index at (x, y) — the cover cell plus its three caption
// rows (rank/title/format), via card_cover_rect (the P17b single geometry
// source).
[[nodiscard]] std::optional<std::size_t> discover_card_at(const App& app, int x, int y);

// Discover: the axis index (into kDiscoverAxes) whose tab covers (x, y) on
// the axis bar row.
[[nodiscard]] std::optional<std::size_t> discover_axis_at(int x, int y);

// Settings: the kSettingsRows index of the Row line at (x, y).
[[nodiscard]] std::optional<std::size_t> settings_row_at(const App& app, int x, int y);
}  // namespace detail

}  // namespace shigoku::tui
