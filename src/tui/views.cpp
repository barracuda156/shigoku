// views.cpp — Browse two-pane + episode grid render (P7). See views.hpp.

#include "views.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

#include "../domain.hpp"

namespace shigoku::tui {

PaneSplit pane_split(int w) {
  PaneSplit s;
  s.list_w = std::max(30, w * 38 / 100);
  s.detail_x = 2 + s.list_w + 2;
  s.detail_w = w - s.detail_x - 1;
  return s;
}

// --- Season chips (DESIGN §2.3/§3.4, render.rs, P30) -----------------------

std::string_view season_name(Season s, bool kanji) {
  switch (s) {
    case Season::Winter: return kanji ? "\xE5\x86\xAC" : "Winter";  // 冬
    case Season::Spring: return kanji ? "\xE6\x98\xA5" : "Spring";  // 春
    case Season::Summer: return kanji ? "\xE5\xA4\x8F" : "Summer";  // 夏
    case Season::Fall:   return kanji ? "\xE7\xA7\x8B" : "Autumn";  // 秋
  }
  return "?";  // unreachable (closed enum).
}

std::string cour_chip(const Cour& cour, bool kanji) {
  return std::string(season_name(cour.season, kanji)) + " " + std::to_string(cour.year);
}

std::optional<std::string> season_chip(std::optional<Season> season,
                                       std::optional<std::uint32_t> year,
                                       bool kanji) {
  if (!season.has_value() || !year.has_value()) return std::nullopt;
  return std::string(season_name(*season, kanji)) + " " + std::to_string(*year);
}

// --- HistoryState (05 §2, ROD-439 chunk 5a) --------------------------------

namespace {
// Cursor walk order (05 §2): group order, never store order.
constexpr ListStatus kGroupOrder[5] = {ListStatus::Watching, ListStatus::Planning,
                                       ListStatus::Paused, ListStatus::Completed,
                                       ListStatus::Dropped};
}  // namespace

std::optional<std::int64_t> HistoryState::anchor_aid() const {
  if (cursor >= order.size()) return std::nullopt;
  return rows[order[cursor]].enrichment.anilist_id;
}

bool HistoryState::matches_filter(const Show& s) const {
  if (filter.empty()) return true;
  std::string needle = filter;
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const Enrichment& e = s.enrichment;
  auto has = [&](std::string_view t) {
    if (t.empty()) return false;
    std::string lower(t);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(needle) != std::string::npos;
  };
  if (has(e.title_romaji)) return true;
  if (e.title_english.has_value() && has(*e.title_english)) return true;
  if (e.title_native.has_value() && has(*e.title_native)) return true;
  return false;
}

void HistoryState::rebuild(std::optional<std::int64_t> anchor) {
  order.clear();
  for (ListStatus status : kGroupOrder) {
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
      if (rows[ix].list_status == status && matches_filter(rows[ix])) {
        order.push_back(ix);
      }
    }
  }
  if (anchor.has_value()) {
    auto it = std::find_if(order.begin(), order.end(), [&](std::size_t ix) {
      return rows[ix].enrichment.anilist_id == *anchor;
    });
    cursor = (it != order.end()) ? static_cast<std::size_t>(it - order.begin())
                                 : (order.empty() ? 0 : order.size() - 1);
  } else {
    cursor = order.empty() ? 0 : std::min(cursor, order.size() - 1);
  }
}

bool HistoryState::select_aid(std::int64_t anilist_id, int visible) {
  auto it = std::find_if(order.begin(), order.end(), [&](std::size_t ix) {
    return rows[ix].enrichment.anilist_id == anilist_id;
  });
  if (it == order.end()) return false;
  cursor = static_cast<std::size_t>(it - order.begin());
  scroll_into_view(visible);
  return true;
}

void HistoryState::on_filter_edited() {
  rebuild(anchor_aid());
  scroll = 0;
}

void HistoryState::on_filter_cleared() {
  filter.clear();
  rebuild(std::nullopt);
  cursor = 0;
  scroll = 0;
}

void HistoryState::nav(int dy, int visible) {
  if (order.empty()) return;
  const long max = static_cast<long>(order.size()) - 1;
  long next = static_cast<long>(cursor) + dy;
  if (next < 0) next = 0;
  if (next > max) next = max;
  cursor = static_cast<std::size_t>(next);
  scroll_into_view(visible);
}

void HistoryState::jump(bool top, int visible) {
  if (order.empty()) return;
  cursor = top ? 0 : order.size() - 1;
  scroll_into_view(visible);
}

std::vector<HistoryLine> HistoryState::layout() const {
  std::vector<HistoryLine> lines;
  std::size_t ord = 0;
  bool any = false;
  for (ListStatus status : kGroupOrder) {
    std::size_t count = 0;
    for (std::size_t ix : order) {
      if (rows[ix].list_status == status) ++count;
    }
    if (count == 0) continue;
    if (any) lines.push_back(HistoryLine{HistoryLine::Kind::Blank, {}, 0, 0});
    any = true;
    lines.push_back(HistoryLine{HistoryLine::Kind::Header, status, count, 0});
    lines.push_back(HistoryLine{HistoryLine::Kind::Rule, {}, 0, 0});
    for (std::size_t i = 0; i < count; ++i) {
      lines.push_back(HistoryLine{HistoryLine::Kind::Title, {}, 0, ord});
      lines.push_back(HistoryLine{HistoryLine::Kind::Bar, {}, 0, ord});
      ++ord;
    }
  }
  return lines;
}

void HistoryState::scroll_into_view(int visible) {
  if (visible <= 0) return;
  const std::vector<HistoryLine> lines = layout();
  auto it = std::find_if(lines.begin(), lines.end(), [&](const HistoryLine& li) {
    return li.kind == HistoryLine::Kind::Title && li.ord == cursor;
  });
  if (it == lines.end()) return;
  const std::size_t title_ix = static_cast<std::size_t>(it - lines.begin());
  // Keep the entry's own group header visible when hugging the top.
  const std::size_t top_pad = title_ix >= 2 ? title_ix - 2 : 0;
  if (top_pad < scroll) scroll = top_pad;
  const std::size_t bottom = title_ix + 2;
  const std::size_t visible_u = static_cast<std::size_t>(visible);
  if (bottom > scroll + visible_u) scroll = bottom - visible_u;
}

// --- ScheduleState (P37) ----------------------------------------------------

const Show* ScheduleState::selected() const {
  std::size_t ord = 0;
  for (const ScheduleGroup& g : groups) {
    for (const ScheduleEntry& e : g.entries) {
      if (ord == cursor) {
        return e.show_index < rows.size() ? &rows[e.show_index] : nullptr;
      }
      ++ord;
    }
  }
  return nullptr;
}

void ScheduleState::rebuild(std::int64_t now_secs) {
  groups = shigoku::schedule(rows, now_secs);
  const std::size_t total = count();
  if (total == 0) {
    cursor = 0;
  } else if (cursor >= total) {
    cursor = total - 1;
  }
  // Clamp the scroll too (P37 review): an on-tick shrink (a countdown hit 0
  // and its row left the lens) with `scroll` past the new layout otherwise
  // draws a blank screen until the next j/k re-anchors it.
  const std::size_t lines = layout().size();
  if (scroll >= lines) scroll = lines == 0 ? 0 : lines - 1;
}

void ScheduleState::nav(int dy, int visible) {
  const std::size_t total = count();
  if (total == 0) return;
  const long max = static_cast<long>(total) - 1;
  long next = static_cast<long>(cursor) + dy;
  if (next < 0) next = 0;
  if (next > max) next = max;
  cursor = static_cast<std::size_t>(next);
  scroll_into_view(visible);
}

void ScheduleState::jump(bool top, int visible) {
  const std::size_t total = count();
  if (total == 0) return;
  cursor = top ? 0 : total - 1;
  scroll_into_view(visible);
}

std::vector<ScheduleState::Line> ScheduleState::layout() const {
  std::vector<Line> lines;
  std::size_t ord = 0;
  bool any = false;
  for (std::size_t g = 0; g < groups.size(); ++g) {
    const ScheduleGroup& grp = groups[g];
    if (grp.entries.empty()) continue;
    if (any) lines.push_back(Line{Line::Kind::Blank, {}, 0, 0, 0, 0});
    any = true;
    lines.push_back(Line{Line::Kind::Header, grp.weekday, grp.entries.size(), 0, 0, 0});
    lines.push_back(Line{Line::Kind::Rule, {}, 0, 0, 0, 0});
    for (std::size_t e = 0; e < grp.entries.size(); ++e) {
      lines.push_back(Line{Line::Kind::Entry, {}, 0, g, e, ord});
      ++ord;
    }
  }
  return lines;
}

void ScheduleState::scroll_into_view(int visible) {
  if (visible <= 0) return;
  const std::vector<Line> lines = layout();
  auto it = std::find_if(lines.begin(), lines.end(), [&](const Line& li) {
    return li.kind == Line::Kind::Entry && li.ord == cursor;
  });
  if (it == lines.end()) return;
  const std::size_t entry_ix = static_cast<std::size_t>(it - lines.begin());
  const std::size_t top_pad = entry_ix >= 2 ? entry_ix - 2 : 0;
  if (top_pad < scroll) scroll = top_pad;
  const std::size_t bottom = entry_ix + 1;
  const std::size_t visible_u = static_cast<std::size_t>(visible);
  if (bottom > scroll + visible_u) scroll = bottom - visible_u;
}

// --- P8 cover geometry (DESIGN §3.3, A4) — pure, in the render target -------
namespace detail {

// The `c` section's row budget (P36): bottom third of the zoom's content
// region, capped at 12 rows, with a blank separator row above it; 0 when the
// frame is too narrow to split legibly (< 4 rows for the section itself).
// Single source for both the render split (draw_detail_zoom) and the
// scroll-into-view math (app.cpp's j/k handlers) — they can never disagree
// on how many lines are actually on screen.
int char_recs_section_height(int content_rows) {
  const int section_h = std::min(content_rows / 3, 12);
  return section_h >= 4 ? section_h : 0;
}

// The flattened line offset of recommendations[rec_idx] in the `c` section
// (P36 slice 3 scroll-into-view): characters header + rows, then a blank
// separator (only if both blocks are non-empty), then the recommendations
// header, then rec_idx rows in. Must walk the SAME shape
// draw_char_recs_section flattens, so app.cpp's scroll math can never
// disagree with what's actually on screen — same discipline as
// episode_cell_at replaying draw_detail_header's geometry.
std::size_t char_recs_rec_line_index(const CharactersAndRecommendations& data,
                                     std::size_t rec_idx) {
  std::size_t line = 0;
  if (!data.characters.empty()) line += 1 + data.characters.size();
  if (!data.characters.empty() && !data.recommendations.empty()) line += 1;  // blank separator.
  return line + 1 + rec_idx;  // +1 for the Recommendations header.
}

// The Enrichment the shared detail pane/zoom renders for the current
// selection (05 §15 pattern extended to History, P16): the Detail zoom reads
// through its origin view (Browse's catalog cursor or History's selection);
// the two-pane views read their own list directly. nullptr when nothing is
// selected in the relevant list.
const Enrichment* selected_enrichment(const App& app) {
  const View source = (app.view == View::Detail) ? app.detail_origin : app.view;
  if (source == View::History) {
    const Show* s = app.history.selected();
    return s == nullptr ? nullptr : &s->enrichment;
  }
  // Schedule (P37): the flattened cursor's Show, same reuse as History.
  if (source == View::Schedule) {
    const Show* s = app.schedule.selected();
    return s == nullptr ? nullptr : &s->enrichment;
  }
  // Discover (and the Detail zoom's Discover origin, P17): the active axis
  // slot's cursor entry, so Enter→Detail zoom reuses the whole single-cover
  // detail pane untouched (05 §15 "one detail renderer").
  if (source == View::Discover) {
    return app.discover.selected_entry();
  }
  // Browse (and the Detail zoom's Browse origin): the catalog cursor.
  if (app.list_cursor < 0 ||
      static_cast<std::size_t>(app.list_cursor) >= app.catalog.size()) {
    return nullptr;
  }
  return &app.catalog[static_cast<std::size_t>(app.list_cursor)].meta;
}

// The library Show for the current selection (P34), or nullptr. Unlike
// selected_enrichment, this exists ONLY for the History/Schedule sources —
// Browse/Discover selections are catalog rows that may not even be library
// members (no user_score to show), and there is no library-membership Show
// to hand back for them.
const Show* selected_show(const App& app) {
  const View source = (app.view == View::Detail) ? app.detail_origin : app.view;
  if (source == View::History) return app.history.selected();
  if (source == View::Schedule) return app.schedule.selected();
  return nullptr;
}

std::optional<CoverTarget> cover_target(const App& app) {
  const Enrichment* m = selected_enrichment(app);
  if (m == nullptr) return std::nullopt;
  if (!m->cover_url.has_value() || m->cover_url->empty()) return std::nullopt;
  return CoverTarget{m->anilist_id, *m->cover_url};
}

Rect cover_rect(const App& app) {
  // A cover shows only where a detail column exists: Browse/History two-pane
  // (≥ kPaneSplitMin) or the Detail zoom. Below the split, or with no
  // selection / no cover url, there is no cover rectangle.
  if (!cover_target(app).has_value()) return Rect{};

  int col_x = 0, col_w = 0;
  if (app.view == View::Detail) {
    col_x = 2;
    col_w = app.win.cols - 4;
  } else if ((app.view == View::Browse || app.view == View::History) &&
             app.win.cols >= kPaneSplitMin) {
    const PaneSplit split = pane_split(app.win.cols);
    col_x = split.detail_x;
    col_w = split.detail_w;
  } else {
    return Rect{};  // list-only Browse/History (or Discover/Settings): no cover.
  }
  if (col_w <= 0) return Rect{};

  // Width tier from the EFFECTIVE column width (DESIGN §3.3), clamped so the
  // cover never overflows the column. Height from the poster aspect + caps.
  const CoverTier tier = cover_tier(col_w);
  int cover_w = tier.cover_w;
  if (cover_w > col_w) cover_w = col_w;
  int rows = detail_cover_rows(tier, app.cover_caps.cell);

  // The full-canvas zoom is the show's dedicated page (§9 polish): with live
  // cell geometry the poster scales with the canvas instead of the column
  // tier — floor at the Discover card's uncapped height (the zoom must never
  // show a smaller cover than the gallery), growth target half the content
  // height, reserve kept for the header + a few episode-grid rows (the grid
  // clips at the bottom, it does not scroll). Width follows from the 2:3
  // aspect so the poster never distorts; placeholder mode (unknown cell)
  // keeps the DESIGN tier box.
  if (app.view == View::Detail && app.cover_caps.cell.known()) {
    constexpr int kZoomCoverReserve = 12;
    const int ch = content_height(app.win.rows);
    const int target = std::max(card_cover_rows(tier, app.cover_caps.cell),
                                std::min(ch / 2, ch - kZoomCoverReserve));
    const int grown_w = poster_cols(target, app.cover_caps.cell);
    cover_w = std::clamp(grown_w, std::min(static_cast<int>(tier.cover_w), col_w),
                         col_w);
    rows = poster_rows(cover_w, app.cover_caps.cell);
  }

  // Content region is rows [kContentY0, win.rows-1). Reserve at least 2 rows
  // for the header/grid below the cover; if the poster can't fit that, drop it
  // (a sliver is worse than none — DESIGN §3.3 MIN_COVER_ROWS spirit).
  const int content_h = content_height(app.win.rows);
  const int max_cover = content_h - 2;      // keep ≥2 rows for header/grid.
  if (max_cover < 1) return Rect{};
  if (rows > max_cover) rows = max_cover;
  if (rows < 1) return Rect{};

  // Origin: detail column top, the content origin (the spacer row above is
  // the "1-above" padding DESIGN §3.3 asks for).
  return Rect{col_x, kContentY0, cover_w, rows};
}

// Provider caption (05 §15): the serving provider leads (▸), then the other
// providers with availability markers ([+] bound / [-] absent / [?] unchecked);
// a pinned provider is flagged (📌). Pure over EpisodeState — no store read.
// "" when nothing serves yet.
std::string provider_caption(const EpisodeState& es) {
  if (es.serving.empty()) return {};
  std::string out = "\xE2\x96\xB8 ";  // ▸ leads the serving provider.
  out += es.serving;
  if (!es.pinned.empty() && es.pinned == es.serving) out += " \xF0\x9F\x93\x8C";  // 📌.
  for (const auto& [name, mark] : es.avail) {
    if (name == es.serving) continue;
    const char* m = "?";
    switch (mark) {
      case AvailMark::Bound:     m = "+"; break;
      case AvailMark::Absent:    m = "-"; break;
      case AvailMark::Unchecked: m = "?"; break;
    }
    out += " \xC2\xB7 ";  // · separator.
    out += name;
    out += "[";
    out += m;
    out += "]";
    if (!es.pinned.empty() && es.pinned == name) out += "\xF0\x9F\x93\x8C";
  }
  return out;
}

std::vector<std::string> wrap_text(std::string_view text, int max_cols) {
  std::vector<std::string> out;
  if (max_cols <= 0) return out;
  std::string line;
  int line_w = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t j = text.find(' ', i);
    if (j == std::string_view::npos) j = text.size();
    std::string_view word = text.substr(i, j - i);
    i = j + 1;
    if (word.empty()) continue;  // space runs collapse.
    int ww = str_width(word);
    // A word wider than the box: flush the line, then hard-break the word on
    // codepoint boundaries (truncate_to_cols with no ellipsis = a raw prefix).
    while (ww > max_cols) {
      if (!line.empty()) {
        out.push_back(std::move(line));
        line.clear();
        line_w = 0;
      }
      const std::string head = truncate_to_cols(word, max_cols, "");
      out.push_back(head);
      word.remove_prefix(head.size());
      ww = str_width(word);
    }
    if (word.empty()) continue;
    if (line.empty()) {
      line.assign(word);
      line_w = ww;
    } else if (line_w + 1 + ww <= max_cols) {
      line += ' ';
      line.append(word);
      line_w += 1 + ww;
    } else {
      out.push_back(std::move(line));
      line.assign(word);
      line_w = ww;
    }
  }
  if (!line.empty()) out.push_back(std::move(line));
  return out;
}

}  // namespace detail

namespace {

// The title a catalog row shows: the DESIGN §8.2 fallback chain
// (domain.hpp's preferred_title) under the config-live preference (P18;
// romaji-first was P7's fixed placeholder per PORT_CPP.md P3).
std::string_view row_title(const Enrichment& e, TitleLanguage pref) {
  return preferred_title(e.title_romaji,
                         e.title_english ? std::optional<std::string_view>(*e.title_english)
                                          : std::nullopt,
                         e.title_native ? std::optional<std::string_view>(*e.title_native)
                                        : std::nullopt,
                         pref);
}

// One list row (§4.1): "[glyph] title… [score]" — title left, score
// right-anchored to the pane's own edge (`x0 + w`), 1-cell left indent + 1
// right padding. No watchlist glyph in Browse (§8.1: that's History only).
void draw_list_row(CellBuffer& buf, int x0, int w, int y, const CatalogRow& row,
                   bool selected, bool list_focused, TitleLanguage title_pref) {
  const Rgb bg = selected && list_focused ? theme::surface : theme::bg;
  buf.fill(Rect{x0, y, w, 1}, bg);

  const Rgb title_col = selected
                            ? (list_focused ? theme::focus : theme::focus)
                            : theme::fg;
  const Style title_st = (selected && list_focused) ? Style::Bold : Style::None;

  int x = x0 + 1;
  if (selected) {
    const Rgb glyph_col = list_focused ? theme::focus : theme::fg3;
    x = buf.put_str(x, y, "\xE2\x96\xB8", glyph_col, bg);  // ▸
  } else {
    x += 1;
  }
  x += 1;

  std::string score = "[--]";
  if (row.meta.score.has_value()) {
    score = "[" + std::to_string(*row.meta.score) + "]";
  }
  const int score_w = str_width(score);
  const int title_max = (x0 + w - 1 - score_w - 1) - x;
  const std::string title = truncate_to_cols(
      row_title(row.meta, title_pref), title_max > 0 ? title_max : 0);
  buf.put_str(x, y, title, title_col, bg, title_st);
  buf.put_str(x0 + w - 1 - score_w, y, score, theme::fg2, bg);
}

// --- History grouped list (05 §2, DESIGN §5.4) ------------------------------

const char* history_status_glyph(ListStatus status) {
  switch (status) {
    case ListStatus::Watching:  return "\xE2\x96\xB8";  // ▸
    case ListStatus::Completed: return "\xE2\x97\x8F";  // ●
    case ListStatus::Planning:  return "\xE2\x97\x8B";  // ○
    case ListStatus::Paused:    return "\xE2\x97\x90";  // ◐
    case ListStatus::Dropped:   return "\xC2\xB7";       // ·
  }
  return "?";  // unreachable.
}

const char* history_status_label(ListStatus status) {
  switch (status) {
    case ListStatus::Watching:  return "watching";
    case ListStatus::Completed: return "completed";
    case ListStatus::Planning:  return "planning";
    case ListStatus::Paused:    return "paused";
    case ListStatus::Dropped:   return "dropped";
  }
  return "";  // unreachable.
}

// DESIGN §4.5: 16 minimum, scaling to 24 with available width.
constexpr int kHistoryBarMin = 16;
constexpr int kHistoryBarMax = 24;

int history_bar_width(int avail) {
  int w = avail - 30;
  if (w < kHistoryBarMin) w = kHistoryBarMin;
  if (w > kHistoryBarMax) w = kHistoryBarMax;
  return w;
}

// Cell geometry for one progress bar (DESIGN §4.5/§8.1). A null total fills a
// third of the bar as a non-zero signal (no denominator, nothing to scale). A
// positive total: filled cell count, floored at one cell once progress > 0 so
// a lone watched episode on a long season doesn't truncate to zero.
int history_bar_filled(std::uint32_t progress, std::optional<std::uint32_t> total, int width) {
  if (total.has_value() && *total > 0) {
    const std::uint32_t p = std::min(progress, *total);
    const std::uint64_t cell = (static_cast<std::uint64_t>(p) * static_cast<std::uint64_t>(width)) /
                               static_cast<std::uint64_t>(*total);
    int filled = static_cast<int>(cell);
    if (progress > 0 && filled < 1) filled = 1;
    return std::min(filled, width);
  }
  return progress > 0 ? width / 3 : 0;
}

// One grouped-list title row: "{glyph} {title}" — no episode count here (that
// rides the bar row; DESIGN §5.4 "not built" note: the row-1 right-meta comp
// was never ratified, so shipped row 1 stays title-only).
void draw_history_title_row(CellBuffer& buf, int x0, int w, int y, const Show& show,
                            bool selected, bool list_focused, TitleLanguage title_pref) {
  const Rgb bg = selected && list_focused ? theme::surface : theme::bg;
  buf.fill(Rect{x0, y, w, 1}, bg);
  const Rgb glyph_col = selected ? (list_focused ? theme::focus : theme::fg3) : theme::fg2;
  const Rgb title_col = selected ? theme::focus : theme::fg;
  const Style title_st = (selected && list_focused) ? Style::Bold : Style::None;
  int x = x0 + 1;
  x = buf.put_str(x, y, history_status_glyph(show.list_status), glyph_col, bg);
  x = buf.put_str(x, y, " ", bg, bg);
  // P37 slice 3: the NEW marker (new-episode notice, cleared on open) reuses
  // the P17 Discover badge's look (" NEW", focus + Bold) but is a persisted
  // per-show flag here, not a recomputed-every-draw cour check.
  const char* marker = show.notice_pending ? " NEW" : "";
  const int marker_w = show.notice_pending ? str_width(marker) : 0;
  const std::string title = truncate_to_cols(row_title(show.enrichment, title_pref),
                                             (x0 + w - 1) - x - marker_w);
  x = buf.put_str(x, y, title, title_col, bg, title_st);
  if (show.notice_pending) buf.put_str(x, y, marker, theme::focus, bg, Style::Bold);
}

// One grouped-list progress-bar row: "[bar]  N / M eps". Completed rows
// de-emphasize to fg3 (DESIGN §5.4: "completed rows use text.dim; they've
// earned their de-emphasis").
void draw_history_bar_row(CellBuffer& buf, int x0, int w, int y, const Show& show,
                          bool selected, bool list_focused) {
  const Rgb bg = selected && list_focused ? theme::surface : theme::bg;
  buf.fill(Rect{x0, y, w, 1}, bg);
  const int bar_w = history_bar_width(w);
  const int filled = history_bar_filled(show.progress, show.enrichment.total_episodes, bar_w);
  const bool dim = show.list_status == ListStatus::Completed;
  const Rgb fill_col = dim ? theme::fg3 : (selected && list_focused ? theme::focus : theme::fg);
  int x = x0 + 1;
  x = buf.put_str(x, y, "[", theme::chrome, bg);
  for (int i = 0; i < bar_w; ++i) {
    const char* glyph = (i < filled) ? "\xE2\x96\x88" : "\xE2\x96\x91";  // █ / ░
    x = buf.put_str(x, y, glyph, i < filled ? fill_col : theme::chrome, bg);
  }
  x = buf.put_str(x, y, "]", theme::chrome, bg);
  const std::string total = show.enrichment.total_episodes.has_value()
                                ? std::to_string(*show.enrichment.total_episodes)
                                : "?";
  const std::string frac = "  " + std::to_string(show.progress) + " / " + total + " eps";
  buf.put_str(x, y, frac, dim ? theme::fg3 : theme::fg2, bg);
}

// Detail header (§4.4): title, chips row (status/season), score+genres line.
// Returns the next free row.
int draw_detail_header(CellBuffer& buf, int x0, int w, int y0, const Enrichment& e,
                       TitleLanguage title_pref,
                       std::optional<std::uint32_t> user_score = std::nullopt) {
  int y = y0;
  const std::string title = truncate_to_cols(row_title(e, title_pref), w);
  buf.put_str(x0, y, title, theme::fg, theme::bg, Style::Bold);
  y += 2;  // blank row after the title stack (§3.7 margins).

  // Chips row: status + season/year.
  {
    int x = x0;
    if (e.status.has_value()) {
      x = buf.put_str(x, y, *e.status, theme::fg2, theme::bg);
      x += 1;
    }
    if (e.year.has_value()) {
      if (x > x0) x = buf.put_str(x, y, " ", theme::bg, theme::bg) ;
      x = buf.put_str(x, y, std::to_string(*e.year), theme::fg2, theme::bg);
    }
    y += 1;
  }

  // "✦ [NN/100]  · genre · genre" (§4.4/§8.1). Fallback "[--/100]" if unscored.
  {
    int x = x0;
    const bool high = e.score.has_value() && *e.score >= 91;
    const Style sc_st = high ? Style::Bold : Style::None;
    const Rgb sc_col = high ? theme::hot : theme::fg2;
    x = buf.put_str(x, y, "\xE2\x9C\xA6 ", sc_col, theme::bg, sc_st);  // ✦
    const std::string score =
        e.score.has_value() ? ("[" + std::to_string(*e.score) + "/100]") : "[--/100]";
    x = buf.put_str(x, y, score, sc_col, theme::bg, sc_st);
    // User score (P34), beside the community score: "you: 7.5" or omitted
    // entirely when unset (0/nullopt) — no clutter for the common case of an
    // unrated library row, and non-library selections never reach here.
    if (user_score.has_value() && *user_score > 0) {
      x = buf.put_str(x, y, "  you: ", theme::fg3, theme::bg);
      const double tenths = static_cast<double>(*user_score) / 10.0;
      std::string you = std::to_string(tenths);
      while (!you.empty() && you.back() == '0') you.pop_back();
      if (!you.empty() && you.back() == '.') you.pop_back();
      x = buf.put_str(x, y, you, theme::focus, theme::bg, Style::Bold);
    }
    for (const auto& g : e.genres) {
      x = buf.put_str(x, y, "  \xC2\xB7 ", theme::fg3, theme::bg);  // ·
      x = buf.put_str(x, y, g, theme::fg2, theme::bg);
      if (x >= x0 + w) break;
    }
    y += 1;
  }

  // Compact metadata line (§5.3a): episodes never omitted, others best-effort.
  {
    std::string line = e.total_episodes.has_value()
                           ? (std::to_string(*e.total_episodes) + " eps")
                           : "? eps";
    if (e.kind.has_value()) line += "  \xC2\xB7 " + *e.kind;
    if (e.duration_minutes.has_value())
      line += "  \xC2\xB7 " + std::to_string(*e.duration_minutes) + "m";
    buf.put_str(x0, y, truncate_to_cols(line, w), theme::fg3, theme::bg);
    y += 2;  // blank row before the grid.
  }
  return y;
}

// Episode grid (§4.6): 5-col cells "[NN] "/"[NNN]", wrapping to fill `w`. A
// provider caption (05 §15) leads on its own row when a provider is serving.
void draw_episode_grid(CellBuffer& buf, int x0, int w, int y0, int y1,
                       const App& app, bool grid_focused) {
  const EpisodeState& es = app.episode;

  if (!app.deps) {
    return;  // no deps wired (e.g. a bare draw() unit test): nothing to show.
  }
  if (es.loading) {
    buf.put_str(x0, y0, "\xE2\xA0\x8B loading episodes\xE2\x80\xA6", theme::focus, theme::bg);
    return;
  }
  if (!es.fetched) {
    return;  // no fetch fired yet (no item selected) — blank by design.
  }
  if (es.no_source && es.episodes.empty()) {
    const char* msg = "no episodes";
    buf.put_str(x0, y0, msg, theme::fg3, theme::bg, Style::Italic);
    return;
  }

  // Caption row (05 §15): serving provider + availability markers, dim.
  if (const std::string cap = detail::provider_caption(es); !cap.empty() && y0 < y1) {
    buf.put_str(x0, y0, truncate_to_cols(cap, w), theme::fg3, theme::bg, Style::None);
    ++y0;
  }

  const int cell_w = 5;
  int x = x0, y = y0;
  for (std::size_t i = 0; i < es.episodes.size(); ++i) {
    if (y >= y1) break;
    const std::string& label = es.episodes[i];
    const bool is_cursor = grid_focused && static_cast<int>(i) == es.cursor;
    // Play state is app-global (04 §7.7); mark the cell only when the play
    // belongs to the show THIS grid was fetched for.
    const bool is_playing = app.play.active && app.play.for_id == es.for_id &&
                            label == app.play.episode;
    // Watched mark (P9, DESIGN §"Dim is receded"): a fully-watched episode
    // recedes to text.dim. es.watched is index-parallel to episodes but may be
    // empty (no store / not yet computed) — bounds-check before reading.
    const bool is_watched = i < es.watched.size() && es.watched[i];

    std::string glyph;
    Rgb fg = theme::fg3;
    Rgb bg = theme::bg;
    Style st = Style::None;

    if (is_playing) {
      const bool slow = (app.tick_count - app.play.started_tick) > kSlowSpinnerTicks;
      glyph = "[" + std::string(kSpinnerFrames[app.spinner_phase]) + "]";
      fg = slow ? theme::hot : theme::focus;
      bg = theme::surface;
      st = Style::Bold;
    } else if (is_cursor) {
      glyph = label.size() <= 2 ? ("[" + label + "] ") : ("[" + label + "]");
      fg = theme::focus;
      bg = theme::surface;
      st = Style::Bold;
    } else {
      glyph = label.size() <= 2 ? ("[" + label + "] ") : ("[" + label + "]");
      // Watched recedes to text.dim; unwatched stays text.muted (DESIGN §"Dim
      // is receded" — watched items dim but stay navigable).
      fg = is_watched ? theme::fg3 : theme::fg2;
      bg = theme::bg;
    }

    buf.put_str(x, y, truncate_to_cols(glyph, cell_w), fg, bg, st);
    x += cell_w;
    if (x + cell_w > x0 + w) {
      x = x0;
      ++y;
    }
  }
}

// Playback state line + position bar (03 §6.3, 04 §7.7): only while a play is
// in flight for the open show. Renders under the grid, so callers pass the
// row just past whatever draw_episode_grid used.
void draw_playback_line(CellBuffer& buf, int x0, int w, int y, const App& app) {
  // Only under the grid of the show that owns the play (browsing to another
  // show hides the line; the play itself keeps running app-globally).
  if (!app.play.active || app.play.for_id != app.episode.for_id) return;
  std::string line = "\xE2\x96\xB6 " + app.play.episode;  // ▶
  if (app.play.duration > 0.0) {
    const int pos_s = static_cast<int>(app.play.position);
    const int dur_s = static_cast<int>(app.play.duration);
    line += "  " + std::to_string(pos_s / 60) + ":" +
           (pos_s % 60 < 10 ? "0" : "") + std::to_string(pos_s % 60) + " / " +
           std::to_string(dur_s / 60) + ":" +
           (dur_s % 60 < 10 ? "0" : "") + std::to_string(dur_s % 60);
  }
  buf.put_str(x0, y, truncate_to_cols(line, w), theme::hot, theme::bg, Style::Bold);
}

// Download state line (P35 slice 3): mirrors draw_playback_line's contract —
// only under the grid of the show that owns the in-flight transfer. Byte
// counts when the server told us a total, an indeterminate spinner otherwise
// (the ffmpeg arm posts no counts in v1).
void draw_download_line(CellBuffer& buf, int x0, int w, int y, const App& app) {
  if (!app.download.active || app.download.for_id != app.episode.for_id) return;
  auto mb = [](std::uint64_t bytes) {
    const std::uint64_t tenths = bytes / (1024ull * 1024ull / 10ull);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
  };
  std::string line = "\xE2\x87\xA3 ep " + app.download.episode;  // ⇣
  if (app.download.total > 0) {
    const std::uint64_t pct = app.download.bytes * 100ull / app.download.total;
    line += "  " + mb(app.download.bytes) + " / " + mb(app.download.total) + "  " +
            std::to_string(pct) + "%";
  } else {
    line += "  downloading" + std::string(kSpinnerFrames[app.spinner_phase]);
    if (app.download.bytes > 0) line += "  " + mb(app.download.bytes);
  }
  buf.put_str(x0, y, truncate_to_cols(line, w), theme::focus, theme::bg, Style::Bold);
}

void draw_synopsis_panel(const App& app, CellBuffer& buf, int x0, int w,
                         int col_y1, const Enrichment& show);

// Shared detail column body (header + grid + playback line + the synopsis
// right of the cover), reused by Browse's two-pane, History's two-pane, and
// the Detail zoom (05 §15 "one detail renderer" pattern extended to History,
// P16). `show` is the already-resolved selection (detail::selected_enrichment);
// callers no-op before this when there is none.
void draw_detail_column(const App& app, CellBuffer& buf, int x0, int w, int y0, int y1,
                        const Enrichment& show, bool grid_focused) {
  // The detail header starts below the cover block (P8, DESIGN §3.3): the cover
  // occupies rows [1, 1+cover_h); leave one blank row, then the header. app.cpp
  // owns the cover rect + placeholder fill inside that region.
  const Rect crect = detail::cover_rect(app);
  const int y_head = crect.empty() ? y0 : (crect.y + crect.h + 1);
  const Show* lib_show = detail::selected_show(app);
  const std::optional<std::uint32_t> user_score =
      lib_show != nullptr ? lib_show->user_score : std::nullopt;
  const int y_grid = draw_detail_header(
      buf, x0, w, y_head, show, parse_title_language(app.config.title_language), user_score);
  // The grid (and its provider caption) belongs to the show it was fetched
  // for (ROD-222/329 render half): after Esc-to-list + scroll, another show's
  // header must not sit over a stale grid — hide it until an engage lands.
  if (app.episode.for_id == show.anilist_id) {
    draw_episode_grid(buf, x0, w, y_grid, y1 - 1, app, grid_focused);
    draw_playback_line(buf, x0, w, y1 - 1, app);
    // The download line shares the bottom row when no play is up, else sits
    // one above (both at once is the rare both-in-flight case; the row it
    // borrows from the grid just clips, DESIGN degrade-not-fight).
    const bool play_line = app.play.active && app.play.for_id == app.episode.for_id;
    draw_download_line(buf, x0, w, play_line ? y1 - 2 : y1 - 1, app);
  }
  draw_synopsis_panel(app, buf, x0, w, y1, show);
}

// The `c`-toggled Characters/Recommendations section (P36 slice 2), Detail
// zoom only. Two blocks separated by a blank row: Characters ("name — role,
// VA name"), then Recommendations ("title  [score]"), the recommendation
// cursor drawn with the same ▸/bold idiom as draw_list_row so slice 3's
// Enter target is visually obvious. Scrolls as one flat line list (`scroll`
// is a line offset, not a row index) so a long characters block can't push
// recommendations fully off screen without a way back.
void draw_char_recs_section(const App& app, CellBuffer& buf, int x0, int w, int y0, int y1) {
  if (y0 >= y1) return;
  const CharactersRecsState& cr = app.char_recs;

  if (cr.loading) {
    buf.put_str(x0, y0, "\xE2\xA0\x8B loading characters\xE2\x80\xA6", theme::focus, theme::bg);
    return;
  }
  if (cr.failed) {
    buf.put_str(x0, y0, "can't reach AniList", theme::warn, theme::bg, Style::Italic);
    return;
  }
  if (!cr.fetched) return;  // reconcile_char_recs hasn't landed an answer yet.
  if (cr.data.characters.empty() && cr.data.recommendations.empty()) {
    buf.put_str(x0, y0, "nothing listed", theme::fg3, theme::bg, Style::Italic);
    return;
  }

  // Flatten to lines so scroll is one offset over the whole section, same
  // idiom as HistoryState::layout(). rec_of[i] names the recommendations
  // index a rendered line promotes (nullopt for headers/character rows).
  struct Line {
    enum class Kind { CharHeader, Char, Blank, RecHeader, Rec } kind;
    std::size_t idx = 0;
  };
  std::vector<Line> lines;
  if (!cr.data.characters.empty()) {
    lines.push_back({Line::Kind::CharHeader, 0});
    for (std::size_t i = 0; i < cr.data.characters.size(); ++i) {
      lines.push_back({Line::Kind::Char, i});
    }
  }
  if (!cr.data.recommendations.empty()) {
    if (!lines.empty()) lines.push_back({Line::Kind::Blank, 0});
    lines.push_back({Line::Kind::RecHeader, 0});
    for (std::size_t i = 0; i < cr.data.recommendations.size(); ++i) {
      lines.push_back({Line::Kind::Rec, i});
    }
  }

  int y = y0;
  for (std::size_t i = cr.scroll; i < lines.size() && y < y1; ++i, ++y) {
    const Line& ln = lines[i];
    switch (ln.kind) {
      case Line::Kind::CharHeader:
        buf.put_str(x0, y, "Characters", theme::fg2, theme::bg, Style::Bold);
        break;
      case Line::Kind::Blank:
        break;
      case Line::Kind::Char: {
        const CharacterEntry& c = cr.data.characters[ln.idx];
        std::string line = c.name;
        if (!c.role.empty() || c.va_name.has_value()) {
          line += "  \xE2\x80\x94 ";  // —
          if (!c.role.empty()) line += c.role;
          if (c.va_name.has_value()) {
            if (!c.role.empty()) line += ", ";
            line += *c.va_name;
          }
        }
        buf.put_str(x0, y, truncate_to_cols(line, w), theme::fg2, theme::bg);
        break;
      }
      case Line::Kind::RecHeader:
        buf.put_str(x0, y, "Recommendations", theme::fg2, theme::bg, Style::Bold);
        break;
      case Line::Kind::Rec: {
        const Enrichment& e = cr.data.recommendations[ln.idx];
        // The row highlight only reads as a live cursor while Tab has given
        // the section focus (cr.focused); grid-focused, it dims to a plain
        // marker so Enter's actual target (the grid) stays visually primary.
        const bool selected = ln.idx == cr.cursor;
        const Rgb cursor_col = cr.focused ? theme::focus : theme::fg2;
        const Rgb bg = selected && cr.focused ? theme::surface : theme::bg;
        buf.fill(Rect{x0, y, w, 1}, bg);
        int x = x0;
        if (selected) {
          x = buf.put_str(x, y, "\xE2\x96\xB8 ", cursor_col, bg);  // ▸
        } else {
          x += 2;
        }
        const std::string score =
            e.score.has_value() ? ("[" + std::to_string(*e.score) + "]") : "[--]";
        const int score_w = str_width(score);
        const int title_max = (x0 + w - score_w - 1) - x;
        const std::string title = truncate_to_cols(
            row_title(e, parse_title_language(app.config.title_language)),
            title_max > 0 ? title_max : 0);
        buf.put_str(x, y, title, selected ? cursor_col : theme::fg,
                    bg, selected && cr.focused ? Style::Bold : Style::None);
        buf.put_str(x0 + w - score_w, y, score, theme::fg2, bg);
        break;
      }
    }
  }
}

// Synopsis panel (§9, the MAL-style dedicated page): the cover leaves the
// region to its right empty — fill it with the show's description, wrapped
// to the box, spanning the cover's own rows. Rendered by draw_detail_column,
// so the Detail zoom AND the two-pane Browse/History panes all carry it
// (the pane used to show no synopsis while the manga app shows
// one per selection — the "compact layout" carve-out is gone). Degrades to
// nothing rather than squeeze: no cover rect (no cover URL / tiny frame), no
// description, or a box under kSynopsisMinCols columns all skip. A
// description longer than the box clips, the last visible row marked with a
// dim ellipsis (no scroll in v1).
void draw_synopsis_panel(const App& app, CellBuffer& buf, int x0, int w,
                         int col_y1, const Enrichment& show) {
  constexpr int kGap = 3;              // breathing room off the poster's edge.
  constexpr int kSynopsisMinCols = 16;
  if (!show.description.has_value() || show.description->empty()) return;
  const Rect crect = detail::cover_rect(app);
  if (crect.empty()) return;
  const int x = crect.x + crect.w + kGap;
  const int box_w = x0 + w - x;
  if (box_w < kSynopsisMinCols) return;
  const int y_bot = std::min(crect.y + crect.h, col_y1);
  int y = crect.y;
  if (y_bot - y < 2) return;  // header + at least one text row.

  buf.put_str(x, y, "Synopsis", theme::fg2, theme::bg, Style::Bold);
  ++y;
  const std::vector<std::string> lines =
      detail::wrap_text(*show.description, box_w);
  for (std::size_t i = 0; i < lines.size() && y < y_bot; ++i, ++y) {
    const bool clipped = y == y_bot - 1 && i + 1 < lines.size();
    if (clipped) {
      const int ex = buf.put_str(x, y, truncate_to_cols(lines[i], box_w - 2, ""),
                                 theme::fg2, theme::bg);
      buf.put_str(ex + 1, y, "\xE2\x80\xA6", theme::fg3, theme::bg);  // …
    } else {
      buf.put_str(x, y, lines[i], theme::fg2, theme::bg);
    }
  }
}

}  // namespace

void draw_browse(const App& app, CellBuffer& buf, int y0, int y1) {
  const int w = buf.width();
  const bool two_pane = w >= kPaneSplitMin;
  const PaneSplit split = pane_split(w);
  const int list_w = two_pane ? split.list_w : (w - 3);

  // List column, from the scroll offset (05 §1.1: the cursor stays visible;
  // on_key's browse_scroll_into_view keeps the offset in step).
  {
    int y = y0;
    const std::size_t first =
        static_cast<std::size_t>(std::max(0, app.browse_scroll));
    for (std::size_t i = first; i < app.catalog.size() && y < y1; ++i, ++y) {
      draw_list_row(buf, 2, list_w, y, app.catalog[i],
                    static_cast<int>(i) == app.list_cursor, app.pane == Pane::List,
                    parse_title_language(app.config.title_language));
    }
    if (app.catalog.empty() && app.search_query.empty()) {
      const char* hint = "/ to search AniList";
      buf.put_str(2, y0, hint, theme::fg3, theme::bg, Style::Italic);
    }
  }

  if (!two_pane) return;  // < kPaneSplitMin: list-only (§3.2).

  // Detail column.
  const Enrichment* show = detail::selected_enrichment(app);
  if (show == nullptr) return;
  draw_detail_column(app, buf, split.detail_x, split.detail_w, y0, y1, *show,
                     app.pane == Pane::Detail);
}

void draw_detail_zoom(const App& app, CellBuffer& buf, int y0, int y1) {
  const int w = buf.width();
  const Enrichment* show = detail::selected_enrichment(app);
  if (show == nullptr) return;
  // The `c` section (P36) claims the bottom third of the zoom canvas when
  // toggled on, leaving the rest to the shared column (header + grid); a
  // narrow zoom (< 6 spare rows) skips the split rather than squeeze both
  // into illegibility — the toggle stays armed, it just renders nothing
  // until the frame grows.
  int col_y1 = y1;
  if (app.char_recs.visible) {
    const int section_h = detail::char_recs_section_height(y1 - y0);
    if (section_h > 0) {
      col_y1 = y1 - section_h - 1;  // blank separator row.
      // Tab-focus is mutually exclusive between the grid and the section
      // (app.hpp's CharactersRecsState::focused doc): the grid cursor only
      // reads as live while the section does NOT hold focus, mirroring
      // draw_char_recs_section's own cr.focused dimming of its row cursor.
      draw_detail_column(app, buf, 2, w - 4, y0, col_y1, *show,
                          /*grid_focused=*/!app.char_recs.focused);
      draw_char_recs_section(app, buf, 2, w - 4, col_y1 + 1, y1);
      return;
    }
  }
  draw_detail_column(app, buf, 2, w - 4, y0, col_y1, *show, /*grid_focused=*/true);
}

void draw_history(const App& app, CellBuffer& buf, int y0, int y1) {
  const int w = buf.width();
  const HistoryState& hs = app.history;

  if (hs.empty()) {
    const int y = y0 + (y1 - y0) / 2;
    if (hs.load_failed) {
      const std::string msg = "couldn't load your watchlist";
      const int mw = str_width(msg);
      buf.put_str(std::max(0, (w - mw) / 2), y, msg, theme::warn, theme::bg, Style::Italic);
      return;
    }
    // First-run empty state (DESIGN §8.3): Discover leads, Browse recedes — an
    // empty watchlist is a user who doesn't yet know what to watch.
    auto centered = [&](int row, std::string_view s, Rgb col, Style st) {
      buf.put_str(std::max(0, (w - str_width(s)) / 2), row, s, col, theme::bg, st);
    };
    centered(y - 1, "nothing watched yet", theme::fg3, Style::Italic);
    centered(y, "D  see what's popular", theme::fg2, Style::None);
    centered(y + 1, "B  search for a show", theme::fg3, Style::None);
    return;
  }

  const bool two_pane = w >= kPaneSplitMin && hs.selected() != nullptr;
  const PaneSplit split = pane_split(w);
  const int list_w = two_pane ? split.list_w : (w - 3);

  // List column: the grouped layout (05 §2 geometry) drives both scroll and
  // draw off the SAME lines vector, so they can never disagree.
  {
    const std::vector<HistoryLine> lines = hs.layout();
    const bool list_focused = app.pane == Pane::List;
    int y = y0;
    for (std::size_t i = hs.scroll; i < lines.size() && y < y1; ++i, ++y) {
      const HistoryLine& li = lines[i];
      switch (li.kind) {
        case HistoryLine::Kind::Blank:
          break;
        case HistoryLine::Kind::Rule: {
          buf.fill(Rect{2, y, list_w, 1}, theme::bg);
          for (int cx = 0; cx < list_w; ++cx) {
            buf.put_str(2 + cx, y, "\xE2\x94\x80", theme::chrome, theme::bg);  // ─
          }
          break;
        }
        case HistoryLine::Kind::Header: {
          int x = 2;
          x = buf.put_str(x, y, history_status_glyph(li.status), theme::focus, theme::bg);
          x = buf.put_str(x, y, " ", theme::bg, theme::bg);
          x = buf.put_str(x, y, history_status_label(li.status), theme::fg, theme::bg,
                          Style::Bold);
          buf.put_str(x, y, " (" + std::to_string(li.count) + ")", theme::fg2, theme::bg);
          break;
        }
        case HistoryLine::Kind::Title: {
          const Show& show = hs.rows[hs.order[li.ord]];
          draw_history_title_row(buf, 2, list_w, y, show, li.ord == hs.cursor, list_focused,
                                 parse_title_language(app.config.title_language));
          break;
        }
        case HistoryLine::Kind::Bar: {
          const Show& show = hs.rows[hs.order[li.ord]];
          draw_history_bar_row(buf, 2, list_w, y, show, li.ord == hs.cursor, list_focused);
          break;
        }
      }
    }
  }

  if (!two_pane) return;  // < kPaneSplitMin, or no focused record: list-only (§5.4a).

  // Detail column: the same shared renderer Browse's two-pane uses.
  const Enrichment* show = detail::selected_enrichment(app);
  if (show == nullptr) return;
  draw_detail_column(app, buf, split.detail_x, split.detail_w, y0, y1, *show,
                     app.pane == Pane::Detail);
}

// --- Schedule (P37) ---------------------------------------------------------

namespace {

const char* weekday_label(Weekday w) {
  switch (w) {
    case Weekday::Monday:    return "monday";
    case Weekday::Tuesday:   return "tuesday";
    case Weekday::Wednesday: return "wednesday";
    case Weekday::Thursday:  return "thursday";
    case Weekday::Friday:    return "friday";
    case Weekday::Saturday:  return "saturday";
    case Weekday::Sunday:    return "sunday";
  }
  return "";  // unreachable.
}

// "2d 4h" / "4h 12m" / "37m" style countdown (03 §-adjacent format, no
// PORT_PARITY table to mirror — v0's own rendering, P37). Always non-empty:
// a sub-minute countdown renders "<1m" rather than "0m". `secs` is always
// > 0 here (schedule() already filtered non-future rows).
}  // namespace

// In detail:: (not the anon namespace) so the DoD's countdown table test can
// pin the boundary strings (P37 review).
std::string detail::countdown_text(std::int64_t secs) {
  if (secs < 60) return "<1m";
  const std::int64_t mins = secs / 60;
  const std::int64_t days = mins / (60 * 24);
  const std::int64_t hours = (mins / 60) % 24;
  const std::int64_t rem_mins = mins % 60;
  if (days > 0) {
    return std::to_string(days) + "d " + std::to_string(hours) + "h";
  }
  if (hours > 0) {
    return std::to_string(hours) + "h " + std::to_string(rem_mins) + "m";
  }
  return std::to_string(rem_mins) + "m";
}

void draw_schedule(const App& app, CellBuffer& buf, int y0, int y1) {
  const int w = buf.width();
  const ScheduleState& ss = app.schedule;

  if (ss.empty()) {
    const int y = y0 + (y1 - y0) / 2;
    const std::string msg = ss.load_failed ? "couldn't load your watchlist"
                                           : "nothing airing soon";
    const int mw = str_width(msg);
    buf.put_str(std::max(0, (w - mw) / 2), y, msg, theme::fg3, theme::bg, Style::Italic);
    return;
  }

  const int list_w = w - 3;
  const TitleLanguage title_pref = parse_title_language(app.config.title_language);
  const std::vector<ScheduleState::Line> lines = ss.layout();
  int y = y0;
  for (std::size_t i = ss.scroll; i < lines.size() && y < y1; ++i, ++y) {
    const ScheduleState::Line& li = lines[i];
    switch (li.kind) {
      case ScheduleState::Line::Kind::Blank:
        break;
      case ScheduleState::Line::Kind::Rule: {
        buf.fill(Rect{2, y, list_w, 1}, theme::bg);
        for (int cx = 0; cx < list_w; ++cx) {
          buf.put_str(2 + cx, y, "\xE2\x94\x80", theme::chrome, theme::bg);  // ─
        }
        break;
      }
      case ScheduleState::Line::Kind::Header: {
        int x = 2;
        x = buf.put_str(x, y, weekday_label(li.weekday), theme::fg, theme::bg, Style::Bold);
        buf.put_str(x, y, " (" + std::to_string(li.count) + ")", theme::fg2, theme::bg);
        break;
      }
      case ScheduleState::Line::Kind::Entry: {
        const ScheduleEntry& entry = ss.groups[li.group_ix].entries[li.entry_ix];
        if (entry.show_index >= ss.rows.size()) break;  // never true by construction.
        const Show& show = ss.rows[entry.show_index];
        const bool selected = li.ord == ss.cursor;
        const Rgb bg = selected ? theme::surface : theme::bg;
        buf.fill(Rect{2, y, list_w, 1}, bg);
        const Rgb title_col = selected ? theme::focus : theme::fg;
        const Style st = selected ? Style::Bold : Style::None;
        const std::string countdown = detail::countdown_text(entry.seconds_until);
        const std::string ep_tag = entry.episode > 0
                                       ? ("ep " + std::to_string(entry.episode) + " \xC2\xB7 ")
                                       : std::string{};
        const std::string tail = ep_tag + countdown;
        const int tail_w = str_width(tail);
        int x = 3;
        const std::string title = truncate_to_cols(row_title(show.enrichment, title_pref),
                                                    (2 + list_w - 1) - x - tail_w - 1);
        buf.put_str(x, y, title, title_col, bg, st);
        buf.put_str(2 + list_w - 1 - tail_w, y, tail, theme::fg2, bg);
        break;
      }
    }
  }
}

// --- Discover card grid (P17a, DESIGN §3.8) --------------------------------

namespace {

// Compact card score badge (render.rs score_badge, DESIGN §2.2): "[97]" / "[--]".
std::string score_badge(std::optional<std::uint32_t> score) {
  return score.has_value() ? ("[" + std::to_string(*score) + "]") : "[--]";
}

// DESIGN §2.2 tier colours (render.rs score_style). cap_hot is the card rule
// (DESIGN §3.8): the 91+ tier steps down to fg so `TOP` keeps the one magenta
// pointer. Returns {fg color, bold?}.
std::pair<Rgb, bool> score_style(std::optional<std::uint32_t> score, bool cap_hot) {
  if (score.has_value()) {
    const std::uint32_t s = *score;
    if (s >= 91) return {cap_hot ? theme::fg : theme::hot, true};
    if (s >= 76) return {theme::fg, false};
    if (s >= 51) return {theme::fg2, false};
  }
  return {theme::fg3, false};
}

// Center `s` on row `y` of the [x0, x0+w) band; clipped, safe on tiny frames.
void draw_centered(CellBuffer& buf, int x0, int w, int y, std::string_view s, Rgb col,
                   Style st = Style::None) {
  const int sw = str_width(s);
  const int x = x0 + std::max(0, (w - sw) / 2);
  buf.put_str(x, y, s, col, theme::bg, st);
}

// One card at (x, y): cover cell + rank/badge/title/format rows, selection
// marker in the LEFT GUTTER (discover.rs draw_card). The cover cell is
// image-owned (returns the exclusion rect for the caller to accumulate) when
// the pool holds Ready pixels for the row's url; otherwise a placeholder
// surface-fill "#N" (with a spinner overlay while that url is Loading), the
// P17a fallback that stays the whole story in placeholder mode (no kitty).
std::optional<Rect> draw_card(const App& app, CellBuffer& buf, const CatalogRow& row,
                              std::size_t rank, bool selected, DiscoverAxis axis,
                              Cour cour, int x, int y, int cover_w, int cover_h) {
  const Enrichment& e = row.meta;

  std::optional<Rect> exclusion;
  const CoverRenderEntry* entry =
      (app.cover_caps.images() && e.cover_url.has_value())
          ? app.cover_render_store.get(*e.cover_url)
          : nullptr;
  if (entry != nullptr) {
    // Image-owned: the diff must not paint text into this cell; run()'s
    // compose_grid_apc transmits the pixels after the diff (R3).
    exclusion = Rect{x, y, cover_w, cover_h};
  } else {
    // Placeholder cover cell: surface fill + centered "#N" (the only
    // surface-elevated element in the grid). The "\n#N" in Rust drops the
    // label one row; mirror by centering on the cell's vertical middle.
    buf.fill(Rect{x, y, cover_w, cover_h}, theme::surface);
    const std::string label = "#" + std::to_string(rank + 1);
    const int lw = str_width(label);
    const int lx = x + std::max(0, (cover_w - lw) / 2);
    const int ly = y + std::min(cover_h - 1, cover_h / 2);
    buf.put_str(lx, ly, label, theme::fg3, theme::surface);
    // Spinner overlay while this url is in flight.
    const CoverSlot* slot =
        e.cover_url.has_value() ? app.cover_pool.get(*e.cover_url) : nullptr;
    if (slot != nullptr && slot->status == CoverSlotStatus::Loading) {
      const char* sp = kSpinnerFrames[app.spinner_phase];
      buf.put_str(x + std::max(0, (cover_w - str_width(sp)) / 2), y, sp, theme::focus,
                  theme::surface);
    }
  }

  const int rank_y = y + cover_h;

  // Left-gutter selection marker: never masks the cover cell (DESIGN §3.8).
  if (selected && x > 0) {
    buf.fill(Rect{x - 1, rank_y, 1, 1}, theme::bg);
    buf.put_str(x - 1, rank_y, "\xE2\x96\xB8", theme::focus, theme::bg);  // ▸
  }

  buf.fill(Rect{x, rank_y, cover_w, 3}, theme::bg);

  // Rank + at most one badge, left-anchored (discover.rs): "#N" then TOP (rank
  // 0, hot) OR NEW (current-cour, not This Season) — exclusive.
  int rx = buf.put_str(x, rank_y, "#" + std::to_string(rank + 1), theme::fg, theme::bg);
  if (rank == 0) {
    buf.put_str(rx, rank_y, " TOP", theme::hot, theme::bg, Style::Bold);
  } else if (is_new_this_cour(e, axis, cour)) {
    buf.put_str(rx, rank_y, " NEW", theme::focus, theme::bg, Style::Bold);
  }

  // Score badge, right-anchored to the cover width (skipped if it won't fit).
  const std::string badge = score_badge(e.score);
  const int badge_w = str_width(badge);
  if (badge_w <= cover_w) {
    const auto [bcol, bbold] = score_style(e.score, /*cap_hot=*/true);
    buf.put_str(x + cover_w - badge_w, rank_y, badge, bcol, theme::bg,
                bbold ? Style::Bold : Style::None);
  }

  // Title row: resolved primary, clipped; focus+bold when selected.
  const std::string_view title = preferred_title(
      e.title_romaji,
      e.title_english ? std::optional<std::string_view>(*e.title_english) : std::nullopt,
      e.title_native ? std::optional<std::string_view>(*e.title_native) : std::nullopt,
      parse_title_language(app.config.title_language));
  const Rgb title_col = selected ? theme::focus : theme::fg;
  const Style title_st = selected ? Style::Bold : Style::None;
  buf.put_str(x, rank_y + 1, truncate_to_cols(title, cover_w), title_col, theme::bg, title_st);

  // Format + episode count left (discover.rs): "TV · 12ep" / "Movie" / "TV ·
  // ??ep" while airing; "—" when the format is unknown.
  const int format_row = rank_y + 2;
  const std::optional<const char*> label =
      format_label(e.kind ? std::optional<std::string_view>(*e.kind) : std::nullopt);
  if (label.has_value()) {
    std::string text;
    if (std::string_view(*label) == "Movie") {
      text = *label;
    } else if (e.total_episodes.has_value()) {
      text = std::string(*label) + " \xC2\xB7 " + std::to_string(*e.total_episodes) + "ep";
    } else if (is_still_airing(e.status ? std::optional<std::string_view>(*e.status)
                                        : std::nullopt)) {
      text = std::string(*label) + " \xC2\xB7 ??ep";
    } else {
      text = *label;
    }
    buf.put_str(x, format_row, truncate_to_cols(text, cover_w), theme::fg2, theme::bg);
  } else {
    buf.put_str(x, format_row, "\xE2\x80\x94", theme::fg3, theme::bg);  // —
  }

  // Genre glyphs right-anchored (discover.rs): up to two, space-joined.
  const std::vector<const char*> glyphs = genre_glyphs(e.genres);
  if (!glyphs.empty()) {
    std::string text;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      if (i > 0) text += " ";
      text += glyphs[i];
    }
    const int gw = str_width(text);
    if (gw < cover_w) {
      buf.put_str(x + cover_w - gw, format_row, text, theme::fg3, theme::bg);
    }
  }
  return exclusion;
}

// The axis bar (discover.rs draw_axis_bar): "[1] Trending · [2] Popular · …",
// the active axis in focus color + bold. Teaches its own 1-4 binds. `filters`
// (P38, §9) appends active-filter chips after the axis labels, right-aligned
// space permitting, truncated (never wrapped — the bar is one row) when the
// canvas is too narrow to hold both; an empty DiscoverFilters renders no
// chips at all, byte-identical to the pre-P38 bar.
void draw_axis_bar(CellBuffer& buf, int w, int y, DiscoverAxis active,
                   const DiscoverFilters& filters) {
  buf.fill(Rect{0, y, w, 1}, theme::bg);
  int x = 2;
  for (std::size_t i = 0; i < kDiscoverAxes.size(); ++i) {
    if (i > 0) x = buf.put_str(x, y, " \xC2\xB7 ", theme::fg3, theme::bg);  // ·
    const bool is_active = kDiscoverAxes[i] == active;
    const Rgb key_col = is_active ? theme::focus : theme::fg2;
    const Rgb label_col = key_col;
    const Style label_st = is_active ? Style::Bold : Style::None;
    x = buf.put_str(x, y, "[" + std::to_string(i + 1) + "]", key_col, theme::bg);
    x = buf.put_str(x, y, std::string(" ") + axis_label(kDiscoverAxes[i]), label_col,
                    theme::bg, label_st);
  }

  if (filters.empty()) return;
  std::vector<std::string> chips;
  for (const std::string& g : filters.genres) chips.push_back(g);
  if (filters.year.has_value()) chips.push_back(std::to_string(*filters.year));
  if (filters.status.has_value()) chips.push_back(*filters.status);
  if (filters.min_score.has_value()) chips.push_back(">" + std::to_string(*filters.min_score));

  const int budget = w - x - 2;  // 2-col right margin, matches the bar's left margin.
  if (budget <= 0) return;
  std::string joined = "f: ";
  for (std::size_t i = 0; i < chips.size(); ++i) {
    if (i > 0) joined += ", ";
    joined += chips[i];
  }
  x = buf.put_str(x, y, "  ", theme::fg3, theme::bg);
  buf.put_str(x, y, truncate_to_cols(joined, budget), theme::warn, theme::bg);
}

// The three no-results states (discover.rs draw_empty_grid, DESIGN §3.8):
// loading (slow-escalated), sticky error, or the genuine empty feed. A slot
// nothing has fetched yet (page == 0, not loading, not failed) stays blank.
void draw_empty_grid(const App& app, CellBuffer& buf, int x0, int w, int y0, int y1,
                     const AxisSlot& slot) {
  const int mid = y0 + (y1 - y0) / 2;
  if (slot.loading) {
    const char* spin = kSpinnerFrames[app.spinner_phase];
    const bool is_slow = (app.tick_count - slot.loading_since) >= kSlowSpinnerTicks;
    const std::string text =
        std::string(spin) + (is_slow ? " taking a moment\xE2\x80\xA6" : " loading feed\xE2\x80\xA6");
    draw_centered(buf, x0, w, mid, text, is_slow ? theme::hot : theme::focus);
  } else if (slot.failed.has_value()) {
    draw_centered(buf, x0, w, mid, "[!] can't reach the feed", theme::hot, Style::Bold);
    draw_centered(buf, x0, w, mid + 1, "check your connection", theme::fg2, Style::Italic);
  } else if (slot.page > 0) {
    draw_centered(buf, x0, w, mid, "no entries", theme::fg2, Style::Italic);
  }
}

// Peek band placeholder (discover.rs draw_peek_band, P17a DEVIATION): surface
// blocks only, no image peeks (Kitty c=/r= scales, not crops — image peeks need
// source-rect keys outside A4). Keeps the "there's more below" affordance.
// Returns true when the band drew.
bool draw_peek_band(CellBuffer& buf, int gx, int gy, int grid_w, int grid_bottom,
                    const AxisSlot& slot, const GridGeo& geo) {
  const int band_y = gy + static_cast<int>(geo.rows_visible) * geo.slot_h;
  const int band_h = grid_bottom - band_y;
  if (band_h < kMinPeekRows) return false;
  const std::size_t peek_row = slot.scroll_row + geo.rows_visible;
  const std::size_t start = peek_row * geo.cols;
  if (start >= slot.entries.size()) return false;
  const int cell_h = std::min(band_h, geo.cover_h);
  std::size_t offset = 0;
  for (std::size_t i = start; i < slot.entries.size() && offset < geo.cols; ++i, ++offset) {
    const int x = gx + 2 + static_cast<int>(offset) * geo.tier.slot_w;
    if (x + geo.tier.cover_w > gx + grid_w) break;
    buf.fill(Rect{x, band_y, geo.tier.cover_w, cell_h}, theme::surface);
  }
  return true;
}

// Load-more footer (discover.rs draw_grid_tail), only when the peek band is
// absent: "loading more…" while fetching, "all entries loaded" when exhausted
// and the last row is visible.
void draw_grid_tail(const App& app, CellBuffer& buf, int gx, int gy, int grid_bottom,
                    const AxisSlot& slot, const GridGeo& geo) {
  const int band_y = gy + static_cast<int>(geo.rows_visible) * geo.slot_h;
  if (grid_bottom <= band_y) return;
  const std::size_t last_row =
      (slot.entries.empty() ? 0 : slot.entries.size() - 1) / geo.cols;
  const bool last_visible = last_row < slot.scroll_row + geo.rows_visible;
  if (slot.loading) {
    const char* spin = kSpinnerFrames[app.spinner_phase];
    buf.put_str(gx + 2, band_y, std::string(spin) + " loading more\xE2\x80\xA6", theme::fg2,
                theme::bg, Style::Italic);
  } else if (slot.exhausted && last_visible) {
    buf.put_str(gx + 2, band_y, "all entries loaded", theme::fg3, theme::bg);
  }
}

}  // namespace

void draw_discover(const App& app, CellBuffer& buf, int y0, int y1) {
  const int w = buf.width();
  if (y1 <= y0) return;
  const DiscoverState& d = app.discover;

  // Axis bar (1 row) + a spacer row; the grid starts 2 rows down.
  draw_axis_bar(buf, w, y0, d.axis(), d.filters());
  const int gy = y0 + 2;
  if (gy >= y1) return;

  const AxisSlot& slot = d.slot();
  if (slot.entries.empty()) {
    draw_empty_grid(app, buf, 0, w, gy, y1, slot);
    return;
  }

  // Grid geometry from the raw canvas width (full-canvas view — DESIGN §3.3)
  // and the content band. `cell` is the terminal's per-cell pixel geometry.
  const int content_h = y1 - y0;
  const GridGeo geo = grid_geo(w, content_h, app.cover_caps.cell);
  const int grid_bottom = y1;
  const Cour cour = app.discover_cour;

  std::vector<Rect> exclusions;
  for (std::size_t i = 0; i < slot.entries.size(); ++i) {
    // card_cover_rect (discover.hpp) is the single geometry source this loop
    // and compose_grid_apc's placement diff both call (P17b, R3) — an empty
    // rect means scrolled-out-of-view or clipped, skip the card whole.
    const Rect r = card_cover_rect(geo, i, slot.scroll_row, /*grid_x=*/2, gy, w,
                                   grid_bottom);
    if (r.empty()) continue;
    if (const std::optional<Rect> excl = draw_card(app, buf, slot.entries[i], i,
                                                    i == slot.cursor, d.axis(), cour,
                                                    r.x, r.y, r.w, r.h);
        excl.has_value()) {
      exclusions.push_back(*excl);
    }
  }
  buf.set_exclusions(std::move(exclusions));

  // The load-more footer yields to the peek band (DESIGN §3.8); when there is
  // nothing to peek, the footer takes the leftover band.
  if (!draw_peek_band(buf, 0, gy, w, grid_bottom, slot, geo)) {
    draw_grid_tail(app, buf, 0, gy, grid_bottom, slot, geo);
  }
}

// --- Settings (P18, DESIGN §5.5) -------------------------------------------

namespace {

constexpr int kSettingsLabelX = 4;
constexpr int kSettingsValueX = 34;

std::vector<std::string> registry_provider_names(const App& app) {
  std::vector<std::string> names;
  if (app.deps != nullptr && app.deps->registry != nullptr) {
    for (std::size_t i = 0; i < app.deps->registry->size(); ++i) {
      names.emplace_back(app.deps->registry->at(i)->name());
    }
  }
  return names;
}

void draw_settings_row(const App& app, CellBuffer& buf, int y, int w, std::size_t ix) {
  const SettingsRow& row = kSettingsRows[ix];
  const bool focused = app.settings.cursor() == ix;
  const bool editing = focused && app.settings.editing();

  if (focused) {
    buf.fill(Rect{0, y, w, 1}, editing ? theme::elevated : theme::surface);
  }
  const Rgb bg = editing ? theme::elevated : (focused ? theme::surface : theme::bg);

  if (focused) {
    buf.put_str(2, y, "\xE2\x96\xB8", editing ? theme::hot : theme::focus, bg);  // ▸
  }
  const Rgb label_color = focused ? theme::focus : theme::fg2;
  buf.put_str(kSettingsLabelX, y, row.label, label_color, bg,
             focused ? Style::Bold : Style::None);

  if (editing) {
    const std::string text = app.settings.edit_buffer() + "\xE2\x96\x88";  // trailing █ cursor.
    buf.put_str(kSettingsValueX, y, text, theme::fg, bg);
    return;
  }

  const std::vector<std::string> providers = registry_provider_names(app);
  if (row.kind == SettingsRowKind::Toggle) {
    const bool on = settings_value(app.config, row.id, providers) == "on";
    if (on) {
      buf.put_str(kSettingsValueX, y, "[\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88 on \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88]",
                 theme::focus, bg);
    } else {
      buf.put_str(kSettingsValueX, y,
                 "[\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88 off \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88]",
                 theme::fg3, bg);
    }
  } else {
    const std::string text = settings_value(app.config, row.id, providers);
    buf.put_str(kSettingsValueX, y, text, focused ? theme::fg : theme::fg2, bg);
  }

  // Hint column, right-anchored (ASCII-only hints, byte len == display width).
  const int hint_w = str_width(row.hint);
  const int x = w - 2 - hint_w;
  if (x > kSettingsValueX) {
    buf.put_str(x, y, row.hint, theme::fg3, bg);
  }
}

}  // namespace

void draw_settings(const App& app, CellBuffer& buf, int y0, int y1) {
  const int w = buf.width();
  if (y1 <= y0 || w <= 0) return;

  const SettingsEnv env{app.settings_covers_dir, app.settings_account, app.settings_version,
                        app.settings_mal_account};
  const std::vector<SettingsLine> lines = settings_layout(env);

  std::size_t cursor_line = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind == SettingsLine::Kind::Row && lines[i].row == app.settings.cursor()) {
      cursor_line = i;
      break;
    }
  }
  // Stateless scroll: keep the focused row on screen, top-anchored otherwise
  // (settings.rs draw's own rule).
  const std::size_t visible = static_cast<std::size_t>(std::max(0, y1 - y0));
  const std::size_t scroll =
      (cursor_line + 1 > visible) ? (cursor_line + 1 - visible) : 0;

  int y = y0;
  for (std::size_t i = scroll; i < lines.size() && y < y1; ++i, ++y) {
    const SettingsLine& li = lines[i];
    switch (li.kind) {
      case SettingsLine::Kind::Blank:
        break;
      case SettingsLine::Kind::Header:
        buf.put_str(kSettingsLabelX - 2, y, li.header, theme::fg, theme::bg, Style::Bold);
        break;
      case SettingsLine::Kind::Rule: {
        const int rule_w = std::max(0, w - 4);
        for (int cx = 0; cx < rule_w; ++cx) {
          buf.put_str(kSettingsLabelX - 2 + cx, y, "\xE2\x94\x80", theme::chrome, theme::bg);
        }
        break;
      }
      case SettingsLine::Kind::Inert:
        buf.put_str(kSettingsLabelX, y, li.inert_label, theme::fg3, theme::bg, Style::Italic);
        buf.put_str(kSettingsValueX, y, li.inert_value, theme::fg3, theme::bg, Style::Italic);
        break;
      case SettingsLine::Kind::Row:
        draw_settings_row(app, buf, y, w, li.row);
        break;
    }
  }
}

// ===========================================================================
// P32 mouse hit-testing (§9). Each helper walks the same geometry source its
// draw function walks (see views.hpp) — the layout logic is shared, only the
// question differs ("what goes at (x,y)?" vs "what is at (x,y)?").
// ===========================================================================

namespace detail {

std::optional<int> browse_row_at(const App& app, int px, int py) {
  if (app.view != View::Browse) return std::nullopt;
  const int w = app.win.cols;
  const bool two_pane = w >= kPaneSplitMin;
  const PaneSplit split = pane_split(w);
  const int list_w = two_pane ? split.list_w : (w - 3);
  if (px < 2 || px >= 2 + list_w) return std::nullopt;
  const int y0 = kContentY0;
  const int y1 = static_cast<int>(app.win.rows) - 1;
  if (py < y0 || py >= y1) return std::nullopt;
  const std::size_t idx =
      static_cast<std::size_t>(std::max(0, app.browse_scroll)) +
      static_cast<std::size_t>(py - y0);
  if (idx >= app.catalog.size()) return std::nullopt;
  return static_cast<int>(idx);
}

std::optional<std::size_t> history_ord_at(const App& app, int px, int py) {
  if (app.view != View::History) return std::nullopt;
  const HistoryState& hs = app.history;
  if (hs.empty()) return std::nullopt;
  const int w = app.win.cols;
  const bool two_pane = w >= kPaneSplitMin && hs.selected() != nullptr;
  const PaneSplit split = pane_split(w);
  const int list_w = two_pane ? split.list_w : (w - 3);
  if (px < 2 || px >= 2 + list_w) return std::nullopt;
  const int y0 = kContentY0;
  const int y1 = static_cast<int>(app.win.rows) - 1;
  if (py < y0 || py >= y1) return std::nullopt;
  const std::vector<HistoryLine> lines = hs.layout();
  const std::size_t idx = hs.scroll + static_cast<std::size_t>(py - y0);
  if (idx >= lines.size()) return std::nullopt;
  const HistoryLine& li = lines[idx];
  if (li.kind != HistoryLine::Kind::Title && li.kind != HistoryLine::Kind::Bar) {
    return std::nullopt;
  }
  return li.ord;
}

bool detail_column_contains(const App& app, int px, int py) {
  const int y0 = kContentY0;
  const int y1 = static_cast<int>(app.win.rows) - 1;
  if (py < y0 || py >= y1) return false;
  if (app.view == View::Detail) return true;
  if (app.view != View::Browse && app.view != View::History) return false;
  if (app.win.cols < kPaneSplitMin) return false;
  if (app.view == View::History && app.history.selected() == nullptr) return false;
  if (selected_enrichment(app) == nullptr) return false;  // column not rendered.
  return px >= pane_split(app.win.cols).detail_x;
}

std::size_t episode_grid_per_row(const App& app) {
  // Same x0/cw branch as episode_cell_at (never re-derive the column width);
  // only the width matters here, so the header-height replay is skipped.
  const int w = app.win.cols;
  int cw = 0;
  if (app.view == View::Detail) {
    cw = w - 4;
  } else if ((app.view == View::Browse || app.view == View::History) &&
             w >= kPaneSplitMin) {
    cw = pane_split(w).detail_w;
  } else {
    return 1;
  }
  const int cell_w = 5;
  return static_cast<std::size_t>(std::max(1, cw / cell_w));
}

std::optional<std::size_t> episode_cell_at(const App& app, int px, int py) {
  // Grid shows only for the selected show's own fetch (draw_detail_column's
  // stale-grid gate) with cells to hit (draw_episode_grid's early-outs, incl.
  // the !deps blank of a bare unit-test draw).
  const Enrichment* show = selected_enrichment(app);
  if (show == nullptr || app.deps == nullptr) return std::nullopt;
  if (app.episode.for_id != show->anilist_id) return std::nullopt;
  const EpisodeState& es = app.episode;
  if (es.loading || !es.fetched || es.episodes.empty()) return std::nullopt;

  // The column the grid renders in (draw_browse / draw_history two-pane /
  // draw_detail_zoom's call into draw_detail_column).
  const int w = app.win.cols;
  const int y0 = kContentY0;
  const int y1 = static_cast<int>(app.win.rows) - 1;
  int x0 = 0;
  int cw = 0;
  int col_y1 = y1;  // the column's exclusive bottom, pre-playback-row.
  if (app.view == View::Detail) {
    x0 = 2;
    cw = w - 4;
    // The `c` section claims the bottom of the zoom (draw_detail_zoom's
    // split): the grid is clipped above it, and a click in the section must
    // not hit-test as a phantom episode cell (P36 review). Replay the same
    // section_h subtraction the draw does.
    if (app.char_recs.visible) {
      const int section_h = char_recs_section_height(y1 - y0);
      if (section_h > 0) col_y1 = y1 - section_h - 1;
    }
  } else if ((app.view == View::Browse || app.view == View::History) &&
             w >= kPaneSplitMin) {
    if (app.view == View::History && app.history.selected() == nullptr) {
      return std::nullopt;
    }
    const PaneSplit split = pane_split(w);
    x0 = split.detail_x;
    cw = split.detail_w;
  } else {
    return std::nullopt;
  }

  // Grid origin: cover block, then the content-dependent header. Replay
  // draw_detail_header into a scratch buffer — it is the single source of the
  // header height, and it only writes cells (pure over App).
  const Rect crect = cover_rect(app);
  const int y_head = crect.empty() ? y0 : (crect.y + crect.h + 1);
  CellBuffer scratch(app.win.cols, app.win.rows);
  int y_grid = draw_detail_header(scratch, x0, cw, y_head, *show,
                                  parse_title_language(app.config.title_language));
  const int grid_y1 = col_y1 - 1;  // draw_detail_column reserves the playback row.
  if (!provider_caption(es).empty() && y_grid < grid_y1) ++y_grid;

  // The cell walk — the same stepping loop draw_episode_grid runs.
  const int cell_w = 5;
  int x = x0;
  int y = y_grid;
  for (std::size_t i = 0; i < es.episodes.size(); ++i) {
    if (y >= grid_y1) break;
    if (py == y && px >= x && px < x + cell_w) return i;
    x += cell_w;
    if (x + cell_w > x0 + cw) {
      x = x0;
      ++y;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> discover_card_at(const App& app, int px, int py) {
  if (app.view != View::Discover) return std::nullopt;
  const int w = app.win.cols;
  const int y0 = kContentY0;
  const int y1 = static_cast<int>(app.win.rows) - 1;
  const int gy = y0 + 2;  // axis bar + spacer (draw_discover).
  if (gy >= y1) return std::nullopt;
  const AxisSlot& slot = app.discover.slot();
  if (slot.entries.empty()) return std::nullopt;
  const GridGeo geo = grid_geo(w, y1 - y0, app.cover_caps.cell);
  for (std::size_t i = 0; i < slot.entries.size(); ++i) {
    const Rect r = card_cover_rect(geo, i, slot.scroll_row, /*grid_x=*/2, gy, w, y1);
    if (r.empty()) continue;  // scrolled out / clipped: not on screen.
    // The clickable card: the cover cell plus its three caption rows
    // (rank/title/format — draw_card's block), clipped to the grid bottom.
    const int card_h = std::min(r.h + 3, y1 - r.y);
    if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + card_h) return i;
  }
  return std::nullopt;
}

std::optional<std::size_t> discover_axis_at(int px, int py) {
  if (py != kContentY0) return std::nullopt;  // the axis bar row (draw_discover's y0).
  int x = 2;
  for (std::size_t i = 0; i < kDiscoverAxes.size(); ++i) {
    if (i > 0) x += 3;  // " · " separator.
    // "[N] Label" — the same segments draw_axis_bar emits.
    const std::string tab =
        "[" + std::to_string(i + 1) + "] " + axis_label(kDiscoverAxes[i]);
    const int tw = str_width(tab);
    if (px >= x && px < x + tw) return i;
    x += tw;
  }
  return std::nullopt;
}

std::optional<std::size_t> settings_row_at(const App& app, int /*px*/, int py) {
  if (app.view != View::Settings) return std::nullopt;
  const int y0 = kContentY0;
  const int y1 = static_cast<int>(app.win.rows) - 1;
  if (py < y0 || py >= y1) return std::nullopt;
  const SettingsEnv env{app.settings_covers_dir, app.settings_account, app.settings_version,
                        app.settings_mal_account};
  const std::vector<SettingsLine> lines = settings_layout(env);
  // The same stateless scroll draw_settings computes: keep the focused row on
  // screen, top-anchored otherwise.
  std::size_t cursor_line = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind == SettingsLine::Kind::Row &&
        lines[i].row == app.settings.cursor()) {
      cursor_line = i;
      break;
    }
  }
  const std::size_t visible = static_cast<std::size_t>(std::max(0, y1 - y0));
  const std::size_t scroll =
      (cursor_line + 1 > visible) ? (cursor_line + 1 - visible) : 0;
  const std::size_t idx = scroll + static_cast<std::size_t>(py - y0);
  if (idx >= lines.size()) return std::nullopt;
  if (lines[idx].kind != SettingsLine::Kind::Row) return std::nullopt;
  return lines[idx].row;
}

}  // namespace detail

}  // namespace shigoku::tui
