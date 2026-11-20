// character_page.hpp — the character page of the Detail zoom: its line
// layout, geometry and render. PURE like views.hpp (reads App, writes cells).
//
// The page is one column of lines right of the portrait (the portrait rides
// the zoom's poster slot, see detail::cover_target): the name, the other
// names, the facts, the description, then the shows the character appears
// in. The column scrolls as a whole; the appearances are the only rows a
// cursor can rest on.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "app.hpp"
#include "cells.hpp"

namespace shigoku::tui {

// Render the character page into the zoom's content rows [y0, y1).
void draw_character_page(const App& app, CellBuffer& buf, int y0, int y1);

namespace detail {

// What a hidden spoiler span reads as.
inline constexpr std::string_view kSpoilerMark = "[spoiler]";

// A character description with its `~!` … `!~` spans hidden behind
// kSpoilerMark, or unwrapped when `reveal`. A span left open runs to the end.
[[nodiscard]] std::string spoiler_text(std::string_view text, bool reveal);

// Whether the page has anything `reveal_spoilers` would change.
[[nodiscard]] bool has_spoilers(const CharacterDetail& data);

struct CharacterLine {
  enum class Kind {
    Name,        // the heading.
    Names,       // native + alternative names (wrapped).
    Fact,        // `label` + `text`.
    Text,        // a description line.
    Note,        // loading / failure / empty state.
    Header,      // "Appears in".
    Appearance,  // data.appearances[idx].
    Blank,
  } kind = Kind::Blank;
  std::string label;  // Fact only.
  std::string text;
  std::size_t idx = 0;  // Appearance only.

  friend bool operator==(const CharacterLine&, const CharacterLine&) = default;
};

// The page's lines for a body `width` columns wide. Single source for the
// render and the key handler's scroll/cursor math.
[[nodiscard]] std::vector<CharacterLine> character_page_lines(const CharacterPageState& page,
                                                              int width);

// The body column for the current frame: right of the portrait when one is
// placed, the full canvas otherwise. `rows` == 0 means nothing can render.
struct CharacterPageBox {
  int x = 0;
  int w = 0;
  int y = 0;
  int rows = 0;
};
[[nodiscard]] CharacterPageBox character_page_box(const App& app);

// The body must keep this many columns for a portrait to be placed beside it.
inline constexpr int kCharacterBodyMinCols = 32;
// The gap between the portrait and the body.
inline constexpr int kCharacterBodyGap = 3;

// One step of the page's cursor walk: down scrolls the text to its end and
// then walks the appearances; up walks back out and scrolls up. `lines` and
// `rows` are what character_page_lines / character_page_box answered.
void character_page_step(CharacterPageState& page, const std::vector<CharacterLine>& lines,
                         int rows, int dir);
// g / G: the top of the page, or its last appearance (the end of the text
// when there are none).
void character_page_jump(CharacterPageState& page, const std::vector<CharacterLine>& lines,
                         int rows, bool top);
// Re-clamp scroll and cursor after the lines or the frame changed.
void character_page_clamp(CharacterPageState& page, const std::vector<CharacterLine>& lines,
                          int rows);

}  // namespace detail

}  // namespace shigoku::tui
