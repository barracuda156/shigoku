// mal_tests.cpp — P31 §9.1 slice 3 MAL API v2 REST client tests. No sabigoku
// golden reference (MAL never existed there). Offline: status-map + body/
// response parsing are pure-function goldens; the two end-to-end cases drive
// Client::fetch at a one-shot fixture server with the exact request shape
// get_list_entry/update_list_entry build internally (mal.hpp's functions
// target the fixed MAL endpoint, so they cannot be pointed at the fixture
// directly — same constraint anilist_tests.cpp works around).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <tuple>

#include "../src/mal.hpp"

using namespace shigoku;
using namespace shigoku::mal;

// ---------------------------------------------------------------------------
// Status map, both directions (PORT_PARITY.md §9 P31)
// ---------------------------------------------------------------------------

TEST_CASE("list_status_to_mal covers every domain status") {
  CHECK(std::string(detail::list_status_to_mal(ListStatus::Planning)) == "plan_to_watch");
  CHECK(std::string(detail::list_status_to_mal(ListStatus::Watching)) == "watching");
  CHECK(std::string(detail::list_status_to_mal(ListStatus::Paused)) == "on_hold");
  CHECK(std::string(detail::list_status_to_mal(ListStatus::Completed)) == "completed");
  CHECK(std::string(detail::list_status_to_mal(ListStatus::Dropped)) == "dropped");
}

TEST_CASE("list_status_from_mal covers every wire value") {
  CHECK(detail::list_status_from_mal("plan_to_watch") == ListStatus::Planning);
  CHECK(detail::list_status_from_mal("watching") == ListStatus::Watching);
  CHECK(detail::list_status_from_mal("on_hold") == ListStatus::Paused);
  CHECK(detail::list_status_from_mal("completed") == ListStatus::Completed);
  CHECK(detail::list_status_from_mal("dropped") == ListStatus::Dropped);
}

TEST_CASE("list_status_from_mal defaults unknown/absent to Planning") {
  CHECK(detail::list_status_from_mal(std::nullopt) == ListStatus::Planning);
  CHECK(detail::list_status_from_mal("some_future_status") == ListStatus::Planning);
  CHECK(detail::list_status_from_mal("") == ListStatus::Planning);
}

TEST_CASE("status map round-trips through both directions") {
  const ListStatus all[] = {ListStatus::Planning, ListStatus::Watching, ListStatus::Paused,
                             ListStatus::Completed, ListStatus::Dropped};
  for (ListStatus s : all) {
    const char* wire = detail::list_status_to_mal(s);
    CHECK(detail::list_status_from_mal(std::string_view(wire)) == s);
  }
}

// ---------------------------------------------------------------------------
// percent_encode
// ---------------------------------------------------------------------------

TEST_CASE("percent_encode passes unreserved chars through raw") {
  CHECK(detail::percent_encode("abcXYZ019-._~") == "abcXYZ019-._~");
}

TEST_CASE("percent_encode escapes everything else") {
  CHECK(detail::percent_encode("a b") == "a%20b");
  CHECK(detail::percent_encode("&=") == "%26%3D");
}

// ---------------------------------------------------------------------------
// put_body
// ---------------------------------------------------------------------------

TEST_CASE("put_body encodes status, progress, score, and always is_rewatching=false") {
  const std::string body = detail::put_body(ListStatus::Watching, 12, 8);
  CHECK(body == "status=watching&num_watched_episodes=12&score=8&is_rewatching=false");
}

TEST_CASE("put_body binds every status") {
  CHECK(detail::put_body(ListStatus::Planning, 0, 0) ==
        "status=plan_to_watch&num_watched_episodes=0&score=0&is_rewatching=false");
  CHECK(detail::put_body(ListStatus::Completed, 25, 9) ==
        "status=completed&num_watched_episodes=25&score=9&is_rewatching=false");
}

// nullopt OMITS the field entirely — MAL reads score=0 as "remove the user's
// score", so a withheld score must never degrade to 0 on the wire (mal.hpp).
TEST_CASE("put_body omits the score field when withheld") {
  CHECK(detail::put_body(ListStatus::Watching, 12, std::nullopt) ==
        "status=watching&num_watched_episodes=12&is_rewatching=false");
}

// ---------------------------------------------------------------------------
// parse_get_response
// ---------------------------------------------------------------------------

TEST_CASE("parse_get_response with a populated my_list_status") {
  const std::string body =
      R"({"id":1,"title":"x","my_list_status":{"status":"watching","num_episodes_watched":7,)"
      R"("score":8,"is_rewatching":false}})";
  auto r = detail::parse_get_response(body);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  CHECK(std::get<0>(**r) == ListStatus::Watching);
  CHECK(std::get<1>(**r) == 7);
  CHECK(std::get<2>(**r) == 8);
}

TEST_CASE("parse_get_response absent my_list_status is not an error") {
  const std::string body = R"({"id":1,"title":"x"})";
  auto r = detail::parse_get_response(body);
  REQUIRE(r.has_value());
  CHECK_FALSE(r->has_value());
}

TEST_CASE("parse_get_response null my_list_status is not an error") {
  const std::string body = R"({"id":1,"title":"x","my_list_status":null})";
  auto r = detail::parse_get_response(body);
  REQUIRE(r.has_value());
  CHECK_FALSE(r->has_value());
}

TEST_CASE("parse_get_response tolerates a missing progress field") {
  const std::string body = R"({"my_list_status":{"status":"completed"}})";
  auto r = detail::parse_get_response(body);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  CHECK(std::get<0>(**r) == ListStatus::Completed);
  CHECK(std::get<1>(**r) == 0);
  CHECK(std::get<2>(**r) == 0);
}

TEST_CASE("parse_get_response tolerates a missing status field") {
  const std::string body = R"({"my_list_status":{"num_episodes_watched":3}})";
  auto r = detail::parse_get_response(body);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  CHECK(std::get<0>(**r) == ListStatus::Planning);
  CHECK(std::get<1>(**r) == 3);
}

TEST_CASE("parse_get_response tolerates a missing score field") {
  const std::string body = R"({"my_list_status":{"status":"watching","num_episodes_watched":3}})";
  auto r = detail::parse_get_response(body);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  CHECK(std::get<2>(**r) == 0);
}

TEST_CASE("parse_get_response malformed json is Decode") {
  auto r = detail::parse_get_response("{not json");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_get_response non-object json is Decode") {
  auto r = detail::parse_get_response("[1,2,3]");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ProviderError::Kind::Decode);
}

// ===========================================================================
// get_list_entry / update_list_entry over http::Client at a fixture server.
// ===========================================================================

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <thread>

namespace {

// Same one-shot fixture shape as anilist_tests.cpp's OneShotServer.
struct OneShotServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;
  std::string captured_request;

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
      ssize_t n = ::read(cfd, buf, sizeof(buf));
      if (n > 0) captured_request.assign(buf, static_cast<std::size_t>(n));
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

std::vector<std::uint8_t> response_with_body(const std::string& status, const std::string& body) {
  std::string head = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n" +
                     "Content-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace

TEST_CASE("get request shape: bearer header, GET, fields=my_list_status") {
  // get_list_entry() targets the fixed MAL endpoint, so the request shape it
  // builds is exercised here by driving Client::fetch with an identical
  // Request against the fixture (same workaround anilist_tests.cpp uses).
  const std::string body =
      R"({"my_list_status":{"status":"watching","num_episodes_watched":4,"score":6}})";
  OneShotServer srv(response_with_body("200 OK", body));
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  http::Request req;
  req.method = http::Method::Get;
  req.url = srv.url() + "anime/123?fields=my_list_status";
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer tok-abc"});

  auto resp = client->fetch(req);
  REQUIRE(resp.has_value());
  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  auto parsed = detail::parse_get_response(raw);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->has_value());
  CHECK(std::get<0>(**parsed) == ListStatus::Watching);
  CHECK(std::get<1>(**parsed) == 4);
  CHECK(std::get<2>(**parsed) == 6);

  CHECK(srv.captured_request.find("GET /anime/123?fields=my_list_status") != std::string::npos);
  CHECK(srv.captured_request.find("Authorization: Bearer tok-abc") != std::string::npos);
}

TEST_CASE("update_list_entry sends a PUT with the form-encoded body") {
  OneShotServer srv(response_with_body("200 OK", "{}"));
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  http::Request req;
  req.method = http::Method::Put;
  req.url = srv.url() + "anime/123/my_list_status";
  req.content_type = "application/x-www-form-urlencoded";
  const std::string put_body = detail::put_body(ListStatus::Completed, 25, 9);
  req.body.assign(put_body.begin(), put_body.end());
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer tok-abc"});

  auto resp = client->fetch(req);
  REQUIRE(resp.has_value());

  CHECK(srv.captured_request.find("PUT /anime/123/my_list_status") != std::string::npos);
  CHECK(srv.captured_request.find("Authorization: Bearer tok-abc") != std::string::npos);
  CHECK(srv.captured_request.find("Content-Type: application/x-www-form-urlencoded") !=
        std::string::npos);
  CHECK(srv.captured_request.find(
            "status=completed&num_watched_episodes=25&score=9&is_rewatching=false") !=
        std::string::npos);
}

TEST_CASE("get_list_entry http error maps to ProviderError") {
  OneShotServer srv(response_with_body("401 Unauthorized", "denied"));
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  http::Request req;
  req.method = http::Method::Get;
  req.url = srv.url() + "anime/123?fields=my_list_status";
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer tok-abc"});

  auto resp = client->fetch(req);
  REQUIRE_FALSE(resp.has_value());
  CHECK(resp.error().kind == ProviderError::Kind::Http);
  CHECK(resp.error().status == 401);
}
