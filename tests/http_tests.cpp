// http_tests.cpp — P2 transport + fetch-guard tests.
//
// Ports src/providers/http.rs and src/fetchguard.rs #[test]s 1:1 (§8: golden
// tests are the port contract). The blocking fixture server is the C++ port of
// testutil.rs serve_once / serve_once_capture (P2 DoD: "port the spirit").
//
// Endianness: the guard assembles addresses from bytes via shift/mask
// arithmetic (value semantics, not memory layout), so these results are
// identical on big- and little-endian hosts (§3). The fixture server speaks
// wire-format HTTP text, also endian-neutral.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/http.hpp"

using namespace shigoku;
using namespace shigoku::http;

// ---------------------------------------------------------------------------
// Fixture server: bind 127.0.0.1:0, accept one connection, drain the request
// (optionally capturing it), write `response`, close. Mirrors serve_once /
// serve_once_capture. Returns the URL; the captured request bytes (if any) are
// delivered through a shared promise the caller joins on.
// ---------------------------------------------------------------------------
struct OneShotServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;
  std::string captured;  // request bytes, filled when capture=true.
  std::atomic<bool> done{false};

  explicit OneShotServer(std::vector<std::uint8_t> response, bool capture = false) {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral.
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);  // ntohs: wire->host, endian-correct.
    REQUIRE(::listen(listen_fd, 1) == 0);

    thread = std::thread([this, response = std::move(response), capture]() mutable {
      const int cfd = ::accept(listen_fd, nullptr, nullptr);
      if (cfd < 0) return;
      char buf[16384];
      ssize_t n = ::read(cfd, buf, sizeof(buf));
      if (capture && n > 0) captured.assign(buf, buf + n);
      ::write(cfd, response.data(), response.size());
      ::close(cfd);
      done.store(true);
    });
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/"; }

  std::string join_captured() {
    if (thread.joinable()) thread.join();
    return captured;
  }

  ~OneShotServer() {
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) ::close(listen_fd);
  }
};

static std::vector<std::uint8_t> response_with_body(const std::string& status,
                                                    const std::string& body) {
  std::string head = "HTTP/1.1 " + status + "\r\nContent-Type: text/plain\r\n" +
                     "Content-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

static Result<std::vector<std::uint8_t>, ProviderError> get_against(
    std::vector<std::uint8_t> response, Accept accept) {
  OneShotServer srv(std::move(response));
  auto client = Client::create();
  REQUIRE(client.has_value());
  Request req;
  req.method = Method::Get;
  req.url = srv.url();
  req.user_agent = "shigoku-test";
  req.accept = accept;
  return client->fetch(req);
}

// ===========================================================================
// Client tests (http.rs) — same names as the Rust #[test]s.
// ===========================================================================

TEST_CASE("assert_http2_available: this curl has nghttp2") {
  // The build REQUIRES nghttp2 (§3). If this fails the local curl is misbuilt.
  auto r = assert_http2_available();
  if (!r.has_value()) FAIL(r.error());
  CHECK(r.has_value());
}

TEST_CASE("ok_body_returned") {
  auto got = get_against(response_with_body("200 OK", "hello"), Accept::Any2xx);
  REQUIRE(got.has_value());
  const std::string s(got->begin(), got->end());
  CHECK(s == "hello");
}

TEST_CASE("ok_only_rejects_other_2xx") {
  auto got = get_against(response_with_body("204 No Content", ""), Accept::OkOnly);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Http);
  CHECK(got.error().status == 204);

  auto ok = get_against(response_with_body("204 No Content", ""), Accept::Any2xx);
  CHECK(ok.has_value());
}

TEST_CASE("status_classes_map_to_taxonomy") {
  auto f = get_against(response_with_body("403 Forbidden", ""), Accept::Any2xx);
  REQUIRE_FALSE(f.has_value());
  CHECK(f.error().kind == ProviderError::Kind::Forbidden);
  CHECK(f.error().status == 403);

  auto s = get_against(response_with_body("503 Service Unavailable", ""), Accept::Any2xx);
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().kind == ProviderError::Kind::Server);
  CHECK(s.error().status == 503);

  auto h = get_against(response_with_body("404 Not Found", ""), Accept::Any2xx);
  REQUIRE_FALSE(h.has_value());
  CHECK(h.error().kind == ProviderError::Kind::Http);
  CHECK(h.error().status == 404);
}

TEST_CASE("fingerprint_none_rides_the_stock_path_unchanged") {
  // P25: the vtable refactor must leave the default (None) path byte-identical
  // to the pre-P25 stock libcurl fetch. A plain GET over the fixture server
  // still returns its body.
  OneShotServer srv(response_with_body("200 OK", "stock-ok"));
  auto client = Client::create();
  REQUIRE(client.has_value());
  Request req;
  req.method = Method::Get;
  req.url = srv.url();
  req.user_agent = "shigoku-test";
  // fingerprint defaults to None — do not set it, to prove the default.
  CHECK(req.fingerprint == Fingerprint::None);
  auto got = client->fetch(req);
  REQUIRE(got.has_value());
  const std::string s(got->begin(), got->end());
  CHECK(s == "stock-ok");
}

TEST_CASE("fingerprint_request_without_the_impersonate_dylib_is_a_clean_error") {
  // P25: when a request asks for a browser fingerprint but libcurl-impersonate
  // is absent (or too old to export curl_easy_impersonate), fetch() must return
  // a Network error, NEVER crash — the provider is unavailable and the fallback
  // walk hops. On this dev box there is no impersonate dylib, so Chrome resolves
  // to "unavailable". (Where the dylib IS present, this fetch would instead try
  // the loopback and fail to connect — also Network — so the assertion holds on
  // both kinds of host: it is a clean error either way, never a crash.)
  auto client = Client::create();
  REQUIRE(client.has_value());
  Request req;
  req.method = Method::Get;
  req.url = "https://127.0.0.1:9/";  // nothing listening; must not be reached anyway.
  req.user_agent = "shigoku-test";
  req.fingerprint = Fingerprint::Chrome;
  auto got = client->fetch(req);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Network);
}

TEST_CASE("redirect_is_refused_not_followed") {
  // A 302 must come back as Http{302}, never followed to the Location.
  std::string raw =
      "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:9/\r\n"
      "Content-Length: 0\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> resp(raw.begin(), raw.end());
  auto got = get_against(std::move(resp), Accept::Any2xx);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Http);
  CHECK(got.error().status == 302);
}

TEST_CASE("connection_refused_is_network") {
  auto client = Client::create();
  REQUIRE(client.has_value());
  Request req;
  req.url = "http://127.0.0.1:1/x";  // nothing listens on port 1.
  req.user_agent = "shigoku-test";
  req.accept = Accept::Any2xx;
  req.timeout_secs = 3;
  auto got = client->fetch(req);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Network);
}

TEST_CASE("body_at_cap_accepted_one_over_refused") {
  std::string at_cap(kMaxRespBytes, 'x');
  auto ok = get_against(response_with_body("200 OK", at_cap), Accept::Any2xx);
  REQUIRE(ok.has_value());
  CHECK(ok->size() == kMaxRespBytes);

  std::string over(kMaxRespBytes + 1, 'x');
  auto bad = get_against(response_with_body("200 OK", over), Accept::Any2xx);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("post_carries_payload_and_headers") {
  OneShotServer srv(response_with_body("200 OK", "ok"), /*capture=*/true);
  auto client = Client::create();
  REQUIRE(client.has_value());
  Request req;
  req.method = Method::Post;
  req.url = srv.url();
  req.content_type = "application/json";
  const std::string payload = "{\"q\":1}";
  req.body.assign(payload.begin(), payload.end());
  req.user_agent = "shigoku-test";
  req.extra_headers = {{"Referer", "https://ref.example/"}};
  req.accept = Accept::OkOnly;
  auto got = client->fetch(req);
  REQUIRE(got.has_value());
  const std::string seen = srv.join_captured();

  auto has = [&](const std::string& needle) {
    // Header names are case-insensitive on the wire; lowercase the haystack.
    std::string low = seen;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    std::string n = needle;
    for (char& c : n) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return low.find(n) != std::string::npos;
  };
  CHECK(seen.rfind("POST / HTTP/", 0) == 0);
  CHECK(has("user-agent: shigoku-test"));
  CHECK(has("content-type: application/json"));
  CHECK(has("referer: https://ref.example/"));
  CHECK(seen.size() >= payload.size());
  CHECK(seen.compare(seen.size() - payload.size(), payload.size(), payload) == 0);
}

// ===========================================================================
// Fetch-guard tests (fetchguard.rs) — same names, 1:1 vectors.
// ===========================================================================

static bool blocked_host(std::string_view u) {
  auto r = guard_fetch_url(u);
  return !r.has_value() && r.error() == GuardError::BlockedHost;
}
static bool bad_url(std::string_view u) {
  auto r = guard_fetch_url(u);
  return !r.has_value() && r.error() == GuardError::BadUrl;
}
static bool allowed(std::string_view u) { return guard_fetch_url(u).has_value(); }

TEST_CASE("private_v4_ranges") {
  using guard_detail::private_v4;
  struct { std::uint8_t o[4]; } blocked[] = {
      {{0, 1, 2, 3}},        {{10, 1, 2, 3}},    {{127, 0, 0, 1}},
      {{100, 64, 0, 1}},     {{169, 254, 169, 254}}, {{172, 16, 0, 1}},
      {{192, 168, 1, 1}},    {{224, 0, 0, 1}},   {{255, 255, 255, 255}},
  };
  for (auto& b : blocked) CHECK(private_v4(b.o[0], b.o[1], b.o[2], b.o[3]));
  struct { std::uint8_t o[4]; } pub[] = {
      {{8, 8, 8, 8}},   {{93, 184, 216, 34}}, {{172, 32, 0, 1}},
      {{100, 128, 0, 1}}, {{169, 253, 0, 1}},
  };
  for (auto& p : pub) CHECK_FALSE(private_v4(p.o[0], p.o[1], p.o[2], p.o[3]));
}

TEST_CASE("private_v6_ranges") {
  using guard_detail::parse_ipv6;
  using guard_detail::private_v6;
  auto is_priv = [](const char* s) {
    std::uint16_t seg[8];
    REQUIRE(parse_ipv6(s, seg));
    return private_v6(seg);
  };
  CHECK(is_priv("::"));
  CHECK(is_priv("::1"));
  CHECK(is_priv("fe80::1"));
  CHECK(is_priv("fd00::1"));
  CHECK(is_priv("fc00::1"));
  CHECK(is_priv("::ffff:127.0.0.1"));
  CHECK(is_priv("::127.0.0.1"));         // IPv4-compatible (ROD-436).
  CHECK(is_priv("::169.254.169.254"));
  CHECK(is_priv("::10.0.0.1"));
  CHECK(is_priv("::2"));                  // ::/96 lands in 0/8.
  CHECK_FALSE(is_priv("2001:4860:4860::8888"));
  CHECK_FALSE(is_priv("::ffff:8.8.8.8"));
  CHECK_FALSE(is_priv("::8.8.8.8"));      // compatible-form public.
}

TEST_CASE("bad_urls_rejected") {
  for (const char* bad : {
           "not a url",
           "ftp://host/v.ts",
           "file:///etc/passwd",
           "data:text/plain,hi",
           "https://allanime.day@evil.example/x",
           "https://user:pw@evil.example/x",
           "http://",
       }) {
    CHECK_MESSAGE(bad_url(bad), bad);
  }
}

TEST_CASE("ssrf_vectors_blocked") {
  for (const char* v : {
           "http://127.0.0.1/x",
           "http://169.254.169.254/latest/meta-data/",
           "http://localhost:8080/admin",
           "http://sub.localhost/x",
           "http://[::1]/x",
           "http://[fe80::1]/x",
           "http://10.0.0.5/x",
           "http://[::169.254.169.254]/latest/meta-data/",
           "http://[::127.0.0.1]/x",
       }) {
    CHECK_MESSAGE(blocked_host(v), v);
  }
}

TEST_CASE("public_hosts_allowed") {
  for (const char* ok : {
           "https://cdn.real.example/v.m3u8",
           "https://allanime.day/apivtwo/clock.json?id=x",
           "http://8.8.8.8/x",
           "https://s4.anilist.co/file/cover.jpg",
       }) {
    CHECK_MESSAGE(allowed(ok), ok);
  }
}

TEST_CASE("percent_encoded_host_bypass_blocked") {
  CHECK(blocked_host("http://127%2e0%2e0%2e1/x"));
  CHECK(blocked_host("http://%6c%6fcalhost:8080/x"));
}

TEST_CASE("alternate_ipv4_spellings_blocked") {
  for (const char* v : {
           "http://2130706433/x",
           "http://2852039166/latest",
           "http://0x7f000001/x",
           "http://0x7f.0.0.1/x",
           "http://127.1/x",
           "http://127.0.0.1./x",
           "http://[::ffff:7f00:1]/x",
       }) {
    CHECK_MESSAGE(blocked_host(v), v);
  }
}

TEST_CASE("numeric_spelling_backstop") {
  using guard_detail::numeric_spelling;
  CHECK(numeric_spelling("0xanything"));
  CHECK(numeric_spelling("123.456.789"));
  CHECK(numeric_spelling("2130706433"));
  CHECK_FALSE(numeric_spelling("example.com"));
  CHECK_FALSE(numeric_spelling("123abc.com"));
  CHECK_FALSE(numeric_spelling(""));
}
