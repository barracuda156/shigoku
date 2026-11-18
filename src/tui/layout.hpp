// layout.hpp — frame geometry and text wrapping shared by every app built on
// the render core. Pure, and free of any app's state type: the anime views
// and the manga app both read the frame rows and the pane split from here.

#pragma once

#include <string>
#include <string_view>
#include <vector>

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

// DESIGN §3.2: below this width a two-pane view collapses to a single-column
// list (no detail pane); the Detail zoom demotes to `.detail` (not `.list`)
// at/above it.
inline constexpr int kPaneSplitMin = 60;

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

namespace detail {
// Word-wrap a sanitized description (a flat single line — the enrichment
// mapper collapses newline runs) into lines of at most `max_cols` display
// columns, breaking on spaces; a word wider than the box hard-breaks on a
// codepoint boundary. Space runs collapse. Exposed for tests.
[[nodiscard]] std::vector<std::string> wrap_text(std::string_view text, int max_cols);
}  // namespace detail

}  // namespace shigoku::tui
