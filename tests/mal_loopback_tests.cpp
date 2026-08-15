// mal_loopback_tests.cpp — P31 §9.1 slice 2 MAL OAuth loopback listener
// tests. No sabigoku golden reference (MAL never existed there). Real
// sockets over an ephemeral loopback port; no real MAL network call (the
// Exchanger is faked).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "../src/auth.hpp"
#include "../src/login.hpp"
#include "../src/mal_loopback.hpp"

using namespace shigoku;
using namespace shigoku::mal_login;
using namespace shigoku::mal_loopback;

namespace {

std::string tmp(const char* name) {
  std::string path = "/tmp/shigoku-mal-loopback-test-";
  path += std::to_string(static_cast<long>(::getpid()));
  path += "-";
  path += name;
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

constexpr const char* kClientId = "test-client-id";
constexpr const char* kAccessToken = "mal-access-token-value-ok";

Exchanger fake_exchanger() {
  return [](std::string_view) -> Result<TokenResponse, ProviderError> {
    TokenResponse t;
    t.access_token = kAccessToken;
    t.refresh_token = "mal-refresh-token-value-ok";
    t.token_type = "Bearer";
    t.expires_in = 3600;
    return t;
  };
}

int connect_to(std::uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  return fd;
}

void send_all(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
    REQUIRE(n > 0);
    off += static_cast<std::size_t>(n);
  }
}

std::string read_response(int fd) {
  std::string out;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return out;
}

}  // namespace

TEST_CASE("callback with code and matching state completes login") {
  const std::string path = tmp("e2e.json");
  auto started = Loopback::start_ephemeral(kClientId);
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  const std::uint16_t port = lp.port();
  const std::string state = lp.state();
  const Exchanger exchanger = fake_exchanger();

  MalConnectResult result;
  std::thread server([&] { result = lp.serve(exchanger, path, 1000, [] {}); });

  {
    const int c = connect_to(port);
    send_all(c, "GET /mal/callback?code=authcode123&state=" + state + " HTTP/1.1\r\n\r\n");
    const std::string done = read_response(c);
    CHECK(done.find("signed in to MyAnimeList") != std::string::npos);
    CHECK(done.find("history.replaceState") != std::string::npos);
    CHECK(done.find("shigoku") != std::string::npos);
  }

  server.join();
  CHECK(result.kind == MalConnectResult::Kind::Ok);
  auto auth = Auth::load(path);
  REQUIRE(auth.mal.bearer().has_value());
  CHECK(*auth.mal.bearer() == kAccessToken);
}

// No relay-page hop for MAL: the very first hit already carries code+state
// (unlike AniList's Implicit Grant, there is no location.hash to bounce
// through JS first) — a non-callback path is just ignored, never treated as
// a relay step.
TEST_CASE("a non callback path is ignored not treated as a relay hit") {
  const std::string path = tmp("ignored.json");
  auto started = Loopback::start_ephemeral(kClientId);
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  const std::uint16_t port = lp.port();
  const std::string state = lp.state();
  const Exchanger exchanger = fake_exchanger();

  MalConnectResult result;
  std::thread server([&] { result = lp.serve(exchanger, path, 0, [] {}); });

  {
    // A stray probe (e.g. favicon.ico) gets no response body worth checking;
    // the connection is simply dropped and serve() keeps waiting.
    const int c = connect_to(port);
    send_all(c, "GET /favicon.ico HTTP/1.1\r\nHost: localhost\r\n\r\n");
    (void)read_response(c);
  }

  {
    const int c2 = connect_to(port);
    send_all(c2, "GET /mal/callback?code=authcode123&state=" + state + " HTTP/1.1\r\n\r\n");
    (void)read_response(c2);
  }

  server.join();
  CHECK(result.kind == MalConnectResult::Kind::Ok);
}

// Forged state must neither exchange nor persist, and must not end the
// wait — same ROD-283 precedent as loopback.cpp's AniList path.
TEST_CASE("callback with wrong state warns and keeps waiting") {
  const std::string path = tmp("badstate.json");
  auto started = Loopback::start_ephemeral(kClientId);
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  const std::uint16_t port = lp.port();
  const std::string state = lp.state();
  std::atomic<int> bad_hits{0};
  bool exchanger_called = false;
  Exchanger spy = [&exchanger_called](std::string_view) -> Result<TokenResponse, ProviderError> {
    exchanger_called = true;
    TokenResponse t;
    t.access_token = kAccessToken;
    return t;
  };

  MalConnectResult result;
  std::thread server([&] {
    result = lp.serve(spy, path, 0, [&] { bad_hits.fetch_add(1, std::memory_order_relaxed); });
  });

  {
    const int c = connect_to(port);
    send_all(c, "GET /mal/callback?code=authcode123&state=forged HTTP/1.1\r\n\r\n");
    const std::string forged = read_response(c);
    CHECK(forged.find("sign-in didn't complete") != std::string::npos);
  }
  CHECK(!exchanger_called);  // a forged state must never reach the exchange.
  CHECK(!std::ifstream(path).good());

  {
    const int c2 = connect_to(port);
    send_all(c2, "GET /mal/callback?code=authcode123&state=" + state + " HTTP/1.1\r\n\r\n");
    (void)read_response(c2);
  }

  server.join();
  CHECK(result.kind == MalConnectResult::Kind::Ok);
  CHECK(bad_hits.load() == 1);
}

TEST_CASE("callback missing code is no code and keeps nothing persisted") {
  const std::string path = tmp("nocode.json");
  auto started = Loopback::start_ephemeral(kClientId);
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  const std::uint16_t port = lp.port();
  const std::string state = lp.state();
  const Exchanger exchanger = fake_exchanger();

  MalConnectResult result;
  std::thread server([&] { result = lp.serve(exchanger, path, 0, [] {}); });

  {
    const int c = connect_to(port);
    send_all(c, "GET /mal/callback?state=" + state + " HTTP/1.1\r\n\r\n");
    (void)read_response(c);
  }

  server.join();
  CHECK(result == MalConnectResult::no_code());
  CHECK(Auth::load(path) == Auth{});
}

TEST_CASE("authorize url embeds the loopback's own state and code challenge") {
  auto started = Loopback::start_ephemeral(kClientId);
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  const std::string url = lp.authorize_url();
  CHECK(url.find(lp.state()) != std::string::npos);
  auto challenge = login::detail::param(url, "code_challenge");
  REQUIRE(challenge.has_value());
  CHECK(*challenge == lp.code_verifier());  // "plain": challenge == verifier.
}

TEST_CASE("cancel wakes a blocked serve") {
  const std::string path = tmp("cancel.json");
  auto started = Loopback::start_ephemeral(kClientId);
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  Canceler canceler = lp.canceler();
  const Exchanger exchanger = fake_exchanger();

  MalConnectResult result;
  std::thread server([&] { result = lp.serve(exchanger, path, 0, [] {}); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // ensure accept() is parked.
  canceler.cancel();
  server.join();

  CHECK(result == MalConnectResult::canceled());
}

TEST_CASE("success and fail pages carry the mal wordmark and scrub the bar") {
  const std::string ok = mal_loopback::detail::success_page();
  CHECK(ok.find("signed in to MyAnimeList") != std::string::npos);
  CHECK(ok.rfind("<!doctype html><html><head><meta charset=\"utf-8\">"
                  "<script>history.replaceState",
                  0) == 0);

  const std::string fail = mal_loopback::detail::fail_page();
  CHECK(fail.find("sign-in didn't complete") != std::string::npos);
}
