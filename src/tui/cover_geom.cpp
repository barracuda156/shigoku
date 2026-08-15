// cover_geom.cpp — cover geometry (P8, ported 1:1 from sabigoku
// src/tui/covers/sizing.rs). Pure integer math; no I/O.

#include "cover_geom.hpp"

#include <algorithm>

namespace shigoku::tui {

CoverTier cover_tier(int effective_w) {
  if (effective_w >= 80) {
    return CoverTier{/*large=*/true, /*cover_w=*/20, /*slot_w=*/22};
  }
  return CoverTier{/*large=*/false, /*cover_w=*/14, /*slot_w=*/16};
}

CellPx cell_px_from_window(std::uint16_t win_xpixel, std::uint16_t win_ypixel,
                           std::uint16_t cols, std::uint16_t rows) {
  if (win_xpixel == 0 || win_ypixel == 0 || cols == 0 || rows == 0) {
    return CellPx{};  // unknown -> fixed floors/caps (A4 placeholder fallback).
  }
  return CellPx{static_cast<std::uint16_t>(win_xpixel / cols),
                static_cast<std::uint16_t>(win_ypixel / rows)};
}

int poster_rows(int cover_w, CellPx cell) {
  // sizing.rs poster_h: rows for a 2:3 poster spanning cover_w columns.
  //   w_px = cover_w * cell.width; rows = (w_px * 3 / 2) / max(cell.height, 1)
  // A degenerate cell.height (0) is floored to 1 (sizing.rs .max(1)).
  const std::uint32_t w_px =
      static_cast<std::uint32_t>(cover_w) * static_cast<std::uint32_t>(cell.width);
  const std::uint32_t denom = cell.height != 0 ? cell.height : 1u;
  return static_cast<int>((w_px * 3u / 2u) / denom);
}

int detail_cover_rows(const CoverTier& t, CellPx cell) {
  // sizing.rs detail_cover_h: adaptive, clamped to 28 (large) / 20 (small),
  // which double as the fixed fallback when geometry is unreported.
  const int cap = t.large ? 28 : 20;
  if (!cell.known()) return cap;
  const int derived = poster_rows(t.cover_w, cell);
  return std::clamp(derived, 1, cap);
}

int poster_cols(int rows, CellPx cell) {
  // poster_rows solved for cover_w:
  //   h_px = rows * cell.height; cols = (h_px * 2 / 3) / max(cell.width, 1)
  const std::uint32_t h_px =
      static_cast<std::uint32_t>(rows) * static_cast<std::uint32_t>(cell.height);
  const std::uint32_t denom = cell.width != 0 ? cell.width : 1u;
  return static_cast<int>((h_px * 2u / 3u) / denom);
}

int card_cover_rows(const CoverTier& t, CellPx cell) {
  // sizing.rs card_cover_h: floor 7 (large) / 5 (small), adaptive above it,
  // never below the floor, no upper cap (cards grow with the cell).
  const int floor = t.large ? 7 : 5;
  if (!cell.known()) return floor;
  return std::max(poster_rows(t.cover_w, cell), floor);
}

}  // namespace shigoku::tui
