// player.cpp — P5 implementation. Ported from sabigoku src/player.rs.
//
// The argv table (03 §6.3.1) and the retry law (04 §7.8) are byte-critical and
// ported 1:1. Deviations from player.rs are v0 narrowings only (no proxy, no
// aniskip — NOTES.md P5) and the platform seam (posix_spawnp + a hand-rolled
// buffered line reader instead of Rust's Command / BufReader).
//
// ENDIANNESS (§3): no multi-byte value is assembled from a byte buffer here.
// IPC is newline-delimited JSON text parsed by nlohmann into host-native
// doubles; peer creds come from the kernel as host ints. No reinterpretation.

// SO_PEERCRED / struct ucred are glibc extensions gated behind _GNU_SOURCE; we
// build with -std=c++23 (extensions OFF) so the macro isn't auto-defined. Set
// it before any system header. Harmless on macOS (uses LOCAL_PEERPID instead).
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1
#endif

#include "player.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// Platform seam for the slave watcher's timed wait: select(2) on Darwin (its
// poll(2) is unreliable on ttys/devices, §3), poll(2) on other Unices — the
// same split term.cpp's wait_readable uses (player must not depend on tui).
#if defined(__APPLE__)
#include <sys/select.h>
#include <sys/time.h>
#else
#include <poll.h>
#endif

#include <csignal>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "debug_log.hpp"
#include "http.hpp"
#include "proxy.hpp"

extern char** environ;

namespace shigoku::player {

using nlohmann::json;

BackendKind parse_backend(std::string_view name) {
  if (name == "mplayer") return BackendKind::Mplayer;
  if (name == "qmplay2") return BackendKind::Qmplay2;
  return BackendKind::Mpv;  // unknown re-enters at mpv (§9 P39 slice 2).
}

// ── argv (03 §6.3.1, the table is law) ──────────────────────────────────────

namespace {

// Provider bytes an argv element may carry: printable ASCII, no space. Catches
// CR/LF (header injection, ROD-92) and >= 0x80 in one range check. Empty is
// allowed: a blank optional field yields an inert flag value. (player.rs:286)
bool arg_clean(std::string_view s) {
  for (unsigned char b : s) {
    if (b < 0x21 || b > 0x7e) return false;
  }
  return true;
}

// UAs are the one field with legitimate spaces, and must be non-empty: an empty
// --user-agent= blanks mpv's UA and defeats the CF bot-score workaround.
// (player.rs:292)
bool ua_clean(std::string_view s) {
  if (s.empty()) return false;
  for (unsigned char b : s) {
    if (b < 0x20 || b > 0x7e) return false;
  }
  return true;
}

}  // namespace

Result<std::vector<std::string>, PlayError> build_argv(
    const StreamLink& link, std::string_view play_url, const PlayOpts& opts,
    std::string_view socket_path) {
  // Positional: any leading '-' reads as a flag (a real http(s) url never
  // starts with one). Stricter than a bare '--' check. (player.rs:232)
  if (!arg_clean(play_url) || (!play_url.empty() && play_url.front() == '-')) {
    return err(PlayError::unsafe_arg("url"));
  }

  std::vector<std::string> argv;
  if (link.referer) {
    if (!arg_clean(*link.referer)) return err(PlayError::unsafe_arg("referer"));
    argv.push_back("--http-header-fields-append=Referer: " + *link.referer);
  }
  if (link.user_agent) {
    // Dedicated flag, never header-append: two UAs = Cloudflare 403.
    if (!ua_clean(*link.user_agent)) {
      return err(PlayError::unsafe_arg("user_agent"));
    }
    argv.push_back("--user-agent=" + *link.user_agent);
  }
  if (is_absolute_url(play_url)) {
    argv.push_back("--stream-lavf-o=multiple_requests=1,icy=0");
  }
  if (link.sub_url) {
    if (!arg_clean(*link.sub_url)) return err(PlayError::unsafe_arg("sub_url"));
    argv.push_back("--sub-file=" + *link.sub_url);
    argv.push_back("--sub-pos=92");
    argv.push_back("--sub-bold=yes");
  }
  if (link.cloaked_segments) {
    argv.push_back("--demuxer-lavf-o=allowed_extensions=ALL");
  }
  // mpv renders this in its own window title / OSD with no framework backstop,
  // so the full ROD-435 strip applies (bidi / zero-width included).
  const std::string title = strip_controls(opts.title);
  if (!title.empty()) {
    argv.push_back("--force-media-title=" + title);
    // ${media-title} expands mpv-side; the raw title never re-parses.
    argv.push_back("--title=shigoku - ${media-title}");
  }
  argv.push_back("--input-ipc-server=" + std::string(socket_path));
  if (std::isfinite(opts.start_secs) && opts.start_secs > 0.0) {
    // Match Rust's `format!("--start={}", f64)`: shortest round-trip form.
    // nlohmann's dump of a double gives the same minimal representation.
    argv.push_back("--start=" + json(opts.start_secs).dump());
  }
  if (opts.skip) {
    // App-constructed (cache path + formatted floats), never provider bytes —
    // rides argv without the vetting above (player.hpp SkipScript comment).
    argv.push_back("--script=" + opts.skip->path);
    argv.push_back("--script-opts=" + opts.skip->opts);
  }
  argv.push_back(std::string(play_url));
  return argv;
}

Result<std::vector<std::string>, PlayError> build_local_argv(
    std::string_view path, const PlayOpts& opts, std::string_view socket_path) {
  if (path.empty()) return err(PlayError::unsafe_arg("local_path"));
  std::vector<std::string> argv;
  // The tail half of build_argv's table, byte-for-byte: title flags, IPC
  // socket, resume start, skip script, positional. None of the resolved-link
  // flags (referer/UA/stream-lavf/sub/allowed_extensions) apply to a file.
  const std::string title = strip_controls(opts.title);
  if (!title.empty()) {
    argv.push_back("--force-media-title=" + title);
    argv.push_back("--title=shigoku - ${media-title}");
  }
  argv.push_back("--input-ipc-server=" + std::string(socket_path));
  if (std::isfinite(opts.start_secs) && opts.start_secs > 0.0) {
    argv.push_back("--start=" + json(opts.start_secs).dump());
  }
  if (opts.skip) {
    argv.push_back("--script=" + opts.skip->path);
    argv.push_back("--script-opts=" + opts.skip->opts);
  }
  std::string positional(path);
  if (positional.front() == '-') positional.insert(0, "./");
  argv.push_back(std::move(positional));
  return argv;
}

// ── mplayer argv (P39 slice 3) ──────────────────────────────────────────────
//
// Deliberately MINIMAL next to mpv's table — §9's rule: alternates get
// playback, nothing beyond it. No subs (mplayer's -sub wants a local file),
// no window title, no aniskip, no demuxer tuning. What stays is what play
// needs: -slave -quiet (the position poll rides the command pipes), -ss (the
// resume law), and the link's own UA/Referer when present (-referrer is
// missing from truly ancient builds, but a cloaked stream needing it routes
// through the P12 proxy anyway, and the G4 tier's recommended shape is
// download-then-play-local where none of this applies). Vetting is the same
// ROD-435 discipline as build_argv: every provider byte is checked, the
// positional can't read as a flag.

Result<std::vector<std::string>, PlayError> build_mplayer_argv(
    const StreamLink& link, std::string_view play_url, const PlayOpts& opts,
    std::string_view /*ipc: slave pipes need no rendezvous path*/) {
  if (!arg_clean(play_url) || (!play_url.empty() && play_url.front() == '-')) {
    return err(PlayError::unsafe_arg("url"));
  }
  std::vector<std::string> argv;
  argv.push_back("-slave");
  argv.push_back("-quiet");
  if (std::isfinite(opts.start_secs) && opts.start_secs > 0.0) {
    argv.push_back("-ss");
    argv.push_back(json(opts.start_secs).dump());  // same minimal form as --start.
  }
  if (link.user_agent) {
    if (!ua_clean(*link.user_agent)) return err(PlayError::unsafe_arg("user_agent"));
    argv.push_back("-user-agent");
    argv.push_back(*link.user_agent);
  }
  if (link.referer) {
    if (!arg_clean(*link.referer)) return err(PlayError::unsafe_arg("referer"));
    argv.push_back("-referrer");
    argv.push_back(*link.referer);
  }
  argv.push_back(std::string(play_url));
  return argv;
}

Result<std::vector<std::string>, PlayError> build_mplayer_local_argv(
    std::string_view path, const PlayOpts& opts, std::string_view /*ipc*/) {
  if (path.empty()) return err(PlayError::unsafe_arg("local_path"));
  std::vector<std::string> argv;
  argv.push_back("-slave");
  argv.push_back("-quiet");
  if (std::isfinite(opts.start_secs) && opts.start_secs > 0.0) {
    argv.push_back("-ss");
    argv.push_back(json(opts.start_secs).dump());
  }
  std::string positional(path);
  if (positional.front() == '-') positional.insert(0, "./");
  argv.push_back(std::move(positional));
  return argv;
}

// ── QMPlay2 argv (P39 slice 4) ──────────────────────────────────────────────
//
// Launch-only (§9): QMPlay2 exposes no portable position interface (MPRIS is
// Linux/D-Bus, absent on macOS) and no CLI flags for headers or a start
// offset, so the argv is JUST the vetted positional. No -ss analogue means
// resume is a no-op here — playback starts at zero and progress is marked
// manually; the A6 no-position law keeps the checkpoint intact.

Result<std::vector<std::string>, PlayError> build_qmplay2_argv(
    const StreamLink& /*link: no header flags exist to carry its fields*/,
    std::string_view play_url, const PlayOpts& /*opts*/,
    std::string_view /*ipc: launch-only needs no rendezvous*/) {
  if (!arg_clean(play_url) || (!play_url.empty() && play_url.front() == '-')) {
    return err(PlayError::unsafe_arg("url"));
  }
  return std::vector<std::string>{std::string(play_url)};
}

Result<std::vector<std::string>, PlayError> build_qmplay2_local_argv(
    std::string_view path, const PlayOpts& /*opts*/, std::string_view /*ipc*/) {
  if (path.empty()) return err(PlayError::unsafe_arg("local_path"));
  std::string positional(path);
  if (positional.front() == '-') positional.insert(0, "./");
  return std::vector<std::string>{std::move(positional)};
}

// ── resume rule (03 §6.3.1, A6) ─────────────────────────────────────────────

double resume_start(double saved_secs, double duration_secs, bool fully_watched,
                    double resume_offset_sec) {
  const bool pos_invalid = !std::isfinite(saved_secs) || saved_secs <= 0.0;
  const bool near_end = std::isfinite(duration_secs) && duration_secs > 0.0 &&
                        saved_secs / duration_secs >= 0.80;
  if (fully_watched || near_end || pos_invalid) return 0.0;
  const double start = saved_secs - resume_offset_sec;
  return start > 0.0 ? start : 0.0;
}

// ── socket path (unique per launch, 0700 dir) ───────────────────────────────

namespace detail {

std::string socket_path(std::string_view dir) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  const unsigned uid = static_cast<unsigned>(::getuid());
  const long pid = static_cast<long>(::getpid());
  std::string p(dir);
  if (!p.empty() && p.back() != '/') p.push_back('/');
  p += "shigoku-mpv-" + std::to_string(uid) + "-" + std::to_string(pid) + "-" +
       std::to_string(n) + ".sock";
  return p;
}

// ── IPC observed state ──────────────────────────────────────────────────────

std::optional<Position> Observed::final_position() const {
  if (!meaningful_secs) return std::nullopt;
  return Position{*meaningful_secs, duration};
}

}  // namespace detail

bool meaningful(double secs) { return std::isfinite(secs) && secs > 0.0; }

// ── IPC (push-based, 03 §6.3.1) ─────────────────────────────────────────────

namespace {

// Whether this platform can answer "what pid is the socket peer?" at all. When
// false (an old Darwin SDK with no LOCAL_PEERPID — the symbol landed in 10.8, so
// the 10.6 PPC target lacks it), the squatter-rejection check is skipped rather
// than failing every connect: the runtime socket already lives in a 0700
// uid-owned dir (paths.cpp), so a squatter must be same-uid anyway; the pid
// check is defense-in-depth on top of that, and losing it degrades to the exact
// pre-hardening behavior instead of breaking IPC (and thus resume) entirely.
#if defined(__linux__) || (defined(__APPLE__) && defined(LOCAL_PEERPID))
constexpr bool kPeerPidSupported = true;
#else
constexpr bool kPeerPidSupported = false;
#endif

// PID of the process on the other end of a connected AF_UNIX socket, or nullopt
// if the kernel cannot answer (treated as "not our child"). Linux SO_PEERCRED;
// macOS LOCAL_PEERPID (LOCAL_PEERCRED's xucred carries no pid). (player.rs:350)
// On a platform where kPeerPidSupported is false this is never called.
std::optional<std::uint32_t> peer_pid(int fd) {
#if defined(__linux__)
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return std::nullopt;
  }
  if (cred.pid <= 0) return std::nullopt;
  return static_cast<std::uint32_t>(cred.pid);
#elif defined(__APPLE__) && defined(LOCAL_PEERPID)
  pid_t pid = 0;
  socklen_t len = sizeof(pid);
  if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) != 0) {
    return std::nullopt;
  }
  if (pid <= 0) return std::nullopt;
  return static_cast<std::uint32_t>(pid);
#else
  (void)fd;
  return std::nullopt;
#endif
}

}  // namespace

namespace detail {

// Connect to `path`, retrying on the ~2s budget. Binds trust to the child pid,
// not the path: a same-uid squatter that won the socket would otherwise feed
// the watcher forged positions straight into the history DB. `gone` short-
// circuits the budget once mpv has exited. Returns a connected fd or -1.
// (player.rs:326)
int connect_ipc(std::string_view path, const std::atomic<bool>& gone,
                std::uint32_t child_pid) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) return -1;
  std::memcpy(addr.sun_path, path.data(), path.size());
  addr.sun_path[path.size()] = '\0';

  for (std::uint32_t i = 0; i < kIpcConnectTries; ++i) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
      if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        // Reject any peer that is not our mpv. An impostor holding the path
        // keeps us probing until it or the budget gives out. Where the peer-pid
        // API is unavailable (old Darwin), the check can't run — accept the
        // connection (see kPeerPidSupported) rather than reject-all and break IPC.
        if (!kPeerPidSupported ||
            peer_pid(fd) == std::optional<std::uint32_t>(child_pid)) {
          return fd;
        }
      }
      ::close(fd);
    }
    // A fast exit-2 mpv never creates the socket; stop burning the budget.
    if (gone.load(std::memory_order_relaxed)) return -1;
    std::this_thread::sleep_for(kIpcConnectStep);
  }
  return -1;
}

// Subscribe, then blocking-read newline-delimited property-change events until
// EOF (mpv exit closes the socket). No read timeout on purpose: a paused player
// is silent for minutes. Malformed lines are skipped, never fatal. A line that
// hits the per-line cap without a newline drops the peer. (player.rs:370)
//
// The C++ reader is hand-rolled: a persistent overflow buffer holds bytes read
// past one newline, so many small events across recv() boundaries still flow
// while any single line is capped at kMaxIpcLineBytes.
void watch_ipc(int fd, Observed& observed, std::mutex& observed_mtx,
               const EventSink& on_event) {
  // Handshake: observe time-pos (id 1) and duration (id 2).
  for (auto [id, prop] : {std::pair{1, "time-pos"}, std::pair{2, "duration"}}) {
    const std::string cmd =
        json{{"command", json::array({"observe_property", id, prop})}}.dump() +
        "\n";
    std::size_t off = 0;
    bool write_failed = false;
    while (off < cmd.size()) {
      const ssize_t w = ::write(fd, cmd.data() + off, cmd.size() - off);
      if (w <= 0) {
        write_failed = true;
        break;
      }
      off += static_cast<std::size_t>(w);
    }
    if (write_failed) return;
  }

  std::string pending;  // bytes read past the last newline.
  std::string line;
  char buf[4096];
  bool eof = false;

  auto handle_line = [&](std::string_view msg_bytes) {
    json msg = json::parse(msg_bytes, nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded()) return;  // not json — skip.
    auto ev = msg.find("event");
    if (ev == msg.end() || !ev->is_string() ||
        ev->get_ref<const std::string&>() != "property-change") {
      return;
    }
    std::optional<double> data;
    if (auto d = msg.find("data"); d != msg.end() && d->is_number()) {
      data = d->get<double>();
    }
    auto idit = msg.find("id");
    if (idit == msg.end() || !idit->is_number_unsigned()) return;
    const std::uint64_t id = idit->get<std::uint64_t>();
    if (id == 1) {
      if (!data) return;
      const double secs = *data;
      std::optional<double> duration;
      {
        std::lock_guard<std::mutex> lk(observed_mtx);
        if (meaningful(secs)) observed.meaningful_secs = secs;
        duration = observed.duration;
      }
      on_event(PlayerEvent::position(Position{secs, duration}));
    } else if (id == 2) {
      if (data) {
        std::lock_guard<std::mutex> lk(observed_mtx);
        observed.duration = *data;
      }
    }
  };

  while (!eof) {
    // Drain complete lines already buffered in `pending`.
    std::size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      line.assign(pending, 0, nl + 1);  // include the '\n', as read_until does.
      pending.erase(0, nl + 1);
      handle_line(line);
    }
    // A newline-starved buffer at/over the cap is an oversize line: drop peer.
    if (pending.size() >= kMaxIpcLineBytes) return;

    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) {  // 0 = EOF (mpv exit); <0 = dead socket.
      eof = true;
      break;
    }
    pending.append(buf, static_cast<std::size_t>(n));
  }
}

// ── retry law (04 §7.8) ─────────────────────────────────────────────────────

// Only exit 2 (mpv MpvOpenFailed) with no meaningful play yet retries; hard
// errors from the attempt abort immediately. Injectable seams (attempt /
// on_retry / backoff) keep the law unit-testable offline. (player.rs:137)
Result<PlayOutcome, PlayError> run_attempts(
    const std::function<Result<AttemptResult, PlayError>(std::uint32_t)>&
        attempt,
    const std::function<void(std::uint32_t)>& on_retry,
    const std::function<void(std::chrono::seconds)>& backoff) {
  for (std::uint32_t n = 1; n <= kMaxPlayAttempts; ++n) {
    auto r = attempt(n);
    if (!r) return err(std::move(r.error()));
    const AttemptResult& result = *r;

    const bool open_failed =
        result.exit_code == 2 && !result.position.has_value();
    if (!open_failed) {
      if (!result.position.has_value() && !result.clean_exit) {
        return err(PlayError::exit(std::to_string(result.exit_code)));
      }
      return PlayOutcome{result.position, n};
    }
    if (n < kMaxPlayAttempts) {
      on_retry(n + 1);
      backoff(kBackoff[n - 1]);
    }
  }
  return err(PlayError::open_failed(kMaxPlayAttempts));
}

// ── mplayer slave watcher (P39 slice 3) ─────────────────────────────────────

std::optional<double> parse_slave_answer(std::string_view line, std::string_view key) {
  // "ANS_<key>=<float>" — exact prefix, no whitespace tolerance (mplayer
  // emits none), strtod over the remainder with a full-consume check so
  // "ANS_time_pos=12.3trailer" is rejected, not truncated. Locale note (§3):
  // slave answers always use '.'; strtod under a comma locale would stop at
  // the point — the full-consume check turns that into a clean reject rather
  // than a silent 12.
  if (line.size() < 4 + key.size() + 1) return std::nullopt;
  if (line.compare(0, 4, "ANS_") != 0) return std::nullopt;
  if (line.compare(4, key.size(), key) != 0) return std::nullopt;
  if (line[4 + key.size()] != '=') return std::nullopt;
  const std::string num(line.substr(4 + key.size() + 1));
  if (num.empty()) return std::nullopt;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(num.c_str(), &end);
  if (end != num.c_str() + num.size() || errno == ERANGE) return std::nullopt;
  if (!std::isfinite(v)) return std::nullopt;
  return v;
}

void watch_slave(int cmd_fd, int ans_fd, Observed& observed, std::mutex& observed_mtx,
                 const EventSink& on_event, const std::atomic<bool>& gone) {
  // A write can race the child's exit; with no MSG_NOSIGNAL analogue for
  // pipes, SIGPIPE must be blocked THREAD-LOCALLY so the write returns EPIPE
  // instead of killing the app. The pending signal dies with this thread.
  sigset_t sigpipe_set;
  sigemptyset(&sigpipe_set);
  sigaddset(&sigpipe_set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &sigpipe_set, nullptr);

  // The timed wait (the ~1s poll cadence AND the `gone` responsiveness bound):
  // select on Darwin, poll elsewhere — same §3 seam as term.cpp.
  const auto readable = [](int fd, int timeout_ms) -> int {
#if defined(__APPLE__)
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
#else
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms);
#endif
  };

  const auto send = [&](std::string_view cmd) -> bool {
    std::size_t off = 0;
    while (off < cmd.size()) {
      const ssize_t w = ::write(cmd_fd, cmd.data() + off, cmd.size() - off);
      if (w <= 0) return false;  // EPIPE/error: child gone.
      off += static_cast<std::size_t>(w);
    }
    return true;
  };

  std::string pending;
  char buf[4096];

  auto handle_line = [&](std::string_view line) {
    if (const auto secs = parse_slave_answer(line, "time_pos")) {
      std::optional<double> duration;
      {
        std::lock_guard<std::mutex> lk(observed_mtx);
        if (meaningful(*secs)) observed.meaningful_secs = *secs;
        duration = observed.duration;
      }
      on_event(PlayerEvent::position(Position{*secs, duration}));
    } else if (const auto len = parse_slave_answer(line, "LENGTH")) {
      std::lock_guard<std::mutex> lk(observed_mtx);
      observed.duration = *len;
    }
    // Anything else (status chatter -quiet lets through, unknown ANS_*): skip.
  };

  // Reads one buffered chunk into `pending` and handles complete lines.
  // Returns false on EOF or a hard error (nothing more will ever arrive).
  const auto drain_once = [&]() -> bool {
    const ssize_t n = ::read(ans_fd, buf, sizeof(buf));
    if (n <= 0) return false;  // EOF: mplayer exited (or hard error).
    pending.append(buf, static_cast<std::size_t>(n));
    std::size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      handle_line(std::string_view(pending).substr(0, nl));
      pending.erase(0, nl + 1);
    }
    return true;
  };

  while (!gone.load(std::memory_order_relaxed)) {
    // One poll round: position always; duration only until it lands (it never
    // changes mid-file, and the extra round-trip is chatter on a G4).
    bool need_len;
    {
      std::lock_guard<std::mutex> lk(observed_mtx);
      need_len = !observed.duration.has_value();
    }
    if (!send("get_time_pos\n")) break;
    if (need_len && !send("get_time_length\n")) break;

    // Gather answers for ~1s in 100ms slices: the cadence between commands,
    // bounded so `gone` (waitpid returned) is noticed fast at quit.
    for (int slice = 0; slice < 10; ++slice) {
      if (gone.load(std::memory_order_relaxed)) break;
      const int pr = readable(ans_fd, 100);
      if (pr < 0) {
        if (errno == EINTR) continue;
        return;
      }
      if (pr == 0) continue;
      if (!drain_once()) return;  // EOF already delivered every byte.
    }
  }

  // `gone` (waitpid returned) or a dead command pipe: the child may have
  // answered the last get_time_pos JUST before exiting, and that answer —
  // still buffered in the pipe — is the checkpoint the A6 law records.
  // Zero-timeout drain until empty/EOF so it is never lost to the race.
  while (readable(ans_fd, 0) > 0) {
    if (!drain_once()) break;
  }
}

}  // namespace detail

// ── backend seam (P39) ──────────────────────────────────────────────────────

namespace {

using detail::AttemptResult;
using detail::Observed;

// How a backend reports playback positions (§9 P39, the "optional position
// source"). The shared machinery — guard/proxy walk, spawn, waitpid, the A6
// retry law — is backend-agnostic; this enum is the one switch it keys on.
enum class PositionSource {
  IpcSocket,   // mpv: JSON IPC over a unix socket the player itself binds.
  SlavePipes,  // mplayer: -slave over the child's stdin/stdout pipes; the
               // watcher polls get_time_pos and parses ANS_time_pos= answers.
  None,        // launch-only (QMPlay2): spawn + waitpid; no positions ever,
               // so the A6 recordPlay gate lawfully stays shut.
};

// One backend = the three per-player variation points (§9 P39): argv
// construction (stream + local), the position source, and the default binary.
struct BackendSpec {
  std::string_view name;            // for logs / error detail.
  std::string_view default_binary;  // when PlayOpts carries no explicit path.
  PositionSource source;
  Result<std::vector<std::string>, PlayError> (*stream_argv)(
      const StreamLink&, std::string_view, const PlayOpts&, std::string_view);
  Result<std::vector<std::string>, PlayError> (*local_argv)(std::string_view,
                                                            const PlayOpts&,
                                                            std::string_view);
};

constexpr BackendSpec kMpvSpec{"mpv", "mpv", PositionSource::IpcSocket,
                               &build_argv, &build_local_argv};
constexpr BackendSpec kMplayerSpec{"mplayer", "mplayer", PositionSource::SlavePipes,
                                   &build_mplayer_argv, &build_mplayer_local_argv};
constexpr BackendSpec kQmplay2Spec{"qmplay2", "QMPlay2", PositionSource::None,
                                   &build_qmplay2_argv, &build_qmplay2_local_argv};

// Kind -> spec. parse_backend already snapped unknown config strings to Mpv,
// so this switch is total over kinds that actually reach it.
const BackendSpec& backend_spec(BackendKind kind) {
  switch (kind) {
    case BackendKind::Mplayer: return kMplayerSpec;
    case BackendKind::Qmplay2: return kQmplay2Spec;
    case BackendKind::Mpv: break;
  }
  return kMpvSpec;
}

// The binary that spawns: mpv reads opts.mpv_path (untouched by P39, §9);
// alternates read opts.player_path, falling back to the spec's default name.
std::string backend_binary(const BackendSpec& spec, const PlayOpts& opts) {
  if (&spec == &kMpvSpec) return opts.mpv_path;
  return opts.player_path.empty() ? std::string(spec.default_binary)
                                  : opts.player_path;
}

// ── one attempt: spawn + observe ────────────────────────────────────────────

// Spawn the player with a built argv, null stdio (or it fights the TUI for
// the terminal it inherited), observe positions per the backend's source, and
// wait. The spawn/observe/wait half shared by the streamed and local arms
// (P35 slice 4 split; P39 slice 1 generalizes it over BackendSpec — the mpv
// path's behavior is byte-identical). `ipc` is the rendezvous the argv was
// built with (socket path for IpcSocket, "" otherwise).
Result<AttemptResult, PlayError> run_child_attempt(const BackendSpec& spec,
                                                   const std::vector<std::string>& argv,
                                                   const std::string& binary,
                                                   const std::string& ipc,
                                                   const EventSink& on_event) {
  // Clear a stale or pre-planted file so the player binds its own socket
  // rather than failing on an existing path. Squatter forgery is caught by
  // the peer-cred check in connect_ipc, not by this unlink.
  if (spec.source == PositionSource::IpcSocket) ::unlink(ipc.c_str());

  // SlavePipes: the -slave protocol rides the child's own stdin/stdout —
  // cmd_pipe carries commands down, ans_pipe carries ANS_* answers back.
  // Every end is CLOEXEC in the parent (adddup2 below clears it on the two
  // fds the child actually keeps, per POSIX dup2 semantics), so a later
  // spawn from another thread can never inherit a stray pipe end.
  int cmd_pipe[2] = {-1, -1};
  int ans_pipe[2] = {-1, -1};
  const auto close_pipes = [&](bool child_ends, bool parent_ends) {
    if (child_ends) {
      if (cmd_pipe[0] >= 0) ::close(cmd_pipe[0]);
      if (ans_pipe[1] >= 0) ::close(ans_pipe[1]);
      cmd_pipe[0] = ans_pipe[1] = -1;
    }
    if (parent_ends) {
      if (cmd_pipe[1] >= 0) ::close(cmd_pipe[1]);
      if (ans_pipe[0] >= 0) ::close(ans_pipe[0]);
      cmd_pipe[1] = ans_pipe[0] = -1;
    }
  };
  if (spec.source == PositionSource::SlavePipes) {
    if (::pipe(cmd_pipe) != 0 || ::pipe(ans_pipe) != 0) {
      const int e = errno;
      close_pipes(true, true);
      return err(PlayError::spawn(std::string("pipe: ") + std::strerror(e)));
    }
    for (const int fd : {cmd_pipe[0], cmd_pipe[1], ans_pipe[0], ans_pipe[1]}) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
  }

  // Build the C argv: [binary, argv..., nullptr].
  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 2);
  cargv.push_back(const_cast<char*>(binary.c_str()));
  for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  // Child stdio via file actions: the slave pipes when the backend speaks
  // over them, /dev/null otherwise (or it fights the TUI for the terminal it
  // inherited); stderr is always /dev/null.
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (spec.source == PositionSource::SlavePipes) {
    posix_spawn_file_actions_adddup2(&fa, cmd_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, ans_pipe[1], STDOUT_FILENO);
  } else {
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  }
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  pid_t child = -1;
  const int rc = ::posix_spawnp(&child, binary.c_str(), &fa, nullptr,
                                cargv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  // The child's ends are its own now (dup'd onto 0/1); the parent copies
  // close either way — on spawn failure everything closes.
  close_pipes(true, rc != 0);
  if (rc != 0) {
    // rc IS the errno value (posix_spawnp returns it directly, doesn't set the
    // global errno) — check it structurally rather than sniffing strerror text
    // (debt #5, P22): ENOENT means the player isn't on PATH. The kind stays
    // MpvNotFound for every backend (renaming would ripple through the P27
    // copy tables); the detail names the actual binary.
    if (rc == ENOENT) {
      return err(PlayError::mpv_not_found(binary + ": " + std::strerror(rc)));
    }
    return err(PlayError::spawn(binary + ": " + std::strerror(rc)));
  }
  shigoku::debug_log("play: spawned " + std::string(spec.name) +
                     " pid=" + std::to_string(child));

  Observed observed;
  std::mutex observed_mtx;
  std::atomic<bool> gone{false};
  const std::uint32_t child_pid = static_cast<std::uint32_t>(child);

  // The position source (P39 seam): mpv's IPC-socket watcher, mplayer's
  // slave-pipe poller; None spawns no watcher at all — the A6 gate simply
  // stays shut (no positions ever).
  std::thread watcher;
  if (spec.source == PositionSource::IpcSocket) {
    watcher = std::thread([&] {
      const int fd = detail::connect_ipc(ipc, gone, child_pid);
      shigoku::debug_log("play: ipc connect fd=" + std::to_string(fd));
      if (fd >= 0) {
        detail::watch_ipc(fd, observed, observed_mtx, on_event);
        shigoku::debug_log("play: ipc watcher saw EOF");
        ::close(fd);
      }
    });
  } else if (spec.source == PositionSource::SlavePipes) {
    watcher = std::thread([&] {
      detail::watch_slave(cmd_pipe[1], ans_pipe[0], observed, observed_mtx,
                          on_event, gone);
      shigoku::debug_log("play: slave watcher done");
    });
  }

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  shigoku::debug_log("play: waitpid=" + std::to_string(waited) +
                     " status=" + std::to_string(status));
  gone.store(true, std::memory_order_relaxed);
  if (watcher.joinable()) {
    watcher.join();
    shigoku::debug_log("play: watcher joined");
  }
  // Parent pipe ends outlive the watcher (it reads/writes them), not the
  // child — closed only after the join.
  close_pipes(false, true);

  // Best-effort: the socket is dead once the player exits; a leftover file is
  // inert.
  if (spec.source == PositionSource::IpcSocket) ::unlink(ipc.c_str());

  if (waited < 0) {
    return err(PlayError::wait(std::strerror(errno)));
  }

  AttemptResult out;
  {
    std::lock_guard<std::mutex> lk(observed_mtx);
    out.position = observed.final_position();
  }
  // Normalize exit to the code() / success() semantics run_attempts keys on:
  // a signalled death is neither exit-2 nor a clean exit.
  if (WIFEXITED(status)) {
    out.exit_code = WEXITSTATUS(status);
    out.clean_exit = (out.exit_code == 0);
  } else {
    out.exit_code = -1;  // signalled / abnormal: not open-failed, not clean.
    out.clean_exit = false;
  }
  shigoku::debug_log("play: attempt returning exit=" +
                     std::to_string(out.exit_code));
  return out;
}

// The streamed arm: guard the resolved url, engage the decloak proxy, build
// the vetted argv, then the shared spawn/observe/wait. Backend-agnostic on
// purpose (P39): the guard and the P12 proxy hop protect EVERY backend's
// stream the same way — the seam varies only argv + position source.
Result<AttemptResult, PlayError> attempt_play(const BackendSpec& spec, const StreamLink& link,
                                              const PlayOpts& opts,
                                              const EventSink& on_event) {
  // Guard the UPSTREAM url BEFORE spawn — always, even under the decloak proxy.
  // The proxy loopback hop must never become a guard bypass: it is the upstream
  // url that a hostile playlist could point at a private IP, and it is vetted
  // here (and again per-hop inside the proxy's fetch_upstream).
  if (auto g = http::guard_fetch_url(link.url); !g) {
    return err(PlayError::unsafe_url(g.error()));
  }
  // Engage the de-cloaking proxy for a flagged link (megaplay-shaped segments,
  // P12); a pass-through otherwise (senshi/anibd: decloak_segments=false, so the
  // play url IS link.url). The guard owns the proxy for exactly this attempt's
  // mpv lifetime — mpv opens guard.url() and the proxy tears down when `decloak`
  // drops at function exit, AFTER the shared wait returns ("proxy: stop" in
  // proxy.cpp logs that window).
  auto decloak = proxy::engage(link);
  if (!decloak) {
    return err(PlayError::spawn("decloak proxy: " + decloak.error().detail));
  }
  const std::string play_url(decloak->url());
  const std::string ipc = spec.source == PositionSource::IpcSocket
                              ? detail::socket_path(opts.socket_dir)
                              : std::string();

  auto argv_r = spec.stream_argv(link, play_url, opts, ipc);
  if (!argv_r) return err(std::move(argv_r.error()));
  return run_child_attempt(spec, *argv_r, backend_binary(spec, opts), ipc, on_event);
}

// The local arm (P35 slice 4): no upstream url, so no guard and no proxy —
// just the local argv and the shared spawn/observe/wait.
Result<AttemptResult, PlayError> attempt_play_local(const BackendSpec& spec,
                                                    const std::string& path,
                                                    const PlayOpts& opts,
                                                    const EventSink& on_event) {
  const std::string ipc = spec.source == PositionSource::IpcSocket
                              ? detail::socket_path(opts.socket_dir)
                              : std::string();
  auto argv_r = spec.local_argv(path, opts, ipc);
  if (!argv_r) return err(std::move(argv_r.error()));
  return run_child_attempt(spec, *argv_r, backend_binary(spec, opts), ipc, on_event);
}

}  // namespace

// ── play(): resolve -> attempt -> retry ─────────────────────────────────────

Result<PlayOutcome, PlayError> play(const PlayOpts& opts,
                                    const ResolveFn& resolve,
                                    const EventSink& on_event) {
  // Ensure the socket dir exists at 0700 (unique-per-launch paths land in it).
  // Best-effort: an existing dir with tighter perms is left as-is.
  ::mkdir(opts.socket_dir.c_str(), 0700);

  const BackendSpec& spec = backend_spec(opts.backend);
  return detail::run_attempts(
      [&](std::uint32_t) -> Result<AttemptResult, PlayError> {
        auto link = resolve();
        if (!link) return err(PlayError::resolve(std::move(link.error())));
        return attempt_play(spec, *link, opts, on_event);
      },
      [&](std::uint32_t attempt) {
        on_event(PlayerEvent::retry(attempt));
      },
      [](std::chrono::seconds d) { std::this_thread::sleep_for(d); });
}

Result<PlayOutcome, PlayError> play_local(const PlayOpts& opts,
                                          const std::string& path,
                                          const EventSink& on_event) {
  ::mkdir(opts.socket_dir.c_str(), 0700);
  const BackendSpec& spec = backend_spec(opts.backend);
  // The same retry law as play() (a corrupt file's exit-2 spends the bounded
  // budget and lands on the same OpenFailed) — one law, one outcome mapping.
  return detail::run_attempts(
      [&](std::uint32_t) -> Result<AttemptResult, PlayError> {
        return attempt_play_local(spec, path, opts, on_event);
      },
      [&](std::uint32_t attempt) {
        on_event(PlayerEvent::retry(attempt));
      },
      [](std::chrono::seconds d) { std::this_thread::sleep_for(d); });
}

}  // namespace shigoku::player
