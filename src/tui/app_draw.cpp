// Cover escape-sequence composition and the screen chrome: draw() and its bars.

#include <map>
#include <string>

#include "../debug_log.hpp"
#include "app.hpp"
#include "iterm.hpp"
#include "kitty.hpp"
#include "sixel.hpp"
#include "character_page.hpp"
#include "views.hpp"

namespace shigoku::tui {

namespace detail {

// --- P8 cover helpers (DESIGN §3.3, A4) ------------------------------------
//
// cover_target()/cover_rect() are PURE geometry over App + cover_caps and live
// in views.cpp (the render target) so views.cpp's own references resolve within
// shigoku_tui_render — app.hpp declares them in detail::. compose_cover_apc()
// stays here: it builds APC bytes via kitty:: (render target) and is only ever
// called from run() (shigoku_tui).

// One cover transmit, whichever protocol the probe selected — the single
// place the three emitters are chosen between, so no call site can drift.
// The switch is deliberate (A2): a fourth backend fails to compile here until
// it is handled. Note what each protocol needs to size the image: kitty takes
// a cell box plus the source pixel dims, OSC 1337 takes a cell box, and SIXEL
// alone is drawn at literal pixel size — hence the per-cell geometry, which
// backend_from guarantees is known whenever it picked Sixel.
std::string cover_transmit(const App& app, const CoverPixels& px,
                           std::uint32_t id, const Rect& r) {
  switch (app.cover_caps.backend) {
    case CoverBackend::Kitty: {
      kitty::Image img;
      img.rgba = px.rgba;
      img.w = px.w;
      img.h = px.h;
      return kitty::transmit(img, id, r.w, r.h);
    }
    case CoverBackend::Iterm:
      return iterm::transmit(px.rgba, px.w, px.h, r.w, r.h);
    case CoverBackend::Sixel:
      return sixel::transmit(px.rgba, px.w, px.h, r.w, r.h,
                             app.cover_caps.cell.width,
                             app.cover_caps.cell.height);
    case CoverBackend::None:
      break;  // placeholder mode; every caller gates on images() first.
  }
  return {};
}

bool purge_placements(CoverBackend backend, PlacedCover& placed_cover,
                      std::map<std::string, Placed>& placed_grid,
                      std::string& purge) {
  purge.clear();
  bool held = false;
  if (placed_cover.id != 0) {
    if (backend == CoverBackend::Kitty)
      purge += kitty::delete_image(placed_cover.id);
    placed_cover = PlacedCover{};
    held = true;
  }
  for (const auto& [url, p] : placed_grid) {
    if (backend == CoverBackend::Kitty) purge += kitty::delete_image(p.id);
    held = true;
  }
  placed_grid.clear();
  return held;
}

bool compose_cover_apc(const App& app, PlacedCover& placed,
                       std::string& pre_diff, std::string& post_diff) {
  pre_diff.clear();
  post_diff.clear();
  const CoverBackend backend = app.cover_caps.backend;
  if (!app.cover_caps.images()) {
    // Placeholder mode: nothing is transmitted. A cover placed before the
    // cover-art setting went off is still on the terminal: kitty gets its
    // delete, and under the other backends the returned true forces the
    // repaint that overwrites it.
    if (placed.id != 0) {
      if (backend == CoverBackend::Kitty) pre_diff += kitty::delete_image(placed.id);
      placed = PlacedCover{};
      return true;
    }
    return false;
  }

  const Rect r = cover_rect(app);
  // Transmit ONLY when the held pixels belong to the currently selected show —
  // the same predicate draw() uses to mark the rect image-owned, so run() and
  // draw() never diverge (a stale cover from a browsed-away show is deleted,
  // not painted over its placeholder). cover_target is the single source of the
  // "which show is selected" truth.
  const std::optional<CoverTarget> target = cover_target(app);
  const bool want =
      app.cover_render.have_pixels && !r.empty() &&
      app.cover_render.pixels.w > 0 && app.cover_render.pixels.h > 0 &&
      target.has_value() && target->id == app.cover_render.for_id;

  if (!want) {
    // No incoming cover: delete whatever the terminal still holds. Kitty gets
    // the delete APC; under iterm no delete exists — the returned true forces
    // the repaint that overwrites the vacated cells, which is how an OSC 1337
    // image dies.
    if (placed.id != 0) {
      if (backend == CoverBackend::Kitty)
        pre_diff += kitty::delete_image(placed.id);
      placed = PlacedCover{};
      return true;
    }
    return false;
  }

  // Steady state: the terminal already holds THIS show's pixels at THIS rect —
  // emit nothing. (Same-show pixel replacement always passes through a !want
  // frame first: begin_fetch clears have_pixels, so the placement is deleted
  // before fresh pixels can land.) Without this short-circuit the full ~180KB
  // transmit went out EVERY frame and flooded slow terminals.
  if (placed.id != 0 && placed.for_id == target->id && placed.rect == r) {
    return false;
  }

  // A4 composition: delete the outgoing placement FIRST (prevents one frame of
  // old+new overlap under contour's fragment model), then transmit the incoming
  // one AFTER the diff (post_diff), CUP'd to the cover origin, C=1 so the image
  // path never moves the cursor the CellBuffer owns. Under iterm there is no
  // delete: the forced repaint overwrites the old rect's cells, and the cursor
  // the OSC moves is hidden and re-CUP'd by the next diff anyway.
  if (placed.id != 0 && backend == CoverBackend::Kitty) {
    pre_diff += kitty::delete_image(placed.id);
  }
  // CUP to the cover origin cell (1-based), then transmit.
  post_diff += "\x1b[";
  post_diff += std::to_string(r.y + 1);
  post_diff += ';';
  post_diff += std::to_string(r.x + 1);
  post_diff += 'H';
  post_diff += cover_transmit(app, app.cover_render.pixels, kDetailCoverImageId, r);
  debug_log("cover transmit: detail id=" + std::to_string(kDetailCoverImageId) +
            " px=" + std::to_string(app.cover_render.pixels.w) + "x" +
            std::to_string(app.cover_render.pixels.h) +
            " cells=" + std::to_string(r.w) + "x" + std::to_string(r.h) +
            " bytes=" + std::to_string(post_diff.size()));
  placed = PlacedCover{kDetailCoverImageId, target->id, r};
  return true;
}

// Generalizes compose_cover_apc for the P17 grid (§6, R1/R3). run() owns
// `placed` (url -> Placed) across frames; draw() stays pure. Per frame, when
// view == Discover && cover_caps.images(): the DESIRED set is exactly the urls
// draw_discover marked as exclusions this frame (same card_cover_rect call,
// same Ready-in-the-render-store predicate — R3, they must never disagree).
// pre_diff deletes every placed url that is gone or moved; post_diff CUP+
// transmits every desired url not already placed at that rect. Returns
// whether ANY placement changed (caller forces mark_all_dirty, same reasoning
// as the single-cover rule: the exclusion made prev skip those cells).
bool compose_grid_apc(const App& app, std::map<std::string, Placed>& placed,
                      std::string& pre_diff, std::string& post_diff) {
  pre_diff.clear();
  post_diff.clear();
  bool changed = false;

  const CoverBackend backend = app.cover_caps.backend;
  std::map<std::string, Rect> desired;
  if (app.view == View::Discover && app.cover_caps.images()) {
    const AxisSlot& slot = app.discover.slot();
    const int w = app.win.cols;
    const int y0 = kContentY0, y1 = static_cast<int>(app.win.rows) - 1;
    if (y1 > y0) {
      const int gy = y0 + 2;
      if (gy < y1) {
        const int content_h = y1 - y0;
        const GridGeo geo = grid_geo(w, content_h, app.cover_caps.cell);
        for (std::size_t i = 0; i < slot.entries.size(); ++i) {
          const Rect r = card_cover_rect(geo, i, slot.scroll_row, /*grid_x=*/2, gy, w, y1);
          if (r.empty()) continue;
          const Enrichment& e = slot.entries[i].meta;
          if (!e.cover_url.has_value()) continue;
          if (app.cover_render_store.get(*e.cover_url) == nullptr) continue;
          desired[*e.cover_url] = r;  // last-wins is fine: a url appears once per slot.
        }
      }
    }
  }

  // pre_diff: delete every placed url that's gone from `desired` OR moved
  // (kitty only — under iterm the forced repaint overwrites the vacated
  // cells, which is the erase).
  for (auto it = placed.begin(); it != placed.end();) {
    const auto want = desired.find(it->first);
    if (want == desired.end() || !(want->second == it->second.rect)) {
      if (backend == CoverBackend::Kitty)
        pre_diff += kitty::delete_image(it->second.id);
      changed = true;
      it = placed.erase(it);
    } else {
      ++it;
    }
  }

  // post_diff: transmit every desired url not already placed at that rect.
  for (const auto& [url, rect] : desired) {
    if (placed.find(url) != placed.end()) continue;  // unchanged, still placed.
    const CoverRenderEntry* entry = app.cover_render_store.get(url);
    if (entry == nullptr) continue;  // adopted-then-evicted race; skip this frame.
    post_diff += "\x1b[";
    post_diff += std::to_string(rect.y + 1);
    post_diff += ';';
    post_diff += std::to_string(rect.x + 1);
    post_diff += 'H';
    post_diff += cover_transmit(app, entry->pixels, entry->id, rect);
    debug_log("cover transmit: grid id=" + std::to_string(entry->id) + " px=" +
              std::to_string(entry->pixels.w) + "x" +
              std::to_string(entry->pixels.h) + " url=" + url);
    placed[url] = Placed{entry->id, rect};
    changed = true;
  }

  return changed;
}


}  // namespace detail

// ===========================================================================
// draw(App) — PURE. Chrome + empty Browse + spinner + toasts, or degraded.
// ===========================================================================

namespace {

const char* view_label(View v) {
  switch (v) {
    case View::Browse: return "Browse";
    case View::History: return "History";
    case View::Discover: return "Discover";
    case View::Settings: return "Settings";
    case View::Schedule: return "Calendar";  // P37; label starts with its key letter, 'C'.
    case View::Detail: return "Detail";  // never in the tab strip (views[]).
  }
  return "";  // unreachable.
}

char view_key(View v) {
  switch (v) {
    case View::Browse: return 'B';
    case View::History: return 'H';
    case View::Discover: return 'D';
    case View::Settings: return 'S';
    case View::Schedule: return 'C';
    case View::Detail: return '?';  // never in the tab strip (views[]).
  }
  return '?';  // unreachable.
}

// Top bar (DESIGN §3.4): "SABIGOKU ░ [B]rowse · [H]istory · …  <chip>" plus the
// right-aligned focus `·`. shigoku renders its own name.
void draw_top_bar(const App& app, CellBuffer& buf) {
  const int w = buf.width();
  buf.fill(Rect{0, 0, w, 1}, theme::bg);
  int x = 1;  // 1-cell left padding within the bar.
  x = buf.put_str(x, 0, "SHIGOKU", theme::fg, theme::bg, Style::Bold);
  x = buf.put_str(x + 1, 0, "\xE2\x96\x91", theme::chrome, theme::bg);  // ░
  x += 1;

  const View views[] = {View::Browse, View::History, View::Discover,
                        View::Schedule, View::Settings};
  bool first = true;
  for (View v : views) {
    if (!first) x = buf.put_str(x, 0, " \xC2\xB7 ", theme::fg3, theme::bg);  // ·
    first = false;
    const bool active = (v == app.view);
    const Rgb col = active ? theme::focus : theme::fg2;
    const Style st = active ? Style::Bold : Style::None;
    // "[B]rowse": the bracket wraps the key letter, the label's remaining
    // letters follow (DESIGN §3.4). view_label starts with the key letter.
    std::string tab = "[";
    tab.push_back(view_key(v));
    tab += "]";
    tab += (view_label(v) + 1);  // label without its leading (key) letter.
    x = buf.put_str(x, 0, tab, col, theme::bg, st);
  }

  // Season chip (05 §7 topBarSeasonChip, app.rs top_bar): the selected card's
  // own season/year; Browse/History fall back to the ambient cour, Discover
  // and the zoom stay absent-not-empty, Settings shows none. Drawn only when
  // it clears the fixed right-edge dot. discover_cour is run()'s boot stamp
  // (draw is pure, no clock; the cour moves once a season).
  if (app.view != View::Settings) {
    const bool kanji = app.config.kanji_chips;
    const Enrichment* sel = detail::selected_enrichment(app);
    std::optional<std::string> chip =
        sel != nullptr ? season_chip(sel->season, sel->year, kanji) : std::nullopt;
    if (!chip.has_value() && (app.view == View::Browse || app.view == View::History)) {
      chip = cour_chip(app.discover_cour, kanji);
    }
    if (chip.has_value() && x + 2 + str_width(*chip) <= w - 2) {
      x = buf.put_str(x + 2, 0, *chip, theme::fg2, theme::bg);
    }
  }

  // Catalog chip: "MAL" while the browse calls are served by MyAnimeList
  // (mode mal, or auto with AniList latched off). Right-aligned before the
  // focus dot; dropped when it would touch what is already drawn.
  if (app.deps != nullptr && app.deps->catalog_badge) {
    const std::string badge = app.deps->catalog_badge();
    if (!badge.empty()) {
      const int bx = w - 3 - str_width(badge);
      if (bx > x + 1) buf.put_str(bx, 0, badge, theme::warn, theme::bg, Style::Bold);
    }
  }

  // Right-aligned active-pane focus indicator (·) in focus color.
  buf.put_str(w - 2, 0, "\xC2\xB7", theme::focus, theme::bg);
}

// Bottom bar (DESIGN §3.5 / §4.9 / §7.5): magenta ▌ + idle help, or the `/`
// search prompt + live query + result count.
void draw_bottom_bar(const App& app, CellBuffer& buf) {
  const int w = buf.width();
  const int y = buf.height() - 1;
  buf.fill(Rect{0, y, w, 1}, theme::bg);

  if (app.confirm_delete.has_value()) {
    // Hard-delete confirm prompt (DESIGN §6.5/§4.2): "[!]" replaces the idle
    // "▌", title tail-truncated so "y confirm · esc cancel" never scrolls off.
    int x = 2;
    x = buf.put_str(x, y, "[!] delete \"", theme::warn, theme::bg, Style::Bold);
    const std::string title = [&]() -> std::string {
      const Show* s = app.history.selected();
      if (s == nullptr) return {};
      const Enrichment& e = s->enrichment;
      return std::string(preferred_title(
          e.title_romaji,
          e.title_english ? std::optional<std::string_view>(*e.title_english) : std::nullopt,
          e.title_native ? std::optional<std::string_view>(*e.title_native) : std::nullopt,
          parse_title_language(app.config.title_language)));
    }();
    x = buf.put_str(x, y, truncate_to_cols(title, 48), theme::warn, theme::bg, Style::Bold);
    x = buf.put_str(x, y, "\"? episode history gone \xC2\xB7 ", theme::fg3, theme::bg);
    x = buf.put_str(x, y, "y", theme::focus, theme::bg, Style::Bold);
    x = buf.put_str(x, y, " confirm \xC2\xB7 ", theme::fg3, theme::bg);
    x = buf.put_str(x, y, "esc", theme::focus, theme::bg, Style::Bold);
    buf.put_str(x, y, " cancel", theme::fg3, theme::bg);
    return;
  }

  if (app.bar == BarMode::Search) {
    // "  /  query_                              [n results]"
    const bool in_history = app.view == View::History;
    const std::string& query = in_history ? app.history.filter : app.search_query;
    int x = 2;
    x = buf.put_str(x, y, "/", theme::focus, theme::bg, Style::Bold);
    x += 2;
    x = buf.put_str(x, y, query, theme::fg, theme::bg, Style::Bold);
    x = buf.put_str(x, y, "_", theme::focus, theme::bg);
    if (in_history) {
      const std::string count = "[" + std::to_string(app.history.count()) + "]";
      buf.put_str(w - 1 - str_width(count), y, count, theme::fg2, theme::bg);
    }
    return;
  }

  if (app.bar == BarMode::Score) {
    // "  s  7.5_                      0-10, decimals · enter save · esc cancel"
    int x = 2;
    x = buf.put_str(x, y, "s", theme::focus, theme::bg, Style::Bold);
    x += 2;
    x = buf.put_str(x, y, app.score_input, theme::fg, theme::bg, Style::Bold);
    x = buf.put_str(x, y, "_", theme::focus, theme::bg);
    const std::string hint = "0-10, decimals \xC2\xB7 enter save \xC2\xB7 esc cancel";
    buf.put_str(w - 1 - str_width(hint), y, hint, theme::fg3, theme::bg);
    return;
  }

  // Idle help line: ▌ (hot) + spaced help text; keybinds fg2+bold, words fg3.
  int x = 2;
  x = buf.put_str(x, y, "\xE2\x96\x8C", theme::hot, theme::bg);  // ▌
  x += 2;
  if (app.any_loading) {
    // Bottom-bar spinner prefix during any in-flight async (DESIGN §4.8).
    // "Slow" is measured from when THIS loading burst started, not app
    // uptime (loading_since_tick is stamped on the false->true edge).
    const Rgb sp = (app.tick_count - app.loading_since_tick) > kSlowSpinnerTicks
                       ? theme::hot
                       : theme::focus;
    x = buf.put_str(x, y, kSpinnerFrames[app.spinner_phase], sp, theme::bg);
    x += 1;
  }
  // Plain help text (segment styling is a P7 nicety; P6 renders it readable).
  // Settings edit mode (P18, DESIGN §7.5) swaps to its own two-state line —
  // idle_help(View, Pane) has no editing flag, so it's special-cased here
  // rather than growing that helper's signature for one view.
  // A show page whose source walk dead-ended has nothing to play: the line
  // must not advertise "enter play" over an empty grid (the one hardware
  // report of "I don't see how to play this").
  const bool detail_surface = app.view == View::Detail || app.pane == Pane::Detail;
  const bool dead_end = detail_surface && app.episode.fetched && app.episode.no_source &&
                        app.episode.episodes.empty();
  // The zoom's cast list and the character page behind it have keys of
  // their own; name them while they hold the keyboard, or Enter on a
  // character stays undiscovered.
  const bool character_page = app.view == View::Detail && app.character.open;
  const bool cast_shown = app.view == View::Detail && !character_page && app.char_recs.visible;
  std::string help;
  if (app.view == View::Settings && app.settings.editing()) {
    help = "type to edit \xC2\xB7 enter confirm \xC2\xB7 esc cancel";
  } else if (character_page) {
    help = detail::has_spoilers(app.character.data)
               ? "jk scroll \xC2\xB7 enter open show \xC2\xB7 s spoilers \xC2\xB7 space/esc back"
               : "jk scroll \xC2\xB7 enter open show \xC2\xB7 space/esc back";
  } else if (cast_shown && app.char_recs.focused) {
    help = "jk move \xC2\xB7 enter open \xC2\xB7 tab grid \xC2\xB7 c hide \xC2\xB7 space/esc back";
  } else if (dead_end) {
    help = "no source has this show \xC2\xB7 v try a source by hand \xC2\xB7 "
           "P save for later \xC2\xB7 space/esc back";
  } else if (cast_shown) {
    help = "hjkl grid \xC2\xB7 enter play \xC2\xB7 tab cast list \xC2\xB7 c hide \xC2\xB7 "
           "~ similar \xC2\xB7 d download \xC2\xB7 space/esc back";
  } else {
    help = detail::idle_help(app.view, app.pane);
  }
  buf.put_str(x, y, help, theme::fg3, theme::bg);
}

// Toasts (DESIGN §4.7): right-aligned, stacked upward from row height-2.
void draw_toasts(const App& app, CellBuffer& buf) {
  const std::vector<Toast> vis = app.toasts.visible();
  if (vis.empty()) return;
  const int w = buf.width();
  int row = buf.height() - 2;  // one above the bottom bar.
  // Render newest nearest the bar: vis is oldest-first, so iterate reversed.
  for (auto it = vis.rbegin(); it != vis.rend() && row >= 1; ++it, --row) {
    const Toast& t = *it;
    const std::string glyph(toast_glyph(t.kind));
    const std::string copy =
        truncate_to_cols(t.text, kToastMaxCopyCols);
    const std::string box = glyph + copy;
    const int bw = str_width(box);
    const int x = (w - 1 - bw > 0) ? (w - 1 - bw) : 0;
    // Fill the box background (elevated) then the text.
    buf.fill(Rect{x, row, bw, 1}, theme::elevated);
    const Style st = toast_bold(t.kind) ? Style::Bold : Style::None;
    buf.put_str(x, row, box, toast_fg(t.kind), theme::elevated, st);
  }
}

// The degraded frame (04 §8): h<4 || w<16 -> a single centered message, never
// an exit.
void draw_degraded(CellBuffer& buf) {
  buf.clear(theme::bg);
  const int w = buf.width();
  const int h = buf.height();
  if (h <= 0 || w <= 0) return;
  // Pick the longest message that fits; "too small" fits the w<16 floor, the
  // full copy shows once there's room (a resize back up re-widens it).
  const char* full = "terminal too small";
  const char* msg = (str_width(full) <= w) ? full : "too small";
  const int mw = str_width(msg);
  const int x = (w - mw) / 2 > 0 ? (w - mw) / 2 : 0;
  const int y = h / 2;
  buf.put_str(x, y, msg, theme::warn, theme::bg);
}

}  // namespace

namespace detail {

std::optional<View> top_bar_tab_at(int x) {
  // The same span walk draw_top_bar runs — name, glyph, spacing, then the
  // four tabs separated by " · " — built from the same view_label/view_key
  // sources, so a renamed tab moves the render and the hit-test together.
  // draw: x=1 + "SHIGOKU" → 8; ░ put at x+1 (9, 1 col) → 10; x += 1 → 11.
  int cx = 1 + str_width("SHIGOKU") + 3;
  const View views[] = {View::Browse, View::History, View::Discover,
                        View::Schedule, View::Settings};
  bool first = true;
  for (View v : views) {
    if (!first) cx += 3;  // " · " separator.
    first = false;
    std::string tab = "[";
    tab.push_back(view_key(v));
    tab += "]";
    tab += (view_label(v) + 1);
    const int tw = str_width(tab);
    if (x >= cx && x < cx + tw) return v;
    cx += tw;
  }
  return std::nullopt;
}

std::string idle_help(View v, Pane p) {
  // DESIGN §7.5 subset for the views v0 implements: Browse/History (both
  // panes) and the Detail zoom are exact copy; Discover/Settings stay minimal
  // stand-ins (their full two-pane/grid render is not built until later).
  switch (v) {
    case View::Browse:
      // "hjkl grid", not "scroll" (KEYS_AUDIT F7): since P33 hjkl navigates
      // the episode grid on the detail surfaces — j/k hop rows, h/l step.
      if (p == Pane::Detail)
        return "hjkl grid · esc back · enter play · v provider · space zoom · q quit";
      return "hjkl · / find anime · space zoom · ~ similar · P save · q quit";
    case View::History:
      if (p == Pane::Detail)
        return "hjkl grid · esc back · enter play · v provider · space zoom · q quit";
      return "jk move · / filter · f status · l/enter detail · p/x/c/w/P status · s score · X delete · r/u reset/undo · q quit";
    case View::Discover:
      // "P save" (KEYS_AUDIT F8): add-to-watchlist worked here all along but
      // was advertised only on Browse's line.
      return "hjkl · enter open · space zoom · ~ similar · f filter · P save · q quit";
    case View::Settings:
      return "jk move · hl cycle · space toggle · enter edit · q save+quit";
    case View::Schedule:
      return "jk move · enter detail · q quit";
    case View::Detail:
      // §7.5 base copy + the §9 keys that only exist here: `c` characters/
      // recommendations (P36), `d` download (P35). A key the bar never names
      // may as well not exist (the P36 section went unnoticed on hardware).
      return "hjkl grid · enter play · v provider · c cast · ~ similar · d download · space/esc back";
  }
  return "q quit";  // unreachable.
}

}  // namespace detail

void draw(const App& app, CellBuffer& buf) {
  buf.resize(app.win.cols, app.win.rows);

  if (buf.height() < 4 || buf.width() < 16) {
    draw_degraded(buf);
    return;
  }

  buf.clear(theme::bg);
  draw_top_bar(app, buf);

  // Content region (rows kContentY0..height-2, below the top bar + spacer
  // row): Browse/History's two-pane list/detail, the Discover card grid, the
  // Settings row list, or the Detail zoom's full-canvas render (views.cpp,
  // pure).
  const int y0 = kContentY0;
  const int y1 = buf.height() - 1;
  if (app.view == View::Browse) {
    draw_browse(app, buf, y0, y1);
  } else if (app.view == View::History) {
    draw_history(app, buf, y0, y1);
  } else if (app.view == View::Discover) {
    draw_discover(app, buf, y0, y1);
  } else if (app.view == View::Settings) {
    draw_settings(app, buf, y0, y1);
  } else if (app.view == View::Detail) {
    draw_detail_zoom(app, buf, y0, y1);
  } else if (app.view == View::Schedule) {
    draw_schedule(app, buf, y0, y1);
  }

  // Cover placement (P8, A4). The rect is derived from the live geometry so
  // draw() and run() agree. When the backend holds live pixels for the open show, the
  // rect is IMAGE-OWNED: mark the exclusion so the diff never paints into the
  // live fragments (A4 cell-ownership rule), and run() transmits after the diff.
  // Otherwise (placeholder mode, loading, suppressed, or a foreign/empty cover)
  // draw the dim box in-band and DO NOT exclude — normal cells render there.
  //
  // Discover already set its own exclusions (draw_discover, P17b, one rect per
  // placed grid cover) — cover_rect(app) is always empty there (views.cpp: "no
  // detail column in Discover"), so this block must not touch the exclusion set
  // and clobber the grid's.
  if (app.view != View::Discover) {
    const Rect crect = detail::cover_rect(app);
    const std::optional<detail::CoverTarget> ctarget = detail::cover_target(app);
    const bool image_owned =
        app.cover_caps.images() && app.cover_render.have_pixels && !crect.empty() &&
        ctarget.has_value() && ctarget->id == app.cover_render.for_id;
    if (image_owned) {
      buf.set_exclusion(crect);
    } else {
      buf.set_exclusion(Rect{});
      if (!crect.empty()) {
        // Placeholder / loading box (DESIGN §3.3): a surface-fill block, a
        // centered spinner while loading, else a dim frame holding the slot.
        buf.fill(crect, theme::surface);
        const bool loading = app.cover.is_loading();
        if (loading) {
          const char* sp = kSpinnerFrames[app.spinner_phase];
          buf.put_str(crect.x + crect.w / 2, crect.y + crect.h / 2, sp,
                      theme::focus, theme::surface);
        }
      }
    }
  }

  // Demo-only: echo the last key + terminal geometry into a debug line so the
  // drive-tui DoD ("keys echo into a debug line", "resize reflows live") is
  // visibly satisfied. Never rendered in the real app.
  if (app.demo) {
    std::string dbg = "demo · key: ";
    dbg += app.last_key_debug.empty() ? "(none)" : app.last_key_debug;
    dbg += "  ·  ";
    dbg += std::to_string(app.win.cols);
    dbg += "x";
    dbg += std::to_string(app.win.rows);
    buf.put_str(2, 2, dbg, theme::fg2, theme::bg);
  }

  // Connect modal (P19, DESIGN 5.5a): a captured overlay drawn LAST, over
  // whatever content/cover drew above (typically Settings) — never a
  // full-pane fill (draw_connect itself falls back to a bare strip on a
  // cramped pane; it never occupies the whole screen).
  if (app.connect.has_value()) {
    const App::ConnectSession& s = *app.connect;
    const TickCount elapsed_ticks = app.tick_count - s.started;
    ConnectView view{s.url, elapsed_ticks / 10, elapsed_ticks * 100, s.copied};
    draw_connect(view, Rect{0, 0, buf.width(), buf.height()}, buf);
  }

  // Discover filter overlay (P38, §9): same "captured, drawn last" shape as
  // the connect modal above; the two never coincide (connect only opens from
  // Settings).
  if (app.discover_filters.visible) {
    const DiscoverFilterOverlay& ov = app.discover_filters;
    DiscoverFilterView view{&ov.draft,        ov.row_cursor,     ov.genre_cursor,
                            &ov.genres,       ov.genres_loading, ov.genres_failed};
    draw_discover_filters(view, Rect{0, 0, buf.width(), buf.height()}, buf);
  }

  draw_toasts(app, buf);
  draw_bottom_bar(app, buf);
}

}  // namespace shigoku::tui
