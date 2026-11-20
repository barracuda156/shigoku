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
//      backend_from); attribute 4 inside the DA1 answer itself ⇒ the sixel
//      backend (no extra query — SIXEL advertises in the attributes we
//      already ask for); a DA1 with none of those ⇒ placeholder mode (a dim
//      box, layout slot preserved).
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
  bool sixel = false;   // DA1 attributes list 4 (sixel.hpp).
  CellPx cell;          // per-cell pixels; {0,0} = unknown (floors/caps).
  friend bool operator==(const CoverProbe&, const CoverProbe&) = default;
};

// The graphics backend the probe verdicts select. Kitty wins when several
// answer (ids + deletes + no re-encode make it the richest wire); OSC 1337 is
// next (still lossless 24-bit); SIXEL is last, a lossy ≤256-colour fallback
// worth taking only when neither richer protocol is there — which is exactly
// the legacy target's case (mlterm speaks SIXEL and nothing else; xterm and
// foot answer SIXEL but not kitty). None ⇒ placeholder mode, nothing
// image-shaped ever emitted.
//
// EXCEPTION: on iTerm2 itself the OSC 1337 backend outranks the kitty ack.
// iTerm2 3.5 answers a=q and even acks every kitty transmit with OK, but its
// kitty implementation does not render our raw-RGBA cell-box placements —
// the screen stays blank while the wire says success. Its native inline-image
// protocol draws them fine, and ReportCellSize (which only iTerm2 answers)
// pins the identification, so WezTerm/Konsole — whose kitty support is real —
// keep the kitty backend.
//
// SIXEL alone carries a PRECONDITION: it is sized in pixels, so it needs the
// per-cell geometry the other two backends let the terminal work out. Without
// it an image cannot be fitted to its cell box, and one drawn too tall scrolls
// the screen out from under the TUI. A sixel terminal that reports no geometry
// therefore falls through to placeholder mode — a defined, already-implemented
// degradation — rather than to a guess.
enum class CoverBackend : std::uint8_t { None, Kitty, Iterm, Sixel };

[[nodiscard]] constexpr CoverBackend backend_from(const CoverProbe& p) {
  if (p.iterm2) return CoverBackend::Iterm;
  if (p.kitty) return CoverBackend::Kitty;
  if (p.iterm) return CoverBackend::Iterm;
  if (p.sixel && p.cell.known()) return CoverBackend::Sixel;
  return CoverBackend::None;
}

// Config::image_protocol: which backend the cover pipeline may use. Auto
// follows the probe; a named protocol is used whatever the probe heard (for a
// terminal whose answer gets lost on the way, a multiplexer for one); Off
// shows no images and sends no probe at all — nothing for a slow or mute
// terminal to stall on, and no late reply for a keypress to collide with.
// Unknown spellings read as Auto.
enum class ImageProtocol : std::uint8_t { Auto, Off, Kitty, Iterm, Sixel };

[[nodiscard]] constexpr ImageProtocol parse_image_protocol(std::string_view s) {
  if (s == "none" || s == "off" || s == "halfblocks") return ImageProtocol::Off;
  if (s == "kitty") return ImageProtocol::Kitty;
  if (s == "iterm" || s == "iterm2") return ImageProtocol::Iterm;
  if (s == "sixel") return ImageProtocol::Sixel;
  return ImageProtocol::Auto;
}

// The backend to use under `want`. A forced SIXEL keeps its precondition: no
// cell geometry, no images.
[[nodiscard]] constexpr CoverBackend backend_for(ImageProtocol want, const CoverProbe& p) {
  switch (want) {
    case ImageProtocol::Auto: break;
    case ImageProtocol::Off: return CoverBackend::None;
    case ImageProtocol::Kitty: return CoverBackend::Kitty;
    case ImageProtocol::Iterm: return CoverBackend::Iterm;
    case ImageProtocol::Sixel:
      return p.cell.known() ? CoverBackend::Sixel : CoverBackend::None;
  }
  return backend_from(p);
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
