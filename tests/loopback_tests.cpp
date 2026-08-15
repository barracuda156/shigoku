// loopback_tests.cpp — P19 OAuth loopback listener tests.
//
// Ports the sabigoku loopback.rs #[test] cases 1:1 (§8: golden tests are the
// port contract). Real sockets over an ephemeral loopback port; no real
// AniList network call (the Verifier is faked).

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
#include "../src/loopback.hpp"
#include "../src/login.hpp"

using namespace shigoku;
using namespace shigoku::login;
using namespace shigoku::loopback;

namespace {

std::string tmp(const char* name) {
  std::string path = "/tmp/shigoku-loopback-test-";
  path += std::to_string(static_cast<long>(::getpid()));
  path += "-";
  path += name;
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

constexpr const char* kToken = "abcdefghijklmnopqrstuvwxyz012345";

Verifier fake_verifier() {
  return [](std::string_view) -> Result<std::optional<anilist::Viewer>, ProviderError> {
    return std::optional<anilist::Viewer>(anilist::Viewer{7, "rod"});
  };
}

// Connect to 127.0.0.1:port, blocking (test helper).
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

TEST_CASE("relay then callback completes login") {
  const std::string path = tmp("e2e.json");
  auto started = Loopback::start_ephemeral();
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  const std::uint16_t port = lp.port();
  const std::string url = lp.authorize_url();
  auto nonce = login::detail::param(url, "state");
  REQUIRE(nonce.has_value());
  const Verifier verifier = fake_verifier();

  ConnectResult result;
  std::thread server([&] { result = lp.serve(verifier, path, 1000, [] {}); });

  // First hit: any path -> relay HTML.
  {
    const int c1 = connect_to(port);
    send_all(c1, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const std::string relay = read_response(c1);
    CHECK(relay.find("location.hash.substring(1)") != std::string::npos);
  }

  // Callback carrying the token + matching state.
  {
    const int c2 = connect_to(port);
    send_all(c2, "GET /callback?access_token=" + std::string(kToken) +
                     "&expires_in=3600&state=" + *nonce + " HTTP/1.1\r\n\r\n");
    const std::string done = read_response(c2);
    CHECK(done.find("signed in to AniList") != std::string::npos);
    CHECK(done.find("history.replaceState") != std::string::npos);
    CHECK(done.find("shigoku") != std::string::npos);
  }

  server.join();
  CHECK(result == ConnectResult::ok("rod"));
  auto auth = Auth::load(path);
  REQUIRE(auth.anilist.bearer().has_value());
  CHECK(*auth.anilist.bearer() == kToken);
}

// The forged callback must neither persist nor end the wait (ROD-283); the
// real callback afterward still completes the login.
TEST_CASE("callback with wrong state warns and keeps waiting") {
  const std::string path = tmp("badstate.json");
  auto started = Loopback::start_ephemeral();
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  const std::uint16_t port = lp.port();
  const std::string url = lp.authorize_url();
  auto nonce = login::detail::param(url, "state");
  REQUIRE(nonce.has_value());
  const Verifier verifier = fake_verifier();
  std::atomic<int> bad_hits{0};

  ConnectResult result;
  std::thread server([&] {
    result = lp.serve(verifier, path, 0, [&] { bad_hits.fetch_add(1, std::memory_order_relaxed); });
  });

  {
    const int c = connect_to(port);
    send_all(c, "GET /callback?access_token=" + std::string(kToken) + "&state=forged HTTP/1.1\r\n\r\n");
    const std::string forged = read_response(c);
    CHECK(forged.find("sign-in didn't complete") != std::string::npos);
  }
  CHECK(!std::ifstream(path).good());  // a forged state must never verify or persist.

  {
    const int c2 = connect_to(port);
    send_all(c2, "GET /callback?access_token=" + std::string(kToken) + "&state=" + *nonce +
                     " HTTP/1.1\r\n\r\n");
    (void)read_response(c2);
  }

  server.join();
  CHECK(result.kind == ConnectResult::Kind::Ok);
  CHECK(bad_hits.load() == 1);
}

TEST_CASE("relay redirects without scrub results scrub first") {
  const std::string relay = loopback::detail::relay_page();
  CHECK(relay.find("location.hash.substring(1)") != std::string::npos);
  CHECK(relay.find("history.replaceState") == std::string::npos);

  const std::string fail = loopback::detail::fail_page();
  CHECK(fail.find("sign-in didn't complete") != std::string::npos);
  CHECK(fail.rfind("<!doctype html><html><head><meta charset=\"utf-8\">"
                    "<script>history.replaceState",
                    0) == 0);
}

TEST_CASE("cancel wakes a blocked serve") {
  const std::string path = tmp("cancel.json");
  auto started = Loopback::start_ephemeral();
  REQUIRE(started.has_value());
  Loopback lp = std::move(*started);
  Canceler canceler = lp.canceler();
  const Verifier verifier = fake_verifier();

  ConnectResult result;
  std::thread server([&] { result = lp.serve(verifier, path, 0, [] {}); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // ensure accept() is parked.
  canceler.cancel();
  server.join();

  CHECK(result == ConnectResult::canceled());
}
