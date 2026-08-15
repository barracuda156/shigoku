// download_tests.cpp — P35 slices 1+2: the headless download core + the
// ffmpeg seam.
//
// No sabigoku reference (shigoku-only, §9). The fixture is a scripted
// variant of the serve_once pattern the other test files use: capture the
// request, then play back the response as timed chunks so the kill-mid-
// transfer and cancel cases are drivable over a real loopback socket. The
// ffmpeg seam runs against stub shell scripts (the stub-mpv pattern): argv
// lands in a capture file, output lands where a real ffmpeg would put it.
//
// The .part/atomic-rename law under test: a completed download IS `dest` and
// nothing else; every interrupted download leaves ONLY the `.part` — a
// corrupt final file must be impossible to observe. Both arms ride it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../src/download.hpp"
#include "../src/http.hpp"

// macOS (and other BSDs) has no MSG_NOSIGNAL; SO_NOSIGPIPE on the accepted fd
// below is the equivalent guard against SIGPIPE when the peer aborts mid-drip.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using namespace shigoku;
using namespace shigoku::download;

namespace {

// One response chunk: optional pause first, then the bytes. A closed-early
// response is simply a script whose bytes stop short of the advertised
// Content-Length (the server closes after the last step).
struct Step {
  std::string bytes;
  int pause_ms = 0;
};

struct ScriptedServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;
  std::string captured;

  explicit ScriptedServer(std::vector<Step> steps) {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral.
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                          &len) == 0);
    port = ntohs(addr.sin_port);
    REQUIRE(::listen(listen_fd, 1) == 0);

    thread = std::thread([this, steps = std::move(steps)]() {
      const int cfd = ::accept(listen_fd, nullptr, nullptr);
      if (cfd < 0) return;
#ifdef SO_NOSIGPIPE
      int nosig = 1;
      ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
      char buf[16384];
      const ssize_t n = ::read(cfd, buf, sizeof(buf));
      if (n > 0) captured.assign(buf, buf + n);
      for (const auto& s : steps) {
        if (s.pause_ms > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(s.pause_ms));
        }
        std::size_t off = 0;
        bool dead = false;
        while (off < s.bytes.size()) {
          const ssize_t k = ::send(cfd, s.bytes.data() + off,
                                   s.bytes.size() - off, MSG_NOSIGNAL);
          if (k <= 0) {  // peer aborted mid-drip: stop scripting.
            dead = true;
            break;
          }
          off += static_cast<std::size_t>(k);
        }
        if (dead) break;
      }
      ::close(cfd);
    });
  }

  std::string url(const char* path = "/ep1.mp4") const {
    return "http://127.0.0.1:" + std::to_string(port) + path;
  }

  std::string join_captured() {
    if (thread.joinable()) thread.join();
    return captured;
  }

  ~ScriptedServer() {
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) ::close(listen_fd);
  }
};

std::string response(const std::string& status, const std::string& extra_headers,
                     const std::string& body,
                     std::optional<std::size_t> claimed_length = std::nullopt) {
  return "HTTP/1.1 " + status + "\r\nContent-Type: video/mp4\r\n" +
         extra_headers + "Content-Length: " +
         std::to_string(claimed_length.value_or(body.size())) +
         "\r\nConnection: close\r\n\r\n" + body;
}

// Fresh dest path in the OS tmpdir; removes prior dest AND .part leftovers.
std::string tmp_dest(const char* name) {
  std::string path = "/tmp/shigoku-download-test-";
  path += std::to_string(static_cast<long>(::getpid()));
  path += "-";
  path += name;
  std::remove(path.c_str());
  std::remove(part_path(path).c_str());
  return path;
}

bool exists(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  REQUIRE(out.good());
}

struct Tick {
  std::uint64_t bytes;
  std::optional<std::uint64_t> total;
};

// Drive one download against a scripted response. `cancel_after_first_tick`
// arms the caller-side kill switch from the progress path.
struct RunResult {
  Result<Unit, DownloadError> outcome{Unit{}};
  std::vector<Tick> ticks;
  std::string captured;
};

RunResult run(ScriptedServer& srv, const std::string& dest,
              bool cancel_after_first_tick = false,
              std::optional<long> timeout_secs = 10) {
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  Options opts;
  opts.url = srv.url();
  opts.dest = dest;
  opts.timeout_secs = timeout_secs;
  std::atomic<bool> cancel{false};
  RunResult r;
  r.outcome = download_to_file(
      *client, opts, cancel,
      [&](std::uint64_t bytes, std::optional<std::uint64_t> total) {
        r.ticks.push_back({bytes, total});
        if (cancel_after_first_tick) {
          cancel.store(true, std::memory_order_release);
        }
      });
  r.captured = srv.join_captured();
  return r;
}

}  // namespace

// ===========================================================================
// The pure convention.
// ===========================================================================

TEST_CASE("part_path is dest + .part") {
  CHECK(part_path("/a/b/12.mp4") == "/a/b/12.mp4.part");
}

// ===========================================================================
// The happy paths.
// ===========================================================================

TEST_CASE("fresh download lands atomically: dest exists, .part gone") {
  const std::string dest = tmp_dest("fresh.mp4");
  ScriptedServer srv({{response("200 OK", "", "hello world"), 0}});
  auto r = run(srv, dest);
  REQUIRE(r.outcome.has_value());
  CHECK(read_file(dest) == "hello world");
  CHECK_FALSE(exists(part_path(dest)));
  REQUIRE_FALSE(r.ticks.empty());
  CHECK(r.ticks.back().bytes == 11);
  REQUIRE(r.ticks.back().total.has_value());
  CHECK(*r.ticks.back().total == 11);
}

TEST_CASE("resume: existing .part sends Range and a 206 appends") {
  const std::string dest = tmp_dest("resume.mp4");
  write_file(part_path(dest), "hello ");
  ScriptedServer srv({{response("206 Partial Content",
                                "Content-Range: bytes 6-10/11\r\n", "world"),
                       0}});
  auto r = run(srv, dest);
  REQUIRE(r.outcome.has_value());
  CHECK(r.captured.find("Range: bytes=6-") != std::string::npos);
  CHECK(read_file(dest) == "hello world");
  CHECK_FALSE(exists(part_path(dest)));
  // Progress counts from the resume offset; total folds the offset back in.
  REQUIRE_FALSE(r.ticks.empty());
  CHECK(r.ticks.back().bytes == 11);
  REQUIRE(r.ticks.back().total.has_value());
  CHECK(*r.ticks.back().total == 11);
}

TEST_CASE("resume: a server ignoring Range (200) restarts clean, not append") {
  const std::string dest = tmp_dest("restart.mp4");
  write_file(part_path(dest), "STALE-GARBAGE-PREFIX");
  ScriptedServer srv({{response("200 OK", "", "hello world"), 0}});
  auto r = run(srv, dest);
  REQUIRE(r.outcome.has_value());
  CHECK(r.captured.find("Range: bytes=20-") != std::string::npos);
  CHECK(read_file(dest) == "hello world");  // truncated, never appended.
  CHECK_FALSE(exists(part_path(dest)));
}

TEST_CASE("empty 200 body lands an empty dest") {
  const std::string dest = tmp_dest("empty.mp4");
  ScriptedServer srv({{response("200 OK", "", ""), 0}});
  auto r = run(srv, dest);
  REQUIRE(r.outcome.has_value());
  CHECK(read_file(dest).empty());
  CHECK_FALSE(exists(part_path(dest)));
}

TEST_CASE("streaming path has no 4 MiB cap") {
  const std::string dest = tmp_dest("big.mp4");
  const std::string body(5u * 1024u * 1024u, 'x');  // > http::kMaxRespBytes.
  ScriptedServer srv({{response("200 OK", "", body), 0}});
  auto r = run(srv, dest);
  REQUIRE(r.outcome.has_value());
  struct stat st{};
  REQUIRE(::stat(dest.c_str(), &st) == 0);
  CHECK(static_cast<std::uint64_t>(st.st_size) == body.size());
  CHECK_FALSE(exists(part_path(dest)));
}

// ===========================================================================
// The interruption law: dest never exists, the .part carries the partial.
// ===========================================================================

TEST_CASE("kill mid-transfer leaves only the .part, never a final") {
  const std::string dest = tmp_dest("killed.mp4");
  // Claim 100 bytes, deliver 20, close: the transport dies (partial file).
  ScriptedServer srv(
      {{response("200 OK", "", "20-bytes-of-payload!", /*claimed=*/100), 0}});
  auto r = run(srv, dest);
  REQUIRE_FALSE(r.outcome.has_value());
  CHECK(r.outcome.error().kind == DownloadError::Kind::Fetch);
  CHECK(r.outcome.error().fetch.kind == ProviderError::Kind::Network);
  CHECK_FALSE(exists(dest));
  REQUIRE(exists(part_path(dest)));
  CHECK(read_file(part_path(dest)) == "20-bytes-of-payload!");
}

TEST_CASE("cancel mid-transfer keeps the .part for a future resume") {
  const std::string dest = tmp_dest("cancelled.mp4");
  // Drip: first chunk arrives, then the line goes quiet long enough for the
  // abort poll to notice the flag the first progress tick armed.
  ScriptedServer srv({{response("200 OK", "", "first-chunk-", /*claimed=*/64), 0},
                      {"never-delivered", 2500}});
  auto r = run(srv, dest, /*cancel_after_first_tick=*/true,
               /*timeout_secs=*/std::nullopt);
  REQUIRE_FALSE(r.outcome.has_value());
  CHECK(r.outcome.error().kind == DownloadError::Kind::Cancelled);
  CHECK_FALSE(exists(dest));
  REQUIRE(exists(part_path(dest)));
  CHECK(read_file(part_path(dest)) == "first-chunk-");
}

// ===========================================================================
// Status handling.
// ===========================================================================

TEST_CASE("http error creates nothing: error body bytes never reach disk") {
  const std::string dest = tmp_dest("notfound.mp4");
  ScriptedServer srv({{response("404 Not Found", "", "<html>nope</html>"), 0}});
  auto r = run(srv, dest);
  REQUIRE_FALSE(r.outcome.has_value());
  CHECK(r.outcome.error().kind == DownloadError::Kind::Fetch);
  CHECK(r.outcome.error().fetch.kind == ProviderError::Kind::Http);
  CHECK(r.outcome.error().fetch.status == 404);
  CHECK_FALSE(exists(dest));
  CHECK_FALSE(exists(part_path(dest)));
  CHECK(r.ticks.empty());
}

TEST_CASE("416 to a ranged request drops the unusable .part") {
  const std::string dest = tmp_dest("gone416.mp4");
  write_file(part_path(dest), "hello world");  // at/past the server's end.
  ScriptedServer srv({{response("416 Range Not Satisfiable", "", ""), 0}});
  auto r = run(srv, dest);
  REQUIRE_FALSE(r.outcome.has_value());
  CHECK(r.outcome.error().kind == DownloadError::Kind::Fetch);
  CHECK(r.outcome.error().fetch.kind == ProviderError::Kind::Http);
  CHECK(r.outcome.error().fetch.status == 416);
  CHECK_FALSE(exists(dest));
  CHECK_FALSE(exists(part_path(dest)));  // dropped: next attempt starts clean.
}

// ===========================================================================
// Slice 2: url classification.
// ===========================================================================

TEST_CASE("classify_url: playlists are Hls, everything else Direct") {
  CHECK(classify_url("https://cdn.example/x/master.m3u8") == LinkKind::Hls);
  CHECK(classify_url("https://cdn.example/x/M.M3U8?tok=1") == LinkKind::Hls);
  CHECK(classify_url("https://cdn.example/x/list.m3u") == LinkKind::Hls);
  CHECK(classify_url("https://cdn.example/x/ep1.mp4") == LinkKind::Direct);
  CHECK(classify_url("https://cdn.example/x/ep1.mp4#t=0") == LinkKind::Direct);
  CHECK(classify_url("https://cdn.example/plain") == LinkKind::Direct);
  // A dot only in an earlier path segment is not an extension.
  CHECK(classify_url("https://cdn.example/a.m3u8/seg") == LinkKind::Direct);
}

// ===========================================================================
// Slice 2: the ffmpeg argv table (golden) + per-field vetting.
// ===========================================================================

namespace {

StreamLink plain_link(const std::string& url) {
  StreamLink l;
  l.url = url;
  return l;
}

}  // namespace

TEST_CASE("ffmpeg argv: minimal table") {
  const auto argv = build_ffmpeg_argv(
      plain_link("https://cdn.example/m.m3u8"), "https://cdn.example/m.m3u8",
      "/dl/1/sub/1.mp4.part");
  REQUIRE(argv.has_value());
  const std::vector<std::string> want = {
      "-nostdin", "-y", "-loglevel", "error",
      "-i", "https://cdn.example/m.m3u8",
      "-c", "copy", "-f", "mp4", "/dl/1/sub/1.mp4.part"};
  CHECK(*argv == want);
}

TEST_CASE("ffmpeg argv: full table — cloaked + referer + user_agent") {
  StreamLink l = plain_link("https://cdn.example/m.m3u8");
  l.cloaked_segments = true;
  l.referer = "https://site.example/";
  l.user_agent = "Mozilla/5.0 (X11) Chrome/126";
  const auto argv = build_ffmpeg_argv(l, "https://cdn.example/v720.m3u8",
                                      "/dl/1/sub/2.mp4.part");
  REQUIRE(argv.has_value());
  const std::vector<std::string> want = {
      "-nostdin", "-y", "-loglevel", "error",
      "-allowed_extensions", "ALL",
      "-headers", "Referer: https://site.example/\r\n",
      "-user_agent", "Mozilla/5.0 (X11) Chrome/126",
      "-i", "https://cdn.example/v720.m3u8",
      "-c", "copy", "-f", "mp4", "/dl/1/sub/2.mp4.part"};
  CHECK(*argv == want);
}

TEST_CASE("ffmpeg argv: banned bytes rejected per field (ROD-92/435)") {
  const std::string url = "https://cdn.example/m.m3u8";

  {  // header injection through the referer.
    StreamLink l = plain_link(url);
    l.referer = "https://x/\r\nCookie: evil";
    auto r = build_ffmpeg_argv(l, url, "/o.part");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == DownloadError::Kind::UnsafeArg);
    CHECK(r.error().detail == "referer");
  }
  {  // referer with a space: not a valid url byte in argv position.
    StreamLink l = plain_link(url);
    l.referer = "https://x/a b";
    auto r = build_ffmpeg_argv(l, url, "/o.part");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().detail == "referer");
  }
  {  // an empty UA blanks the bot-score workaround.
    StreamLink l = plain_link(url);
    l.user_agent = "";
    auto r = build_ffmpeg_argv(l, url, "/o.part");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().detail == "user_agent");
  }
  {  // control bytes in the UA.
    StreamLink l = plain_link(url);
    l.user_agent = "Mozilla\n5.0";
    auto r = build_ffmpeg_argv(l, url, "/o.part");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().detail == "user_agent");
  }
  {  // input positional must not read as a flag.
    auto r = build_ffmpeg_argv(plain_link(url), "--evil=1", "/o.part");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().detail == "url");
  }
  {  // input with a space.
    auto r = build_ffmpeg_argv(plain_link(url), "https://x/a b", "/o.part");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().detail == "url");
  }
}

// ===========================================================================
// Slice 2: master → variant (the P23 cap policy through the download path).
// ===========================================================================

namespace {

const char* kMaster =
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=4000000,RESOLUTION=1920x1080\n"
    "v1080.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720\n"
    "v720.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=854x480\n"
    "v480.m3u8\n";

const char* kMedia =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXTINF:4.0,\n"
    "seg0.ts\n"
    "#EXT-X-ENDLIST\n";

}  // namespace

TEST_CASE("choose_from_master: joins and picks per the cap policy") {
  const std::string base = "https://cdn.example/x/master.m3u8";
  auto best = download::detail::choose_from_master(kMaster, base, Quality::Best);
  REQUIRE(best.has_value());
  CHECK(*best == "https://cdn.example/x/v1080.m3u8");

  auto capped =
      download::detail::choose_from_master(kMaster, base, Quality::P720);
  REQUIRE(capped.has_value());
  CHECK(*capped == "https://cdn.example/x/v720.m3u8");

  auto worst =
      download::detail::choose_from_master(kMaster, base, Quality::Worst);
  REQUIRE(worst.has_value());
  CHECK(*worst == "https://cdn.example/x/v480.m3u8");
}

TEST_CASE("choose_from_master: a media playlist yields nothing") {
  CHECK_FALSE(download::detail::choose_from_master(
                  kMedia, "https://cdn.example/x/m.m3u8", Quality::Best)
                  .has_value());
}

// ===========================================================================
// Slice 2: run_ffmpeg against stub scripts (the stub-mpv pattern).
// ===========================================================================

namespace {

std::string write_stub(const char* name, const std::string& body) {
  const std::string path = tmp_dest(name);
  write_file(path, "#!/bin/sh\n" + body);
  ::chmod(path.c_str(), 0755);
  return path;
}

std::atomic<bool> g_never_cancel{false};

}  // namespace

TEST_CASE("run_ffmpeg: argv reaches the child; exit 0 is Ok") {
  const std::string argv_out = tmp_dest("ffmpeg_argv.txt");
  const std::string out = tmp_dest("stub_out.bin");
  ::setenv("STUB_ARGV_OUT", argv_out.c_str(), 1);
  const std::string stub = write_stub("stub_ffmpeg_ok.sh",
                                      "printf '%s\\n' \"$@\" > \"$STUB_ARGV_OUT\"\n"
                                      "for a; do o=\"$a\"; done\n"
                                      "printf 'FAKEVIDEO' > \"$o\"\n"
                                      "exit 0\n");
  const std::vector<std::string> argv = {"-nostdin", "-y", "-i", "u", out};
  auto r = run_ffmpeg(stub, argv, g_never_cancel);
  REQUIRE(r.has_value());
  CHECK(read_file(argv_out) == "-nostdin\n-y\n-i\nu\n" + out + "\n");
  CHECK(read_file(out) == "FAKEVIDEO");
}

TEST_CASE("run_ffmpeg: absent binary degrades to FfmpegNotFound") {
  const std::string missing = tmp_dest("no-such-ffmpeg");  // removed by tmp_dest.
  auto r = run_ffmpeg(missing, {"-nostdin"}, g_never_cancel);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == DownloadError::Kind::FfmpegNotFound);
}

TEST_CASE("run_ffmpeg: nonzero exit is Ffmpeg with the status") {
  const std::string stub = write_stub("stub_ffmpeg_fail.sh", "exit 3\n");
  auto r = run_ffmpeg(stub, {"-nostdin"}, g_never_cancel);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == DownloadError::Kind::Ffmpeg);
  CHECK(r.error().detail == "exit 3");
}

TEST_CASE("run_ffmpeg: cancel SIGTERMs the child and reports Cancelled") {
  const std::string stub = write_stub("stub_ffmpeg_sleep.sh", "exec sleep 10\n");
  std::atomic<bool> cancel{true};  // pre-armed: first poll tick terminates.
  const auto t0 = std::chrono::steady_clock::now();
  auto r = run_ffmpeg(stub, {"-nostdin"}, cancel);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == DownloadError::Kind::Cancelled);
  CHECK(elapsed < std::chrono::seconds(5));  // never rides out the sleep.
}

// ===========================================================================
// Slice 2: download_hls end-to-end (stub ffmpeg + loopback playlist server).
// ===========================================================================

TEST_CASE("download_hls: media playlist falls through to the link url; "
          "stub output lands atomically") {
  const std::string dest = tmp_dest("hls_media.mp4");
  const std::string argv_out = tmp_dest("hls_argv.txt");
  ::setenv("STUB_ARGV_OUT", argv_out.c_str(), 1);
  const std::string stub = write_stub("stub_ffmpeg_hls.sh",
                                      "printf '%s\\n' \"$@\" > \"$STUB_ARGV_OUT\"\n"
                                      "for a; do o=\"$a\"; done\n"
                                      "printf 'FAKEVIDEO' > \"$o\"\n"
                                      "exit 0\n");
  ScriptedServer srv({{response("200 OK", "", kMedia), 0}});
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  const StreamLink link = plain_link(srv.url("/stream.m3u8"));
  std::atomic<bool> cancel{false};
  auto r = download_hls(*client, link, dest, Quality::Best, stub, cancel);
  REQUIRE(r.has_value());
  CHECK(read_file(dest) == "FAKEVIDEO");
  CHECK_FALSE(exists(part_path(dest)));
  // The argv capture shows the media-playlist fallback: -i <link url>, and
  // the output positional is the .part (renamed only after exit 0).
  const std::string captured_argv = read_file(argv_out);
  CHECK(captured_argv.find("-i\n" + link.url + "\n") != std::string::npos);
  CHECK(captured_argv.find(part_path(dest)) != std::string::npos);
}

TEST_CASE("download_hls: a failing ffmpeg leaves only the .part") {
  const std::string dest = tmp_dest("hls_fail.mp4");
  const std::string stub = write_stub("stub_ffmpeg_hls_fail.sh",
                                      "for a; do o=\"$a\"; done\n"
                                      "printf 'HALF' > \"$o\"\n"
                                      "exit 1\n");
  ScriptedServer srv({{response("200 OK", "", kMedia), 0}});
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  const StreamLink link = plain_link(srv.url("/stream.m3u8"));
  std::atomic<bool> cancel{false};
  auto r = download_hls(*client, link, dest, Quality::Best, stub, cancel);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == DownloadError::Kind::Ffmpeg);
  CHECK_FALSE(exists(dest));
  REQUIRE(exists(part_path(dest)));
  CHECK(read_file(part_path(dest)) == "HALF");
}

TEST_CASE("download_hls: a master's variant pick is re-guarded — a private "
          "host never reaches ffmpeg") {
  const std::string dest = tmp_dest("hls_guarded.mp4");
  // The master's variants join against the loopback base: the pick is a
  // private-host url and MUST die at the guard, not spawn ffmpeg (a missing
  // binary would report FfmpegNotFound — the UnsafeUrl proves the order).
  ScriptedServer srv({{response("200 OK", "", kMaster), 0}});
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  const StreamLink link = plain_link(srv.url("/master.m3u8"));
  std::atomic<bool> cancel{false};
  auto r = download_hls(*client, link, dest, Quality::Best,
                        "/no/such/ffmpeg", cancel);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == DownloadError::Kind::UnsafeUrl);
  CHECK(r.error().guard == GuardError::BlockedHost);
  CHECK_FALSE(exists(dest));
  CHECK_FALSE(exists(part_path(dest)));
}

// ===========================================================================
// Slice 2: download_link owns THE guard.
// ===========================================================================

// ===========================================================================
// Slice 3: the on-disk convention.
// ===========================================================================

TEST_CASE("safe_episode_component: separators and hostile labels never escape") {
  CHECK(safe_episode_component("12") == "12");
  CHECK(safe_episode_component("SP1") == "SP1");
  CHECK(safe_episode_component("a/b") == "a_b");
  CHECK(safe_episode_component("a\\b") == "a_b");
  CHECK(safe_episode_component("..") == "ep");
  CHECK(safe_episode_component(".") == "ep");
  CHECK(safe_episode_component("") == "ep");
  CHECK(safe_episode_component("1\x1b[31m2") == "1[31m2");  // controls stripped.
}

TEST_CASE("episode_dest: <dir>/<id>/<track>/<label>.<ext>") {
  CHECK(episode_dest("/dl", 700, Translation::Sub, "7",
                     "https://cdn.example/v/7.mp4") == "/dl/700/sub/7.mp4");
  CHECK(episode_dest("/dl/", 700, Translation::Dub, "7",
                     "https://cdn.example/v/7.MKV?tok=1") == "/dl/700/dub/7.mkv");
  // HLS lands as mp4 (the ffmpeg arm's -f mp4), whatever the playlist ext.
  CHECK(episode_dest("/dl", 700, Translation::Sub, "7",
                     "https://cdn.example/v/master.m3u8") == "/dl/700/sub/7.mp4");
  // No ext / junk ext falls back to mp4.
  CHECK(episode_dest("/dl", 700, Translation::Sub, "7",
                     "https://cdn.example/stream") == "/dl/700/sub/7.mp4");
  CHECK(episode_dest("/dl", 700, Translation::Sub, "7",
                     "https://cdn.example/v/x.longext7") == "/dl/700/sub/7.mp4");
  // The label rides safe_episode_component.
  CHECK(episode_dest("/dl", 700, Translation::Sub, "../evil",
                     "https://cdn.example/v/e.mp4") == "/dl/700/sub/.._evil.mp4");
}

TEST_CASE("ensure_parent_dirs: mkdir -p the chain, idempotent") {
  const std::string root = tmp_dest("dirs");
  const std::string dest = root + "/700/sub/7.mp4";
  REQUIRE(ensure_parent_dirs(dest).has_value());
  struct stat st{};
  REQUIRE(::stat((root + "/700/sub").c_str(), &st) == 0);
  CHECK(S_ISDIR(st.st_mode));
  CHECK(ensure_parent_dirs(dest).has_value());  // second run: all EEXIST.
  ::rmdir((root + "/700/sub").c_str());
  ::rmdir((root + "/700").c_str());
  ::rmdir(root.c_str());
}

// ===========================================================================
// Slice 4: the play-prefers-local scan.
// ===========================================================================

namespace {

// mkdir -p + write a tiny file (a scan fixture under the tmp root).
void plant(const std::string& dir, const std::string& name) {
  const std::string path = dir + "/" + name;
  REQUIRE(ensure_parent_dirs(path).has_value());
  FILE* f = std::fopen(path.c_str(), "wb");
  REQUIRE(f != nullptr);
  std::fputs("x", f);
  std::fclose(f);
}

// Remove leftovers from a prior run (tmp_dest only clears the exact path).
void scrub(const std::string& dir, std::initializer_list<const char*> names) {
  for (const char* n : names) std::remove((dir + "/" + n).c_str());
}

}  // namespace

TEST_CASE("find_local_episode: exactly (show, track, ep) — track mismatch "
          "never matches (P35 slice 4)") {
  const std::string root = tmp_dest("local-scan");
  scrub(root + "/700/sub", {"7.mp4", "77.mp4", "a_b.mp4"});
  plant(root + "/700/sub", "7.mp4");
  auto hit = find_local_episode(root, 700, Translation::Sub, "7");
  REQUIRE(hit.has_value());
  CHECK(*hit == root + "/700/sub/7.mp4");
  // The DoD case: a dub play never adopts the sub download (and vice versa).
  CHECK_FALSE(find_local_episode(root, 700, Translation::Dub, "7").has_value());
  // Nor another episode, another show, or downloads-disabled ("").
  CHECK_FALSE(find_local_episode(root, 700, Translation::Sub, "8").has_value());
  CHECK_FALSE(find_local_episode(root, 701, Translation::Sub, "7").has_value());
  CHECK_FALSE(find_local_episode("", 700, Translation::Sub, "7").has_value());
  // The stem is exact: ep 7 never adopts 77.mp4.
  plant(root + "/700/sub", "77.mp4");
  CHECK(*find_local_episode(root, 700, Translation::Sub, "7") ==
        root + "/700/sub/7.mp4");
  // The label scans by its safe written spelling — a hostile one never escapes.
  plant(root + "/700/sub", "a_b.mp4");
  CHECK(find_local_episode(root, 700, Translation::Sub, "a/b").has_value());
}

TEST_CASE("find_local_episode: a .part never plays; ties pick deterministically") {
  const std::string root = tmp_dest("local-part");
  scrub(root + "/700/sub", {"7.mp4.part", "7.part", "7.mkv", "7.mp4"});
  // A killed transfer leaves only .part shapes — neither may ever play.
  plant(root + "/700/sub", "7.mp4.part");
  plant(root + "/700/sub", "7.part");
  CHECK_FALSE(find_local_episode(root, 700, Translation::Sub, "7").has_value());
  // Completed files beside them are found; two exts pick the lexicographic
  // min (deterministic under any readdir order).
  plant(root + "/700/sub", "7.mp4");
  plant(root + "/700/sub", "7.mkv");
  auto hit = find_local_episode(root, 700, Translation::Sub, "7");
  REQUIRE(hit.has_value());
  CHECK(*hit == root + "/700/sub/7.mkv");
}

TEST_CASE("download_link: the stream url is guarded before any arm runs") {
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  std::atomic<bool> cancel{false};
  const StreamLink link = plain_link("http://127.0.0.1:9/ep1.mp4");
  auto r = download_link(*client, link, "/tmp/never.mp4", Quality::Best,
                         "/no/such/ffmpeg", cancel, {});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == DownloadError::Kind::UnsafeUrl);
  CHECK(r.error().guard == GuardError::BlockedHost);
}
