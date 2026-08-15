// smoke.cpp — P0 deliverable.
//
// Proves the terminal foundation the whole TUI rests on: enter raw mode +
// alternate screen, emit a truecolor swatch of the DESIGN palette, restore the
// terminal cleanly on EVERY exit path (normal return, SIGINT, SIGTERM).
//
// This is deliberately dependency-free (POSIX termios + unistd only). The real
// CellBuffer / term module lands in P6; this only has to leave the terminal
// sane. Run it, hit Ctrl-C or any key, and your shell must look untouched.

#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// The subset of ANSI we need without a cell buffer yet.
constexpr const char* kEnterAltScreen = "\x1b[?1049h";
constexpr const char* kLeaveAltScreen = "\x1b[?1049l";
constexpr const char* kHideCursor = "\x1b[?25l";
constexpr const char* kShowCursor = "\x1b[?25h";
constexpr const char* kReset = "\x1b[0m";
constexpr const char* kClear = "\x1b[2J\x1b[H";

// DESIGN §1.1 palette (a handful — theme.hpp will own the full set in P6).
struct Rgb {
  unsigned char r, g, b;  // memory-order bytes; never packed into an int (§3, PPC).
};
constexpr Rgb kBgBase{0x02, 0x0d, 0x06};
constexpr Rgb kTextPrimary{0x39, 0xff, 0x6a};
constexpr Rgb kStateFocus{0x20, 0xff, 0xdd};
constexpr Rgb kStateNow{0xff, 0x2d, 0x78};
constexpr Rgb kStateWarn{0xe5, 0xb8, 0x00};

// The saved cooked-mode termios, and whether we ever changed it. A signal
// handler restores from these, so they are plain globals guarded by an atomic.
termios g_saved_termios{};
std::atomic<bool> g_raw_active{false};

void write_all(const char* s) {
  const size_t n = std::strlen(s);
  size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(STDOUT_FILENO, s + off, n - off);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      break;  // best effort on the restore path; nothing useful to do on error.
    }
    off += static_cast<size_t>(w);
  }
}

// Restore the terminal to exactly how we found it. Safe to call more than once
// and safe to call from a signal handler (only write() + tcsetattr(), both
// async-signal-safe).
void restore_terminal() {
  if (g_raw_active.exchange(false)) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    write_all(kShowCursor);
    write_all(kLeaveAltScreen);
    write_all(kReset);
  }
}

extern "C" void on_fatal_signal(int sig) {
  restore_terminal();
  // Re-raise with the default disposition so the exit status reflects the
  // signal (what a well-behaved TUI does — the shell sees a real ^C).
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

bool enter_raw_mode() {
  if (!::isatty(STDIN_FILENO)) return false;
  if (::tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) return false;

  termios raw = g_saved_termios;
  // cfmakeraw-equivalent, spelled out (portable to old MacPorts libc).
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

void install_signal_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_fatal_signal;
  // sigemptyset is a function-like macro on macOS/BSD (<signal.h>), so it must
  // stay unqualified — `::sigemptyset` fails to expand. glibc exposes it as a
  // real function that unqualified lookup still resolves via <csignal>.
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART — we want read() to return on signal.
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
}

// Emit "\x1b[38;2;R;G;Bm" (truecolor fg) into a buffer and write it.
void set_fg(Rgb c) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof buf, "\x1b[38;2;%u;%u;%um",
                              c.r, c.g, c.b);
  if (n > 0) ::write(STDOUT_FILENO, buf, static_cast<size_t>(n));
}
void set_bg(Rgb c) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof buf, "\x1b[48;2;%u;%u;%um",
                              c.r, c.g, c.b);
  if (n > 0) ::write(STDOUT_FILENO, buf, static_cast<size_t>(n));
}

void draw_swatch() {
  write_all(kClear);
  set_bg(kBgBase);
  write_all("\x1b[2K");  // paint the void behind our line

  const struct {
    Rgb c;
    const char* name;
  } rows[] = {
      {kTextPrimary, "text.primary  #39ff6a  phosphor green"},
      {kStateFocus, "state.focus   #20ffdd  cyan ghost"},
      {kStateNow, "state.now     #ff2d78  spectral magenta"},
      {kStateWarn, "state.warn    #e5b800  warning amber"},
  };

  write_all("\r\n");
  set_fg(kTextPrimary);
  write_all("  shigoku smoke — terminal foundation OK\r\n\r\n");

  for (const auto& row : rows) {
    write_all("  ");
    set_bg(row.c);
    write_all("      ");  // color block
    write_all(kReset);
    set_bg(kBgBase);
    set_fg(row.c);
    write_all("  ");
    write_all(row.name);
    write_all("\r\n");
  }

  write_all(kReset);
  set_bg(kBgBase);
  set_fg(kTextPrimary);
  write_all("\r\n  press any key (or Ctrl-C) to exit cleanly\r\n");
}

}  // namespace

int main() {
  if (!enter_raw_mode()) {
    std::fprintf(stderr, "shigoku smoke: not a tty (or termios unavailable)\n");
    return 1;
  }
  install_signal_handlers();

  write_all(kEnterAltScreen);
  write_all(kHideCursor);
  draw_swatch();

  // Block for one keypress. SIGINT/SIGTERM interrupt read() and route through
  // on_fatal_signal, which restores and re-raises.
  char c = 0;
  ssize_t r;
  do {
    r = ::read(STDIN_FILENO, &c, 1);
  } while (r < 0 && errno == EINTR);

  restore_terminal();
  return 0;
}
