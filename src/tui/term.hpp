// term.hpp — raw mode, alternate screen, winsize (cells + pixels), and
// restore-on-every-exit-path (P6, A3). The P0 smoke proved the termios floor;
// this promotes it to an RAII guard the app owns for its lifetime.
//
// A1: SIGWINCH sets a volatile sig_atomic_t flag only; the ≤100ms tick notices
// and re-queries TIOCGWINSZ. No self-pipe. Signal-based restore (SIGINT/TERM)
// writes only async-signal-safe calls.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace shigoku::tui {

// Terminal geometry: cells AND pixels (ws_xpixel/ypixel, needed for Kitty cover
// sizing in P8). Pixel fields are 0 when the terminal doesn't report them.
struct WinSize {
  std::uint16_t cols = 0;
  std::uint16_t rows = 0;
  std::uint16_t xpixel = 0;
  std::uint16_t ypixel = 0;
  friend bool operator==(const WinSize&, const WinSize&) = default;
};

// Query the current window size via TIOCGWINSZ. Returns {0,0,0,0} if stdout is
// not a tty or the ioctl fails.
[[nodiscard]] WinSize query_winsize();

// True if a SIGWINCH has fired since the last call, and clears the flag. The
// tick loop polls this; the handler only sets a sig_atomic_t.
[[nodiscard]] bool take_resize_flag();

// Wait until `fd` is readable or `timeout_ms` elapses. Returns >0 (readable),
// 0 (timed out), or -1 (error; errno set — callers retry on EINTR).
//
// PLATFORM SEAM (§3): uses select(2) on macOS, poll(2) elsewhere. Darwin's
// poll() is historically unreliable on ttys/devices (it has silently failed to
// report readiness on some fds), so on the legacy 10.5/10.6 PPC target we use
// select() — which is well-behaved there — for the single-fd stdin waits the
// TUI does (the input reader loop and the P8 cover probe). select()'s
// FD_SETSIZE ceiling is irrelevant here: STDIN_FILENO is 0, far under it.
[[nodiscard]] int wait_readable(int fd, int timeout_ms);

// RAII terminal owner. Construction enters raw mode + alternate screen + hides
// the cursor and installs SIGWINCH + SIGINT/SIGTERM handlers. Destruction (and
// any fatal-signal path) restores the terminal exactly as found. Non-copyable.
class Terminal {
 public:
  // Enters raw mode; `ok()` is false if stdin/stdout is not a tty (the caller
  // should fall back to a non-TUI path). Even when not ok, destruction is safe.
  Terminal();
  ~Terminal();

  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

  [[nodiscard]] bool ok() const { return active_; }

  // Write a buffer to stdout, retrying on EINTR / partial writes. This is the
  // one output path the flush uses.
  void write(const char* data, std::size_t n) const;
  void write(std::string_view s) const;

  // Restore now (idempotent). Called by the destructor; exposed so an explicit
  // clean-quit path can restore before other teardown.
  void restore();

  // Turn on mouse reporting (P32: ?1002 button events, ?1006 SGR encoding).
  // Called by run() AFTER the cover probe — the probe reads stdin raw, and a
  // click mid-probe must not interleave report bytes into its answer. Every
  // restore path (destructor, fatal signal, atexit) writes the matching
  // disable unconditionally, so this needs no paired "off" call.
  void enable_mouse() const;

 private:
  bool active_ = false;
};

}  // namespace shigoku::tui
