// player.hpp — mpv spawn + push-based JSON IPC + the play retry policy (P5).
//
// Ported from sabigoku src/player.rs (03 §6.3.1 is the normative table, A6).
// StreamLink in, Position events out. Progress writes stay caller-side so the
// 02 §4b recordPlay gate has one owner (tui::workers glue, P7); play() only
// reports positions and the final meaningful one via PlayOutcome.
//
// v0 narrowing vs player.rs (recorded in NOTES.md P5):
//   * No de-cloaking proxy (§2.3): senshi sets decloak_segments=false, so the
//     play url IS link.url — `build_argv` still takes a separate `play_url` so
//     the M2 proxy seam is additive (a decloak provider would pass the loopback
//     url here), but v0 never engages one.
//   * AniSkip (§2.2) lands at P22: SkipScript in PlayOpts / argv (aniskip.hpp).
//
// PlayError lives in error.hpp (P1); PlayerEvent is player-local (the tui
// worker maps it to the Event variant's PositionUpdate / PlayRetry in P7).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "result.hpp"

namespace shigoku::player {

// Retry budget (A6, player.rs:19). Only mpv exit-2 with no meaningful playback
// yet is retry-eligible; each retry re-resolves a fresh url.
inline constexpr std::uint32_t kMaxPlayAttempts = 3;

// One backoff gap per retry; indexing by attempt-1 is safe only while the
// length stays kMaxPlayAttempts-1. Keep them in lockstep (player.rs:22).
inline constexpr std::chrono::seconds kBackoff[kMaxPlayAttempts - 1] = {
    std::chrono::seconds(2), std::chrono::seconds(4)};

// Connect budget ~2s (03 §6.3.1): mpv creates the socket only after argv parse.
inline constexpr std::uint32_t kIpcConnectTries = 40;
inline constexpr std::chrono::milliseconds kIpcConnectStep{50};

// Ceiling on one IPC line (player.rs:30). Real property-change events are
// ~100 bytes; a peer streaming an endless line would otherwise grow the read
// buffer to a process-wide OOM. A full-cap read with no newline drops the peer.
inline constexpr std::uint64_t kMaxIpcLineBytes = 64u * 1024u;

// A single observed playback position. `duration` is nullopt until mpv learns
// it (never, for some live streams).
struct Position {
  double secs = 0.0;
  std::optional<double> duration;
  friend bool operator==(const Position&, const Position&) = default;
};

// "Meaningful" position = finite and > 0 (A6). Gates retry eligibility and the
// final-position persistence (02 §4b). Exposed for the golden test.
[[nodiscard]] bool meaningful(double secs);

// Push-based player event. The tui worker (P7) maps Position -> PositionUpdate
// and Retry -> PlayRetry on the Event queue; player itself stays queue-agnostic.
struct PlayerEvent {
  enum class Kind {
    Position,  // a time-pos property-change (carries `pos`)
    Retry,     // a retry is scheduled (carries `attempt`); fired BEFORE the
               // backoff sleep so the UI shows it during the wait.
  };
  Kind kind;
  Position pos;            // meaningful only for Position.
  std::uint32_t attempt = 0;  // meaningful only for Retry (the upcoming n).

  static PlayerEvent position(Position p) { return {Kind::Position, p, 0}; }
  static PlayerEvent retry(std::uint32_t n) { return {Kind::Retry, {}, n}; }
  friend bool operator==(const PlayerEvent&, const PlayerEvent&) = default;
};

// The event sink. Called from the IPC watcher thread (Position) and the retry
// loop (Retry); must be safe to invoke from another thread and copyable (each
// attempt hands a copy to its watcher). std::function satisfies the bound.
using EventSink = std::function<void(const PlayerEvent&)>;

// Resolve callback: runs once per attempt so every retry fires on a fresh url
// (04 §7.8). Err carries a provider message that becomes PlayError::Resolve.
using ResolveFn =
    std::function<Result<StreamLink, std::string>()>;

struct PlayOutcome {
  // Last meaningful position (finite, > 0). nullopt = the recordPlay gate stays
  // shut (02 §4b): zero store writes for this play.
  std::optional<Position> position;
  std::uint32_t attempts = 0;
};

// mpv auto-skip wiring (P22, 03 §9): the script path and its --script-opts
// payload. Both are app-constructed (cache path + formatted floats), never
// provider bytes, so they ride argv without the provider-field vetting.
struct SkipScript {
  std::string path;
  std::string opts;
  friend bool operator==(const SkipScript&, const SkipScript&) = default;
};

// P39 (§9): which player runs the spawned playback. mpv is the default AND
// the reference — full tracking (JSON IPC), aniskip, the exit-2 retry law.
// Alternates are optional playback targets; features beyond playback are
// explicitly not guaranteed outside mpv (no "player parity" concept).
enum class BackendKind { Mpv, Mplayer, Qmplay2 };

// Config string -> kind. Anything unrecognized (including a value written by
// a newer build) plays as mpv — the same "unknown re-enters at mpv" degrade
// the Settings wheel pins (§9 P39 slice 2), so a stale config never bricks
// playback.
[[nodiscard]] BackendKind parse_backend(std::string_view name);

struct PlayOpts {
  std::string mpv_path;
  // P39: the backend that plays, and (alternates only) its binary path.
  // mpv keeps reading mpv_path — the two fields never mix (§9 P39: "mpv_path
  // untouched"). Empty player_path = the kind's default binary name.
  BackendKind backend = BackendKind::Mpv;
  std::string player_path;
  // IPC socket dir (paths.runtime); passed in so player stays path-agnostic.
  // play() ensures it exists at 0700 before deriving a socket path in it.
  std::string socket_dir;
  std::string title;
  // Resume start (03 §6.3.1 rule; compute with resume_start() caller-side).
  // Emitted only when finite and > 0.
  double start_secs = 0.0;
  // AniSkip adjunct (P22, 03 §9), prepared caller-side; nullopt = plain play.
  std::optional<SkipScript> skip;
};

// resume_start (03 §6.3.1, A6) — the resume rule as a pure function:
//   start = 0  if fully watched, or pos/duration >= 0.80, or pos invalid;
//   else max(0, saved - resume_offset_sec).
// `saved`/`duration` are the store's last checkpoint; `duration<=0` means
// unknown. Pure and total so it unit-tests offline.
[[nodiscard]] double resume_start(double saved_secs, double duration_secs,
                                  bool fully_watched,
                                  double resume_offset_sec = 5.0);

// build_argv (03 §6.3.1, the table is law) — pure, unit-testable. `play_url` is
// the positional handed to mpv (== link.url in v0; the proxy loopback url under
// a future decloak provider). Rejects CR/LF / non-printable bytes in every
// provider-derived field (ROD-92 header injection). Err = PlayError::UnsafeArg
// with the offending field name in `detail`.
[[nodiscard]] Result<std::vector<std::string>, PlayError> build_argv(
    const StreamLink& link, std::string_view play_url, const PlayOpts& opts,
    std::string_view socket_path);

// build_local_argv (P35 slice 4) — build_argv's tail half (title / ipc /
// start / skip / positional) for a completed local download. The path is
// app-built (download::episode_dest over the config download dir), never
// provider bytes, so the ROD-92 per-field vetting does NOT apply — spaces
// and UTF-8 in a user's download_dir must play. The one argv hazard left is
// a leading '-' reading as a flag (a relative download_dir could produce
// one); such a path is fenced with "./" rather than rejected.
[[nodiscard]] Result<std::vector<std::string>, PlayError> build_local_argv(
    std::string_view path, const PlayOpts& opts, std::string_view socket_path);

// mplayer argv (P39 slice 3) — deliberately minimal next to mpv's table (§9:
// alternates get playback, nothing beyond it): -slave -quiet, -ss for the
// resume law, the link's UA/Referer when present, then the vetted positional.
// Same ROD-435/ROD-92 per-field vetting as build_argv. The trailing
// string_view is the ipc rendezvous slot the BackendSpec signature carries;
// slave pipes need none, so it is ignored.
[[nodiscard]] Result<std::vector<std::string>, PlayError> build_mplayer_argv(
    const StreamLink& link, std::string_view play_url, const PlayOpts& opts,
    std::string_view ipc);
[[nodiscard]] Result<std::vector<std::string>, PlayError> build_mplayer_local_argv(
    std::string_view path, const PlayOpts& opts, std::string_view ipc);

// QMPlay2 argv (P39 slice 4) — launch-only: JUST the vetted positional (no
// portable flags exist for headers, start offset, or a position interface).
// The local arm keeps the "./" leading-dash fence from build_local_argv.
[[nodiscard]] Result<std::vector<std::string>, PlayError> build_qmplay2_argv(
    const StreamLink& link, std::string_view play_url, const PlayOpts& opts,
    std::string_view ipc);
[[nodiscard]] Result<std::vector<std::string>, PlayError> build_qmplay2_local_argv(
    std::string_view path, const PlayOpts& opts, std::string_view ipc);

// One full play: resolve -> guard -> spawn mpv -> observe IPC -> retry per the
// A6 law. Blocking (returns when mpv exits or all attempts are spent). Events
// stream via `on_event` from the watcher thread; the final meaningful position
// rides PlayOutcome. `resolve` runs once per attempt.
[[nodiscard]] Result<PlayOutcome, PlayError> play(const PlayOpts& opts,
                                                  const ResolveFn& resolve,
                                                  const EventSink& on_event);

// play_local (P35 slice 4): play a completed local download — the same spawn
// / IPC-observe / exit-and-retry law as play() (checkpoints, ratchet and
// aniskip all work on a local file), with resolve, the SSRF guard and the
// decloak proxy all skipped: there is no upstream url, and the path is
// app-built from the download-dir convention, never provider bytes.
[[nodiscard]] Result<PlayOutcome, PlayError> play_local(const PlayOpts& opts,
                                                        const std::string& path,
                                                        const EventSink& on_event);

// --- Internals, exposed for the golden tests (player.rs mod tests) ----------
namespace detail {

// Observed IPC state: last meaningful time-pos (the recordPlay gate hangs off
// this) and the last known duration. Guarded by an external mutex when the
// watcher thread and the waiter both touch it.
struct Observed {
  std::optional<double> meaningful_secs;
  std::optional<double> duration;
  [[nodiscard]] std::optional<Position> final_position() const;
};

// One attempt's outcome, normalized to what the retry law keys on:
// `exit_code` == 2 with no position retries; `clean_exit` (exit 0) with no
// position is the gate-shut Ok path; any other no-position exit is a hard Exit.
struct AttemptResult {
  std::optional<Position> position;
  int exit_code = 0;
  bool clean_exit = false;
};

// A unique per-launch socket path in `dir`: shigoku-mpv-<uid>-<pid>-<n>.sock.
[[nodiscard]] std::string socket_path(std::string_view dir);

// Connect to `path` on the ~2s budget, adopting only a peer whose pid ==
// `child_pid` (SO_PEERCRED / LOCAL_PEERPID). `gone` short-circuits once mpv has
// exited. Returns a connected fd (caller closes) or -1. Exposed for the
// squatter-reject test.
[[nodiscard]] int connect_ipc(std::string_view path,
                              const std::atomic<bool>& gone,
                              std::uint32_t child_pid);

// Handshake + blocking read loop over a connected IPC fd; posts Position events
// and records the final meaningful position / duration into `observed`.
void watch_ipc(int fd, Observed& observed, std::mutex& observed_mtx,
               const EventSink& on_event);

// "ANS_<key>=<float>" -> the float (P39 slice 3): exact prefix, strtod with a
// full-consume check (trailing junk and a comma-locale half-parse both reject
// cleanly), non-finite rejected. nullopt = not this answer. Exposed for the
// parse-table test.
[[nodiscard]] std::optional<double> parse_slave_answer(std::string_view line,
                                                       std::string_view key);

// The mplayer -slave poll loop (P39 slice 3): writes get_time_pos (and
// get_time_length until a duration lands) down `cmd_fd` on a ~1s cadence,
// parses ANS_* answers from `ans_fd` into `observed` + Position events, and
// returns on EOF, a dead pipe, or `gone` — draining what the child wrote
// before exiting so the final checkpoint answer is never lost to the race.
// Blocks SIGPIPE thread-locally (a command write can race the child's exit).
void watch_slave(int cmd_fd, int ans_fd, Observed& observed, std::mutex& observed_mtx,
                 const EventSink& on_event, const std::atomic<bool>& gone);

// The retry law with injectable seams (attempt / on_retry / backoff) so the
// 04 §7.8 policy unit-tests without spawning mpv.
[[nodiscard]] Result<PlayOutcome, PlayError> run_attempts(
    const std::function<Result<AttemptResult, PlayError>(std::uint32_t)>&
        attempt,
    const std::function<void(std::uint32_t)>& on_retry,
    const std::function<void(std::chrono::seconds)>& backoff);

}  // namespace detail

}  // namespace shigoku::player
