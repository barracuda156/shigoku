// cover_geom.hpp — cover geometry (P8, DESIGN §3.3/§3.8; ported from
// sabigoku src/tui/covers/sizing.rs). Pure width-tier selection + adaptive
// poster heights from reported cell-pixel geometry, with the fixed floors/caps
// the Rust pins when the terminal reports no pixel size. The kitty emitter
// (kitty.hpp) takes a CELL box (cols×rows); this module computes that box from
// the effective column width and the per-cell pixel geometry.
//
// Pixel geometry itself comes from term.cpp's WinSize (ioctl TIOCGWINSZ
// ws_xpixel/ypixel). The fallback chain (ioctl → CSI 14t → placeholder) is A4:
// no reported geometry ⇒ CellPx{0,0} ⇒ the fixed floors/caps below (never a
// broken sub-pixel box).
//
// ENDIANNESS (§3): plain integer arithmetic on dimensions; no byte packing.

#pragma once

#include <cstdint>

namespace shigoku::tui {

// Per-cell pixel geometry (ratatui-image's FontSize). {0,0} = the terminal
// reported no geometry (tmux/SSH/xterm-without-pixels) — sizing then uses the
// DESIGN fixed floors/caps, never a derived value.
struct CellPx {
  std::uint16_t width = 0;   // pixels per cell column.
  std::uint16_t height = 0;  // pixels per cell row.
  [[nodiscard]] bool known() const { return width != 0 && height != 0; }
  friend bool operator==(const CellPx&, const CellPx&) = default;
};

// DESIGN §3.8 width tiers: ≥80 effective cols → a 20-col cover in a 22-col
// slot; below → 14-in-16. Selected from the EFFECTIVE column width (detail_w in
// a pane, canvas width minus margins in the zoom), never raw terminal width
// (DESIGN §3.3).
struct CoverTier {
  bool large = false;
  std::uint16_t cover_w = 0;  // cover width in cells (the c= box width).
  std::uint16_t slot_w = 0;   // reserved slot width (cover + margin).
  friend bool operator==(const CoverTier&, const CoverTier&) = default;
};

[[nodiscard]] CoverTier cover_tier(int effective_w);

// Derive per-cell pixel geometry from a window's total pixel size and its cell
// grid (TIOCGWINSZ ws_xpixel/ypixel ÷ cols/rows, or the CSI 14t total-pixel
// reply ÷ cols/rows). Returns {0,0} — "unknown" — when any input is 0, which
// routes sizing to the fixed floors/caps (A4's placeholder fallback).
[[nodiscard]] CellPx cell_px_from_window(std::uint16_t win_xpixel,
                                         std::uint16_t win_ypixel,
                                         std::uint16_t cols, std::uint16_t rows);

// Detail cover height in rows (DESIGN §3.3): adaptive from a 2:3 poster at the
// reported cell geometry, clamped to the 28/20 aesthetic caps, which double as
// the fixed fallback when geometry is unreported. This is the c=/r= row count
// the emitter transmits (the grid-yield cap_height/synopsis_cap live in the
// view, per DESIGN §3.3's "single source of truth" note — not here).
[[nodiscard]] int detail_cover_rows(const CoverTier& t, CellPx cell);

// Inverse of poster_rows (§9 zoom scaling): the columns whose 2:3 poster
// spans `rows` rows at this cell geometry. Floor semantics like poster_rows;
// degenerate cell.width floored to 1.
[[nodiscard]] int poster_cols(int rows, CellPx cell);

// Rows a 2:3 poster spanning `cover_w` columns occupies at `cell` pixels
// (sizing.rs poster_h). Exposed for the tests; callers use detail_cover_rows.
[[nodiscard]] int poster_rows(int cover_w, CellPx cell);

// Card cover height in rows (DESIGN §3.8, sizing.rs card_cover_h): a ~2:3
// poster fills the card width. Unknown cell geometry falls back to the fixed
// floors 7 (large tier) / 5 (small); the adaptive value NEVER shrinks below
// them and — unlike detail_cover_rows — has NO upper cap (cards deliberately
// grow with the cell, the detail clamp at 28/20 does not apply here).
[[nodiscard]] int card_cover_rows(const CoverTier& t, CellPx cell);

}  // namespace shigoku::tui
