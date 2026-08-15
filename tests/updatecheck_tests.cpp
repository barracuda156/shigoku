// updatecheck_tests.cpp — P28 boot-time update check tests.
//
// Ports sabigoku src/updatecheck.rs #[test] cases 1:1 (§8: golden tests are
// the port contract) for the pure/fs pieces (is_fresh, parse_cache,
// parse_latest_tag, cache round-trip), plus a live-fixture pass over
// fetch_latest/check/latest_fresh through the SHIGOKU_UPDATE_URL test seam and
// tests/http_tests.cpp's OneShotServer (no real GitHub hit).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "../src/http.hpp"
#include "../src/updatecheck.hpp"

using namespace shigoku;
using namespace shigoku::updatecheck;

namespace {

// Same shape as http_tests.cpp's OneShotServer (a separate translation unit,
// so duplicated rather than shared — the test binaries don't link each other).
struct OneShotServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;

  explicit OneShotServer(std::vector<std::uint8_t> response) {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
    REQUIRE(::listen(listen_fd, 1) == 0);

    thread = std::thread([this, response = std::move(response)]() mutable {
      const int cfd = ::accept(listen_fd, nullptr, nullptr);
      if (cfd < 0) return;
      char buf[16384];
      (void)::read(cfd, buf, sizeof(buf));
      ::write(cfd, response.data(), response.size());
      ::close(cfd);
    });
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/"; }

  ~OneShotServer() {
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) ::close(listen_fd);
  }
};

std::vector<std::uint8_t> json_response(const std::string& body) {
  std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" +
                     ("Content-Length: " + std::to_string(body.size())) +
                     "\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// RAII env var setter (SHIGOKU_UPDATE_URL test seam), restoring on scope exit
// so tests never leak state into each other.
struct ScopedEnv {
  std::string key;
  bool had_prior = false;
  std::string prior;
  ScopedEnv(std::string k, const std::string& value) : key(std::move(k)) {
    if (const char* p = std::getenv(key.c_str())) {
      had_prior = true;
      prior = p;
    }
    ::setenv(key.c_str(), value.c_str(), 1);
  }
  ~ScopedEnv() {
    if (had_prior) {
      ::setenv(key.c_str(), prior.c_str(), 1);
    } else {
      ::unsetenv(key.c_str());
    }
  }
};

std::string temp_dir(const char* name) {
  auto dir = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(dir);
  return dir.string();
}

}  // namespace

TEST_CASE("is_fresh_within_ttl_stale_past_and_future") {
  const std::int64_t now = 1'000'000;
  CHECK(is_fresh(now, now));
  CHECK(is_fresh(now - (kCheckTtlSecs - 1), now));
  CHECK_FALSE(is_fresh(now - kCheckTtlSecs, now));
  CHECK_FALSE(is_fresh(now + 1, now));
}

// A release build's wrapped subtraction read i64::MIN as permanently fresh,
// silencing the check forever from one edited cache line.
TEST_CASE("is_fresh_survives_adversarial_extremes") {
  const std::int64_t now = 1'700'000'000;
  CHECK_FALSE(is_fresh(std::numeric_limits<std::int64_t>::min(), now));
  CHECK_FALSE(is_fresh(std::numeric_limits<std::int64_t>::min() + 1, now));
  CHECK_FALSE(is_fresh(std::numeric_limits<std::int64_t>::max(), now));
  CHECK_FALSE(is_fresh(-1, now));
}

TEST_CASE("parse_cache_valid_two_line_body") {
  auto entry = parse_cache("1700000000\nv0.5.0\n");
  REQUIRE(entry.has_value());
  CHECK(entry->checked_at == 1'700'000'000);
  CHECK(entry->latest == "v0.5.0");
}

TEST_CASE("parse_cache_tolerates_trailing_cr_and_no_final_newline") {
  auto entry = parse_cache("1700000000\r\nv0.5.0");
  REQUIRE(entry.has_value());
  CHECK(entry->checked_at == 1'700'000'000);
  CHECK(entry->latest == "v0.5.0");
}

TEST_CASE("parse_cache_rejects_malformed_bodies") {
  for (const char* bad :
       {"", "1700000000\n", "1700000000\n\n", "notanumber\nv0.5.0\n", "onlyoneline"}) {
    INFO("bad = ", bad);
    CHECK_FALSE(parse_cache(bad).has_value());
  }
}

// The cache file is as untrusted as the network body; a poisoned tag line
// must come out escape-free and capped, not ride into App state.
TEST_CASE("parse_cache_sanitizes_and_caps_the_tag_line") {
  auto poisoned = parse_cache("1700000000\nv1.0.0\x1b[2J\x07\n");
  REQUIRE(poisoned.has_value());
  CHECK(poisoned->latest == "v1.0.0[2J");

  std::string flood_line(10'000'000, 'x');
  auto flood = parse_cache("1700000000\n" + flood_line + "\n");
  REQUIRE(flood.has_value());
  CHECK(flood->latest.size() == 64);

  CHECK_FALSE(parse_cache("1700000000\n\x1b\x07\n").has_value());
}

// write_cache is private; exercised indirectly through latest_fresh (the
// public surface that calls it) against a live fixture — asserts the cache
// file lands and no .tmp survives the write-then-rename.
TEST_CASE("latest_fresh_writes_the_cache_and_leaves_no_tmp_behind") {
  const std::string dir = temp_dir("shigoku-updatecheck-tmpfile");
  std::filesystem::create_directories(dir);
  OneShotServer srv(json_response(R"({"tag_name":"v0.9.0"})"));
  ScopedEnv seam("SHIGOKU_UPDATE_URL", srv.url());
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  auto tag = latest_fresh(*client, dir, 1'700'000'000);
  REQUIRE(tag.has_value());
  CHECK(*tag == "v0.9.0");
  CHECK(std::filesystem::exists(dir + "/update_check"));
  CHECK_FALSE(std::filesystem::exists(dir + "/update_check.tmp"));
}

TEST_CASE("parse_latest_tag_pulls_tag_name_ignores_the_rest") {
  const std::string body = R"({"url":"https://api.github.com/x","tag_name":"v0.5.0","draft":false})";
  auto tag = parse_latest_tag(body);
  REQUIRE(tag.has_value());
  CHECK(*tag == "v0.5.0");
}

TEST_CASE("parse_latest_tag_rejects_no_usable_tag") {
  CHECK_FALSE(parse_latest_tag(R"({"tag_name":""})").has_value());
  CHECK_FALSE(parse_latest_tag("{}").has_value());
  CHECK_FALSE(parse_latest_tag("not json").has_value());
}

TEST_CASE("parse_latest_tag_strips_terminal_escapes_from_a_forged_tag") {
  auto a = parse_latest_tag("{\"tag_name\":\"v1.0\\u001b[2J\"}");
  REQUIRE(a.has_value());
  CHECK(*a == "v1.0[2J");
  CHECK_FALSE(parse_latest_tag("{\"tag_name\":\"\\u0000\\u0007\"}").has_value());
}

TEST_CASE("check_answers_from_a_fresh_cache_without_network") {
  const std::string dir = temp_dir("shigoku-updatecheck-fresh");
  std::filesystem::create_directories(dir);
  {
    std::ofstream f(dir + "/update_check", std::ios::binary | std::ios::trunc);
    f << 1'700'000'000 << "\n" << "v0.9.0" << "\n";
  }
  // Point the seam at an unreachable address: a fresh cache must short-circuit
  // before any fetch, so reaching the network here would be the bug.
  ScopedEnv seam("SHIGOKU_UPDATE_URL", "http://127.0.0.1:1/unreachable");
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  const std::int64_t now = 1'700'000'000;

  auto a = check(*client, dir, "0.8.0", now);
  REQUIRE(a.has_value());
  CHECK(*a == "v0.9.0");
  CHECK_FALSE(check(*client, dir, "0.9.0", now).has_value());
  CHECK_FALSE(check(*client, dir, "1.0.0", now).has_value());
}

TEST_CASE("check_fetches_and_caches_on_a_cold_start") {
  const std::string dir = temp_dir("shigoku-updatecheck-cold");
  OneShotServer srv(json_response(R"({"tag_name":"v2.0.0"})"));
  ScopedEnv seam("SHIGOKU_UPDATE_URL", srv.url());
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  auto tag = check(*client, dir, "1.0.0", 1'700'000'000);
  REQUIRE(tag.has_value());
  CHECK(*tag == "v2.0.0");
  CHECK(std::filesystem::exists(dir + "/update_check"));
}

TEST_CASE("latest_fresh_bypasses_a_fresh_cache") {
  const std::string dir = temp_dir("shigoku-updatecheck-bypass");
  std::filesystem::create_directories(dir);
  {
    std::ofstream f(dir + "/update_check", std::ios::binary | std::ios::trunc);
    f << 1'700'000'000 << "\n" << "v0.1.0" << "\n";  // stale-looking on purpose.
  }
  OneShotServer srv(json_response(R"({"tag_name":"v3.0.0"})"));
  ScopedEnv seam("SHIGOKU_UPDATE_URL", srv.url());
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  auto tag = latest_fresh(*client, dir, 1'700'000'000);
  REQUIRE(tag.has_value());
  CHECK(*tag == "v3.0.0");  // network answer, not the cached v0.1.0.
}
