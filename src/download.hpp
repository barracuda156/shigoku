// download.hpp — the headless episode-download core (P35 slices 1+2).
//
// shigoku-only (§9 improvement; sabigoku has no downloads — PORT_PARITY.md
// P35). Direct-file links stream to disk through the A5 wrapper's
// fetch_to_sink: the transfer writes `<dest>.part`, a completed transfer
// fsyncs and atomically renames it to `<dest>`, and an interrupted one leaves
// ONLY the `.part` behind — a corrupt final file must be impossible. An
// existing `.part` resumes via `Range: bytes=<size>-` when the server
// cooperates (206); a server that answers a ranged request with a full 200
// restarts the file cleanly (truncate, not append). HLS links go through
// external `ffmpeg -c copy` spawned the way player.cpp spawns mpv (vetted
// argv, posix_spawnp, null stdio), under the same .part/rename law. A master
// playlist is read first (ladder.hpp) and the asked-for rung taken off it:
// the video variant, plus its audio rendition as a second input when the
// audio is separate. A relayed link is read through its own proxy, so its
// variants can be picked like any other's.
//
// download_link is the policy entry (it owns the SSRF guard); the lower-level
// cores are mechanism — like player::build_argv's play_url, their url inputs
// must already have passed http::guard_fetch_url (the loopback test fixtures
// depend on the cores not re-guarding). probe() answers what a link offers
// before anything is fetched.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "hls.hpp"
#include "http.hpp"
#include "offer.hpp"
#include "proxy.hpp"
#include "result.hpp"

namespace shigoku::download {

// The download failure classes — the closed taxonomy the whole pipeline
// (curl arm AND ffmpeg arm) reports through, so the slice-3 toast mapping is
// one exhaustive switch. Every arm leaves `dest` untouched; whether the
// `.part` survives is per-arm (see download_to_file / run_ffmpeg).
struct DownloadError {
  enum class Kind {
    Fetch,           // transport/status failure (`fetch` carries the taxonomy)
    Io,              // open/write/fsync/rename failed (`detail` = op + errno)
    Cancelled,       // the caller's cancel flag stopped it; .part kept
    UnsafeUrl,       // a link/variant url failed the SSRF guard (`guard`)
    UnsafeArg,       // an ffmpeg argv field had banned bytes (`detail` = field)
    FfmpegNotFound,  // spawn ENOENT: the "install ffmpeg" degrade (slice 2)
    Ffmpeg,          // ffmpeg spawn/wait/exit failure (`detail` says which)
  };

  Kind kind;
  ProviderError fetch{ProviderError::Kind::Network, 0, {}};  // Fetch only.
  std::string detail;                    // Io / UnsafeArg / Ffmpeg[NotFound].
  GuardError guard = GuardError::BadUrl;  // UnsafeUrl only.

  static DownloadError fetch_err(ProviderError e) {
    return {Kind::Fetch, std::move(e), {}, GuardError::BadUrl};
  }
  static DownloadError io(std::string what) {
    return {Kind::Io, {ProviderError::Kind::Network, 0, {}}, std::move(what),
            GuardError::BadUrl};
  }
  static DownloadError cancelled() {
    return {Kind::Cancelled, {ProviderError::Kind::Network, 0, {}}, {},
            GuardError::BadUrl};
  }
  static DownloadError unsafe_url(GuardError g) {
    return {Kind::UnsafeUrl, {ProviderError::Kind::Network, 0, {}}, {}, g};
  }
  static DownloadError unsafe_arg(std::string field) {
    return {Kind::UnsafeArg, {ProviderError::Kind::Network, 0, {}},
            std::move(field), GuardError::BadUrl};
  }
  static DownloadError ffmpeg_not_found(std::string why) {
    return {Kind::FfmpegNotFound, {ProviderError::Kind::Network, 0, {}},
            std::move(why), GuardError::BadUrl};
  }
  static DownloadError ffmpeg(std::string why) {
    return {Kind::Ffmpeg, {ProviderError::Kind::Network, 0, {}},
            std::move(why), GuardError::BadUrl};
  }
};

// Progress tick, fired per received chunk from the transfer thread. `bytes` =
// total bytes on disk so far (a resume's prior offset included); `total` =
// final file size when the server sent a Content-Length, else nullopt.
using ProgressFn =
    std::function<void(std::uint64_t bytes, std::optional<std::uint64_t> total)>;

struct Options {
  // Already guarded (header contract): the orchestrator ran guard_fetch_url.
  std::string url;
  // Final path; the transfer itself writes part_path(dest).
  std::string dest;
  // Provider-derived CDN gate headers, same meaning as on StreamLink.
  std::optional<std::string> referer;
  std::string user_agent = http::kBrowserUserAgent;
  // Tests only: a wall rail. Absent = fetch_to_sink's stall rail (a real
  // download must never trip a wall clock).
  std::optional<long> timeout_secs;
};

// `<dest>.part` — the one temp-name convention, shared with the tests and the
// slice-4 play-prefers-local scan (which must NEVER match a .part).
[[nodiscard]] std::string part_path(const std::string& dest);

// Blocking single-file download (the slice-3 worker owns threading; one
// transfer at a time is that layer's law, not this one's).
//
//   Ok            -> dest exists with the full body; the .part is gone.
//   Err Fetch     -> dest untouched; the .part keeps any partial bytes for a
//                    future resume — EXCEPT a 416 answer to a ranged request
//                    (our offset is at/past the server's end, so the .part is
//                    unusable): it is dropped so the next attempt starts clean.
//   Err Io        -> dest untouched; the .part is kept best-effort.
//   Err Cancelled -> dest untouched; the .part keeps the partial (resume).
[[nodiscard]] Result<Unit, DownloadError> download_to_file(
    const http::Client& client, const Options& opts,
    const std::atomic<bool>& cancel, const ProgressFn& progress);

// --- The HLS arm: external ffmpeg (P35 slice 2) ----------------------------

// Direct file vs HLS playlist, by url path extension (query/fragment
// stripped, ASCII case-folded). Unknown -> Direct: curl-to-file serves any
// static file; ffmpeg is only ever needed for playlists.
enum class LinkKind {
  Direct,
  Hls,
};
[[nodiscard]] LinkKind classify_url(std::string_view url);

// The same call for a resolved link, which knows more than its url: a link
// whose segments are cloaked (plain or decoy-prefixed) or whose playlists
// arrive encrypted is HLS whatever its playlist url ends in — a master served
// as `master.txt` must never land on disk as a text file. Otherwise the url
// decides.
[[nodiscard]] LinkKind classify_link(const StreamLink& link);

// Pure ffmpeg argv builder (the golden tests pin this table; the ROD-435
// guard applies as in player::build_argv — every provider-derived field is
// vetted, Err = UnsafeArg with the field name):
//   -nostdin -y -loglevel error
//   <input options> -f hls -i <input_url>
//   [<input options> -f hls -i <audio_url>  -map 0:v:0 -map 1:a:0]
//   -c copy -f mp4 <out_path>
// where <input options>, repeated per input because ffmpeg applies them to
// the next -i only, are
//   [-allowed_extensions ALL]            (link.cloaked_segments, as mpv's
//                                         --demuxer-lavf-o allowed_extensions)
//   [-extension_picky 0]                 (link.cloaked_segments AND
//                                         extension_picky_off)
//   [-headers "Referer: <referer>\r\n"]  (link.referer)
//   [-user_agent <ua>]                   (link.user_agent)
// `input_url` is the positional the caller resolved (variant pick / decloak
// loopback / the link url itself); like build_argv's play_url it must not
// start with '-'. `audio_url`, when non-empty, is the separate audio
// rendition that goes with a video-only input, resolved by the caller the
// same way and vetted the same way (field name "audio_url"): the explicit
// maps take the video from the first input and the audio from the second.
// `out_path` is app-constructed (the .part), unvetted like SkipScript.
// link.sub_url is deliberately ignored: downloads carry the stream as-is,
// softsubs stay a streaming feature.
[[nodiscard]] Result<std::vector<std::string>, DownloadError> build_ffmpeg_argv(
    const StreamLink& link, std::string_view input_url,
    std::string_view out_path, bool extension_picky_off = false,
    std::string_view audio_url = {});

// Whether this ffmpeg's hls demuxer has `extension_picky`. From 7.1.1 it
// defaults on and refuses segment names that don't match their format (the
// cloaked `.jpg` and extensionless ones); an older ffmpeg treats the unknown
// option as fatal, so the flag goes out only on a yes. Asked once per path via
// `-h demuxer=hls`; a yes or no is remembered, a failed ask is retried next
// time.
[[nodiscard]] bool ffmpeg_has_extension_picky(const std::string& ffmpeg_path);

// Spawn ffmpeg (posix_spawnp, null stdio — the mpv shape) and wait, polling
// `cancel` at ~5 Hz: a tripped flag SIGTERMs the child (SIGKILL escalation
// after ~2s) and reports Cancelled. ENOENT = FfmpegNotFound — the runtime-only
// dependency degrade ("install ffmpeg"). Nonzero exit / signalled death =
// Ffmpeg. Like the curl arm, whatever the child wrote stays in the .part on
// every failure path; the caller renames on Ok.
// `child_pid_out`, when non-null, publishes the live child's pid (0 when
// none): the TUI quit path SIGTERMs it directly — downloads have no drain
// rights (A1), so quit cannot wait for this poll loop to notice the flag.
// `on_poll`, when set, runs once per poll while the child lives: ffmpeg
// reports nothing itself, so a caller that wants progress reads it off the
// output file there.
[[nodiscard]] Result<Unit, DownloadError> run_ffmpeg(
    const std::string& ffmpeg_path, const std::vector<std::string>& argv,
    const std::atomic<bool>& cancel, std::atomic<long>* child_pid_out = nullptr,
    const std::function<void()>& on_poll = {});

// --- A transfer, worked out and then run -----------------------------------

// A transfer worked out but not started: which arm, which urls, and — for a
// relayed link — the proxy that has to stay up until it ends (the plan owns
// it; dropping the plan stops it).
struct Plan {
  LinkKind kind = LinkKind::Direct;
  // What is fetched: the link's url, a variant taken off its ladder, or the
  // loopback form of either.
  std::string input;
  // The separate audio rendition fetched beside a video-only input.
  std::optional<std::string> audio;
  // The rung actually taken, when the link had a ladder to take one from:
  // what the transfer will be, as against what was asked for.
  std::optional<hls::Variant> variant;
  std::optional<proxy::Decloak> relay;
};

// Work a transfer out. Contract as download_to_file: link.url must already
// have passed the guard (download_link below owns it).
//   * a direct link is its own plan;
//   * a link play would proxy — decoy-prefixed segments, encrypted
//     playlists, one audio language of several — engages the same proxy, and
//     everything below is read through its loopback;
//   * the playlist is fetched (best effort, link headers): from a master,
//     `rung` picks the variant (hls::pick_variant) and its audio rendition,
//     each re-guarded when it is an upstream url (provider bytes); a media
//     playlist or a failed fetch leaves the link url as the input (never
//     worse than play);
//   * a link that names its own audio rendition (StreamLink::audio_url)
//     keeps it, guarded and relayed like the rest.
// Err: UnsafeUrl for a pick that fails the guard, Io when the proxy cannot
// start.
[[nodiscard]] Result<Plan, DownloadError> plan(const http::Client& client,
                                               const StreamLink& link, const Rung& rung);

// Run a plan into `dest` (through the .part + fsync + atomic-rename law):
// download_to_file for a direct one (link headers, Range resume, byte counts
// with the server's total), ffmpeg for a playlist — `progress` then gets the
// .part's size as it grows, with no total.
[[nodiscard]] Result<Unit, DownloadError> run(
    const http::Client& client, const StreamLink& link, const Plan& plan,
    const std::string& dest, const std::string& ffmpeg_path,
    const std::atomic<bool>& cancel, const ProgressFn& progress,
    std::atomic<long>* child_pid_out = nullptr);

// plan() + run() for an HLS link, for callers that have no use for the plan.
[[nodiscard]] Result<Unit, DownloadError> download_hls(
    const http::Client& client, const StreamLink& link, const std::string& dest,
    const Rung& rung, const std::string& ffmpeg_path,
    const std::atomic<bool>& cancel, std::atomic<long>* child_pid_out = nullptr);

// The one-call entry: guard link.url, then plan() and run(). Blocking.
[[nodiscard]] Result<Unit, DownloadError> download_link(
    const http::Client& client, const StreamLink& link, const std::string& dest,
    const Rung& rung, const std::string& ffmpeg_path,
    const std::atomic<bool>& cancel, const ProgressFn& progress,
    std::atomic<long>* child_pid_out = nullptr);

// --- What a link offers ----------------------------------------------------

// What `link` can deliver, read the way plan() reads it (the same proxy, the
// same playlist), so every row listed is one a transfer can take: a row's
// `rung` handed to plan() lands on that row's variant. Contract as plan():
// link.url already guarded. Never fails — a link whose playlist cannot be
// read answers with its one as-served row.
[[nodiscard]] StreamOffer probe(const http::Client& client, const StreamLink& link);

// The row of an offer a rung lands on: the index plan() would take for it.
// 0 for an offer with one row.
[[nodiscard]] std::size_t offer_row_for(const StreamOffer& offer, const Rung& rung);

// --- The on-disk convention (slices 3+4) -----------------------------------
//
// <download_dir>/<anilist_id>/<track>/<label>.<ext> — the layout `d`/the
// subcommand write and the slice-4 play-prefers-local scan reads. One
// convention, one builder.

// A provider episode label as a single path component: ROD-435-stripped, path
// separators replaced, never empty / "." / ".." (a hostile label must not
// escape the download dir).
[[nodiscard]] std::string safe_episode_component(std::string_view label);

// The final path for one episode. The resolved link picks the extension: an
// HLS link (classify_link) lands as .mp4 (the ffmpeg arm's -f mp4); a direct
// link keeps its url extension when it has a sane one, else .mp4.
[[nodiscard]] std::string episode_dest(std::string_view download_dir,
                                       std::int64_t anilist_id,
                                       Translation translation,
                                       std::string_view episode_label,
                                       const StreamLink& link);

// The same for a show only its source can name (a search hit no catalogue
// entry was found for): its folder is `<provider>-<provider_id>`, so two such
// shows never share one, and none reads as an AniList id (those are plain or
// negative numbers). No play-prefers-local scan looks there.
[[nodiscard]] std::string source_episode_dest(std::string_view download_dir,
                                              std::string_view provider,
                                              std::string_view provider_id,
                                              Translation translation,
                                              std::string_view episode_label,
                                              const StreamLink& link);

// mkdir -p the dest's parent chain (0755). Err = Io with the failing errno.
[[nodiscard]] Result<Unit, DownloadError> ensure_parent_dirs(
    const std::string& dest);

// The play-prefers-local scan (slice 4): the completed local file for (show,
// track, ep), or nullopt. A match is exactly <download_dir>/<id>/<track>/
// <safe-label>.<ext> where <ext> is a single dot-free suffix and never
// "part" — an in-flight or killed transfer's `.part` (and its
// `<label>.<ext>.part` spelling) must never play. Track lives in the path, so
// a dub download never answers a sub play. With several extensions present
// the lexicographically smallest wins (deterministic under any readdir
// order). "" download_dir = downloads disabled = nullopt.
[[nodiscard]] std::optional<std::string> find_local_episode(
    std::string_view download_dir, std::int64_t anilist_id,
    Translation translation, std::string_view episode_label);

// A show's downloads follow it to a new id (a synthetic MAL-only id taking
// its real AniList id): <download_dir>/<old_id>/ becomes <download_dir>/
// <new_id>/. When the new id already has a tree the two merge track by track
// and file by file; whatever the new tree already holds stays, and the old
// copy is left where it was. Best effort, no report: a file that cannot move
// stays under the old id. "" download_dir = downloads disabled = no-op.
void move_show_downloads(std::string_view download_dir, std::int64_t old_id,
                         std::int64_t new_id);

// --- Internals, exposed for the golden tests -------------------------------
namespace detail {

// The `-h demuxer=hls` text lists the extension_picky option.
[[nodiscard]] bool help_lists_extension_picky(std::string_view help);

// The rows a master playlist read from `base` offers, best first: one per
// variant a rung can land on (a variant hls::choose_pair would not hand back
// for its own rung is left out), variants that say the same about themselves
// folded into one. Empty for a media playlist.
[[nodiscard]] std::vector<OfferRow> rows_from_master(std::string_view playlist,
                                                     std::string_view base);

}  // namespace detail

}  // namespace shigoku::download
