// download.cpp — headless episode-download core (P35 slices 1+2). See
// download.hpp for the contract; no sabigoku reference (shigoku-only, §9).

#include "download.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "hls.hpp"
#include "proxy.hpp"

extern char** environ;

namespace shigoku::download {

namespace {

std::string errno_text() { return std::strerror(errno); }

}  // namespace

std::string part_path(const std::string& dest) { return dest + ".part"; }

Result<Unit, DownloadError> download_to_file(const http::Client& client,
                                             const Options& opts,
                                             const std::atomic<bool>& cancel,
                                             const ProgressFn& progress) {
  const std::string part = part_path(opts.dest);

  // Resume offset: an existing non-empty .part resumes at its size.
  std::uint64_t offset = 0;
  {
    struct stat st{};
    if (::stat(part.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 0) {
      offset = static_cast<std::uint64_t>(st.st_size);
    }
  }

  http::Request req;
  req.method = http::Method::Get;
  req.url = opts.url;
  req.user_agent = opts.user_agent;
  req.timeout_secs = opts.timeout_secs;
  if (opts.referer) req.extra_headers.push_back({"Referer", *opts.referer});
  if (offset > 0) {
    req.extra_headers.push_back(
        {"Range", "bytes=" + std::to_string(offset) + "-"});
  }

  int fd = -1;
  std::uint64_t written = offset;
  std::optional<std::uint64_t> total;
  std::string io_detail;

  auto sink = [&](const http::StreamMeta& meta, const std::uint8_t* data,
                  std::size_t len) -> bool {
    if (fd < 0) {
      // 206 = the server honored our Range: append to the .part. Anything
      // else 2xx (a 200 answering a ranged request included) is the full
      // body: truncate — appending a full body to a stale prefix would build
      // a corrupt file.
      const bool resume = meta.status == 206 && offset > 0;
      const int flags = O_WRONLY | O_CREAT | (resume ? O_APPEND : O_TRUNC);
      fd = ::open(part.c_str(), flags, 0644);
      if (fd < 0) {
        io_detail = "open " + part + ": " + errno_text();
        return false;
      }
      if (!resume) written = 0;
      if (meta.content_length) {
        total = *meta.content_length + (resume ? offset : 0);
      }
    }
    std::size_t done = 0;
    while (done < len) {
      const ssize_t n = ::write(fd, data + done, len - done);
      if (n < 0) {
        if (errno == EINTR) continue;
        io_detail = "write " + part + ": " + errno_text();
        return false;
      }
      done += static_cast<std::size_t>(n);
    }
    written += len;
    if (progress) progress(written, total);
    return true;
  };

  const auto status = client.fetch_to_sink(req, sink, &cancel);

  if (fd >= 0 && !status.has_value()) ::close(fd);

  // Classification order: an Io fault is root-cause even if it also killed
  // the transfer; a tripped cancel flag beats the transport error it caused.
  if (!io_detail.empty()) return err(DownloadError::io(io_detail));
  if (cancel.load(std::memory_order_acquire)) {
    return err(DownloadError::cancelled());
  }
  if (!status.has_value()) {
    // 416 to a ranged request: our offset is at/past the server's end — the
    // .part can never complete. Drop it so the next attempt starts clean.
    if (offset > 0 && status.error().kind == ProviderError::Kind::Http &&
        status.error().status == 416) {
      std::remove(part.c_str());
    }
    return err(DownloadError::fetch_err(status.error()));
  }

  // Success. A bodiless 2xx never called the sink: a 206 means the .part
  // already holds every byte; anything else is an empty file — create it so
  // the rename below has a source either way.
  if (fd < 0 && *status != 206) {
    fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      return err(DownloadError::io("open " + part + ": " + errno_text()));
    }
  }

  // fsync before the rename: the atomic-publish guarantee is only as strong
  // as the data being on disk when the new name appears.
  if (fd >= 0) {
    const bool synced = ::fsync(fd) == 0;
    const std::string sync_err = synced ? std::string() : errno_text();
    ::close(fd);
    if (!synced) {
      return err(DownloadError::io("fsync " + part + ": " + sync_err));
    }
  }

  if (std::rename(part.c_str(), opts.dest.c_str()) != 0) {
    return err(DownloadError::io("rename " + part + " -> " + opts.dest + ": " +
                                 errno_text()));
  }
  return Unit{};
}

// ===========================================================================
// The HLS arm: external ffmpeg (slice 2).
// ===========================================================================

namespace {

// Provider bytes an argv element may carry (player.rs:286 parity, ROD-92):
// printable ASCII, no space. Empty allowed — an absent optional never reaches
// here.
bool arg_clean(std::string_view s) {
  for (unsigned char b : s) {
    if (b < 0x21 || b > 0x7e) return false;
  }
  return true;
}

// UAs are the one field with legitimate spaces, and must be non-empty
// (player.rs:292 parity — an empty UA defeats the CF bot-score workaround).
bool ua_clean(std::string_view s) {
  if (s.empty()) return false;
  for (unsigned char b : s) {
    if (b < 0x20 || b > 0x7e) return false;
  }
  return true;
}

// fsync-then-rename publish of a finished .part (the curl arm fsyncs its own
// write fd inline; the ffmpeg arm syncs the child's output after the fact —
// POSIX fsync works on a read-only descriptor).
Result<Unit, DownloadError> finalize_part(const std::string& part,
                                          const std::string& dest) {
  const int fd = ::open(part.c_str(), O_RDONLY);
  if (fd < 0) {
    return err(DownloadError::io("open " + part + ": " + errno_text()));
  }
  const bool synced = ::fsync(fd) == 0;
  const std::string sync_err = synced ? std::string() : errno_text();
  ::close(fd);
  if (!synced) {
    return err(DownloadError::io("fsync " + part + ": " + sync_err));
  }
  if (std::rename(part.c_str(), dest.c_str()) != 0) {
    return err(DownloadError::io("rename " + part + " -> " + dest + ": " +
                                 errno_text()));
  }
  return Unit{};
}

}  // namespace

LinkKind classify_url(std::string_view url) {
  // Path component only: strip query/fragment, then take the last extension.
  std::size_t end = url.size();
  for (std::size_t i = 0; i < url.size(); ++i) {
    if (url[i] == '?' || url[i] == '#') {
      end = i;
      break;
    }
  }
  const std::string_view path = url.substr(0, end);
  const std::size_t dot = path.rfind('.');
  const std::size_t slash = path.rfind('/');
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash)) {
    return LinkKind::Direct;
  }
  std::string ext(path.substr(dot + 1));
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return (ext == "m3u8" || ext == "m3u") ? LinkKind::Hls : LinkKind::Direct;
}

Result<std::vector<std::string>, DownloadError> build_ffmpeg_argv(
    const StreamLink& link, std::string_view input_url,
    std::string_view out_path) {
  // Positional: any leading '-' reads as a flag (player.rs:232 shape).
  if (!arg_clean(input_url) ||
      (!input_url.empty() && input_url.front() == '-')) {
    return err(DownloadError::unsafe_arg("url"));
  }

  std::vector<std::string> argv = {"-nostdin", "-y", "-loglevel", "error"};
  if (link.cloaked_segments) {
    // The hls demuxer's gate on .jpg-cloaked segment names — ffmpeg's
    // spelling of mpv's --demuxer-lavf-o=allowed_extensions=ALL.
    argv.push_back("-allowed_extensions");
    argv.push_back("ALL");
  }
  if (link.referer) {
    if (!arg_clean(*link.referer)) {
      return err(DownloadError::unsafe_arg("referer"));
    }
    argv.push_back("-headers");
    argv.push_back("Referer: " + *link.referer + "\r\n");
  }
  if (link.user_agent) {
    if (!ua_clean(*link.user_agent)) {
      return err(DownloadError::unsafe_arg("user_agent"));
    }
    argv.push_back("-user_agent");
    argv.push_back(*link.user_agent);
  }
  argv.push_back("-i");
  argv.push_back(std::string(input_url));
  argv.push_back("-c");
  argv.push_back("copy");
  // The .part carries no useful extension, so the container must be explicit;
  // mp4 is the convention slice 4's local scan expects for HLS grabs.
  argv.push_back("-f");
  argv.push_back("mp4");
  argv.push_back(std::string(out_path));
  return argv;
}

Result<Unit, DownloadError> run_ffmpeg(const std::string& ffmpeg_path,
                                       const std::vector<std::string>& argv,
                                       const std::atomic<bool>& cancel,
                                       std::atomic<long>* child_pid_out) {
  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 2);
  cargv.push_back(const_cast<char*>(ffmpeg_path.c_str()));
  for (const std::string& a : argv) {
    cargv.push_back(const_cast<char*>(a.c_str()));
  }
  cargv.push_back(nullptr);

  // Null stdio (the mpv shape): the child must not fight the TUI for the
  // terminal, and -loglevel error output has nowhere useful to go.
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  pid_t child = -1;
  const int rc = ::posix_spawnp(&child, ffmpeg_path.c_str(), &fa, nullptr,
                                cargv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    // rc IS the errno value (posix_spawnp returns it directly). ENOENT is the
    // runtime-dep degrade, not a fault (debt #5 shape).
    if (rc == ENOENT) {
      return err(DownloadError::ffmpeg_not_found(ffmpeg_path + ": " +
                                                 std::strerror(rc)));
    }
    return err(DownloadError::ffmpeg(ffmpeg_path + ": " + std::strerror(rc)));
  }
  if (child_pid_out != nullptr) {
    child_pid_out->store(static_cast<long>(child), std::memory_order_release);
  }
  // Clear the published pid on EVERY return below (the pid is dead or about
  // to be reaped; a stale value would have quit signal a recycled pid).
  struct PidClear {
    std::atomic<long>* out;
    ~PidClear() {
      if (out != nullptr) out->store(0, std::memory_order_release);
    }
  } pid_clear{child_pid_out};

  // Wait with a ~5 Hz cancel poll. SIGTERM first (ffmpeg finalizes and
  // exits); SIGKILL if it lingers past ~2s. The child is always reaped —
  // no zombie on any path.
  bool termed = false;
  int ticks_since_term = 0;
  for (;;) {
    int status = 0;
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited < 0) {
      if (errno == EINTR) continue;
      return err(DownloadError::ffmpeg(std::string("waitpid: ") + errno_text()));
    }
    if (waited == child) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return Unit{};
      if (termed || cancel.load(std::memory_order_acquire)) {
        return err(DownloadError::cancelled());
      }
      if (WIFEXITED(status)) {
        return err(DownloadError::ffmpeg("exit " +
                                         std::to_string(WEXITSTATUS(status))));
      }
      const int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
      return err(DownloadError::ffmpeg("signal " + std::to_string(sig)));
    }
    if (cancel.load(std::memory_order_acquire)) {
      if (!termed) {
        ::kill(child, SIGTERM);
        termed = true;
      } else if (++ticks_since_term >= 10) {
        ::kill(child, SIGKILL);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

namespace detail {

std::optional<std::string> choose_from_master(std::string_view playlist,
                                              std::string_view base,
                                              Quality quality) {
  const auto variants = hls::parse_master_playlist(playlist);
  if (variants.empty()) return std::nullopt;  // media playlist.

  std::vector<StreamLink> links;
  for (const auto& v : variants) {
    const auto joined = hls::join_url(base, v.url);
    if (!joined.has_value()) continue;
    if (!arg_clean(*joined)) continue;
    StreamLink link;
    link.url = *joined;
    link.resolution = v.resolution;
    links.push_back(std::move(link));
  }
  const auto* pick = hls::select_variant(links, quality);
  if (pick == nullptr) return std::nullopt;
  return pick->url;
}

}  // namespace detail

Result<Unit, DownloadError> download_hls(const http::Client& client,
                                         const StreamLink& link,
                                         const std::string& dest,
                                         Quality quality,
                                         const std::string& ffmpeg_path,
                                         const std::atomic<bool>& cancel,
                                         std::atomic<long>* child_pid_out) {
  std::string input = link.url;

  // Decloak links transfer through the P12 proxy exactly as play does; the
  // guard object keeps the proxy alive for ffmpeg's lifetime. Such providers
  // never cap (the P23 law), so no variant walk on this arm.
  std::optional<proxy::Decloak> decloak;
  if (link.decloak_segments) {
    auto engaged = proxy::engage(link);
    if (!engaged) {
      return err(
          DownloadError::io("decloak proxy: " + engaged.error().detail));
    }
    decloak.emplace(std::move(*engaged));
    input = std::string(decloak->url());
  } else if (classify_url(link.url) == LinkKind::Hls) {
    // Best-effort master walk (senshi::cap_variant's spirit: a fetch/parse
    // failure falls back to the link url — a download is never worse off
    // than handing ffmpeg what mpv would have gotten).
    http::Request req;
    req.url = link.url;
    if (link.user_agent) req.user_agent = *link.user_agent;
    if (link.referer) req.extra_headers.push_back({"Referer", *link.referer});
    if (auto body = client.fetch(req); body.has_value()) {
      const std::string_view text(reinterpret_cast<const char*>(body->data()),
                                  body->size());
      if (auto picked = detail::choose_from_master(text, link.url, quality)) {
        // The variant url is provider bytes off the playlist: re-guard.
        if (auto g = http::guard_fetch_url(*picked); !g.has_value()) {
          return err(DownloadError::unsafe_url(g.error()));
        }
        input = std::move(*picked);
      }
    }
  }

  const std::string part = part_path(dest);
  auto argv = build_ffmpeg_argv(link, input, part);
  if (!argv.has_value()) return err(std::move(argv.error()));

  if (auto ran = run_ffmpeg(ffmpeg_path, *argv, cancel, child_pid_out);
      !ran.has_value()) {
    return ran;  // whatever ffmpeg wrote stays in the .part (never a final).
  }
  return finalize_part(part, dest);
}

Result<Unit, DownloadError> download_link(const http::Client& client,
                                          const StreamLink& link,
                                          const std::string& dest,
                                          Quality quality,
                                          const std::string& ffmpeg_path,
                                          const std::atomic<bool>& cancel,
                                          const ProgressFn& progress,
                                          std::atomic<long>* child_pid_out) {
  // THE guard on the resolved stream url (03 §6.7) — both arms ride it.
  if (auto g = http::guard_fetch_url(link.url); !g.has_value()) {
    return err(DownloadError::unsafe_url(g.error()));
  }
  // A decloak link is HLS by construction (decoy-prefixed segments): route it
  // to the proxy arm no matter what its url spelling classifies as — the curl
  // arm would land the decoy bytes in the file verbatim.
  if (link.decloak_segments) {
    return download_hls(client, link, dest, quality, ffmpeg_path, cancel,
                        child_pid_out);
  }
  switch (classify_url(link.url)) {
    case LinkKind::Direct: {
      Options opts;
      opts.url = link.url;
      opts.dest = dest;
      opts.referer = link.referer;
      if (link.user_agent) opts.user_agent = *link.user_agent;
      return download_to_file(client, opts, cancel, progress);
    }
    case LinkKind::Hls:
      return download_hls(client, link, dest, quality, ffmpeg_path, cancel,
                          child_pid_out);
  }
  return err(DownloadError::io("unreachable"));  // closed enum.
}

// ===========================================================================
// The on-disk convention (slices 3+4).
// ===========================================================================

std::string safe_episode_component(std::string_view label) {
  std::string s = strip_controls(label);
  for (char& c : s) {
    if (c == '/' || c == '\\') c = '_';
  }
  if (s.empty() || s == "." || s == "..") return "ep";
  return s;
}

namespace {

// <download_dir>/<anilist_id>/<track> — the shared prefix episode_dest writes
// under and find_local_episode scans.
std::string track_dir(std::string_view download_dir, std::int64_t anilist_id,
                      Translation translation) {
  std::string p(download_dir);
  if (!p.empty() && p.back() != '/') p.push_back('/');
  p += std::to_string(anilist_id);
  p += '/';
  p += to_string(translation);
  return p;
}

// Extension for the final file. HLS lands as mp4 (the ffmpeg arm's -f mp4);
// a direct link keeps a sane url extension (1-5 alnum chars), else mp4.
std::string dest_extension(std::string_view resolved_url) {
  if (classify_url(resolved_url) == LinkKind::Hls) return "mp4";
  std::size_t end = resolved_url.size();
  for (std::size_t i = 0; i < resolved_url.size(); ++i) {
    if (resolved_url[i] == '?' || resolved_url[i] == '#') {
      end = i;
      break;
    }
  }
  const std::string_view path = resolved_url.substr(0, end);
  const std::size_t dot = path.rfind('.');
  const std::size_t slash = path.rfind('/');
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash)) {
    return "mp4";
  }
  std::string ext(path.substr(dot + 1));
  if (ext.empty() || ext.size() > 5) return "mp4";
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum) return "mp4";
  }
  return ext;
}

}  // namespace

std::string episode_dest(std::string_view download_dir, std::int64_t anilist_id,
                         Translation translation,
                         std::string_view episode_label,
                         std::string_view resolved_url) {
  std::string p = track_dir(download_dir, anilist_id, translation);
  p += '/';
  p += safe_episode_component(episode_label);
  p += '.';
  p += dest_extension(resolved_url);
  return p;
}

std::optional<std::string> find_local_episode(std::string_view download_dir,
                                              std::int64_t anilist_id,
                                              Translation translation,
                                              std::string_view episode_label) {
  if (download_dir.empty()) return std::nullopt;
  const std::string dir = track_dir(download_dir, anilist_id, translation);
  const std::string stem = safe_episode_component(episode_label);
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return std::nullopt;  // nothing downloaded for this track.
  std::optional<std::string> best;
  while (const struct dirent* e = ::readdir(d)) {
    const std::string_view name = e->d_name;
    // Exactly <stem>.<ext>: a dot right after the stem, then a non-empty
    // dot-free suffix that isn't "part" (this one shape also rejects the
    // in-flight "<stem>.<ext>.part" spelling).
    if (name.size() <= stem.size() + 1) continue;
    if (name.substr(0, stem.size()) != stem || name[stem.size()] != '.') continue;
    const std::string_view ext = name.substr(stem.size() + 1);
    if (ext == "part" || ext.find('.') != std::string_view::npos) continue;
    std::string path = dir + '/' + std::string(name);
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    if (!best.has_value() || path < *best) best = std::move(path);
  }
  ::closedir(d);
  return best;
}

Result<Unit, DownloadError> ensure_parent_dirs(const std::string& dest) {
  const std::size_t last_slash = dest.rfind('/');
  if (last_slash == std::string::npos || last_slash == 0) return Unit{};
  const std::string dir = dest.substr(0, last_slash);
  // mkdir each prefix; EEXIST is the common no-op.
  for (std::size_t i = 1; i <= dir.size(); ++i) {
    if (i != dir.size() && dir[i] != '/') continue;
    const std::string prefix = dir.substr(0, i);
    if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) {
      return err(DownloadError::io("mkdir " + prefix + ": " + errno_text()));
    }
  }
  return Unit{};
}

}  // namespace shigoku::download
