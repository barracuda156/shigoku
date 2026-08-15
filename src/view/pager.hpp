// pager.hpp — the PURE, testable core of shigoku-view.
// NO SDL, NO decode — just the paging state machine, CLI parse, page-list
// build + numeric-aware sort, fit-rect geometry, and the exit report line.
// sdl_compat.{hpp,cpp} + main.cpp own everything that touches SDL/stb; this
// header is what `viewer_tests` links against with zero graphics dependency.
//
// Design (mine, greenfield — no sabigoku reference): one page at a time (no
// double-spread in v1, parked), two fit modes plus a zoom ladder on top of
// them, RTL as a spatial-key mirror only. The state machine is a pure
// `advance(state, key, viewport) -> state` so the whole key table is a table
// test; the viewport (content/viewport pixel sizes) is threaded in only so
// scroll and pan clamp deterministically.
//
// ENDIANNESS: pure integer geometry; no byte packing. The RGBA upload
// (sdl_compat) uses SDL_PIXELFORMAT_RGBA32, the byte-order-safe alias.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../result.hpp"

namespace shigoku::view {

// Fit mode. Page = whole page visible, centered (comic default). Width = fill
// the window width, scroll vertically (webtoon-ish / detail reading).
enum class Fit { Page, Width };

// A physical key, already decoded from SDL by sdl_compat. The semantics
// (reading-order vs spatial, RTL mirroring) live in advance(), not the SDL
// layer — so the pin "RTL mirrors spatial keys only" is one testable place.
enum class Key {
  Space,      // next (reading order — NEVER mirrored)
  PageDown,   // next
  J,          // next
  Backspace,  // prev (reading order — NEVER mirrored)
  PageUp,     // prev
  K,          // prev
  ArrowRight,  // spatial — mirrored under RTL; pans first when zoomed in
  ArrowLeft,   // spatial — mirrored under RTL; pans first when zoomed in
  ArrowUp,     // scroll up (fit-width / zoomed)
  ArrowDown,   // scroll down (fit-width / zoomed)
  Home,        // first page
  End,         // last page
  F,           // toggle fit (page <-> width)
  Fullscreen,  // toggle fullscreen (Shift+F, F11, Cmd+Ctrl+F, the macOS menu)
  ZoomIn,      // one step in
  ZoomOut,     // one step out
  ZoomReset,   // back to the fit size
  P,           // toggle the page-number HUD
  Q,           // quit
  Escape,      // leave fullscreen if in it, else quit
  Other,       // ignored
};

// A destination rectangle in window pixels (fit_rect output; also the SDL
// blit target). For Fit::Width the height may exceed the window (the
// scrollable overflow); the caller offsets y by -scroll_y.
struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  friend bool operator==(const Rect&, const Rect&) = default;
};

// Parsed command line. `paths` are the raw positionals (one directory OR N
// files); build_page_list() resolves them to the actual page files.
struct Options {
  std::vector<std::string> paths;
  bool rtl = false;
  int start_page = 1;  // 1-based; clamped to the page count by main.
  std::optional<std::string> report_file;
  Fit fit = Fit::Page;
  std::optional<std::string> title;
  bool help = false;  // --help/-h: print usage() and exit 0 (no paths needed).
};

// Zoom is a percentage of the FIT size, so 100 is the classic behavior (the
// page exactly fits per its Fit mode) and the ladder below is what the zoom
// keys walk. Percent (not a float factor) keeps the state integral and the
// steps exactly reversible.
inline constexpr int kZoomFit = 100;

// Upper bound on the scaled surface (RGBA, 4 bytes/px) a zoom may ask for.
// Only bites on very large displays at max zoom — the point is that no window
// size can make one page allocation unbounded on a small machine.
inline constexpr long long kMaxZoomPixels = 12000000;

// The mutable paging state. advance() is the only mutator.
struct ViewState {
  int page = 0;        // 0-based current page.
  int page_count = 1;  // total pages (>= 1 once main has a list).
  Fit fit = Fit::Page;
  bool rtl = false;
  bool fullscreen = false;
  bool hud = true;     // corner page-number overlay; p toggles.
  bool quit = false;
  int zoom = kZoomFit;  // percent of the fit size.
  int scroll_x = 0;    // horizontal pan, pixels; 0 unless zoomed past the edge.
  int scroll_y = 0;    // vertical scroll, pixels; 0 when the page fits.
  friend bool operator==(const ViewState&, const ViewState&) = default;
};

// Pixel metrics for scroll clamping: the rendered page size and the window
// size. Zeros (unknown / content fits) make scroll a no-op on that axis.
// The content dims are the ALREADY-ZOOMED ones the renderer put on screen —
// that is what the zoom keys rescale the scroll offsets against.
struct Viewport {
  int content_h = 0;
  int viewport_h = 0;
  int content_w = 0;
  int viewport_w = 0;
};

// The one usage string (also printed on a parse error).
[[nodiscard]] std::string usage();

// Parse argv (argv[0] is the program name, skipped). Err carries a
// human-readable reason. Flags: --rtl, --start-page N (1-based, >=1),
// --report-file PATH, --fit page|width, --title S, --help/-h, -- (end of
// flags); positionals = one dir or N files (at least one required, except
// under --help).
[[nodiscard]] Result<Options, std::string> parse_cli(int argc, char** argv);

// Numeric-aware ("natural") comparison so 002.jpg < 010.jpg and page_2 <
// page_10: maximal digit runs compare by value (leading zeros ignored),
// everything else byte-wise. A superset of the MangaDex source's
// chapter_less (which parses only a LEADING number) — page filenames carry
// the number mid-string, so the whole-string natural walk is the right
// tool. Total strict-weak order.
[[nodiscard]] bool natural_less(const std::string& a, const std::string& b);

// Resolve positionals to the sorted page-file list. One directory -> its
// .jpg/.jpeg/.png/.webp entries; N paths -> those that are image files.
// Sorted by natural_less on the full path. Missing paths / non-images are
// skipped (never an error here — main decides what an empty list means).
[[nodiscard]] std::vector<std::string> build_page_list(
    const std::vector<std::string>& paths);

// The pure key table (the heart of the viewer). Reading-order keys
// (space/PgDn/j = next; backspace/PgUp/k = prev) are NEVER mirrored; the
// spatial arrows (<-/->) ARE mirrored under RTL, but only once there is
// nothing left to pan on that side — zoomed in, they walk the page first.
// Up/Down half-window scroll, clamped by `vp`. A page change or fit toggle
// resets scroll; zoom survives both.
[[nodiscard]] ViewState advance(ViewState s, Key k, Viewport vp = {});

// One step along the zoom ladder from `percent` (which need not be on it —
// off-ladder values snap to the next/previous step). Clamped at both ends, so
// zoom_next at the top and zoom_prev at the bottom are no-ops.
[[nodiscard]] int zoom_next(int percent);
[[nodiscard]] int zoom_prev(int percent);

// Destination rect for an img_w x img_h page in a win_w x win_h window.
// Fit::Page: scaled to fit entirely, centered (both dims <= window).
// Fit::Width: width == win_w, height scaled (may exceed win_h -> scroll), at
// x=0,y=0 (the caller applies -scroll_y). Any non-positive input -> {0,0,0,0}.
[[nodiscard]] Rect fit_rect(int img_w, int img_h, int win_w, int win_h, Fit fit);

// `fit` scaled by `zoom` percent and re-centered on each axis that still fits
// the window (an axis that overflows starts at 0 and is panned by the caller
// via -scroll_x/-scroll_y). zoom == kZoomFit returns `fit` untouched — the
// unzoomed viewer never goes through the zoom arithmetic at all. A rect whose
// area would exceed `max_pixels` is scaled back to that budget, aspect kept.
[[nodiscard]] Rect apply_zoom(Rect fit, int win_w, int win_h, int zoom,
                              long long max_pixels = kMaxZoomPixels);

// The exit report the TUI parses for mid-chapter resume: "LAST_PAGE=<N>\n"
// with N the 1-based page (idx is 0-based).
[[nodiscard]] std::string report_line(int idx);

// "N/M" (1-based) for the HUD box and the title bar, plus " Z%" while zoomed
// (the zoom level is otherwise invisible). Manga pages carry no printed
// numbers of their own — the viewer is the only place the reader can see
// where they are, so this must not depend on the page images.
[[nodiscard]] std::string hud_text(const ViewState& s);

// A rasterized HUD label: `text` in a translucent dark box, RGBA32 row-major
// (w*h*4). Glyphs come from a tiny embedded 5x7 font (digits, '/' and '%';
// anything else is a blank cell) scaled by `scale` — no font dependency, so
// the whole rasterizer is table-testable. Empty (w=h=0) if `text` is empty
// or scale < 1. main uploads the result as the overlay texture.
struct HudImage {
  std::vector<std::uint8_t> rgba;
  int w = 0;
  int h = 0;
};
[[nodiscard]] HudImage render_hud(const std::string& text, int scale);

}  // namespace shigoku::view
