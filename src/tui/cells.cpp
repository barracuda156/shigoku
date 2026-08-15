// cells.cpp — CellBuffer, width tables, and the ANSI diff flush (P6, A3).
//
// ENDIANNESS (§3): every color is emitted from Rgb bytes via decimal text; no
// glyph or color is ever packed into a wider int. UTF-8 is decoded by hand
// (no locale). The whole module is byte-order independent.

#include "cells.hpp"

#include <array>

namespace shigoku::tui {

namespace {

// Decode one UTF-8 codepoint from s[i], advancing i past it. Malformed leads /
// truncated sequences consume one byte and yield that byte value (rendered as a
// single-column replacement). Mirrors domain.cpp's decoder, kept local.
char32_t decode_utf8(std::string_view s, std::size_t& i) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  auto cont = [&](std::size_t k) -> char32_t {
    if (i + k < s.size()) return static_cast<unsigned char>(s[i + k]) & 0x3Fu;
    return 0;
  };
  auto ok = [&](std::size_t need) {
    for (std::size_t k = 1; k <= need; ++k) {
      if (i + k >= s.size() ||
          (static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u)
        return false;
    }
    return true;
  };
  if (b0 < 0x80) {
    ++i;
    return b0;
  }
  if ((b0 & 0xE0u) == 0xC0u && ok(1)) {
    const char32_t cp = ((b0 & 0x1Fu) << 6) | cont(1);
    i += 2;
    return cp;
  }
  if ((b0 & 0xF0u) == 0xE0u && ok(2)) {
    const char32_t cp = ((b0 & 0x0Fu) << 12) | (cont(1) << 6) | cont(2);
    i += 3;
    return cp;
  }
  if ((b0 & 0xF8u) == 0xF0u && ok(3)) {
    const char32_t cp =
        ((b0 & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
    i += 4;
    return cp;
  }
  ++i;  // stray continuation / invalid lead.
  return b0;
}

void encode_utf8(char32_t c, std::string& out) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

struct Range {
  char32_t lo, hi;
};

// Zero-width: combining marks, ZWSP/joiners, variation selectors, the C0/C1
// controls (a control never advances the cursor here — strip_controls removes
// them before text reaches a cell, but classify defensively). A compact subset,
// enough for v0 (no full Unicode combining table).
constexpr std::array<Range, 8> kZeroWidth = {{
    {0x0300, 0x036F},  // combining diacritical marks
    {0x0483, 0x0489},  // combining Cyrillic
    {0x200B, 0x200F},  // ZWSP, ZWNJ/ZWJ, LRM/RLM
    {0x202A, 0x202E},  // bidi embeddings/overrides
    {0x2060, 0x2064},  // word joiner, invisible ops
    {0xFE00, 0xFE0F},  // variation selectors
    {0xFEFF, 0xFEFF},  // BOM / ZWNBSP
    {0x1AB0, 0x1AFF},  // combining diacritical marks extended
}};

// East-Asian Wide + Fullwidth ranges (the kanji chips, CJK titles, and common
// wide emoji the UI actually renders). A compact vendored subset of the EAW
// table — the full property table is thousands of entries; these cover the
// scripts sabigoku's fixtures and the season kanji use.
constexpr std::array<Range, 16> kWide = {{
    {0x1100, 0x115F},  // Hangul Jamo (wide)
    {0x2E80, 0x303E},  // CJK radicals, Kangxi, CJK symbols (incl. 冬春夏秋)
    {0x3041, 0x33FF},  // Hiragana, Katakana, CJK symbols/compat
    {0x3400, 0x4DBF},  // CJK Ext A
    {0x4E00, 0x9FFF},  // CJK Unified Ideographs
    {0xA000, 0xA4CF},  // Yi
    {0xAC00, 0xD7A3},  // Hangul syllables
    {0xF900, 0xFAFF},  // CJK compat ideographs
    {0xFE30, 0xFE4F},  // CJK compat forms
    {0xFF00, 0xFF60},  // Fullwidth forms
    {0xFFE0, 0xFFE6},  // Fullwidth signs
    {0x1F000, 0x1F02F}, // mahjong tiles (wide)
    {0x1F300, 0x1F64F}, // emoji: symbols/pictographs + emoticons
    {0x1F900, 0x1F9FF}, // supplemental symbols/pictographs
    {0x20000, 0x2FFFD}, // CJK Ext B..F (plane 2)
    {0x30000, 0x3FFFD}, // CJK Ext G (plane 3)
}};

bool in_ranges(char32_t cp, const Range* r, std::size_t n) {
  // Ranges are sorted and disjoint; a small linear scan is fine (tables tiny).
  for (std::size_t k = 0; k < n; ++k) {
    if (cp < r[k].lo) return false;  // sorted: no later range can match.
    if (cp <= r[k].hi) return true;
  }
  return false;
}

}  // namespace

int char_width(char32_t cp) {
  if (cp == 0) return 0;
  if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return 0;  // C0/C1 controls.
  if (in_ranges(cp, kZeroWidth.data(), kZeroWidth.size())) return 0;
  if (in_ranges(cp, kWide.data(), kWide.size())) return 2;
  return 1;
}

int str_width(std::string_view utf8) {
  int w = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    w += char_width(decode_utf8(utf8, i));
  }
  return w;
}

std::string truncate_to_cols(std::string_view utf8, int max_cols,
                             std::string_view ellipsis) {
  if (max_cols <= 0) return std::string();
  if (str_width(utf8) <= max_cols) return std::string(utf8);

  const int ell_w = str_width(ellipsis);
  const int budget = max_cols - ell_w;  // room for content before the ellipsis.
  if (budget <= 0) {
    // Not even room for the ellipsis: emit as many ellipsis columns as fit is
    // overkill; just return the ellipsis clipped to max_cols worth of bytes.
    return std::string(ellipsis).substr(0, ellipsis.size());
  }
  std::string out;
  int w = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    const std::size_t start = i;
    const char32_t cp = decode_utf8(utf8, i);
    const int cw = char_width(cp);
    if (w + cw > budget) break;
    out.append(utf8.substr(start, i - start));
    w += cw;
  }
  out.append(ellipsis);
  return out;
}

// --- CellBuffer ------------------------------------------------------------

void CellBuffer::resize(int w, int h) {
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  if (w == w_ && h == h_) return;
  w_ = w;
  h_ = h;
  cells_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_),
                Cell{});
  force_full_ = true;  // a resize invalidates any prior on-screen state.
}

void CellBuffer::clear(Rgb bg) {
  Cell blank;
  blank.glyph = U' ';
  blank.fg = theme::fg;
  blank.bg = bg;
  blank.style = Style::None;
  blank.continuation = false;
  for (auto& c : cells_) c = blank;
}

void CellBuffer::set(int x, int y, const Cell& c) {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
  cells_[static_cast<std::size_t>(y) * w_ + x] = c;
}

const Cell& CellBuffer::at(int x, int y) const {
  static const Cell kBlank{};
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return kBlank;
  return cells_[static_cast<std::size_t>(y) * w_ + x];
}

void CellBuffer::fill(const Rect& r, Rgb bg) {
  Cell blank;
  blank.bg = bg;
  for (int y = r.y; y < r.y + r.h; ++y)
    for (int x = r.x; x < r.x + r.w; ++x) set(x, y, blank);
}

int CellBuffer::put_str(int x, int y, std::string_view utf8, Rgb fg, Rgb bg,
                        Style style) {
  int col = x;
  for (std::size_t i = 0; i < utf8.size();) {
    const char32_t cp = decode_utf8(utf8, i);
    const int cw = char_width(cp);
    if (cw == 0) continue;  // drop zero-width in v0 (no combine-onto-prev yet).
    if (col >= w_) break;   // clip at the row's right edge.
    if (cw == 2 && col + 1 >= w_) {
      // A wide glyph that would straddle the edge: pad a blank rather than
      // spill a continuation past the buffer.
      Cell pad;
      pad.glyph = U' ';
      pad.fg = fg;
      pad.bg = bg;
      set(col, y, pad);
      col += 1;
      break;
    }
    Cell c;
    c.glyph = cp;
    c.fg = fg;
    c.bg = bg;
    c.style = style;
    set(col, y, c);
    if (cw == 2) {
      Cell cont;
      cont.glyph = cp;  // carry the glyph so an accidental read isn't blank.
      cont.fg = fg;
      cont.bg = bg;
      cont.style = style;
      cont.continuation = true;
      set(col + 1, y, cont);
    }
    col += cw;
  }
  return col;
}

namespace {

// Append "CSI y+1 ; x+1 H" (1-based cursor position).
void append_cup(std::string& out, int x, int y) {
  out += "\x1b[";
  out += std::to_string(y + 1);
  out += ';';
  out += std::to_string(x + 1);
  out += 'H';
}

// SGR for a cell: reset, then fg/bg/style. Simple and correct; the diff already
// minimizes how many cells emit at all, so per-emitted-cell SGR is acceptable
// for v0 (a run-length SGR optimizer is a deferred nicety).
void append_sgr(std::string& out, const Cell& c) {
  out += "\x1b[0";  // reset attributes; fg/bg follow.
  if (has(c.style, Style::Bold)) out += ";1";
  if (has(c.style, Style::Dim)) out += ";2";
  if (has(c.style, Style::Italic)) out += ";3";
  out += 'm';
  append_fg(out, c.fg);
  append_bg(out, c.bg);
}

}  // namespace

void CellBuffer::flush(CellBuffer& prev, std::string& out,
                       std::string_view pre_diff, std::string_view post_diff,
                       std::optional<std::pair<int, int>> cursor) const {
  const bool resized = prev.w_ != w_ || prev.h_ != h_;
  const bool full = force_full_ || resized;
  if (full) prev.resize(w_, h_);  // resize clears prev -> every cell differs.

  out += "\x1b[?2026h";  // begin synchronized output.
  // Geometry changed: the terminal's own resize handling (crop / reflow /
  // scroll) can leave residue anywhere — including cells a mid-drag winsize
  // mismatch keeps outside the grid we are about to repaint. One ED clear so
  // the full repaint below starts from blank; inside synchronized output it
  // can never flash. mark_all_dirty-only full repaints skip it: there the
  // screen is still cell-accurate and repainting every cell suffices.
  if (resized) out += "\x1b[2J";
  out.append(pre_diff.data(), pre_diff.size());

  int cur_x = -1, cur_y = -1;  // -1 = cursor position unknown (force a CUP).
  for (int y = 0; y < h_; ++y) {
    for (int x = 0; x < w_; ++x) {
      const Cell& c = at(x, y);
      if (c.continuation) continue;  // emitted with its wide lead.
      // image-owned (A4): skip if ANY exclusion rect claims this cell. The
      // rect count stays small (window + peek, ≤ ~20) so a linear scan is fine.
      bool excluded = false;
      for (const Rect& r : exclusions_) {
        if (r.contains(x, y)) { excluded = true; break; }
      }
      if (excluded) continue;
      if (!full && prev.at(x, y) == c) continue;  // unchanged.

      if (y != cur_y || x != cur_x) {
        append_cup(out, x, y);
        cur_x = x;
        cur_y = y;
      }
      append_sgr(out, c);
      if (c.glyph == 0) {
        out.push_back(' ');
      } else {
        encode_utf8(c.glyph, out);
      }
      cur_x += (char_width(c.glyph) == 2) ? 2 : 1;
    }
  }

  out += "\x1b[0m";  // leave SGR clean.
  out.append(post_diff.data(), post_diff.size());
  if (cursor) append_cup(out, cursor->first, cursor->second);
  out += "\x1b[?2026l";  // end synchronized output.

  // The screen now matches *this* buffer; make prev an exact copy so the next
  // flush diffs against reality.
  prev.w_ = w_;
  prev.h_ = h_;
  prev.cells_ = cells_;
  prev.exclusions_ = exclusions_;
  prev.force_full_ = false;
  force_full_ = false;
}

}  // namespace shigoku::tui
