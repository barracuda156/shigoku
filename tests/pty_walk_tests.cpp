// pty_walk_tests.cpp — P7 DoD: "a FixtureProvider (canned search/episodes/
// resolve) drives an automated pty test that walks search -> grid -> play
// against a stub mpv script and asserts the frames contain expected strings."
//
// This binary IS both the driver and (via --child) the driven app: the
// parent forkpty()s /proc/self/exe --child, types keystrokes into the pty
// master, and greps the (ANSI-stripped) output for the strings each step
// should have produced. Net-new pattern (no Rust precedent — PORT_CPP.md P7
// names it but the bible has nothing to port from; confirmed via research).
//
// Coarse on purpose (§8 test policy): frame-contains-substring, not a byte-
// exact golden, so DESIGN pixel polish doesn't thrash this test.

#include <fcntl.h>
// forkpty() lives in <pty.h> on glibc/Linux, <util.h> on macOS/BSD.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Self-executable path resolution: /proc/self/exe (Linux) has no macOS analogue;
// there it's _NSGetExecutablePath from <mach-o/dyld.h>.
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../src/provider.hpp"
#include "../src/tui/app.hpp"
#include "fixture_provider.hpp"

namespace {

using namespace shigoku;

// --- child role: the real app, wired to fixtures, no network -------------

int run_child() {
  test::FixtureProvider fixture;
  std::vector<std::unique_ptr<StreamProvider>> providers;
  providers.push_back(std::make_unique<test::FixtureProvider>());
  ProviderRegistry registry(std::move(providers));

  tui::AppDeps deps;
  deps.registry = &registry;
  deps.http = nullptr;  // never touched: deps.search is canned, no anilist::search call.
  deps.search = [](std::string_view, std::uint32_t) -> Result<tui::SearchPage, ProviderError> {
    Enrichment show;
    show.anilist_id = 1;
    show.mal_id = 1;
    show.title_romaji = "Fixture Frieren";
    show.title_english = "Fixture Frieren English";  // distinct from romaji (P18 title-language leg).
    show.score = 97;
    show.total_episodes = 2;
    return tui::SearchPage{{CatalogRow{show}}, /*has_next=*/false};
  };
  // Canned Discover feed (P17): a distinct row per axis so a `]` switch is
  // observable. First page only, exhausted (has_next=false) so no prefetch.
  deps.discover =
      [](DiscoverAxis axis, std::uint32_t, const DiscoverFilters&)
      -> Result<tui::DiscoverPage, ProviderError> {
    Enrichment show;
    show.anilist_id = 100 + static_cast<std::int64_t>(axis);
    show.title_romaji = std::string("Discover ") + tui::axis_label(axis);
    show.score = 88;
    show.total_episodes = 12;
    show.kind = "TV";
    return tui::DiscoverPage{{CatalogRow{show}}, /*has_next=*/false};
  };
  const char* stub = std::getenv("SHIGOKU_TEST_STUB_MPV");
  deps.mpv_path = (stub != nullptr) ? stub : "stub_mpv.py";
  const char* rundir = std::getenv("SHIGOKU_TEST_RUNTIME_DIR");
  deps.socket_dir = (rundir != nullptr) ? rundir : "/tmp";

  // P39 slice 3: the backend walk rides Config (the same fields the Settings
  // wheel writes), not AppDeps — spawn_play reads app.config per play. Unset
  // envs leave Config{} defaults, identical to the null-config runs before.
  Config config;
  const char* player = std::getenv("SHIGOKU_TEST_PLAYER");
  if (player != nullptr) config.player = player;
  const char* player_path = std::getenv("SHIGOKU_TEST_PLAYER_PATH");
  if (player_path != nullptr) config.player_path = player_path;

  return tui::run(/*demo_mode=*/false, &deps, &config);
}

// --- parent role: pty harness ----------------------------------------------

// Strip CSI/OSC-shaped ANSI escapes so substring search sees only the text a
// human would read (P6 NOTES.md: per-cell SGR means raw bytes never hold a
// contiguous run for multi-char strings).
std::string strip_ansi(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] == '\x1b' && i + 1 < raw.size()) {
      if (raw[i + 1] == '[') {
        std::size_t j = i + 2;
        while (j < raw.size() && !(raw[j] >= '@' && raw[j] <= '~')) ++j;
        i = (j < raw.size()) ? j + 1 : raw.size();
        continue;
      }
      i += 2;  // other 2-byte escape (e.g. ESC ( B) — skip both bytes.
      continue;
    }
    out.push_back(raw[i]);
    ++i;
  }
  return out;
}

struct PtySession {
  int master = -1;
  pid_t child = -1;
  std::string accumulated;

  void write_str(std::string_view s) { ::write(master, s.data(), s.size()); }

  // Drain whatever the child has written so far (non-blocking) into
  // `accumulated`, waiting up to `timeout_ms` for at least one byte if
  // nothing is buffered yet.
  void pump(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[65536];
    while (std::chrono::steady_clock::now() < deadline) {
      const ssize_t n = ::read(master, buf, sizeof(buf));
      if (n > 0) {
        accumulated.append(buf, static_cast<std::size_t>(n));
        continue;  // keep draining while data is flowing.
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      break;  // EOF or hard error.
    }
  }

  [[nodiscard]] std::string visible_text() const { return strip_ansi(accumulated); }
};

PtySession spawn_child_pty(const std::string& self_exe, const std::string& stub_mpv,
                           const std::string& runtime_dir,
                           const std::string& player = {},
                           const std::string& player_path = {}) {
  PtySession s;
  winsize ws{};
  ws.ws_row = 24;
  ws.ws_col = 100;
  const pid_t pid = ::forkpty(&s.master, nullptr, nullptr, &ws);
  if (pid == 0) {
    ::setenv("SHIGOKU_TEST_STUB_MPV", stub_mpv.c_str(), 1);
    ::setenv("SHIGOKU_TEST_RUNTIME_DIR", runtime_dir.c_str(), 1);
    if (!player.empty()) ::setenv("SHIGOKU_TEST_PLAYER", player.c_str(), 1);
    if (!player_path.empty()) {
      ::setenv("SHIGOKU_TEST_PLAYER_PATH", player_path.c_str(), 1);
    }
    char arg0[] = "shigoku_pty_child";
    char arg1[] = "--child";
    char* argv[] = {arg0, arg1, nullptr};
    ::execv(self_exe.c_str(), argv);
    ::_exit(127);  // execv failed.
  }
  s.child = pid;
  // Non-blocking reads so pump()'s loop can poll instead of hanging forever.
  const int flags = ::fcntl(s.master, F_GETFL, 0);
  ::fcntl(s.master, F_SETFL, flags | O_NONBLOCK);
  return s;
}

}  // namespace

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

// Path to this running test binary, so the parent can re-exec it with --child.
// Linux: /proc/self/exe; macOS: _NSGetExecutablePath (no /proc). Both yield a
// path that exec()s the same image, which is all the re-exec needs.
static std::string self_exe_path() {
  char buf[4096];
#if defined(__APPLE__)
  std::uint32_t size = sizeof(buf);
  if (::_NSGetExecutablePath(buf, &size) != 0) return {};  // buf too small.
  return std::string(buf);
#else
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  return std::string(buf);
#endif
}

TEST_CASE("pty walk: search -> episode grid -> play (FixtureProvider + stub mpv)") {
  const std::string self_path = self_exe_path();
  REQUIRE(!self_path.empty());

  const std::string fixtures_dir = SHIGOKU_TEST_FIXTURES_DIR;
  const std::string stub_mpv = fixtures_dir + "/stub_mpv.py";
  const std::string runtime_dir = "/tmp/shigoku-pty-test";
  ::mkdir(runtime_dir.c_str(), 0700);

  PtySession s = spawn_child_pty(self_path, stub_mpv, runtime_dir);

  // 1) Initial frame. The default landing (last_watched) parks an EMPTY
  // watchlist on History's first-run state (DESIGN §8.3, P30 parity fix);
  // `B` hops to Browse idle with the "/ to search" hint.
  s.pump(500);
  CHECK(s.visible_text().find("SHIGOKU") != std::string::npos);
  CHECK(s.visible_text().find("nothing watched yet") != std::string::npos);
  s.write_str("B");
  s.pump(200);
  CHECK(s.visible_text().find("to search AniList") != std::string::npos);

  // 2) `/` then a query, then Enter locks the search + fires the (canned)
  // search worker after the 300ms debounce.
  s.write_str("/");
  s.pump(100);
  s.write_str("frieren");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));  // debounce + fixture "fetch".
  s.pump(400);
  s.write_str("\r");  // Enter: lock search, focus -> list.
  s.pump(200);
  CHECK(s.visible_text().find("Fixture Frieren") != std::string::npos);

  // 3) Enter again: list -> detail pane, fires the (canned) episode fetch.
  s.write_str("\r");
  s.pump(400);
  const std::string after_detail = s.visible_text();
  CHECK(after_detail.find("[1]") != std::string::npos);
  CHECK(after_detail.find("[2]") != std::string::npos);
  CHECK(after_detail.find("97/100") != std::string::npos);

  // 4) Enter on the focused grid cell (cursor starts at episode "1"): resolve
  // + play via the stub mpv. Give the stub's IPC handshake + events time to
  // land (kIpcConnectTries budget ~2s + the stub's own short sleeps).
  s.write_str("\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  s.pump(1500);
  const std::string after_play = s.visible_text();
  // Either the playback line rendered while in flight, or it already
  // resolved to the "episode done" success toast (0.80 natural-end ratio,
  // stub sends time-pos=20 against duration=24 -> 0.833 >= 0.80).
  const bool saw_playback_line = after_play.find("\xE2\x96\xB6") != std::string::npos;  // ▶
  const bool saw_done_toast = after_play.find("episode done") != std::string::npos;
  CHECK((saw_playback_line || saw_done_toast));

  // 5) Quit cleanly; the child must exit 0 and the pty must not hang.
  s.write_str("q");
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    if (::waitpid(s.child, &status, WNOHANG) == s.child) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::close(s.master);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

// P39 slice 3 DoD: the same search -> grid -> play walk, but the play spawns
// stub_mplayer.py through the SlavePipes backend (config.player = "mplayer",
// the exact fields the Settings wheel writes). The stub answers three
// advancing ANS_time_pos (3/9/20) + ANS_LENGTH=24 on the ~1s poll cadence,
// then exits clean — 20/24 = 0.833 >= 0.80, so the checkpoint lands as the
// natural-end "episode done" toast (the identical observable the mpv walk
// pins; positions HAD to ride the slave pipes for it to appear).
TEST_CASE("pty walk: play through the mplayer slave backend (stub mplayer)") {
  const std::string self_path = self_exe_path();
  REQUIRE(!self_path.empty());

  const std::string fixtures_dir = SHIGOKU_TEST_FIXTURES_DIR;
  const std::string stub_mpv = fixtures_dir + "/stub_mpv.py";
  const std::string stub_mplayer = fixtures_dir + "/stub_mplayer.py";
  const std::string runtime_dir = "/tmp/shigoku-pty-test";
  ::mkdir(runtime_dir.c_str(), 0700);

  PtySession s = spawn_child_pty(self_path, stub_mpv, runtime_dir,
                                 /*player=*/"mplayer",
                                 /*player_path=*/stub_mplayer);

  // Compressed front half (asserted step-by-step in the mpv walk above).
  s.pump(500);
  s.write_str("B");
  s.pump(200);
  s.write_str("/");
  s.pump(100);
  s.write_str("frieren");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  s.pump(400);
  s.write_str("\r");
  s.pump(200);
  CHECK(s.visible_text().find("Fixture Frieren") != std::string::npos);
  s.write_str("\r");
  s.pump(400);
  CHECK(s.visible_text().find("[1]") != std::string::npos);

  // Play. The slave poll cadence is ~1s per round and the stub exits after
  // its third answer (~2.1s), so the budget here is wider than the mpv walk's.
  s.write_str("\r");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  s.pump(2500);
  const std::string after_play = s.visible_text();
  const bool saw_playback_line = after_play.find("\xE2\x96\xB6") != std::string::npos;  // ▶
  const bool saw_done_toast = after_play.find("episode done") != std::string::npos;
  CHECK((saw_playback_line || saw_done_toast));

  // Quit cleanly; quit-never-hangs holds through the pipe watcher too.
  s.write_str("q");
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    if (::waitpid(s.child, &status, WNOHANG) == s.child) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::close(s.master);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("pty walk: Discover grid — axis bar, feed, axis switch, add toast") {
  const std::string self_path = self_exe_path();
  REQUIRE(!self_path.empty());

  const std::string fixtures_dir = SHIGOKU_TEST_FIXTURES_DIR;
  const std::string stub_mpv = fixtures_dir + "/stub_mpv.py";
  const std::string runtime_dir = "/tmp/shigoku-pty-test";
  ::mkdir(runtime_dir.c_str(), 0700);

  PtySession s = spawn_child_pty(self_path, stub_mpv, runtime_dir);
  s.pump(500);

  // 1) `D` enters Discover: the axis bar renders and the tick pump fires the
  // canned feed for the active (Trending) axis → the card placeholder + title.
  s.write_str("D");
  s.pump(600);
  const std::string disc = s.visible_text();
  CHECK(disc.find("Trending") != std::string::npos);
  CHECK(disc.find("Popular") != std::string::npos);
  CHECK(disc.find("Discover Trending") != std::string::npos);  // the canned card.
  CHECK(disc.find("#1") != std::string::npos);                  // rank placeholder.

  // 2) `]` cycles to the Popular axis; its own feed fetches and renders.
  s.write_str("]");
  s.pump(600);
  CHECK(s.visible_text().find("Discover Popular") != std::string::npos);

  // 3) `P` adds the selected show to the watchlist → a success toast (no store
  // is wired in this pty harness, so the add is optimistic — still toasts).
  s.write_str("P");
  s.pump(300);
  CHECK(s.visible_text().find("added to your list") != std::string::npos);

  // 4) Quit cleanly.
  s.write_str("q");
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    if (::waitpid(s.child, &status, WNOHANG) == s.child) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::close(s.master);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("pty walk: P32 mouse — click selects, double-click opens, reporting off at quit") {
  const std::string self_path = self_exe_path();
  REQUIRE(!self_path.empty());

  const std::string fixtures_dir = SHIGOKU_TEST_FIXTURES_DIR;
  const std::string stub_mpv = fixtures_dir + "/stub_mpv.py";
  const std::string runtime_dir = "/tmp/shigoku-pty-test";
  ::mkdir(runtime_dir.c_str(), 0700);

  PtySession s = spawn_child_pty(self_path, stub_mpv, runtime_dir);
  s.pump(500);
  // The app turned mouse reporting on at boot (raw bytes, pre-strip) — one
  // mode per sequence (HW #4: legacy terminals reject a combined form whole).
  CHECK(s.accumulated.find("\x1b[?1000h\x1b[?1002h\x1b[?1015h\x1b[?1006h") !=
        std::string::npos);

  // Search for the fixture row (the same steps the main walk takes).
  s.write_str("B");
  s.pump(200);
  s.write_str("/");
  s.pump(100);
  s.write_str("frieren");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  s.pump(400);
  s.write_str("\r");
  s.pump(200);
  REQUIRE(s.visible_text().find("Fixture Frieren") != std::string::npos);

  // Double-click the result row (SGR reports, 1-based wire coords: column 4,
  // row 3 = cell (3,2) — the content origin below the top bar + spacer). All
  // four reports in ONE write so both presses land inside the tick window
  // deterministically: the first selects the (already focused) row, the
  // second is the view's Enter → the detail pane opens and the canned episode
  // grid lands.
  s.write_str("\x1b[<0;4;3M\x1b[<0;4;3m\x1b[<0;4;3M\x1b[<0;4;3m");
  s.pump(500);
  const std::string after_click = s.visible_text();
  CHECK(after_click.find("[1]") != std::string::npos);  // grid cells rendered.
  CHECK(after_click.find("[2]") != std::string::npos);

  // A top-bar tab click hops views like its letter: [S]ettings spans 0-based
  // columns 60..69 (P37 inserted [C]alendar before it) → 1-based column 61.
  s.write_str("\x1b[<0;61;1M\x1b[<0;61;1m");
  s.pump(300);
  CHECK(s.visible_text().find("preferred provider") != std::string::npos);

  // Quit; the restore must turn reporting back off (P32 DoD: no ?1002
  // residue on the shell after exit).
  s.write_str("q");
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    if (::waitpid(s.child, &status, WNOHANG) == s.child) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  s.pump(300);  // drain the teardown bytes (the restore sequence).
  ::close(s.master);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(s.accumulated.find("\x1b[?1006l\x1b[?1015l\x1b[?1002l\x1b[?1000l") !=
        std::string::npos);
}

// Resize repaint fix: shrinking the pty must produce an ED clear (the
// terminal's own crop/reflow can leave residue the cell diff alone cannot
// see) followed by a full re-render at the new geometry. TIOCSWINSZ on the
// master delivers SIGWINCH to the child; the ≤100ms tick polls the flag.
TEST_CASE("pty walk: resize ED-clears and repaints the full frame") {
  const std::string self_path = self_exe_path();
  REQUIRE(!self_path.empty());

  const std::string fixtures_dir = SHIGOKU_TEST_FIXTURES_DIR;
  const std::string stub_mpv = fixtures_dir + "/stub_mpv.py";
  const std::string runtime_dir = "/tmp/shigoku-pty-test";
  ::mkdir(runtime_dir.c_str(), 0700);

  PtySession s = spawn_child_pty(self_path, stub_mpv, runtime_dir);

  s.pump(500);
  CHECK(s.visible_text().find("SHIGOKU") != std::string::npos);

  // Everything after `mark` is post-resize output only.
  const std::size_t mark = s.accumulated.size();
  winsize ws{};
  ws.ws_row = 18;
  ws.ws_col = 60;
  REQUIRE(::ioctl(s.master, TIOCSWINSZ, &ws) == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // flag poll + paint.
  s.pump(600);
  const std::string post = s.accumulated.substr(mark);
  CHECK(post.find("\x1b[2J") != std::string::npos);          // starts from blank...
  CHECK(strip_ansi(post).find("SHIGOKU") != std::string::npos);  // ...then full re-render.

  s.write_str("q");
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    if (::waitpid(s.child, &status, WNOHANG) == s.child) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::close(s.master);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("pty walk: Settings title-language change alters the rendered title (P18 DoD)") {
  const std::string self_path = self_exe_path();
  REQUIRE(!self_path.empty());

  const std::string fixtures_dir = SHIGOKU_TEST_FIXTURES_DIR;
  const std::string stub_mpv = fixtures_dir + "/stub_mpv.py";
  const std::string runtime_dir = "/tmp/shigoku-pty-test";
  ::mkdir(runtime_dir.c_str(), 0700);

  PtySession s = spawn_child_pty(self_path, stub_mpv, runtime_dir);
  s.pump(500);
  // Default landing parks the empty watchlist on History (P30) — hop out.
  s.write_str("B");
  s.pump(200);

  // 1) Search for the fixture show; its romaji title renders (the config
  // default, per Config::title_language = "romaji").
  s.write_str("/");
  s.pump(100);
  s.write_str("frieren");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  s.pump(400);
  s.write_str("\r");
  s.pump(200);
  CHECK(s.visible_text().find("Fixture Frieren") != std::string::npos);
  CHECK(s.visible_text().find("Fixture Frieren English") == std::string::npos);

  // 2) `S` into Settings, `j` onto the title language row (index 11: Player
  // 0-4, Provider 5, Interface 6-11 — title language is Interface's last
  // row), `l` cycles romaji -> english.
  s.write_str("S");
  s.pump(200);
  CHECK(s.visible_text().find("Player") != std::string::npos);
  for (int i = 0; i < 11; ++i) s.write_str("j");
  s.pump(100);
  s.write_str("l");
  s.pump(200);
  CHECK(s.visible_text().find("english") != std::string::npos);

  // 3) Back to Browse: the same row now renders the English title (draw-time
  // projection — no re-fetch, title_language needs no live-effect callback).
  s.write_str("B");
  s.pump(300);
  CHECK(s.visible_text().find("Fixture Frieren English") != std::string::npos);

  // 4) Quit cleanly.
  s.write_str("q");
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    if (::waitpid(s.child, &status, WNOHANG) == s.child) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::close(s.master);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--child") == 0) {
    return run_child();
  }
  doctest::Context ctx;
  ctx.applyCommandLine(argc, argv);
  return ctx.run();
}
