// term.cpp — raw mode + winsize + restore (P6, A3). Termios logic promoted
// from the P0 smoke; SIGWINCH is a sig_atomic_t flag (A1) the tick polls.

#include "term.hpp"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// Platform seam for wait_readable: select(2) on Darwin (its poll(2) is
// unreliable on ttys, §3), poll(2) on other Unices.
#if defined(__APPLE__)
#include <sys/select.h>
#include <sys/time.h>
#else
#include <poll.h>
#endif

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

namespace shigoku::tui {

namespace {

constexpr const char* kEnterAltScreen = "\x1b[?1049h";
constexpr const char* kLeaveAltScreen = "\x1b[?1049l";
constexpr const char* kHideCursor = "\x1b[?25l";
constexpr const char* kShowCursor = "\x1b[?25h";
constexpr const char* kReset = "\x1b[0m";
// Delete ALL kitty graphics (a=d delete, d=A CAPITAL = free stored data too,
// A4). The clean quit path deletes the one held image by id before restoring;
// this belt-and-braces "clear everything" runs on EVERY restore so a crash or
// abort mid-frame can't leave a stray cover painted over the shell. A fixed
// literal written with a single ::write, so it stays async-signal-safe.
constexpr const char* kDeleteAllGraphics = "\x1b_Ga=d,d=A\x1b\\";
// Mouse reporting (P32, §9): basic press/release tracking (?1000) as the
// floor, button-event tracking (?1002) layered on top, then the encoding
// extensions oldest-first — urxvt decimal (?1015), SGR (?1006) — so the most
// capable one a terminal knows wins (xterm-family terminals let the last one
// set decide the wire form; the decoder handles X10, 1015, and SGR alike).
// ONE MODE PER SEQUENCE: the combined
// "?1000;1002;1006h" form is rejected WHOLE by some legacy terminals when any
// single parameter is unknown to them — which left the PPC target with no
// mouse reporting at all despite the ?1000 floor. Separate sequences make
// each mode's fate independent, matching what ncurses/notcurses emit.
// The disable runs on EVERY restore path — even when reporting was never
// enabled the reset is harmless — and both are fixed literals written with
// single ::write calls, so the fatal-signal path stays async-signal-safe.
constexpr const char* kEnableMouse =
    "\x1b[?1000h\x1b[?1002h\x1b[?1015h\x1b[?1006h";
constexpr const char* kDisableMouse =
    "\x1b[?1006l\x1b[?1015l\x1b[?1002l\x1b[?1000l";

// Saved cooked-mode termios + whether we changed it (async-signal-safe restore
// reads these). One terminal per process, so file-scope state is fine.
termios g_saved{};
std::atomic<bool> g_raw_active{false};

// SIGWINCH landed since last poll. sig_atomic_t + volatile per A1.
volatile std::sig_atomic_t g_winch = 0;

void write_all_fd(const char* s) {
  const std::size_t n = std::strlen(s);
  std::size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(STDOUT_FILENO, s + off, n - off);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      break;  // best-effort on the restore path.
    }
    off += static_cast<std::size_t>(w);
  }
}

void do_restore() {
  if (g_raw_active.exchange(false)) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
    // Stop mouse reporting FIRST (P32) so no report bytes land on the
    // restored shell, then wipe any stray kitty image before leaving the alt
    // screen — otherwise a crash mid-frame could leave a cover painted over
    // the restored shell.
    write_all_fd(kDisableMouse);
    write_all_fd(kDeleteAllGraphics);
    write_all_fd(kShowCursor);
    write_all_fd(kLeaveAltScreen);
    write_all_fd(kReset);
  }
}

extern "C" void on_winch(int) { g_winch = 1; }

// Fatal-signal restore: put the terminal back, then re-raise with the default
// disposition so the process dies the way it would have — exit status reflects
// the signal (a real ^C to the shell) and a crash (SIGSEGV/SIGABRT/…) still
// dumps core. Only async-signal-safe calls here: tcsetattr, write, signal,
// raise (do_restore is all four). No malloc, no stdio.
extern "C" void on_fatal_signal(int sig) {
  do_restore();
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

// atexit(3) backstop: if control ever reaches std::exit() or returns out of a
// non-TUI path with the terminal still raw (belt-and-braces for a code path
// that forgets its RAII guard), this restores. Idempotent via the exchange in
// do_restore(), so it's harmless when the destructor already ran.
extern "C" void restore_at_exit() { do_restore(); }

bool enter_raw_mode() {
  if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) return false;
  if (::tcgetattr(STDIN_FILENO, &g_saved) != 0) return false;

  termios raw = g_saved;
  raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(tcflag_t)(OPOST);
  raw.c_cflag |= (tcflag_t)(CS8);
  raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
  g_raw_active.store(true);
  return true;
}

void install_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_fatal_signal;
  // sigemptyset is a function-like macro on macOS/BSD (<signal.h>), so it must
  // stay unqualified — `::sigemptyset` fails to expand. glibc exposes it as a
  // real function that unqualified lookup still resolves via <csignal>.
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART — read()/poll() must return on signal.
  // Interactive quit (SIGINT/SIGTERM/SIGQUIT) AND crash/abort (P10 quit-path
  // audit: "restore on every exit path incl. panic/abort"). A C++ panic —
  // uncaught exception → std::terminate → abort() — arrives as SIGABRT, so
  // catching it here is what restores the terminal after a panic. The hardware
  // faults (SEGV/BUS/FPE/ILL) get the same treatment: restore, then re-raise
  // to the default handler so a core dump still happens.
  for (const int sig :
       {SIGINT, SIGTERM, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
    ::sigaction(sig, &sa, nullptr);
  }

  struct sigaction sw {};
  sw.sa_handler = on_winch;
  sigemptyset(&sw.sa_mask);
  sw.sa_flags = SA_RESTART;  // winch shouldn't tear an in-flight read.
  ::sigaction(SIGWINCH, &sw, nullptr);

  // atexit backstop for a plain std::exit()/return that skips the RAII guard.
  std::atexit(restore_at_exit);
}

}  // namespace

WinSize query_winsize() {
  winsize ws{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return WinSize{};
  return WinSize{ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel};
}

bool take_resize_flag() {
  if (g_winch) {
    g_winch = 0;
    return true;
  }
  return false;
}

int wait_readable(int fd, int timeout_ms) {
#if defined(__APPLE__)
  // Darwin: select(2). poll(2) on macOS has a long history of not reporting
  // readiness correctly on ttys/devices (§3); select is reliable there. A
  // negative timeout means "block" (null timeval); otherwise a bounded wait.
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);
  timeval tv;
  timeval* tvp = nullptr;
  if (timeout_ms >= 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    tvp = &tv;
  }
  return ::select(fd + 1, &rfds, nullptr, nullptr, tvp);
#else
  pollfd pfd{fd, POLLIN, 0};
  return ::poll(&pfd, 1, timeout_ms);
#endif
}

Terminal::Terminal() {
  if (!enter_raw_mode()) {
    active_ = false;
    return;
  }
  install_handlers();
  write_all_fd(kEnterAltScreen);
  write_all_fd(kHideCursor);
  active_ = true;
}

Terminal::~Terminal() { restore(); }

void Terminal::restore() { do_restore(); }

void Terminal::enable_mouse() const {
  if (active_) write_all_fd(kEnableMouse);
}

void Terminal::write(const char* data, std::size_t n) const {
  std::size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(STDOUT_FILENO, data + off, n - off);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      break;
    }
    off += static_cast<std::size_t>(w);
  }
}

void Terminal::write(std::string_view s) const { write(s.data(), s.size()); }

}  // namespace shigoku::tui
