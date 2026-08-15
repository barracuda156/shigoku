// discover.cpp — Discover card-grid STATE (P17a). Ported 1:1 from sabigoku
// src/tui/view/discover.rs (lines 1-364). See discover.hpp. Rendering is in
// views.cpp; this TU is the only one that touches the store (upsert on feed),
// so it links the sqlite graph like app.cpp, never the render core.

#include "discover.hpp"

#include <algorithm>

namespace shigoku::tui {

const char* axis_label(DiscoverAxis axis) {
  switch (axis) {
    case DiscoverAxis::Trending:   return "Trending";
    case DiscoverAxis::Popular:    return "Popular";
    case DiscoverAxis::TopRated:   return "Top Rated";
    case DiscoverAxis::ThisSeason: return "This Season";
  }
  return "Trending";  // unreachable (closed enum).
}

std::size_t axis_index(DiscoverAxis axis) {
  for (std::size_t i = 0; i < kDiscoverAxes.size(); ++i) {
    if (kDiscoverAxes[i] == axis) return i;
  }
  return 0;
}

const std::array<std::pair<const char*, const char*>, 18>& genre_glyphs_table() {
  // discover.rs GENRE_GLYPHS — must match DESIGN §3.8a exactly (drift is rot).
  static const std::array<std::pair<const char*, const char*>, 18> kTable = {{
      {"Action", "\xE2\x9A\x94"},          // ⚔
      {"Adventure", "\xE2\x9A\x91"},       // ⚑
      {"Comedy", "\xE2\x98\xBA"},          // ☺
      {"Drama", "\xE2\x97\x86"},           // ◆
      {"Ecchi", "\xE2\x99\xA8"},           // ♨
      {"Fantasy", "\xE2\x9A\x9C"},         // ⚜
      {"Horror", "\xE2\x98\xA0"},          // ☠
      {"Mahou Shoujo", "\xE2\x9C\xBF"},    // ✿
      {"Mecha", "\xE2\x9A\x99"},           // ⚙
      {"Music", "\xE2\x99\xAA"},           // ♪
      {"Mystery", "\xE2\x97\x88"},         // ◈
      {"Psychological", "\xE2\x97\x90"},   // ◐
      {"Romance", "\xE2\x99\xA5"},         // ♥
      {"Sci-Fi", "\xE2\xAC\xA1"},          // ⬡
      {"Slice of Life", "\xE2\x9D\x96"},   // ❖
      {"Sports", "\xE2\x97\x8E"},          // ◎
      {"Supernatural", "\xE2\x98\xBD"},    // ☽
      {"Thriller", "\xE2\x86\xAF"},        // ↯
  }};
  return kTable;
}

std::vector<const char*> genre_glyphs(const std::vector<std::string>& genres) {
  std::vector<const char*> out;
  const auto& table = genre_glyphs_table();
  for (const std::string& g : genres) {
    for (const auto& [name, glyph] : table) {
      if (g == name) {
        out.push_back(glyph);
        break;
      }
    }
    if (out.size() >= 2) break;  // first two mappable, in AniList order.
  }
  return out;
}

std::optional<const char*> format_label(std::optional<std::string_view> kind) {
  // render.rs format_label: the AniList `format` → a short display string.
  if (!kind.has_value()) return std::nullopt;
  const std::string_view k = *kind;
  if (k == "TV" || k == "TV_SHORT") return "TV";
  if (k == "MOVIE") return "Movie";
  if (k == "OVA") return "OVA";
  if (k == "ONA") return "ONA";
  if (k == "SPECIAL") return "Spec";
  if (k == "MUSIC") return "Music";
  return std::nullopt;
}

GridGeo grid_geo(int w, int content_h, CellPx cell) {
  GridGeo g;
  g.tier = cover_tier(w);
  g.cover_h = card_cover_rows(g.tier, cell);
  g.slot_h = g.cover_h + 4;
  const int inner = w > 2 ? w - 2 : 0;
  g.cols = std::max<std::size_t>(1, static_cast<std::size_t>(inner) / g.tier.slot_w);
  const int grid_h = content_h > 2 ? content_h - 2 : 0;
  g.rows_visible = std::max<std::size_t>(1, static_cast<std::size_t>(grid_h / g.slot_h));
  return g;
}

Rect card_cover_rect(const GridGeo& geo, std::size_t i, std::size_t scroll_row,
                     int grid_x, int grid_y, int canvas_w, int grid_bottom) {
  const std::size_t card_row = i / geo.cols;
  const std::size_t col = i % geo.cols;
  if (card_row < scroll_row || card_row >= scroll_row + geo.rows_visible) {
    return Rect{};
  }
  const int x = grid_x + static_cast<int>(col) * geo.tier.slot_w;
  const int y = grid_y + static_cast<int>(card_row - scroll_row) * geo.slot_h;
  // A card that would clip the canvas right edge / grid bottom is skipped
  // whole (matches draw_discover's card loop exactly — the single geometry
  // source, R3).
  if (x + geo.tier.cover_w > canvas_w || y + geo.slot_h > grid_bottom) {
    return Rect{};
  }
  return Rect{x, y, geo.tier.cover_w, geo.cover_h};
}

bool is_new_this_cour(const Enrichment& e, DiscoverAxis axis, Cour cour) {
  return axis != DiscoverAxis::ThisSeason && e.season.has_value() &&
         *e.season == cour.season && e.year.has_value() && *e.year == cour.year;
}

AxisSlot& DiscoverState::active() { return slots_[axis_index(axis_)]; }

const Enrichment* DiscoverState::selected_entry() const {
  const AxisSlot& s = slot();
  if (s.cursor >= s.entries.size()) return nullptr;
  return &s.entries[s.cursor].meta;
}

Enrichment* DiscoverState::selected_entry_mut() {
  AxisSlot& s = active();
  if (s.cursor >= s.entries.size()) return nullptr;
  return &s.entries[s.cursor].meta;
}

void DiscoverState::cycle_axis(std::int64_t delta) {
  const std::int64_t n = static_cast<std::int64_t>(kDiscoverAxes.size());
  const std::int64_t cur = static_cast<std::int64_t>(axis_index(axis_));
  // rem_euclid: a nonnegative residue for any signed delta.
  std::int64_t idx = (cur + delta) % n;
  if (idx < 0) idx += n;
  axis_ = kDiscoverAxes[static_cast<std::size_t>(idx)];
  active().failed.reset();
}

void DiscoverState::select_axis(std::size_t index) {
  if (index < kDiscoverAxes.size()) {
    axis_ = kDiscoverAxes[index];
    active().failed.reset();
  }
}

void DiscoverState::nav(std::int64_t dx, std::int64_t dy, const GridGeo& geo) {
  const std::size_t cols = geo.cols;
  AxisSlot& s = active();
  const std::size_t len = s.entries.size();
  if (len == 0) return;
  if (dx != 0) {
    const std::size_t row_start = (s.cursor / cols) * cols;
    const std::int64_t row_len =
        static_cast<std::int64_t>(std::min(len - row_start, cols));
    const std::int64_t col = static_cast<std::int64_t>(s.cursor - row_start);
    std::int64_t nc = (col + dx) % row_len;
    if (nc < 0) nc += row_len;
    s.cursor = row_start + static_cast<std::size_t>(nc);
  } else if (dy != 0) {
    const std::int64_t moved =
        static_cast<std::int64_t>(s.cursor) + dy * static_cast<std::int64_t>(cols);
    const std::int64_t hi = static_cast<std::int64_t>(len) - 1;
    s.cursor = static_cast<std::size_t>(std::clamp<std::int64_t>(moved, 0, hi));
  }
  clamp_scroll(geo);
}

void DiscoverState::select(std::size_t index, const GridGeo& geo) {
  AxisSlot& s = active();
  if (s.entries.empty()) return;
  s.cursor = std::min(index, s.entries.size() - 1);
  clamp_scroll(geo);
}

void DiscoverState::clamp_scroll(const GridGeo& geo) {
  AxisSlot& s = active();
  const std::size_t row = s.cursor / geo.cols;
  s.scroll_row = std::min(s.scroll_row, row);
  if (row >= s.scroll_row + geo.rows_visible) {
    s.scroll_row = row + 1 - geo.rows_visible;
  }
}

void DiscoverState::set_filters(DiscoverFilters filters) {
  // No-op commit (P38 review): re-applying an identical filter set must not
  // wipe every slot's pages/scroll and refetch page 1 four times over.
  if (filters == filters_) return;
  filters_ = std::move(filters);
  for (AxisSlot& s : slots_) {
    s = AxisSlot{};
  }
  // Invalidate every fetch still in flight under the old filters: their
  // answers would otherwise file as perfectly-positioned pages of the new
  // set (P38 review).
  ++filter_gen_;
}

std::optional<std::uint32_t> DiscoverState::wanted_fetch(const GridGeo& geo) const {
  const AxisSlot& s = slot();
  if (s.loading || s.failed.has_value() || s.exhausted) return std::nullopt;
  if (s.entries.empty()) return std::uint32_t{1};
  const std::size_t last_row = (s.entries.size() - 1) / geo.cols;
  const std::size_t cursor_row = s.cursor / geo.cols;
  const std::size_t gap = last_row > cursor_row ? last_row - cursor_row : 0;
  if (gap <= kPrefetchRows) return s.page + 1;
  return std::nullopt;
}

void DiscoverState::arm_loading(TickCount now) {
  AxisSlot& s = active();
  s.loading = true;
  s.loading_since = now;
}

void DiscoverState::disarm_loading() { active().loading = false; }

void DiscoverState::on_feed_error(DiscoverAxis axis, std::string cause, std::uint32_t gen) {
  if (gen != filter_gen_) return;  // stale generation: discard whole (P38 review).
  AxisSlot& s = slots_[axis_index(axis)];
  s.loading = false;
  s.failed = std::move(cause);
}

}  // namespace shigoku::tui
