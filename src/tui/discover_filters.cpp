// discover_filters.cpp — the Discover filter overlay (P38, §9). See
// discover_filters.hpp.

#include "discover_filters.hpp"

#include <algorithm>
#include <cctype>

namespace shigoku::tui {

namespace {

constexpr int kBoxW = 50;
constexpr int kBoxH = 15;
constexpr int kMinCols = 24;

void draw_centered(CellBuffer& buf, const Rect& area, int y, const std::string& line, Rgb fg,
                   Rgb bg, Style style = Style::None) {
  if (y < 0 || y >= area.h) return;
  const int lw = str_width(line);
  const int x = area.x + std::max(0, (area.w - lw) / 2);
  buf.put_str(x, area.y + y, line, fg, bg, style);
}

const char* row_label(FilterRow r) {
  switch (r) {
    case FilterRow::Genre:    return "genre";
    case FilterRow::Year:     return "year";
    case FilterRow::Status:   return "status";
    case FilterRow::MinScore: return "min score";
  }
  return "";  // unreachable (closed enum).
}

std::string status_display(std::string_view s) {
  // A short, cased display form of the raw AniList MediaStatus ("RELEASING"
  // -> "Releasing"); the draft/wire value stays the raw uppercase string.
  if (s.empty()) return "any";
  std::string out(s);
  std::transform(out.begin() + 1, out.end(), out.begin() + 1,
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  std::replace(out.begin(), out.end(), '_', ' ');
  return out;
}

}  // namespace

void draw_discover_filters(const DiscoverFilterView& view, const Rect& area, CellBuffer& buf) {
  if (area.w == 0 || area.h == 0 || view.draft == nullptr) return;

  const int bw = std::min(kBoxW, std::max(0, area.w - 4));
  const int bh = std::min(kBoxH, std::max(0, area.h - 2));
  if (bw < kMinCols || bh < 10) {
    const int row = area.h / 2;
    const Rect strip{area.x, area.y + row, area.w, 1};
    buf.fill(strip, theme::elevated);
    draw_centered(buf, area, row, "discover filters: esc to close", theme::fg2, theme::elevated);
    return;
  }

  const Rect modal{area.x + std::max(0, (area.w - bw) / 2), area.y + std::max(0, (area.h - bh) / 2),
                    bw, bh};
  buf.fill(modal, theme::elevated);

  draw_centered(buf, modal, 0, "Discover Filters", theme::hot, theme::elevated, Style::Bold);

  const auto row_y = [](std::size_t idx) { return 2 + static_cast<int>(idx) * 2; };

  for (std::size_t i = 0; i < kFilterRows.size(); ++i) {
    const FilterRow r = kFilterRows[i];
    const bool active = i == view.row_cursor;
    const Rgb label_col = active ? theme::focus : theme::fg2;
    const int y = row_y(i);
    if (y >= modal.h) break;

    std::string value;
    switch (r) {
      case FilterRow::Genre: {
        if (view.genres_loading) {
          value = "loading\xE2\x80\xA6";
        } else if (view.genres_failed) {
          value = "can't reach AniList";
        } else if (view.draft->genres.empty()) {
          value = "any";
        } else {
          value.clear();
          for (std::size_t g = 0; g < view.draft->genres.size(); ++g) {
            if (g > 0) value += ", ";
            value += view.draft->genres[g];
          }
        }
        break;
      }
      case FilterRow::Year:
        value = view.draft->year.has_value() ? std::to_string(*view.draft->year) : "any";
        break;
      case FilterRow::Status:
        value = view.draft->status.has_value() ? status_display(*view.draft->status) : "any";
        break;
      case FilterRow::MinScore:
        value = view.draft->min_score.has_value() ? (">" + std::to_string(*view.draft->min_score))
                                                    : "any";
        break;
    }

    int x = modal.x + 2;
    x = buf.put_str(x, modal.y + y, row_label(r), label_col, theme::elevated,
                    active ? Style::Bold : Style::None);
    x = buf.put_str(x, modal.y + y, "  ", theme::fg2, theme::elevated);
    const int max_val_cols = std::max(0, modal.w - (x - modal.x) - 2);
    buf.put_str(x, modal.y + y, truncate_to_cols(value, max_val_cols), theme::fg, theme::elevated);

    // Genre row grows a second line: the picker cursor over the fetched
    // vocabulary, only while that row is active (keeps the other rows'
    // positions stable when Genre isn't focused).
    if (r == FilterRow::Genre && active && view.genres != nullptr && !view.genres->empty()) {
      const int py = y + 1;
      if (py < modal.h) {
        const std::size_t gi = std::min(view.genre_cursor, view.genres->size() - 1);
        const bool selected =
            std::find(view.draft->genres.begin(), view.draft->genres.end(), (*view.genres)[gi]) !=
            view.draft->genres.end();
        const std::string marker = selected ? "[x] " : "[ ] ";
        buf.put_str(modal.x + 4, modal.y + py, marker + (*view.genres)[gi],
                    selected ? theme::fg : theme::fg2, theme::elevated);
      }
    }
  }

  const int hint_y = modal.h - 2;
  draw_centered(buf, modal, hint_y,
               "jk row \xC2\xB7 hl cycle \xC2\xB7 space toggle genre \xC2\xB7 c clear",
               theme::fg2, theme::elevated, Style::Italic);
  draw_centered(buf, modal, hint_y + 1, "enter apply \xC2\xB7 esc cancel", theme::fg2,
               theme::elevated, Style::Italic);
}

void cycle_year(std::optional<std::uint32_t>& year, std::int64_t delta,
               std::uint32_t current_year) {
  constexpr std::uint32_t kFloor = 1960;
  if (!year.has_value()) {
    year = delta > 0 ? kFloor : current_year;
    return;
  }
  const std::int64_t next = static_cast<std::int64_t>(*year) + delta;
  if (next < static_cast<std::int64_t>(kFloor) || next > static_cast<std::int64_t>(current_year)) {
    year = std::nullopt;
    return;
  }
  year = static_cast<std::uint32_t>(next);
}

void cycle_status(std::optional<std::string>& status, std::int64_t delta) {
  const std::int64_t n = static_cast<std::int64_t>(kStatusCycle.size());
  if (!status.has_value()) {
    if (delta > 0) {
      status = kStatusCycle.front();
    } else {
      status = kStatusCycle.back();
    }
    return;
  }
  std::int64_t idx = -1;
  for (std::int64_t i = 0; i < n; ++i) {
    if (*status == kStatusCycle[static_cast<std::size_t>(i)]) {
      idx = i;
      break;
    }
  }
  const std::int64_t next = (idx < 0 ? 0 : idx) + delta;
  if (next < 0 || next >= n) {
    status = std::nullopt;
    return;
  }
  status = kStatusCycle[static_cast<std::size_t>(next)];
}

void cycle_min_score(std::optional<std::uint32_t>& score, std::int64_t delta) {
  const std::int64_t n = static_cast<std::int64_t>(kMinScoreCycle.size());
  if (!score.has_value()) {
    if (delta > 0) {
      score = kMinScoreCycle.front();
    } else {
      score = kMinScoreCycle.back();
    }
    return;
  }
  std::int64_t idx = -1;
  for (std::int64_t i = 0; i < n; ++i) {
    if (*score == kMinScoreCycle[static_cast<std::size_t>(i)]) {
      idx = i;
      break;
    }
  }
  const std::int64_t next = (idx < 0 ? 0 : idx) + delta;
  if (next < 0 || next >= n) {
    score = std::nullopt;
    return;
  }
  score = kMinScoreCycle[static_cast<std::size_t>(next)];
}

void toggle_genre(std::vector<std::string>& genres, const std::string& genre) {
  auto it = std::find(genres.begin(), genres.end(), genre);
  if (it != genres.end()) {
    genres.erase(it);
  } else {
    genres.push_back(genre);
  }
}

}  // namespace shigoku::tui
