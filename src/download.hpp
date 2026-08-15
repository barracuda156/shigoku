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
// argv, posix_spawnp, null stdio), under the same .part/rename law; a master
// playlist gets the P23 variant walk via hls.hpp first, and decloak links
// transfer through the P12 proxy exactly as play does.
//
// download_link is the policy entry (it owns the SSRF guard); the lower-level
// cores are mechanism — like player::build_argv's play_url, their url inputs
// must already have passed http::guard_fetch_url (the loopback test fixtures
// depend on the cores not re-guarding). Surfaces (worker/keybind/subcommand)
// are slice 3.

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
#include "http.hpp"
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

// Pure ffmpeg argv builder (the golden tests pin this table; the ROD-435
// guard applies as in player::build_argv — every provider-derived field is
// vetted, Err = UnsafeArg with the field name):
//   -nostdin -y -loglevel error
//   [-allowed_extensions ALL]            (link.cloaked_segments, as mpv's
//                                         --demuxer-lavf-o allowed_extensions)
//   [-headers "Referer: <referer>\r\n"]  (link.referer)
//   [-user_agent <ua>]                   (link.user_agent)
//   -i <input_url> -c copy -f mp4 <out_path>
// `input_url` is the positional the caller resolved (variant pick / decloak
// loopback / the link url itself); like build_argv's play_url it must not
// start with '-'. `out_path` is app-constructed (the .part), unvetted like
// SkipScript. link.sub_url is deliberately ignored: v1 downloads carry the
// stream as-is, softsubs stay a streaming feature.
[[nodiscard]] Result<std::vector<std::string>, DownloadError> build_ffmpeg_argv(
    const StreamLink& link, std::string_view input_url,
    std::string_view out_path);

// Spawn ffmpeg (posix_spawnp, null stdio — the mpv shape) and wait, polling
// `cancel` at ~5 Hz: a tripped flag SIGTERMs the child (SIGKILL escalation
// after ~2s) and reports Cancelled. ENOENT = FfmpegNotFound — the runtime-only
// dependency degrade ("install ffmpeg", slice 3's toast). Nonzero exit /
// signalled death = Ffmpeg. Like the curl arm, whatever the child wrote stays
// in the .part on every failure path; the caller renames on Ok.
// `child_pid_out`, when non-null, publishes the live child's pid (0 when
// none): the TUI quit path SIGTERMs it directly — downloads have no drain
// rights (A1), so quit cannot wait for this poll loop to notice the flag.
[[nodiscard]] Result<Unit, DownloadError> run_ffmpeg(
    const std::string& ffmpeg_path, const std::vector<std::string>& argv,
    const std::atomic<bool>& cancel, std::atomic<long>* child_pid_out = nullptr);

// Download an HLS link via external ffmpeg -c copy into `dest` (through the
// .part + fsync + atomic-rename law). Contract as download_to_file: link.url
// must already have passed the guard (download_link below owns it).
//   * decloak links (megaplay-shaped segments) engage the P12 proxy for the
//     transfer and hand ffmpeg the loopback url — such providers never cap
//     (the P23 law), so no variant walk.
//   * otherwise the playlist is fetched (best-effort, link headers): a master
//     gets the P23 variant walk — join + hls::select_variant under `quality`,
//     the pick re-guarded (provider bytes) — and a media playlist / failed
//     fetch falls back to the link url untouched (never worse than play).
// No progress ticks in v1: ffmpeg owns the transfer, so the surface shows an
// indeterminate spinner (slice 3).
[[nodiscard]] Result<Unit, DownloadError> download_hls(
    const http::Client& client, const StreamLink& link, const std::string& dest,
    Quality quality, const std::string& ffmpeg_path,
    const std::atomic<bool>& cancel, std::atomic<long>* child_pid_out = nullptr);

// The one entry a surface calls (slice 3): guard link.url, then route by
// classify_url — Direct -> download_to_file (link headers, Range resume),
// Hls -> download_hls (ffmpeg). Blocking; one transfer at a time is the
// caller's law.
[[nodiscard]] Result<Unit, DownloadError> download_link(
    const http::Client& client, const StreamLink& link, const std::string& dest,
    Quality quality, const std::string& ffmpeg_path,
    const std::atomic<bool>& cancel, const ProgressFn& progress,
    std::atomic<long>* child_pid_out = nullptr);

// --- The on-disk convention (slices 3+4) -----------------------------------
//
// <download_dir>/<anilist_id>/<track>/<label>.<ext> — the layout `d`/the
// subcommand write and the slice-4 play-prefers-local scan reads. One
// convention, one builder.

// A provider episode label as a single path component: ROD-435-stripped, path
// separators replaced, never empty / "." / ".." (a hostile label must not
// escape the download dir).
[[nodiscard]] std::string safe_episode_component(std::string_view label);

// The final path for one episode. `resolved_url` picks the extension: an HLS
// link lands as .mp4 (the ffmpeg arm's -f mp4); a direct link keeps its url
// extension when it has a sane one, else .mp4.
[[nodiscard]] std::string episode_dest(std::string_view download_dir,
                                       std::int64_t anilist_id,
                                       Translation translation,
                                       std::string_view episode_label,
                                       std::string_view resolved_url);

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

// --- Internals, exposed for the golden tests -------------------------------
namespace detail {

// Master-playlist text -> the variant url for `quality` (the P23 cap policy
// via hls::select_variant), joined against `base` (senshi::cap_variant's
// shape). nullopt = media playlist / nothing usable: caller keeps the
// original url.
[[nodiscard]] std::optional<std::string> choose_from_master(
    std::string_view playlist, std::string_view base, Quality quality);

}  // namespace detail

}  // namespace shigoku::download
