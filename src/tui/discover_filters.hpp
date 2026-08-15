// discover_filters.hpp — the Discover filter overlay (P38, §9 — no Rust
// precedent). `f` on Discover opens a captured modal (modeled on connect.hpp/
// connect.cpp's centered-elevated-float shape, P19): genre (multi-select,
// from AniList GenreCollection), year, status, minimum score. Session-scoped
// (never persisted to config or store) — the working draft lives here, the
// committed DiscoverFilters lives on DiscoverState (discover.hpp).
//
// Pure state + pure render, no I/O: the genre list is fetched app.cpp-side
// (worker thread) and handed in as `view.genres`; this header only tracks
// which rows/genres are selected and draws them. Joins shigoku_tui_render.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../domain.hpp"
#include "cells.hpp"

namespace shigoku::tui {

// The four fixed overlay rows, in on-screen order.
enum class FilterRow {
  Genre,
  Year,
  Status,
  MinScore,
};
inline constexpr std::array<FilterRow, 4> kFilterRows = {
    FilterRow::Genre, FilterRow::Year, FilterRow::Status, FilterRow::MinScore};

// Fixed AniList MediaStatus cycle (status filter row), plus a leading "any"
// slot (nullopt). Order matches AniList's own enum declaration.
inline constexpr std::array<const char*, 5> kStatusCycle = {
    "RELEASING", "FINISHED", "NOT_YET_RELEASED", "HIATUS", "CANCELLED"};

// Minimum-score cycle (min_score filter row): 0 (unused, "any") then 10..90
// by 10, matching Enrichment::score's 0..=100 AniList averageScore scale.
inline constexpr std::array<std::uint32_t, 9> kMinScoreCycle = {
    10, 20, 30, 40, 50, 60, 70, 80, 90};

// The overlay's live state: a working DRAFT (committed to DiscoverState only
// on Enter/apply) plus the fetched genre vocabulary and cursor bookkeeping.
// `visible` gates every key/mouse capture (app.cpp, mirrors App::connect).
struct DiscoverFilterOverlay {
  bool visible = false;
  DiscoverFilters draft;  // edited copy; Esc discards it, Enter commits it.

  // Row cursor (0..3, indexes kFilterRows) — j/k move between Genre/Year/
  // Status/MinScore. h/l cycle the value under the cursor for Year/Status/
  // MinScore; Genre's h/l instead walks `genre_cursor` over `genres` and
  // space toggles membership in draft.genres (a multi-select, unlike the
  // other three single-value rows).
  std::size_t row_cursor = 0;
  std::size_t genre_cursor = 0;

  // Session-cached genre vocabulary (P38): fetched once, lazily, on first
  // open. Kept here (not just app-global) so the overlay's own render/nav
  // logic never has to reach past its own state — app.cpp copies the
  // fetched list in on GenreCollectionDone.
  std::vector<std::string> genres;
  bool genres_fetched = false;
  bool genres_loading = false;
  bool genres_failed = false;
};

// Render inputs, owned app-side (mirrors ConnectView's split from its live
// session state).
struct DiscoverFilterView {
  const DiscoverFilters* draft = nullptr;
  std::size_t row_cursor = 0;
  std::size_t genre_cursor = 0;
  const std::vector<std::string>* genres = nullptr;
  bool genres_loading = false;
  bool genres_failed = false;
};

void draw_discover_filters(const DiscoverFilterView& view, const Rect& area, CellBuffer& buf);

// --- Pure editing helpers (app.cpp's on_key calls these; kept free so the
// key-dispatch switch stays a thin table, and so they're unit-testable
// without a live overlay/App) ------------------------------------------------

// Year cycle: h/l step by 1 starting from "any" (nullopt) <-> the current
// broadcast year, clamped to a sane catalog floor (1960). `current_year`
// anchors the top of the cycle (no wall clock read here — the caller passes
// it, matching Cour's own now-injection discipline, 04 §8).
void cycle_year(std::optional<std::uint32_t>& year, std::int64_t delta,
                std::uint32_t current_year);

// Status cycle over kStatusCycle, with a leading "any" (nullopt) slot.
void cycle_status(std::optional<std::string>& status, std::int64_t delta);

// Min-score cycle over kMinScoreCycle, with a leading "any" (nullopt) slot.
void cycle_min_score(std::optional<std::uint32_t>& score, std::int64_t delta);

// Toggle `genre` in/out of `genres` (space on the Genre row): present ->
// removed, absent -> appended. Order is append-order, not AniList's — display
// only, no ranking meaning.
void toggle_genre(std::vector<std::string>& genres, const std::string& genre);

}  // namespace shigoku::tui
