// viewer_tests.cpp — golden tests for the PURE viewer core
// (src/view/pager.hpp/.cpp). No SDL, no decode: CLI parse, natural sort +
// page-list build, the key table (incl. the "RTL mirrors spatial keys only"
// rule), fit-rect and zoom geometry, and the exit report line.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "../src/view/pager.hpp"

using namespace shigoku;        // Result<T, E>.
using namespace shigoku::view;

namespace {

// parse_cli takes (argc, char**). Build a mutable argv from literals with a
// leading program name; parse_cli never writes through the pointers.
Result<Options, std::string> parse(std::vector<const char*> args) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("shigoku-view"));
  for (const char* a : args) argv.push_back(const_cast<char*>(a));
  return parse_cli(static_cast<int>(argv.size()), argv.data());
}

std::vector<std::string> basenames(const std::vector<std::string>& paths) {
  std::vector<std::string> out;
  for (const auto& p : paths) out.push_back(std::filesystem::path(p).filename().string());
  return out;
}

}  // namespace

// ===========================================================================
// parse_cli
// ===========================================================================

TEST_CASE("parse_cli_defaults_one_positional") {
  auto r = parse({"dir"});
  REQUIRE(r.has_value());
  CHECK(r->paths == std::vector<std::string>{"dir"});
  CHECK_FALSE(r->rtl);
  CHECK(r->start_page == 1);
  CHECK_FALSE(r->report_file.has_value());
  CHECK(r->fit == Fit::Page);
  CHECK_FALSE(r->title.has_value());
}

TEST_CASE("parse_cli_all_flags") {
  auto r = parse({"--rtl", "--start-page", "7", "--report-file", "/tmp/rep",
                  "--fit", "width", "--title", "Chainsaw Man Ch. 1", "a.jpg", "b.jpg"});
  REQUIRE(r.has_value());
  CHECK(r->rtl);
  CHECK(r->start_page == 7);
  CHECK(r->report_file == std::optional<std::string>("/tmp/rep"));
  CHECK(r->fit == Fit::Width);
  CHECK(r->title == std::optional<std::string>("Chainsaw Man Ch. 1"));
  CHECK(r->paths == std::vector<std::string>{"a.jpg", "b.jpg"});
}

TEST_CASE("parse_cli_help_needs_no_paths") {
  // --help is the one flag that parses without a positional: printing the key
  // list is the whole job.
  for (const char* flag : {"--help", "-h"}) {
    CAPTURE(flag);
    auto r = parse({flag});
    REQUIRE(r.has_value());
    CHECK(r->help);
    CHECK(r->paths.empty());
  }
  auto plain = parse({"dir"});
  REQUIRE(plain.has_value());
  CHECK_FALSE(plain->help);
  // The usage text is where the keys are documented — it must list them.
  CHECK(usage().find("shift+f / F11") != std::string::npos);
  CHECK(usage().find("zoom in / out / reset") != std::string::npos);
}

TEST_CASE("parse_cli_end_of_flags_double_dash") {
  // After --, a leading-dash token is a positional (a file literally named --rtl).
  auto r = parse({"--", "--rtl", "weird.png"});
  REQUIRE(r.has_value());
  CHECK_FALSE(r->rtl);
  CHECK(r->paths == std::vector<std::string>{"--rtl", "weird.png"});
}

TEST_CASE("parse_cli_errors") {
  CHECK_FALSE(parse({}).has_value());                       // no positional.
  CHECK_FALSE(parse({"--start-page"}).has_value());          // missing value.
  CHECK_FALSE(parse({"--start-page", "0", "d"}).has_value()); // must be >= 1.
  CHECK_FALSE(parse({"--start-page", "x", "d"}).has_value()); // not an int.
  CHECK_FALSE(parse({"--start-page", "3x", "d"}).has_value());// trailing junk.
  CHECK_FALSE(parse({"--fit", "cover", "d"}).has_value());    // bad fit value.
  CHECK_FALSE(parse({"--fit"}).has_value());                 // missing value.
  CHECK_FALSE(parse({"--title"}).has_value());               // missing value.
  CHECK_FALSE(parse({"--nope", "d"}).has_value());           // unknown flag.
}

// ===========================================================================
// natural_less
// ===========================================================================

TEST_CASE("natural_less_numeric_aware") {
  CHECK(natural_less("002.jpg", "010.jpg"));       // 2 < 10 despite equal width.
  CHECK_FALSE(natural_less("010.jpg", "002.jpg"));
  CHECK(natural_less("page_2.png", "page_10.png"));  // embedded number.
  CHECK(natural_less("1.png", "2.png"));
  CHECK_FALSE(natural_less("5", "5"));               // irreflexive.
  // Equal magnitude, different zero-padding: shorter run sorts first (stable).
  CHECK(natural_less("7.jpg", "07.jpg"));
  CHECK_FALSE(natural_less("07.jpg", "7.jpg"));
  // Pure alpha falls back to byte order.
  CHECK(natural_less("a.jpg", "b.jpg"));
  // A digit run vs a letter at the same position: '0'..'9' < letters by byte.
  CHECK(natural_less("1.jpg", "a.jpg"));
}

TEST_CASE("natural_less_is_a_strict_weak_order_on_a_small_set") {
  // Sorting with it must be deterministic and not crash on ties/prefixes.
  std::vector<std::string> v{"010.jpg", "2.jpg", "002.jpg", "1.jpg", "10.jpg"};
  std::sort(v.begin(), v.end(), natural_less);
  // 1 < 2(==002 magnitude, "2" shorter run than "002") < 002 < 10(==010) < 010.
  CHECK(v == std::vector<std::string>{"1.jpg", "2.jpg", "002.jpg", "10.jpg", "010.jpg"});
}

// ===========================================================================
// build_page_list (real temp directory)
// ===========================================================================

TEST_CASE("build_page_list_filters_and_natural_sorts_a_directory") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "shigoku_viewer_test_pages";
  fs::remove_all(dir);
  fs::create_directories(dir);
  for (const char* name : {"010.jpg", "002.jpg", "1.png", "notes.txt",
                           "cover.jpeg", "thumb.webp"}) {
    std::ofstream(dir / name) << "x";  // content irrelevant (no decode here).
  }

  const auto pages = build_page_list({dir.string()});
  // .txt excluded; .webp included; the rest natural-sorted by name.
  CHECK(basenames(pages) == std::vector<std::string>{"1.png", "002.jpg", "010.jpg",
                                                      "cover.jpeg", "thumb.webp"});

  fs::remove_all(dir);
}

TEST_CASE("build_page_list_explicit_files_filter_and_sort") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "shigoku_viewer_test_files";
  fs::remove_all(dir);
  fs::create_directories(dir);
  for (const char* name : {"b.png", "a.jpg", "skip.txt"}) {
    std::ofstream(dir / name) << "x";
  }
  const auto pages = build_page_list(
      {(dir / "b.png").string(), (dir / "a.jpg").string(), (dir / "skip.txt").string(),
       (dir / "missing.jpg").string()});
  // skip.txt (non-image) and missing.jpg (absent) dropped; a before b.
  CHECK(basenames(pages) == std::vector<std::string>{"a.jpg", "b.png"});
  fs::remove_all(dir);
}

TEST_CASE("build_page_list_empty_when_nothing_matches") {
  CHECK(build_page_list({"/nonexistent/path/xyz"}).empty());
}

// ===========================================================================
// advance — the key table
// ===========================================================================

namespace {
ViewState st(int page, int count, bool rtl = false, Fit fit = Fit::Page) {
  ViewState s;
  s.page = page;
  s.page_count = count;
  s.rtl = rtl;
  s.fit = fit;
  return s;
}
}  // namespace

TEST_CASE("advance_reading_order_keys_never_mirror") {
  // space / PgDn / j = next; backspace / PgUp / k = prev — in BOTH ltr and rtl.
  for (bool rtl : {false, true}) {
    CAPTURE(rtl);
    CHECK(advance(st(0, 5, rtl), Key::Space).page == 1);
    CHECK(advance(st(0, 5, rtl), Key::PageDown).page == 1);
    CHECK(advance(st(0, 5, rtl), Key::J).page == 1);
    CHECK(advance(st(3, 5, rtl), Key::Backspace).page == 2);
    CHECK(advance(st(3, 5, rtl), Key::PageUp).page == 2);
    CHECK(advance(st(3, 5, rtl), Key::K).page == 2);
  }
}

TEST_CASE("advance_spatial_arrows_mirror_under_rtl_ONLY") {
  // LTR: right = next, left = prev.
  CHECK(advance(st(1, 5, /*rtl=*/false), Key::ArrowRight).page == 2);
  CHECK(advance(st(1, 5, /*rtl=*/false), Key::ArrowLeft).page == 0);
  // RTL: right = PREV, left = NEXT (the pinned mirror).
  CHECK(advance(st(1, 5, /*rtl=*/true), Key::ArrowRight).page == 0);
  CHECK(advance(st(1, 5, /*rtl=*/true), Key::ArrowLeft).page == 2);
}

TEST_CASE("advance_page_nav_clamps_at_both_ends") {
  CHECK(advance(st(0, 3), Key::K).page == 0);          // already first.
  CHECK(advance(st(2, 3), Key::Space).page == 2);      // already last.
  CHECK(advance(st(1, 3), Key::Home).page == 0);
  CHECK(advance(st(1, 3), Key::End).page == 2);
}

TEST_CASE("advance_page_change_resets_scroll") {
  ViewState s = st(1, 5);
  s.scroll_y = 400;
  CHECK(advance(s, Key::Space).scroll_y == 0);   // moved: reset.
  CHECK(advance(s, Key::Space).page == 2);
  // A no-op move (clamped at the end) keeps scroll.
  ViewState last = st(4, 5);
  last.scroll_y = 400;
  CHECK(advance(last, Key::Space).scroll_y == 400);
}

TEST_CASE("advance_scroll_half_window_clamped_by_viewport") {
  // content 2000px in a 1000px window: max scroll 1000, step 500.
  const Viewport vp{2000, 1000};
  ViewState s = st(0, 3, false, Fit::Width);
  s = advance(s, Key::ArrowDown, vp);
  CHECK(s.scroll_y == 500);
  s = advance(s, Key::ArrowDown, vp);
  CHECK(s.scroll_y == 1000);
  s = advance(s, Key::ArrowDown, vp);
  CHECK(s.scroll_y == 1000);  // clamped at the bottom.
  s = advance(s, Key::ArrowUp, vp);
  CHECK(s.scroll_y == 500);
  // Content fits (fit-page): no scroll possible.
  ViewState p = st(0, 3);
  CHECK(advance(p, Key::ArrowDown, Viewport{800, 1000}).scroll_y == 0);
}

TEST_CASE("advance_fit_toggle_and_fullscreen_and_quit") {
  ViewState s = st(0, 3);
  s.scroll_y = 300;
  s.scroll_x = 40;
  ViewState f = advance(s, Key::F);
  CHECK(f.fit == Fit::Width);
  CHECK(f.scroll_y == 0);              // layout change resets both offsets.
  CHECK(f.scroll_x == 0);
  CHECK(advance(f, Key::F).fit == Fit::Page);
  CHECK(advance(s, Key::Fullscreen).fullscreen);
  CHECK(advance(advance(s, Key::Fullscreen), Key::Fullscreen).fullscreen == false);
  CHECK(advance(s, Key::Q).quit);
  CHECK(advance(s, Key::Escape).quit);
  CHECK_FALSE(advance(s, Key::Other).quit);  // unknown key: no change.
  CHECK(advance(s, Key::Other) == s);
}

TEST_CASE("advance_escape_leaves_fullscreen_before_it_quits") {
  // Fullscreen hides the window chrome and (on macOS) the menu bar, so Esc has
  // to be the way back out; only a windowed Esc quits. q quits either way.
  ViewState full = st(0, 3);
  full.fullscreen = true;
  const ViewState back = advance(full, Key::Escape);
  CHECK_FALSE(back.fullscreen);
  CHECK_FALSE(back.quit);
  CHECK(advance(back, Key::Escape).quit);
  CHECK(advance(full, Key::Q).quit);
  CHECK(advance(full, Key::Q).fullscreen);  // q does not undress the window.
}

// ===========================================================================
// zoom — the ladder, the key table, and apply_zoom geometry
// ===========================================================================

TEST_CASE("zoom_ladder_steps_and_clamps_at_both_ends") {
  CHECK(zoom_next(kZoomFit) == 125);
  CHECK(zoom_prev(kZoomFit) == 75);
  // A round trip lands exactly where it started (why the ladder is a table).
  CHECK(zoom_prev(zoom_next(kZoomFit)) == kZoomFit);
  CHECK(zoom_next(zoom_prev(kZoomFit)) == kZoomFit);
  // Off-ladder values snap to the neighbouring step rather than drifting.
  CHECK(zoom_next(110) == 125);
  CHECK(zoom_prev(110) == 100);
  // Ends clamp: no runaway in either direction.
  CHECK(zoom_next(300) == 300);
  CHECK(zoom_next(1000) == 300);
  CHECK(zoom_prev(50) == 50);
  CHECK(zoom_prev(1) == 50);
}

TEST_CASE("advance_zoom_keys_walk_the_ladder_and_reset") {
  ViewState s = st(0, 3);
  CHECK(s.zoom == kZoomFit);
  const ViewState in = advance(s, Key::ZoomIn);
  CHECK(in.zoom == 125);
  CHECK(advance(in, Key::ZoomOut).zoom == kZoomFit);
  CHECK(advance(advance(in, Key::ZoomIn), Key::ZoomReset).zoom == kZoomFit);
  // Zoom survives page turns and fit toggles — a reader who zoomed in stays in.
  CHECK(advance(in, Key::Space).zoom == 125);
  CHECK(advance(in, Key::F).zoom == 125);
  // Already at the fit size: reset is a no-op, not a state change.
  CHECK(advance(s, Key::ZoomReset) == s);
}

TEST_CASE("advance_zoom_keeps_the_window_centre_over_the_same_point") {
  // A 1000x1000 window on 2000x2000 of content, scrolled to the dead centre
  // of both axes (max offset 1000, sitting at 500).
  const Viewport vp{2000, 1000, 2000, 1000};
  ViewState s = st(0, 3);
  s.scroll_x = 500;
  s.scroll_y = 500;
  // In one step: the content becomes 2500 wide, the page point under the
  // window's centre moves to 1250, so the top-left follows to 1250 - 500.
  const ViewState z = advance(s, Key::ZoomIn, vp);
  CHECK(z.zoom == 125);
  CHECK(z.scroll_x == 750);
  CHECK(z.scroll_y == 750);
  // Back out with the metrics that zoom produced: the centre returns to where
  // it started, not to the top-left corner.
  const ViewState back = advance(z, Key::ZoomOut, Viewport{2500, 1000, 2500, 1000});
  CHECK(back.zoom == kZoomFit);
  CHECK(back.scroll_x == 500);
  CHECK(back.scroll_y == 500);
}

TEST_CASE("advance_zoom_clamps_scroll_when_the_content_shrinks_to_fit") {
  // Zoomed in and panned, then reset: nothing overflows any more, so both
  // offsets must land at 0 rather than pointing past the page.
  const Viewport vp{2000, 1000, 1500, 1000};
  ViewState s = st(0, 3);
  s.zoom = 200;
  s.scroll_x = 500;
  s.scroll_y = 1000;
  const ViewState r = advance(s, Key::ZoomReset, vp);
  CHECK(r.zoom == kZoomFit);
  CHECK(r.scroll_x == 0);
  CHECK(r.scroll_y == 0);
}

TEST_CASE("advance_arrows_pan_before_turning_the_page_when_zoomed") {
  // 1500px of content in a 1000px window: 500px of pan, half-window steps.
  const Viewport vp{1000, 1000, 1500, 1000};
  ViewState s = st(1, 5);
  s.zoom = 150;
  const ViewState r1 = advance(s, Key::ArrowRight, vp);
  CHECK(r1.scroll_x == 500);  // clamped at the right edge…
  CHECK(r1.page == 1);        // …and the page has not moved.
  const ViewState r2 = advance(r1, Key::ArrowRight, vp);
  CHECK(r2.page == 2);        // at the edge, the key means "next page" again.
  CHECK(r2.scroll_x == 0);    // the new page starts at its own left edge.
  // Back the same way: pan to the left edge first, then the previous page.
  const ViewState l1 = advance(r1, Key::ArrowLeft, vp);
  CHECK(l1.scroll_x == 0);
  CHECK(l1.page == 1);
  CHECK(advance(l1, Key::ArrowLeft, vp).page == 0);
  // RTL still mirrors the PAGE turn, never the pan direction.
  ViewState rtl = st(1, 5, /*rtl=*/true);
  rtl.zoom = 150;
  CHECK(advance(rtl, Key::ArrowRight, vp).scroll_x == 500);
  CHECK(advance(advance(rtl, Key::ArrowRight, vp), Key::ArrowRight, vp).page == 0);
}

TEST_CASE("advance_arrows_still_turn_pages_when_nothing_overflows") {
  // The unzoomed fit-page case: no pan is possible, so the arrows are exactly
  // the page keys they always were.
  const Viewport vp{800, 1000, 600, 1000};
  CHECK(advance(st(1, 5), Key::ArrowRight, vp).page == 2);
  CHECK(advance(st(1, 5), Key::ArrowLeft, vp).page == 0);
  CHECK(advance(st(1, 5), Key::ArrowRight, vp).scroll_x == 0);
}

TEST_CASE("apply_zoom_scales_and_recentres_only_what_still_fits") {
  const Rect fit{100, 50, 600, 900};  // centred in a 800x1000 window.
  // The fit size itself is untouched — the unzoomed path never rounds.
  CHECK(apply_zoom(fit, 800, 1000, kZoomFit) == fit);
  // 50%: 300x450, both axes still fit, so both re-centre.
  CHECK(apply_zoom(fit, 800, 1000, 50) == Rect{250, 275, 300, 450});
  // 200%: 1200x1800 overflows both axes -> origin 0,0 and the caller pans.
  CHECK(apply_zoom(fit, 800, 1000, 200) == Rect{0, 0, 1200, 1800});
  // One axis over, one under: only the under one centres.
  CHECK(apply_zoom(Rect{0, 0, 800, 200}, 800, 1000, 125) ==
        Rect{0, 375, 1000, 250});
}

TEST_CASE("apply_zoom_never_asks_for_more_pixels_than_the_budget") {
  // A budget of 1M px against a 4M px request: scaled back along the aspect,
  // so no window size can turn a zoom into an unbounded allocation.
  const Rect r = apply_zoom(Rect{0, 0, 1000, 1000}, 1000, 1000, 200,
                            /*max_pixels=*/1000000);
  CHECK(static_cast<long long>(r.w) * r.h <= 1000000);
  CHECK(r.w == r.h);  // aspect kept.
  CHECK(r.w == 1000);
  // Under budget, the cap does not touch the geometry.
  CHECK(apply_zoom(Rect{0, 0, 100, 100}, 1000, 1000, 200, 1000000) ==
        Rect{400, 400, 200, 200});
}

TEST_CASE("apply_zoom_rejects_degenerate_input") {
  CHECK(apply_zoom(Rect{}, 800, 600, 200) == Rect{});
  CHECK(apply_zoom(Rect{0, 0, 600, 900}, 800, 600, 0) == Rect{0, 0, 600, 900});
  CHECK(apply_zoom(Rect{0, 0, 600, 900}, 800, 600, -50) == Rect{0, 0, 600, 900});
}

// ===========================================================================
// fit_rect
// ===========================================================================

TEST_CASE("fit_rect_page_centers_and_scales_to_fit") {
  // Portrait page 600x900 in a 1200x900 window: height-bound, scale 1.0 ->
  // 600x900 centered horizontally.
  CHECK(fit_rect(600, 900, 1200, 900, Fit::Page) == Rect{300, 0, 600, 900});
  // Landscape 1000x500 in an 800x800 window: width-bound, scale 0.8 ->
  // 800x400 centered vertically.
  CHECK(fit_rect(1000, 500, 800, 800, Fit::Page) == Rect{0, 200, 800, 400});
}

TEST_CASE("fit_rect_width_fills_width_may_overflow_height") {
  // 600x900 at window width 300: scale 0.5 -> 300x450 at origin.
  CHECK(fit_rect(600, 900, 300, 400, Fit::Width) == Rect{0, 0, 300, 450});
  // Tall page overflowing the window (the scrollable case): 500x2000 at
  // width 500 -> 500x2000, taller than a 1000px window.
  CHECK(fit_rect(500, 2000, 500, 1000, Fit::Width) == Rect{0, 0, 500, 2000});
}

TEST_CASE("fit_rect_rejects_nonpositive_dims") {
  CHECK(fit_rect(0, 900, 800, 600, Fit::Page) == Rect{});
  CHECK(fit_rect(600, 0, 800, 600, Fit::Page) == Rect{});
  CHECK(fit_rect(600, 900, 0, 600, Fit::Width) == Rect{});
  CHECK(fit_rect(600, 900, 800, 0, Fit::Width) == Rect{});
}

// ===========================================================================
// report_line
// ===========================================================================

TEST_CASE("report_line_is_one_based") {
  CHECK(report_line(0) == "LAST_PAGE=1\n");
  CHECK(report_line(13) == "LAST_PAGE=14\n");
}

// ===========================================================================
// page-number HUD
// ===========================================================================

TEST_CASE("advance_p_toggles_hud_and_nothing_else") {
  ViewState s = st(2, 9);
  CHECK(s.hud);  // on by default — the page number must be discoverable.
  const ViewState off = advance(s, Key::P);
  CHECK_FALSE(off.hud);
  CHECK(off.page == s.page);
  CHECK(advance(off, Key::P).hud);
  CHECK(advance(s, Key::Space).hud);  // page turns leave the toggle alone.
}

TEST_CASE("hud_text_is_one_based_page_over_count") {
  CHECK(hud_text(st(0, 12)) == "1/12");
  CHECK(hud_text(st(11, 12)) == "12/12");
}

TEST_CASE("hud_text_shows_the_zoom_only_while_zoomed") {
  ViewState s = st(2, 20);
  CHECK(hud_text(s) == "3/20");  // at the fit size there is nothing to say.
  s.zoom = 150;
  CHECK(hud_text(s) == "3/20 150%");
  s.zoom = 50;
  CHECK(hud_text(s) == "3/20 50%");
}

TEST_CASE("render_hud_has_ink_for_the_percent_sign") {
  // '%' joined the digits and '/' in the font when zoom reached the HUD; a
  // missing glyph would silently render a blank cell.
  bool ink = false;
  const HudImage img = render_hud("%", 1);
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    if (img.rgba[i] == 255) ink = true;
  }
  CHECK(ink);
}

TEST_CASE("render_hud_geometry_scales_with_text_and_scale") {
  const HudImage a = render_hud("1/2", 1);
  // 3 cells of 6px minus the trailing spacing + 2px padding each side.
  CHECK(a.w == 2 * 2 + 6 * 3 - 1);
  CHECK(a.h == 2 * 2 + 7);
  CHECK(a.rgba.size() == static_cast<std::size_t>(a.w) * a.h * 4u);
  const HudImage b = render_hud("1/2", 2);
  CHECK(b.w == 2 * a.w);  // every metric is linear in scale.
  CHECK(b.h == 2 * a.h);
  CHECK(render_hud("", 2).w == 0);
  CHECK(render_hud("1", 0).w == 0);
}

TEST_CASE("render_hud_draws_ink_on_a_translucent_box") {
  const HudImage img = render_hud("8", 1);
  REQUIRE(img.w > 0);
  // Corner pixel = box (padding), never ink.
  CHECK(img.rgba[3] == 150);
  CHECK(img.rgba[0] == 0);
  // Somewhere in the glyph there is white ink at full text alpha.
  bool ink = false;
  for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
    if (img.rgba[i] == 255 && img.rgba[i + 3] == 235) ink = true;
  }
  CHECK(ink);
  // A glyphless char renders box only (a blank cell, not garbage).
  const HudImage blank = render_hud("x", 1);
  bool any_ink = false;
  for (std::size_t i = 0; i < blank.rgba.size(); i += 4) {
    if (blank.rgba[i] == 255) any_ink = true;
  }
  CHECK_FALSE(any_ink);
}
