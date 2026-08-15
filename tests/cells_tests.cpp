// cells_tests.cpp — P6 CellBuffer / width / diff / exclusion tests (A3).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "../src/tui/cells.hpp"

using namespace shigoku::tui;

namespace {
// Reconstruct the visible glyphs from a flush blob by stripping every ANSI
// escape sequence (CSI "\x1b[ … final" and the "\x1b[?…h/l" toggles). Each cell
// re-emits its own SGR (the run-length optimizer is a deferred A3 nicety), so
// glyphs are not contiguous in the raw output — the diff correctness lives in
// which glyphs survive, which this exposes.
std::string visible_text(const std::string& out) {
  std::string v;
  for (std::size_t i = 0; i < out.size();) {
    if (out[i] == '\x1b') {
      i += 2;  // skip ESC + '['
      while (i < out.size() && !(out[i] >= 0x40 && out[i] <= 0x7e)) ++i;
      if (i < out.size()) ++i;  // skip the final byte
    } else {
      v.push_back(out[i]);
      ++i;
    }
  }
  return v;
}
}  // namespace

TEST_CASE("char_width classifies ascii / wide / zero-width") {
  CHECK(char_width(U'a') == 1);
  CHECK(char_width(U' ') == 1);
  CHECK(char_width(U'\n') == 0);   // C0 control
  CHECK(char_width(0x200B) == 0);  // zero-width space
  CHECK(char_width(0x0301) == 0);  // combining acute
  CHECK(char_width(U'冬') == 2);   // CJK (season kanji)
  CHECK(char_width(U'あ') == 2);   // Hiragana
  CHECK(char_width(0x1F600) == 2); // emoji (grinning face)
}

TEST_CASE("str_width sums display columns") {
  CHECK(str_width("abc") == 3);
  CHECK(str_width("冬 2026") == 2 + 1 + 4);  // 冬(2) space(1) 2026(4)
  CHECK(str_width("") == 0);
}

TEST_CASE("truncate_to_cols budgets the ellipsis and cuts on boundary") {
  // Fits: returned unchanged.
  CHECK(truncate_to_cols("frieren", 10) == "frieren");
  // Over budget: content + "…" within max_cols.
  const std::string t = truncate_to_cols("frieren beyond journey", 10);
  CHECK(str_width(t) <= 10);
  CHECK(t.substr(t.size() - 3) == "\xE2\x80\xA6");  // ends with …
  // Wide glyphs don't get split: a 2-col glyph that would overflow is dropped.
  const std::string w = truncate_to_cols("冬冬冬冬冬", 5);  // 5 cols budget
  CHECK(str_width(w) <= 5);
}

TEST_CASE("put_str advances by display width and marks continuations") {
  CellBuffer b(10, 1);
  const int end = b.put_str(0, 0, "a冬b", theme::fg, theme::bg);
  CHECK(end == 4);  // a(1) 冬(2) b(1) = col 4
  CHECK(b.at(0, 0).glyph == U'a');
  CHECK(b.at(1, 0).glyph == U'冬');
  CHECK(b.at(1, 0).continuation == false);
  CHECK(b.at(2, 0).continuation == true);  // right half of 冬
  CHECK(b.at(3, 0).glyph == U'b');
}

TEST_CASE("flush emits a full frame first, then only diffs") {
  CellBuffer prev;  // empty -> forces full paint of `buf`.
  CellBuffer buf(20, 3);
  buf.clear(theme::bg);
  buf.put_str(0, 0, "hi", theme::fg, theme::bg);

  std::string out1;
  buf.flush(prev, out1);
  CHECK(out1.find("\x1b[?2026h") != std::string::npos);  // sync begin
  CHECK(out1.find("\x1b[?2026l") != std::string::npos);  // sync end
  CHECK(visible_text(out1).find("hi") != std::string::npos);

  // No change -> the diff is empty: no glyphs re-emitted (only the frame wrap).
  std::string out2;
  buf.flush(prev, out2);
  CHECK(visible_text(out2).find("hi") == std::string::npos);  // not re-emitted
  CHECK(out2.find("\x1b[?2026h") != std::string::npos);  // still a framed flush

  // One cell changes -> only that shows up.
  buf.put_str(0, 1, "X", theme::fg, theme::bg);
  std::string out3;
  buf.flush(prev, out3);
  CHECK(visible_text(out3).find("X") != std::string::npos);
  CHECK(visible_text(out3).find("hi") == std::string::npos);
}

TEST_CASE("exclusion rectangle is never painted by the diff") {
  CellBuffer prev;
  CellBuffer buf(20, 5);
  buf.clear(theme::bg);
  // Mark rows 1..3, cols 2..7 image-owned, then try to write text into it.
  buf.set_exclusion(Rect{2, 1, 6, 3});
  buf.put_str(2, 2, "SHOULD_NOT_APPEAR", theme::fg, theme::bg);
  // A glyph outside the rect must still paint.
  buf.put_str(0, 0, "OUTSIDE", theme::fg, theme::bg);

  std::string out;
  buf.flush(prev, out);
  const std::string vis = visible_text(out);
  CHECK(vis.find("OUTSIDE") != std::string::npos);
  CHECK(vis.find("SHOULD_NOT_APPEAR") == std::string::npos);
  // The 'S' at col 2 (inside the rect) must not have leaked.
  CHECK(vis.find("SHOULD") == std::string::npos);
}

TEST_CASE("multi-exclusion: two disjoint rects are both skipped by the diff") {
  CellBuffer prev;
  CellBuffer buf(30, 5);
  buf.clear(theme::bg);
  buf.set_exclusions({Rect{1, 1, 4, 2}, Rect{20, 1, 4, 2}});
  buf.put_str(1, 1, "LEFT", theme::fg, theme::bg);
  buf.put_str(20, 1, "RIGHT", theme::fg, theme::bg);
  buf.put_str(10, 1, "MIDDLE", theme::fg, theme::bg);

  std::string out;
  buf.flush(prev, out);
  const std::string vis = visible_text(out);
  CHECK(vis.find("LEFT") == std::string::npos);
  CHECK(vis.find("RIGH") == std::string::npos);  // "RIGHT" truncated at col 24, still excluded.
  CHECK(vis.find("MIDDLE") != std::string::npos);
}

TEST_CASE("multi-exclusion: clearing a rect lets the diff repaint that region") {
  CellBuffer prev;
  CellBuffer buf(20, 5);
  buf.clear(theme::bg);
  buf.set_exclusions({Rect{2, 1, 6, 3}});
  buf.put_str(2, 2, "HIDDEN", theme::fg, theme::bg);
  std::string out1;
  buf.flush(prev, out1);
  CHECK(visible_text(out1).find("HIDDEN") == std::string::npos);

  // Clear the exclusion and force a full repaint (a real placement-change
  // caller always pairs the clear with mark_all_dirty, app.cpp's rule).
  buf.set_exclusions({});
  buf.mark_all_dirty();
  std::string out2;
  buf.flush(prev, out2);
  CHECK(visible_text(out2).find("HIDDEN") != std::string::npos);
}

TEST_CASE("flush splices pre/post raw payloads in composition order") {
  CellBuffer prev;
  CellBuffer buf(10, 2);
  buf.clear(theme::bg);
  buf.put_str(0, 0, "z", theme::fg, theme::bg);

  std::string out;
  buf.flush(prev, out, "PRE_DELETE", "POST_PLACE");
  const auto h = out.find("\x1b[?2026h");
  const auto pre = out.find("PRE_DELETE");
  const auto glyph = out.find("z");
  const auto post = out.find("POST_PLACE");
  const auto l = out.find("\x1b[?2026l");
  // Order: sync begin < pre-diff < cell diff < post-diff < sync end.
  CHECK(h < pre);
  CHECK(pre < glyph);
  CHECK(glyph < post);
  CHECK(post < l);
}

TEST_CASE("resize forces a full repaint against a stale prev") {
  CellBuffer prev(20, 3);
  CellBuffer buf(20, 3);
  buf.clear(theme::bg);
  buf.put_str(0, 0, "aa", theme::fg, theme::bg);
  std::string out1;
  buf.flush(prev, out1);  // establishes prev

  // Grow: prev geometry differs -> full repaint (aa re-emitted).
  buf.resize(30, 4);
  buf.clear(theme::bg);
  buf.put_str(0, 0, "aa", theme::fg, theme::bg);
  std::string out2;
  buf.flush(prev, out2);
  CHECK(visible_text(out2).find("aa") != std::string::npos);
}

TEST_CASE("char_width: every kWide range is reachable (P30 sort-order pin)") {
  // The mahjong-tiles range sat after plane-2/3 CJK and was unreachable under
  // in_ranges' sorted-early-return until the P30 audit re-sorted the table.
  CHECK(char_width(0x1F004) == 2);  // 🀄 mahjong red dragon
  CHECK(char_width(0x1F02F) == 2);  // last mahjong tile
  CHECK(char_width(0x20000) == 2);  // CJK Ext B still wide after the re-sort
  CHECK(char_width(0x3134A) == 2);  // CJK Ext G
  CHECK(char_width(0x1F300) == 2);  // emoji pictographs unaffected
}

TEST_CASE("flush ED-clears on a geometry change, not on a forced full repaint") {
  // A resize means the terminal's own crop/reflow/scroll may have left
  // residue anywhere (even cells a mid-drag winsize mismatch kept outside
  // the grid we repaint) — the flush must start that frame from blank.
  CellBuffer prev;  // 0x0 -> the first flush IS a geometry change.
  CellBuffer buf(20, 3);
  buf.clear(theme::bg);
  std::string out1;
  buf.flush(prev, out1);
  CHECK(out1.find("\x1b[2J") != std::string::npos);

  // Same size, forced full (the cover-changed path): every cell re-emits but
  // the screen is still cell-accurate — no ED, no flash risk on slow links.
  buf.mark_all_dirty();
  std::string out2;
  buf.flush(prev, out2);
  CHECK(out2.find("\x1b[2J") == std::string::npos);

  // Shrink -> geometry change again -> ED.
  buf.resize(12, 2);
  buf.clear(theme::bg);
  std::string out3;
  buf.flush(prev, out3);
  CHECK(out3.find("\x1b[2J") != std::string::npos);
}
