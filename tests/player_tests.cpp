// player_tests.cpp — P5 golden tests, ported from sabigoku src/player.rs's
// `mod tests`. The argv table (03 §6.3.1) and retry law (04 §7.8) are the port
// contract; the two required IPC tests (ipc_handshake_events_and_final_position,
// ipc_without_meaningful_position_keeps_the_gate_shut) drive watch_ipc over a
// socketpair. Tests that only exist for the deferred proxy machinery
// (decloak_*) are intentionally NOT ported (NOTES.md P5); argv_skip_script_*
// lands at P22 (argv_skip_script_lands_in_table_position, below).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/player.hpp"

using namespace shigoku;
using namespace shigoku::player;
using namespace shigoku::player::detail;
using nlohmann::json;

namespace {

StreamLink full_link() {
  StreamLink l;
  l.url = "https://cdn.example/x.m3u8";
  l.resolution = 1080u;
  l.referer = "https://ref.example/";
  l.user_agent = "Mozilla/5.0 (X11; Linux) Gecko";
  l.cloaked_segments = true;
  l.decloak_segments = false;
  l.sub_url = "https://sub.example/s.vtt";
  return l;
}

PlayOpts opts(std::string title, double start) {
  PlayOpts o;
  o.mpv_path = "mpv";
  o.socket_dir = "/run/user/1000/shigoku";
  o.title = std::move(title);
  o.start_secs = start;
  return o;
}

// A completed attempt (position, exit-code, clean flag) for run_attempts.
AttemptResult res(std::optional<Position> pos, int code, bool clean) {
  AttemptResult r;
  r.position = pos;
  r.exit_code = code;
  r.clean_exit = clean;
  return r;
}

AttemptResult open_failed() { return res(std::nullopt, 2, false); }

// Write a full C-string to a fd (test helper; small writes never short here).
void put(int fd, const std::string& s) {
  CHECK(::write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size()));
}

// Read one '\n'-terminated line from a fd, byte at a time (test-only, tiny).
std::string read_line(int fd) {
  std::string out;
  char c;
  while (::read(fd, &c, 1) == 1) {
    out.push_back(c);
    if (c == '\n') break;
  }
  return out;
}

}  // namespace

// ===========================================================================
// argv (03 §6.3.1, the table is law)
// ===========================================================================

TEST_CASE("argv_full_house_matches_the_table") {
  const StreamLink link = full_link();
  auto o = opts("Frieren 冒険", 42.5);
  auto r = build_argv(link, link.url, o, "/run/user/1000/shigoku/s.sock");
  REQUIRE(r.has_value());
  const std::vector<std::string> expected = {
      "--http-header-fields-append=Referer: https://ref.example/",
      "--user-agent=Mozilla/5.0 (X11; Linux) Gecko",
      "--stream-lavf-o=multiple_requests=1,icy=0",
      "--sub-file=https://sub.example/s.vtt",
      "--sub-pos=92",
      "--sub-bold=yes",
      "--demuxer-lavf-o=allowed_extensions=ALL",
      "--force-media-title=Frieren 冒険",
      "--title=shigoku - ${media-title}",
      "--input-ipc-server=/run/user/1000/shigoku/s.sock",
      "--start=42.5",
      "https://cdn.example/x.m3u8",
  };
  CHECK(*r == expected);
}

TEST_CASE("argv_minimal_link_skips_every_optional_flag") {
  StreamLink link;
  link.url = "https://cdn.example/plain.mp4";
  link.cloaked_segments = false;
  auto o = opts("", 0.0);
  auto r = build_argv(link, link.url, o, "/tmp/s.sock");
  REQUIRE(r.has_value());
  const std::vector<std::string> expected = {
      "--stream-lavf-o=multiple_requests=1,icy=0",
      "--input-ipc-server=/tmp/s.sock",
      "https://cdn.example/plain.mp4",
  };
  CHECK(*r == expected);
  for (const auto& a : *r) CHECK(a.find("user-agent") == std::string::npos);
}

// P22: aniskip's --script/--script-opts pair, position-based (not exact-vector,
// since every other flag in this fixture is orthogonal) — lands right after
// --start= and right before the positional url (03 §6.3.1 table order).
TEST_CASE("argv_skip_script_lands_in_table_position") {
  const StreamLink link = full_link();
  auto o = opts("t", 42.5);
  o.skip = SkipScript{"/cache/skip.lua", "aniskip-op_start=12.5,aniskip-mode=both"};
  auto r = build_argv(link, link.url, o, "/tmp/s.sock");
  REQUIRE(r.has_value());
  const std::vector<std::string>& argv = *r;
  auto at = [&](const std::string& needle) -> std::size_t {
    auto it = std::find(argv.begin(), argv.end(), needle);
    REQUIRE(it != argv.end());
    return static_cast<std::size_t>(it - argv.begin());
  };
  const std::size_t script_at = at("--script=/cache/skip.lua");
  CHECK(script_at + 1 ==
        at("--script-opts=aniskip-op_start=12.5,aniskip-mode=both"));
  CHECK(script_at > at("--start=42.5"));
  CHECK(argv.back() == link.url);
}

TEST_CASE("argv_rejects_injection_in_provider_fields") {
  const char* sock = "/tmp/s.sock";

  {
    StreamLink link = full_link();
    link.referer = "https://e/\r\nX-Evil: 1";
    auto r = build_argv(link, link.url, opts("t", 0.0), sock);
    REQUIRE(!r.has_value());
    CHECK(r.error().kind == PlayError::Kind::UnsafeArg);
    CHECK(r.error().detail == "referer");
  }
  {
    StreamLink link = full_link();
    link.user_agent = "UA\nUA";
    auto r = build_argv(link, link.url, opts("t", 0.0), sock);
    REQUIRE(!r.has_value());
    CHECK(r.error().detail == "user_agent");
  }
  {
    StreamLink link = full_link();
    // A raw 0x80 byte in the sub url — over-range, must be rejected.
    std::string sub = "https://e/s.vtt";
    sub.push_back(static_cast<char>(0x80));
    link.sub_url = sub;
    auto r = build_argv(link, link.url, opts("t", 0.0), sock);
    REQUIRE(!r.has_value());
    CHECK(r.error().detail == "sub_url");
  }
  {
    // A play_url that looks like a flag is rejected as the positional.
    const StreamLink link = full_link();
    auto r = build_argv(link, "--script=/tmp/evil.lua", opts("t", 0.0), sock);
    REQUIRE(!r.has_value());
    CHECK(r.error().detail == "url");
  }
}

TEST_CASE("argv_title_strips_control_chars_and_empty_title_skips_flags") {
  const StreamLink link = full_link();
  const char* sock = "/tmp/s.sock";

  // ESC + CR/LF stripped (C0 + C1 class).
  auto r1 = build_argv(link, link.url, opts("A\x1b[31mB\r\n", 0.0), sock);
  REQUIRE(r1.has_value());
  bool found = false;
  for (const auto& a : *r1)
    if (a == "--force-media-title=A[31mB") found = true;
  CHECK(found);

  // A control-only title yields no title flags at all.
  auto r2 = build_argv(link, link.url, opts("\r\n", 0.0), sock);
  REQUIRE(r2.has_value());
  for (const auto& a : *r2) CHECK(a.find("title") == std::string::npos);

  // mpv's own title rendering has no framework backstop, so the bidi /
  // zero-width class dies here too. Build the title from explicit UTF-8 bytes
  // (U+202E override, U+200B zero-width) so no raw hostile codepoint sits in
  // the source -- the warning fence forbids the literal form on purpose (P1).
  std::string bidi = "A";
  bidi += "\xE2\x80\xAE";  // U+202E RIGHT-TO-LEFT OVERRIDE
  bidi += "B";
  bidi += "\xE2\x80\x8B";  // U+200B ZERO WIDTH SPACE
  auto r3 = build_argv(link, link.url, opts(bidi, 0.0), sock);
  REQUIRE(r3.has_value());
  found = false;
  for (const auto& a : *r3)
    if (a == "--force-media-title=AB") found = true;
  CHECK(found);
}

TEST_CASE("argv_start_only_when_positive_and_finite") {
  const StreamLink link = full_link();
  const char* sock = "/tmp/s.sock";
  for (double start : {0.0, -3.0, std::nan("")}) {
    auto r = build_argv(link, link.url, opts("t", start), sock);
    REQUIRE(r.has_value());
    for (const auto& a : *r) CHECK(a.rfind("--start=", 0) != 0);
  }
}

// ===========================================================================
// local argv (P35 slice 4): build_argv's tail half for a downloaded file.
// ===========================================================================

TEST_CASE("local_argv_full_house_matches_the_table") {
  auto o = opts("Frieren 冒険", 42.5);
  o.skip = SkipScript{"/cache/skip.lua", "aniskip-mode=both"};
  auto r = build_local_argv("/dl/700/sub/7.mp4", o, "/run/user/1000/shigoku/s.sock");
  REQUIRE(r.has_value());
  const std::vector<std::string> expected = {
      "--force-media-title=Frieren 冒険",
      "--title=shigoku - ${media-title}",
      "--input-ipc-server=/run/user/1000/shigoku/s.sock",
      "--start=42.5",
      "--script=/cache/skip.lua",
      "--script-opts=aniskip-mode=both",
      "/dl/700/sub/7.mp4",
  };
  CHECK(*r == expected);
  // None of the resolved-link flags apply to a file.
  for (const auto& a : *r) {
    CHECK(a.find("stream-lavf") == std::string::npos);
    CHECK(a.find("user-agent") == std::string::npos);
    CHECK(a.find("Referer") == std::string::npos);
  }
}

TEST_CASE("local_argv_takes_paths_build_argv_would_veto") {
  // The path is app-built, not provider bytes: spaces and UTF-8 (a user's
  // download_dir, a provider's episode label) must play. build_argv's
  // arg_clean would reject both — that vetting is exactly what this builder
  // exists to skip.
  auto o = opts("", 0.0);
  auto r = build_local_argv("/home/a b/dl/700/sub/第1話.mp4", o, "/tmp/s.sock");
  REQUIRE(r.has_value());
  CHECK(r->back() == "/home/a b/dl/700/sub/第1話.mp4");
  // The one hazard kept: a leading '-' (relative download_dir) is fenced with
  // ./ so it can never read as a flag; an empty path is refused outright.
  auto fenced = build_local_argv("-dl/700/sub/7.mp4", o, "/tmp/s.sock");
  REQUIRE(fenced.has_value());
  CHECK(fenced->back() == "./-dl/700/sub/7.mp4");
  auto empty = build_local_argv("", o, "/tmp/s.sock");
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().kind == PlayError::Kind::UnsafeArg);
}

TEST_CASE("socket_paths_are_unique_per_launch") {
  const std::string a = socket_path("/tmp");
  const std::string b = socket_path("/tmp");
  CHECK(a != b);
  // Base name shape: shigoku-mpv-<...>.sock
  const auto slash = a.find_last_of('/');
  const std::string name = a.substr(slash + 1);
  CHECK(name.rfind("shigoku-mpv-", 0) == 0);
  CHECK(name.size() > 5);
  CHECK(name.substr(name.size() - 5) == ".sock");
}

// ===========================================================================
// resume rule (03 §6.3.1, A6)
// ===========================================================================

TEST_CASE("resume_start_rule") {
  // Fully watched -> 0.
  CHECK(resume_start(120.0, 300.0, /*fully_watched=*/true) == 0.0);
  // >= 80% of duration -> 0 (240/300 = 0.80).
  CHECK(resume_start(240.0, 300.0, false) == 0.0);
  // Invalid saved pos -> 0.
  CHECK(resume_start(0.0, 300.0, false) == 0.0);
  CHECK(resume_start(-5.0, 300.0, false) == 0.0);
  CHECK(resume_start(std::nan(""), 300.0, false) == 0.0);
  // Normal resume: saved - 5s.
  CHECK(resume_start(100.0, 300.0, false) == doctest::Approx(95.0));
  // Floor at 0 when saved < offset.
  CHECK(resume_start(3.0, 300.0, false) == 0.0);
  // Unknown duration (<=0) can't be "near end": still resumes with offset.
  CHECK(resume_start(100.0, 0.0, false) == doctest::Approx(95.0));
}

// ===========================================================================
// retry policy (04 §7.8)
// ===========================================================================

TEST_CASE("open_failed_retries_with_backoff_then_succeeds") {
  std::vector<AttemptResult> scripted = {
      open_failed(), open_failed(),
      res(Position{3.0, 24.0}, 0, true)};
  std::size_t idx = 0;
  std::vector<std::uint32_t> retries;
  std::vector<std::chrono::seconds> sleeps;

  auto out = run_attempts(
      [&](std::uint32_t) -> Result<AttemptResult, PlayError> {
        REQUIRE(idx < scripted.size());
        return scripted[idx++];
      },
      [&](std::uint32_t n) { retries.push_back(n); },
      [&](std::chrono::seconds d) { sleeps.push_back(d); });

  REQUIRE(out.has_value());
  CHECK(out->attempts == 3);
  REQUIRE(out->position.has_value());
  CHECK(out->position->secs == 3.0);
  CHECK(sleeps == std::vector<std::chrono::seconds>{std::chrono::seconds(2),
                                                    std::chrono::seconds(4)});
  CHECK(retries == std::vector<std::uint32_t>{2, 3});
}

TEST_CASE("exit_two_with_meaningful_play_never_retries") {
  std::size_t idx = 0;
  std::vector<AttemptResult> scripted = {res(Position{300.0, std::nullopt}, 2, false)};
  auto out = run_attempts(
      [&](std::uint32_t) -> Result<AttemptResult, PlayError> {
        return scripted[idx++];
      },
      [](std::uint32_t) { FAIL("no retry"); },
      [](std::chrono::seconds) { FAIL("no backoff"); });
  REQUIRE(out.has_value());
  CHECK(out->attempts == 1);
  CHECK(out->position->secs == 300.0);
}

TEST_CASE("exhausted_budget_is_open_failed") {
  std::vector<AttemptResult> scripted = {open_failed(), open_failed(), open_failed()};
  std::size_t idx = 0;
  std::vector<std::chrono::seconds> sleeps;
  auto out = run_attempts(
      [&](std::uint32_t) -> Result<AttemptResult, PlayError> {
        return scripted[idx++];
      },
      [](std::uint32_t) {}, [&](std::chrono::seconds d) { sleeps.push_back(d); });
  REQUIRE(!out.has_value());
  CHECK(out.error().kind == PlayError::Kind::OpenFailed);
  CHECK(out.error().attempts == 3);
  CHECK(sleeps == std::vector<std::chrono::seconds>{std::chrono::seconds(2),
                                                    std::chrono::seconds(4)});
}

TEST_CASE("other_exit_codes_fail_without_retry") {
  std::size_t idx = 0;
  std::vector<AttemptResult> scripted = {res(std::nullopt, 1, false)};
  auto out = run_attempts(
      [&](std::uint32_t) -> Result<AttemptResult, PlayError> {
        return scripted[idx++];
      },
      [](std::uint32_t) { FAIL("no retry"); },
      [](std::chrono::seconds) { FAIL("no backoff"); });
  REQUIRE(!out.has_value());
  CHECK(out.error().kind == PlayError::Kind::Exit);
}

TEST_CASE("clean_exit_without_playback_is_ok_with_gate_shut") {
  std::size_t idx = 0;
  std::vector<AttemptResult> scripted = {res(std::nullopt, 0, true)};
  auto out = run_attempts(
      [&](std::uint32_t) -> Result<AttemptResult, PlayError> {
        return scripted[idx++];
      },
      [](std::uint32_t) {}, [](std::chrono::seconds) { FAIL("no backoff"); });
  REQUIRE(out.has_value());
  CHECK(!out->position.has_value());
}

TEST_CASE("hard_attempt_error_aborts_immediately") {
  auto out = run_attempts(
      [&](std::uint32_t) -> Result<AttemptResult, PlayError> {
        return err(PlayError::unsafe_arg("url"));
      },
      [](std::uint32_t) { FAIL("no retry"); },
      [](std::chrono::seconds) { FAIL("no backoff"); });
  REQUIRE(!out.has_value());
  CHECK(out.error().kind == PlayError::Kind::UnsafeArg);
}

// ===========================================================================
// IPC watcher
// ===========================================================================

TEST_CASE("meaningful_is_finite_and_positive") {
  for (double bad : {0.0, -3.0, std::nan(""), std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity()}) {
    CHECK(!meaningful(bad));
  }
  CHECK(meaningful(0.001));
}

TEST_CASE("ipc_handshake_events_and_final_position") {
  int sv[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  const int client = sv[0];
  const int server = sv[1];

  for (const char* line : {
           R"({"event":"property-change","id":1,"name":"time-pos","data":5.5})",
           R"({"event":"property-change","id":2,"name":"duration","data":24.0})",
           R"({"event":"property-change","id":1,"name":"time-pos","data":6.5})",
           R"({"event":"property-change","id":1,"name":"time-pos","data":null})",
           "not json at all",
           R"({"request_id":0,"error":"success"})",
           R"({"event":"property-change","id":1,"name":"time-pos","data":0.0})",
       }) {
    put(server, std::string(line) + "\n");
  }
  // EOF the client's read side so watch_ipc returns after draining.
  REQUIRE(::shutdown(server, SHUT_WR) == 0);

  Observed observed;
  std::mutex mtx;
  std::vector<PlayerEvent> events;
  watch_ipc(client, observed, mtx, [&](const PlayerEvent& e) { events.push_back(e); });

  // The two handshake commands must have reached the server, in order.
  for (auto [id, name] : {std::pair{1, "time-pos"}, std::pair{2, "duration"}}) {
    const std::string got = read_line(server);
    const json sent = json::parse(got);
    const json expected = {{"command", json::array({"observe_property", id, name})}};
    CHECK(sent == expected);
  }

  const std::vector<PlayerEvent> expected = {
      PlayerEvent::position(Position{5.5, std::nullopt}),
      PlayerEvent::position(Position{6.5, 24.0}),
      PlayerEvent::position(Position{0.0, 24.0}),
  };
  CHECK(events == expected);
  CHECK(observed.final_position() == std::optional<Position>(Position{6.5, 24.0}));

  ::close(client);
  ::close(server);
}

TEST_CASE("ipc_without_meaningful_position_keeps_the_gate_shut") {
  int sv[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  const int client = sv[0];
  const int server = sv[1];

  put(server, std::string(R"({"event":"property-change","id":1,"name":"time-pos","data":0.0})") + "\n");
  REQUIRE(::shutdown(server, SHUT_WR) == 0);

  Observed observed;
  std::mutex mtx;
  watch_ipc(client, observed, mtx, [](const PlayerEvent&) {});
  CHECK(observed.final_position() == std::nullopt);

  ::close(client);
  ::close(server);
}

TEST_CASE("ipc_rejects_a_peer_that_is_not_our_child") {
  std::string path = "/tmp/shigoku-peer-" + std::to_string(::getpid()) + ".sock";
  ::unlink(path.c_str());

  // A same-uid squatter: bind + listen, so connect() succeeds and the peer pid
  // is this test process, never the mpv child the watcher would expect.
  const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  REQUIRE(listener >= 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  REQUIRE(path.size() < sizeof(addr.sun_path));
  std::memcpy(addr.sun_path, path.data(), path.size());
  REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  REQUIRE(::listen(listener, 4) == 0);

  // gone=true bounds the loop to one reject-then-bail pass.
  std::atomic<bool> gone_true{true};
  const int rejected =
      connect_ipc(path, gone_true, static_cast<std::uint32_t>(::getpid()) + 1u);
  CHECK(rejected < 0);

  // The same socket IS adopted when the expected pid matches its peer (us).
  std::atomic<bool> gone_false{false};
  const int adopted =
      connect_ipc(path, gone_false, static_cast<std::uint32_t>(::getpid()));
  CHECK(adopted >= 0);
  if (adopted >= 0) ::close(adopted);

  ::close(listener);
  ::unlink(path.c_str());
}

TEST_CASE("parse_backend_unknown_reenters_at_mpv") {
  // P39 slice 2: config string -> kind; anything unrecognized (newer build's
  // value, hand-edit, empty) plays as mpv so a stale config never bricks play.
  CHECK(parse_backend("mpv") == BackendKind::Mpv);
  CHECK(parse_backend("mplayer") == BackendKind::Mplayer);
  CHECK(parse_backend("qmplay2") == BackendKind::Qmplay2);
  CHECK(parse_backend("") == BackendKind::Mpv);
  CHECK(parse_backend("vlc") == BackendKind::Mpv);
  CHECK(parse_backend("MPLAYER") == BackendKind::Mpv);  // exact-match only.
}

// ===========================================================================
// P39 slice 3: the mplayer backend — argv table, ANS_* parse, slave watcher
// ===========================================================================

TEST_CASE("mplayer_argv_full_house_matches_the_table") {
  const StreamLink link = full_link();
  auto o = opts("title is mpv-only", 42.5);
  auto r = build_mplayer_argv(link, link.url, o, "");
  REQUIRE(r.has_value());
  // Minimal by design (§9: alternates get playback, nothing beyond it): no
  // subs, no title, no aniskip, no demuxer tuning — and no ipc rendezvous.
  const std::vector<std::string> expected = {
      "-slave",
      "-quiet",
      "-ss",
      "42.5",
      "-user-agent",
      "Mozilla/5.0 (X11; Linux) Gecko",
      "-referrer",
      "https://ref.example/",
      "https://cdn.example/x.m3u8",
  };
  CHECK(*r == expected);
}

TEST_CASE("mplayer_argv_minimal_link_is_slave_quiet_url") {
  StreamLink link;
  link.url = "https://cdn.example/plain.mp4";
  auto o = opts("", 0.0);
  auto r = build_mplayer_argv(link, link.url, o, "");
  REQUIRE(r.has_value());
  const std::vector<std::string> expected = {"-slave", "-quiet",
                                             "https://cdn.example/plain.mp4"};
  CHECK(*r == expected);
}

TEST_CASE("mplayer_argv_rejects_dirty_provider_bytes") {
  // Same ROD-435/ROD-92 vetting as build_argv: every provider byte checked,
  // the positional can never read as a flag.
  StreamLink link = full_link();
  auto o = opts("", 0.0);
  SUBCASE("url reading as a flag") {
    auto r = build_mplayer_argv(link, "-fs", o, "");
    REQUIRE(!r.has_value());
    CHECK(r.error().kind == PlayError::Kind::UnsafeArg);
  }
  SUBCASE("crlf in referer") {
    link.referer = "https://ref.example/\r\nInjected: yes";
    auto r = build_mplayer_argv(link, link.url, o, "");
    REQUIRE(!r.has_value());
    CHECK(r.error().kind == PlayError::Kind::UnsafeArg);
  }
  SUBCASE("control byte in user agent") {
    link.user_agent = "Mozilla\x01Evil";
    auto r = build_mplayer_argv(link, link.url, o, "");
    REQUIRE(!r.has_value());
    CHECK(r.error().kind == PlayError::Kind::UnsafeArg);
  }
}

TEST_CASE("mplayer_local_argv_fences_a_leading_dash_and_carries_ss") {
  auto o = opts("", 42.5);
  auto r = build_mplayer_local_argv("-weird name.mkv", o, "");
  REQUIRE(r.has_value());
  const std::vector<std::string> expected = {"-slave", "-quiet", "-ss", "42.5",
                                             "./-weird name.mkv"};
  CHECK(*r == expected);
  auto plain = build_mplayer_local_argv("/dl/ep 1.mkv", opts("", 0.0), "");
  REQUIRE(plain.has_value());
  CHECK(*plain == std::vector<std::string>{"-slave", "-quiet", "/dl/ep 1.mkv"});
  auto empty = build_mplayer_local_argv("", o, "");
  REQUIRE(!empty.has_value());
  CHECK(empty.error().kind == PlayError::Kind::UnsafeArg);
}

TEST_CASE("parse_slave_answer_table") {
  // Accepts: exactly "ANS_<key>=<full-consume finite float>".
  CHECK(parse_slave_answer("ANS_time_pos=12.5", "time_pos") == 12.5);
  CHECK(parse_slave_answer("ANS_time_pos=0", "time_pos") == 0.0);
  CHECK(parse_slave_answer("ANS_LENGTH=24.0", "LENGTH") == 24.0);
  // Parse-level negatives pass; meaningful() filters them at the record gate.
  CHECK(parse_slave_answer("ANS_time_pos=-3.5", "time_pos") == -3.5);
  // Rejects: trailing junk (also the comma-locale half-parse shape), empty
  // number, wrong/truncated key, wrong separator, non-finite, overflow.
  CHECK(!parse_slave_answer("ANS_time_pos=12.3trailer", "time_pos"));
  CHECK(!parse_slave_answer("ANS_time_pos=", "time_pos"));
  CHECK(!parse_slave_answer("ANS_time_po=1", "time_pos"));
  CHECK(!parse_slave_answer("time_pos=1", "time_pos"));
  CHECK(!parse_slave_answer("ANS_time_pos:1", "time_pos"));
  CHECK(!parse_slave_answer("ANS_time_pos=1", "LENGTH"));
  CHECK(!parse_slave_answer("ANS_time_pos=nan", "time_pos"));
  CHECK(!parse_slave_answer("ANS_time_pos=inf", "time_pos"));
  CHECK(!parse_slave_answer("ANS_time_pos=1e999", "time_pos"));
  CHECK(!parse_slave_answer("", "time_pos"));
}

TEST_CASE("watch_slave_polls_answers_and_drains_the_exit_race") {
  // Two pipes stand in for the child's stdin/stdout: the test plays mplayer.
  int cmd[2];  // watcher writes commands -> test reads.
  int ans[2];  // test writes answers -> watcher reads.
  REQUIRE(::pipe(cmd) == 0);
  REQUIRE(::pipe(ans) == 0);

  Observed observed;
  std::mutex mtx;
  std::atomic<bool> gone{false};
  std::vector<PlayerEvent> events;
  std::mutex events_mtx;
  EventSink sink = [&](const PlayerEvent& e) {
    std::lock_guard<std::mutex> lk(events_mtx);
    events.push_back(e);
  };

  std::thread watcher([&] { watch_slave(cmd[1], ans[0], observed, mtx, sink, gone); });

  // Round 1: no duration yet -> both commands, in order.
  CHECK(read_line(cmd[0]) == "get_time_pos\n");
  CHECK(read_line(cmd[0]) == "get_time_length\n");
  put(ans[1], "ANS_time_pos=7.5\n");
  put(ans[1], "ANS_LENGTH=100\n");

  // Round 2 (~1s later): duration landed -> get_time_pos only.
  CHECK(read_line(cmd[0]) == "get_time_pos\n");
  put(ans[1], "ANS_time_pos=12.25\n");

  // The exit race: the answer above may still be buffered when waitpid
  // returns and `gone` flips — the drain must record it regardless.
  gone.store(true, std::memory_order_relaxed);
  watcher.join();

  {
    std::lock_guard<std::mutex> lk(mtx);
    REQUIRE(observed.meaningful_secs.has_value());
    CHECK(*observed.meaningful_secs == 12.25);
    REQUIRE(observed.duration.has_value());
    CHECK(*observed.duration == 100.0);
  }
  {
    std::lock_guard<std::mutex> lk(events_mtx);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == PlayerEvent::Kind::Position);
    CHECK(events[0].pos.secs == 7.5);
    CHECK(events[1].pos.secs == 12.25);
    CHECK(events[1].pos.duration == 100.0);  // rides once LENGTH landed.
  }

  for (const int fd : {cmd[0], cmd[1], ans[0], ans[1]}) ::close(fd);
}

TEST_CASE("mplayer_stub_play_local_lands_the_final_position") {
  // The full spawn -> slave-poll -> waitpid walk against stub_mplayer.py:
  // three advancing answers (3.0 / 9.0 / 20.0) + ANS_LENGTH=24.0, then a
  // clean exit — PlayDone with the last meaningful position is the
  // checkpoint the A6 law records.
  const std::string stub = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/stub_mplayer.py";
  PlayOpts o;
  o.mpv_path = "mpv-must-not-run";
  o.backend = BackendKind::Mplayer;
  o.player_path = stub;
  o.socket_dir = "/tmp/shigoku-player-test";
  std::vector<PlayerEvent> events;
  std::mutex events_mtx;
  auto r = play_local(o, "/tmp/does-not-need-to-exist.mkv", [&](const PlayerEvent& e) {
    std::lock_guard<std::mutex> lk(events_mtx);
    events.push_back(e);
  });
  REQUIRE(r.has_value());
  REQUIRE(r->position.has_value());
  CHECK(r->position->secs == 20.0);
  REQUIRE(r->position->duration.has_value());
  CHECK(*r->position->duration == 24.0);
  std::lock_guard<std::mutex> lk(events_mtx);
  std::size_t positions = 0;
  for (const auto& e : events) {
    if (e.kind == PlayerEvent::Kind::Position) ++positions;
  }
  CHECK(positions == 3);
}

// ===========================================================================
// P39 slice 4: the QMPlay2 backend — launch-only, the A6 gate stays shut
// ===========================================================================

TEST_CASE("qmplay2_argv_is_just_the_vetted_positional") {
  const StreamLink link = full_link();
  auto o = opts("ignored", 42.5);  // no -ss analogue: start offset dropped.
  auto r = build_qmplay2_argv(link, link.url, o, "");
  REQUIRE(r.has_value());
  CHECK(*r == std::vector<std::string>{"https://cdn.example/x.m3u8"});
  auto flag = build_qmplay2_argv(link, "-fs", o, "");
  REQUIRE(!flag.has_value());
  CHECK(flag.error().kind == PlayError::Kind::UnsafeArg);
  auto local = build_qmplay2_local_argv("-weird.mkv", o, "");
  REQUIRE(local.has_value());
  CHECK(*local == std::vector<std::string>{"./-weird.mkv"});
  auto empty = build_qmplay2_local_argv("", o, "");
  REQUIRE(!empty.has_value());
  CHECK(empty.error().kind == PlayError::Kind::UnsafeArg);
}

TEST_CASE("qmplay2_stub_playdone_without_position_keeps_the_gate_shut") {
  // Launch-only (A6 re-pinned through this backend): spawn + waitpid, no
  // watcher, no Position events ever — a clean exit is PlayDone with
  // position=nullopt, so recordPlay writes NOTHING for this play.
  const std::string stub = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/stub_qmplay2.py";
  PlayOpts o;
  o.mpv_path = "mpv-must-not-run";
  o.backend = BackendKind::Qmplay2;
  o.player_path = stub;
  o.socket_dir = "/tmp/shigoku-player-test";
  std::vector<PlayerEvent> events;
  std::mutex events_mtx;
  auto r = play_local(o, "/tmp/does-not-need-to-exist.mkv", [&](const PlayerEvent& e) {
    std::lock_guard<std::mutex> lk(events_mtx);
    events.push_back(e);
  });
  REQUIRE(r.has_value());
  CHECK(!r->position.has_value());
  std::lock_guard<std::mutex> lk(events_mtx);
  for (const auto& e : events) CHECK(e.kind != PlayerEvent::Kind::Position);
}
