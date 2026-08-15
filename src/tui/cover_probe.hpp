// cover_probe.hpp — startup terminal probe for cover support (P8, A4). Runs
// ONCE at app launch, after the alternate screen is entered and BEFORE the
// stdin reader thread exists (it reads stdio directly — same ordering law as
// sabigoku's Picker::from_query_stdio, tui/mod.rs:60-64). Two questions,
// answered by one round-trip:
//
//   1. Which image protocol does the terminal speak? — active probes (kitty
//      a=q + OSC 1337 ReportCellSize + XTVERSION + DA1), never env sniffing
//      (contour sets no $KITTY_WINDOW_ID, A4; $TERM_PROGRAM does not survive
//      ssh). A graphics APC reply before the DA1 answer ⇒ kitty; a
//      ReportCellSize answer (the OSC 1337 protocol's own query — only iTerm2
//      implements it) or an XTVERSION reply naming an OSC 1337 adopter ⇒ the
//      iterm backend; iTerm2 ITSELF outranks its own kitty ack (see
//      backend_from); DA1 alone ⇒ placeholder mode (a dim box, layout slot
//      preserved).
//   2. What is the per-cell pixel geometry? — TIOCGWINSZ first (term.cpp), then
//      a CSI 14t window-pixel reply as the fallback; neither ⇒ CellPx{0,0}
//      (fixed floors/caps). This is A4's ioctl → CSI 14t → placeholder ladder.
//
// The probe is best-effort: a terminal that answers nothing degrades to
// placeholder mode with unknown geometry — never a hang (bounded poll).

#pragma once

#include <cstdint>
#include <string_view>

#include "cover_geom.hpp"

namespace shigoku::tui {

// Result of the startup probe.
struct CoverProbe {
  bool kitty = false;   // terminal acked the a=q graphics query.
  bool iterm = false;   // answered ReportCellSize / named an adopter (iterm.hpp).
  bool iterm2 = false;  // the terminal IS iTerm2 (ReportCellSize / XTVERSION name).
  CellPx cell;          // per-cell pixels; {0,0} = unknown (floors/caps).
  friend bool operator==(const CoverProbe&, const CoverProbe&) = default;
};

// The graphics backend the probe verdicts select. Kitty wins when both answer
// (ids + deletes + no re-encode make it the richer wire); the OSC 1337 path
// is the fallback for terminals that only speak iTerm2's protocol. None ⇒
// placeholder mode, nothing image-shaped ever emitted.
//
// EXCEPTION: on iTerm2 itself the OSC 1337 backend outranks the kitty ack.
// iTerm2 3.5 answers a=q and even acks every kitty transmit with OK, but its
// kitty implementation does not render our raw-RGBA cell-box placements —
// the screen stays blank while the wire says success. Its native inline-image
// protocol draws them fine, and ReportCellSize (which only iTerm2 answers)
// pins the identification, so WezTerm/Konsole — whose kitty support is real —
// keep the kitty backend.
enum class CoverBackend : std::uint8_t { None, Kitty, Iterm };

[[nodiscard]] constexpr CoverBackend backend_from(const CoverProbe& p) {
  if (p.iterm2) return CoverBackend::Iterm;
  if (p.kitty) return CoverBackend::Kitty;
  if (p.iterm) return CoverBackend::Iterm;
  return CoverBackend::None;
}

// Run the probe against the controlling terminal. `cols`/`rows` are the current
// cell grid (from query_winsize) used to divide a CSI 14t window-pixel reply
// into per-cell geometry. Writes the probe sequence to stdout, then reads the
// reply with a bounded poll deadline (default ~200ms — a terminal that hasn't
// answered by then is treated as placeholder / no-geometry). Not a tty ⇒
// {false, {0,0}} with nothing written.
[[nodiscard]] CoverProbe probe_cover_support(std::uint16_t cols,
                                             std::uint16_t rows,
                                             int deadline_ms = 200);

// --- parse helpers, exposed for tests --------------------------------------
namespace cover_probe_detail {

// Parse a CSI 14t window-pixel report: ESC [ 4 ; <height_px> ; <width_px> t.
// On success writes width/height in pixels to out_w/out_h and returns true.
// (The report is `4` = window size in pixels; `height` precedes `width`.)
[[nodiscard]] bool parse_csi_14t(std::string_view reply, std::uint16_t& out_w,
                                 std::uint16_t& out_h);

}  // namespace cover_probe_detail

}  // namespace shigoku::tui
