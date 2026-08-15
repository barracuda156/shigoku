// cells.hpp — the hand-rolled cell buffer + width tables + ANSI diff flush
// (P6, A3). ratatui's role in ~1 file: draw code writes cells; flush() diffs
// against the previous frame and emits minimal truecolor ANSI, wrapped in one
// synchronized-output frame (CSI ?2026h … ?2026l), one write().
//
// A4 seam: flush() takes raw escape payloads to splice at defined points of the
// frame — the cover emitter (P8) deletes the outgoing placement BEFORE the diff
// and transmits the incoming one AFTER it, all inside the same ?2026 wrap. An
// exclusion rectangle marks the image-owned cover region so the diff never
// paints into live kitty fragments (A4 cell-ownership rule).
//
// Width: East-Asian wide glyphs occupy two cells; a compact vendored table
// (no locale wcwidth(), broken on the legacy targets, §3) classifies them.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "theme.hpp"

namespace shigoku::tui {

// Style bits (DESIGN §1.3 type system: bold/dim/italic; underline/blink unused
// but the bits exist so the enum is the full closed set A2 guards).
enum class Style : std::uint8_t {
  None = 0,
  Bold = 1u << 0,
  Dim = 1u << 1,
  Italic = 1u << 2,
};
inline Style operator|(Style a, Style b) {
  return static_cast<Style>(static_cast<std::uint8_t>(a) |
                            static_cast<std::uint8_t>(b));
}
inline bool has(Style set, Style bit) {
  return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(bit)) != 0;
}

// One terminal cell. `glyph == 0` is an empty cell (rendered as a space over
// its bg). The trailing half of a wide glyph is a `continuation` cell: the diff
// skips emitting it (the wide glyph to its left already advanced two columns).
struct Cell {
  char32_t glyph = U' ';
  Rgb fg = theme::fg;
  Rgb bg = theme::bg;
  Style style = Style::None;
  bool continuation = false;  // right half of a wide glyph; never emitted alone.
  friend bool operator==(const Cell&, const Cell&) = default;
};

// Display width of a codepoint: 0 (zero-width / combining), 1 (normal), or 2
// (East-Asian wide / fullwidth / most emoji). Vendored ranges, no locale.
[[nodiscard]] int char_width(char32_t cp);

// Display width of a UTF-8 string in terminal columns (sum of char_width over
// its codepoints). Malformed bytes count as width 1 (one replacement column).
[[nodiscard]] int str_width(std::string_view utf8);

// Truncate `utf8` to at most `max_cols` display columns, appending `ellipsis`
// (default "…") if truncation happened, keeping the RESULT within max_cols
// (the ellipsis's own width is budgeted). Cuts on a codepoint boundary.
[[nodiscard]] std::string truncate_to_cols(std::string_view utf8, int max_cols,
                                           std::string_view ellipsis = "…");

// A rectangle in cell coordinates (x0,y0 inclusive; w,h in cells). Empty when
// w==0 || h==0.
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  [[nodiscard]] bool empty() const { return w <= 0 || h <= 0; }
  [[nodiscard]] bool contains(int cx, int cy) const {
    return !empty() && cx >= x && cx < x + w && cy >= y && cy < y + h;
  }
  friend bool operator==(const Rect&, const Rect&) = default;
};

// The grid. Draw code fills it each frame; flush() diffs it against the prior
// frame. Resizing clears to blank (a resize always forces a full repaint).
class CellBuffer {
 public:
  CellBuffer() = default;
  CellBuffer(int w, int h) { resize(w, h); }

  [[nodiscard]] int width() const { return w_; }
  [[nodiscard]] int height() const { return h_; }

  // Resize to w×h, clearing every cell to blank-on-bg. No-op if unchanged.
  void resize(int w, int h);

  // Clear every cell to a blank cell with the given bg (default theme bg).
  void clear(Rgb bg = theme::bg);

  // Bounds-checked cell access. Out-of-range writes are dropped; reads return a
  // blank cell. (Draw code stays branch-light; the buffer clips.)
  void set(int x, int y, const Cell& c);
  [[nodiscard]] const Cell& at(int x, int y) const;

  // Fill a rectangle with a blank cell of the given bg.
  void fill(const Rect& r, Rgb bg);

  // Write `utf8` starting at (x,y), advancing by each glyph's display width
  // (wide glyphs occupy two cells; the second is a continuation cell). Stops at
  // the row's right edge or the buffer edge. Returns the column after the last
  // glyph written. Zero-width codepoints are dropped (no combining support in
  // v0 — they would need to merge onto the previous cell; not needed yet).
  int put_str(int x, int y, std::string_view utf8, Rgb fg, Rgb bg,
              Style style = Style::None);

  // Exclusion rectangles (A4): while set, flush() emits NO cell output inside
  // ANY of them — each region is image-owned (a live kitty placement). Empty
  // vector = none. set_exclusion is the single-rect convenience overload (the
  // detail cover call site); set_exclusions replaces the whole set (the P17b
  // grid, one rect per placed cover).
  void set_exclusion(const Rect& r) {
    exclusions_.clear();
    if (!r.empty()) exclusions_.push_back(r);
  }
  void set_exclusions(std::vector<Rect> rs) { exclusions_ = std::move(rs); }
  [[nodiscard]] const std::vector<Rect>& exclusions() const { return exclusions_; }

  // Diff this buffer against `prev` and append the frame to `out`, wrapped in
  // one synchronized-output block. Composition order (A4, normative):
  //   ?2026h  ->  pre_diff  ->  <cell diff>  ->  post_diff  ->  CUP(cursor)  ->
  //   ?2026l
  // `pre_diff` is where the cover delete goes; `post_diff` is where the cover
  // transmit+place goes (both raw escape payloads, empty in P6). After a
  // successful append, `prev` is updated to match this buffer so the NEXT flush
  // diffs against what is now on screen. `cursor` is where the real cursor is
  // left (hidden anyway in the TUI, but CUP keeps state owned by the buffer).
  void flush(CellBuffer& prev, std::string& out,
             std::string_view pre_diff = {}, std::string_view post_diff = {},
             std::optional<std::pair<int, int>> cursor = std::nullopt) const;

  // Force the next flush to repaint every cell (used after a resize/full
  // invalidate, where `prev` no longer matches the screen).
  void mark_all_dirty() { force_full_ = true; }

 private:
  int w_ = 0, h_ = 0;
  std::vector<Cell> cells_;
  std::vector<Rect> exclusions_;
  mutable bool force_full_ = false;
};

}  // namespace shigoku::tui
