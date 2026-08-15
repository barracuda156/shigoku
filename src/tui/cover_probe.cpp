// cover_probe.cpp — startup cover-support probe (P8, A4). Best-effort terminal
// round-trip; bounded read so a mute terminal degrades, never hangs.

#include "cover_probe.hpp"

#include <unistd.h>

#include <cerrno>
#include <string>
#include <string_view>

#include "../debug_log.hpp"
#include "iterm.hpp"
#include "kitty.hpp"
#include "term.hpp"  // wait_readable — the poll/select platform seam (§3).

namespace shigoku::tui {

namespace cover_probe_detail {

bool parse_csi_14t(std::string_view reply, std::uint16_t& out_w,
                   std::uint16_t& out_h) {
  // Look for ESC [ 4 ; H ; W t anywhere in the reply. H and W are decimal.
  constexpr std::string_view kPrefix = "\x1b[4;";
  const std::size_t p = reply.find(kPrefix);
  if (p == std::string_view::npos) return false;
  std::size_t i = p + kPrefix.size();

  auto read_uint = [&](std::uint32_t& out) -> bool {
    std::uint32_t v = 0;
    bool any = false;
    while (i < reply.size() && reply[i] >= '0' && reply[i] <= '9') {
      v = v * 10u + static_cast<std::uint32_t>(reply[i] - '0');
      if (v > 65535u) v = 65535u;  // clamp to u16 range; a sane terminal fits.
      ++i;
      any = true;
    }
    out = v;
    return any;
  };

  std::uint32_t h = 0, w = 0;
  if (!read_uint(h)) return false;
  if (i >= reply.size() || reply[i] != ';') return false;
  ++i;  // skip ';'
  if (!read_uint(w)) return false;
  if (i >= reply.size() || reply[i] != 't') return false;
  if (h == 0 || w == 0) return false;
  out_h = static_cast<std::uint16_t>(h);
  out_w = static_cast<std::uint16_t>(w);
  return true;
}

}  // namespace cover_probe_detail

namespace {

// Write the whole buffer to stdout, retrying on EINTR / short writes.
void write_all(std::string_view s) {
  std::size_t off = 0;
  while (off < s.size()) {
    const ssize_t w = ::write(STDOUT_FILENO, s.data() + off, s.size() - off);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      break;  // best-effort.
    }
    off += static_cast<std::size_t>(w);
  }
}

// Drain the terminal reply for up to `deadline_ms`, or until a DA1 answer
// (CSI ? … c / CSI … c) closes the window. DA1 is requested last, so seeing its
// terminator means every earlier reply (graphics ack, CSI 14t) has arrived.
std::string read_reply(int deadline_ms) {
  std::string reply;
  char buf[512];
  // A DA1 answer ends in 'c' after a CSI. We stop as soon as we see a 'c' that
  // followed an ESC[ — the bounded poll is the backstop if none arrives.
  const int per_poll = deadline_ms > 40 ? 40 : deadline_ms;
  int spent = 0;
  while (spent < deadline_ms) {
    const int pr = wait_readable(STDIN_FILENO, per_poll);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) {
      spent += per_poll;
      // If we already captured a DA1 terminator, we're done early.
      if (reply.find("\x1b[") != std::string::npos &&
          reply.find_last_of('c') != std::string::npos)
        break;
      continue;
    }
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    reply.append(buf, static_cast<std::size_t>(n));
    // DA1 terminator seen: the reply window is closed.
    if (reply.find('c') != std::string::npos &&
        reply.find("\x1b[") != std::string::npos)
      break;
  }
  return reply;
}

}  // namespace

CoverProbe probe_cover_support(std::uint16_t cols, std::uint16_t rows,
                               int deadline_ms) {
  CoverProbe out;
  if (!::isatty(STDOUT_FILENO) || !::isatty(STDIN_FILENO)) {
    return out;  // headless: no probe, placeholder + unknown geometry.
  }

  // First geometry source: TIOCGWINSZ pixels (term.cpp). If present it is the
  // most reliable; CSI 14t only fills in when the ioctl reports 0 pixels.
  const WinSize ws = query_winsize();
  out.cell = cell_px_from_window(ws.xpixel, ws.ypixel, cols, rows);

  // Write the probe in reply order: CSI 14t (window pixels) → a=q graphics
  // query → ReportCellSize (the OSC 1337 capability probe — iTerm2 answers
  // this, NOT XTVERSION) → XTVERSION → a single DA1 (CSI c). DA1 answers
  // last, so its terminator in the reply means every earlier answer has
  // already arrived. (ReportCellSize's reply body contains no lowercase 'c',
  // so the terminator scan below cannot close the window on it early.)
  std::string seq;
  seq += "\x1b[14t";              // request window size in pixels.
  seq += kitty::query_apc(1);     // a=t transmit-ack probe (no DA1 of its own).
  seq += iterm::query_cell_size();  // OSC 1337 capability + cell geometry.
  seq += iterm::query_xtversion();  // name-yourself DCS (adopter fallback).
  seq += "\x1b[c";                // DA1 terminator, answered after all four.

  write_all(seq);
  const std::string reply = read_reply(deadline_ms);

  // The raw wire truth, for the hardware seam: which queries this terminal
  // answered decides everything downstream, and a mis-probe on a real
  // terminal is invisible without it.
  if (debug_log_enabled()) {
    debug_log("probe: raw reply (" + std::to_string(reply.size()) + "B) " +
              debug_escape(reply));
  }

  out.kitty = kitty::probe_reply_is_kitty(reply);
  out.iterm = iterm::probe_reply_is_iterm(reply);
  out.iterm2 = iterm::probe_reply_is_iterm2(reply);

  // If TIOCGWINSZ gave no pixel geometry, fall back to the CSI 14t reply,
  // then to the ReportCellSize answer (points × scale = per-cell pixels —
  // the common case on iTerm2, where the ioctl reports no pixels).
  if (!out.cell.known()) {
    std::uint16_t w = 0, h = 0;
    if (cover_probe_detail::parse_csi_14t(reply, w, h)) {
      out.cell = cell_px_from_window(w, h, cols, rows);
    }
  }
  if (!out.cell.known()) {
    double cw = 0.0, ch = 0.0;
    if (iterm::parse_cell_size_reply(reply, cw, ch) && cw >= 1.0 &&
        ch >= 1.0 && cw < 512.0 && ch < 512.0) {
      out.cell = CellPx{static_cast<std::uint16_t>(cw + 0.5),
                        static_cast<std::uint16_t>(ch + 0.5)};
    }
  }
  return out;
}

}  // namespace shigoku::tui
