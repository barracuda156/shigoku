// anilibria_tests.cpp — golden tests for the AniLibria provider: pure parsers
// + guards offline (search/release JSON, ordinal labels, quality ladder), plus
// transport cases over a loopback fixture server. No fixture files (JSON
// inline).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../src/anilibria.hpp"

using namespace shigoku;
using namespace shigoku::anilibria;
using namespace shigoku::anilibria::detail;

// ===========================================================================
// parse_search
// ===========================================================================

static const char* kSearch = R"([
  {"id":9542,"type":{"value":"TV"},"year":2023,
   "name":{"main":"Провожающая в последний путь Фрирен",
           "english":"Sousou no Frieren","alternative":null},
   "alias":"sousou-no-frieren","episodes_total":28,
   "is_blocked_by_geo":false,"is_blocked_by_copyrights":false},
  {"id":413,"name":{"main":"Наруто","english":null},
   "year":null,"episodes_total":null}
])";

TEST_CASE("parse_search_reads_id_titles_year_and_count") {
  auto hits = parse_search(kSearch);
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  CHECK((*hits)[0].id == 9542);
  CHECK((*hits)[0].title_main == "Провожающая в последний путь Фрирен");
  CHECK((*hits)[0].title_english == std::optional<std::string>("Sousou no Frieren"));
  CHECK((*hits)[0].year == std::optional<std::uint32_t>(2023));
  CHECK((*hits)[0].episodes_total == std::optional<std::uint32_t>(28));
  CHECK((*hits)[1].id == 413);
  CHECK((*hits)[1].title_english == std::nullopt);
  CHECK((*hits)[1].year == std::nullopt);
  CHECK((*hits)[1].episodes_total == std::nullopt);
}

TEST_CASE("parse_search_drops_junk_rows") {
  auto hits = parse_search(R"([
    {"name":{"main":"no id"}},
    {"id":-3,"name":{"main":"negative"}},
    {"id":0,"name":{"main":"zero"}},
    {"id":5},
    {"id":6,"name":{"main":""}},
    42,
    {"id":7,"name":{"main":"keeper"}}
  ])");
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 1);
  CHECK((*hits)[0].id == 7);
  CHECK((*hits)[0].title_main == "keeper");
}

TEST_CASE("parse_search_not_an_array_is_decode_not_empty") {
  // An API change must not read as "no results" and score a durable miss.
  CHECK(!parse_search("{}").has_value());
  CHECK(!parse_search("junk").has_value());
  CHECK(!parse_search("").has_value());
  auto empty = parse_search("[]");
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

// ===========================================================================
// parse_release
// ===========================================================================

static const char* kRelease = R"({
  "id":9542,
  "name":{"main":"Фрирен","english":"Sousou no Frieren"},
  "is_blocked_by_geo":false,"is_blocked_by_copyrights":false,
  "episodes":[
    {"ordinal":2,"name":"B",
     "hls_480":"https://cdn.test/2/480.m3u8",
     "hls_720":"https://cdn.test/2/720.m3u8",
     "hls_1080":"https://cdn.test/2/1080.m3u8"},
    {"ordinal":1,"name":"A",
     "hls_480":"https://cdn.test/1/480.m3u8",
     "hls_720":null,
     "hls_1080":"https://cdn.test/1/1080.m3u8"},
    {"ordinal":11.5,"name":"Special","hls_480":"https://cdn.test/s/480.m3u8"},
    {"ordinal":1,"name":"dup","hls_480":"https://cdn.test/dup/480.m3u8"}
  ]})";

TEST_CASE("parse_release_sorts_dedupes_and_reads_hls") {
  auto rel = parse_release(kRelease);
  REQUIRE(rel.has_value());
  REQUIRE(rel->episodes.size() == 3);
  CHECK(rel->episodes[0].ordinal == 1.0);
  CHECK(rel->episodes[0].name == "A");  // first listed wins the dedup.
  CHECK(rel->episodes[0].hls720 == std::nullopt);
  CHECK(rel->episodes[0].hls1080 == std::optional<std::string>("https://cdn.test/1/1080.m3u8"));
  CHECK(rel->episodes[1].ordinal == 2.0);
  CHECK(rel->episodes[2].ordinal == 11.5);
  CHECK(!rel->blocked_geo);
  CHECK(!rel->blocked_copyright);
}

TEST_CASE("parse_release_drops_junk_ordinals") {
  auto rel = parse_release(R"({"episodes":[
    {"ordinal":-1,"hls_480":"https://cdn.test/x.m3u8"},
    {"ordinal":"seven","hls_480":"https://cdn.test/x.m3u8"},
    {"ordinal":1e18,"hls_480":"https://cdn.test/x.m3u8"},
    {"hls_480":"https://cdn.test/x.m3u8"},
    "row",
    {"ordinal":3,"hls_480":"https://cdn.test/3.m3u8","hls_720":""}
  ]})");
  REQUIRE(rel.has_value());
  REQUIRE(rel->episodes.size() == 1);
  CHECK(rel->episodes[0].ordinal == 3.0);
  CHECK(rel->episodes[0].hls720 == std::nullopt);  // empty string is absent.
}

TEST_CASE("parse_release_reads_block_flags") {
  auto rel = parse_release(R"({"is_blocked_by_geo":true,"episodes":[]})");
  REQUIRE(rel.has_value());
  CHECK(rel->blocked_geo);
  auto rel2 = parse_release(R"({"is_blocked_by_copyrights":true})");
  REQUIRE(rel2.has_value());
  CHECK(rel2->blocked_copyright);
  CHECK(rel2->episodes.empty());
}

TEST_CASE("parse_release_malformed") {
  CHECK(!parse_release("[]").has_value());
  CHECK(!parse_release("junk").has_value());
  auto bare = parse_release("{}");
  REQUIRE(bare.has_value());
  CHECK(bare->episodes.empty());
}

// ===========================================================================
// ordinal labels + quality ladder
// ===========================================================================

TEST_CASE("format_ordinal_integral_and_fractional") {
  CHECK(format_ordinal(1.0) == "1");
  CHECK(format_ordinal(28.0) == "28");
  CHECK(format_ordinal(0.0) == "0");
  CHECK(format_ordinal(11.5) == "11.5");
  CHECK(format_ordinal(7.25) == "7.25");
}

TEST_CASE("label_matches_numeric_and_fractional") {
  CHECK(label_matches(1.0, "1"));
  CHECK(label_matches(1.0, "01"));  // zero-padded labels still name it.
  CHECK(label_matches(11.5, "11.5"));
  CHECK(!label_matches(11.5, "11"));
  CHECK(!label_matches(2.0, "1"));
  CHECK(!label_matches(1.0, ""));
  CHECK(!label_matches(1.0, "x1"));
}

TEST_CASE("pick_hls_walks_the_ladder") {
  Episode full;
  full.hls480 = "u480";
  full.hls720 = "u720";
  full.hls1080 = "u1080";
  CHECK(pick_hls(full, Quality::Best) == std::optional<std::string>("u1080"));
  CHECK(pick_hls(full, Quality::P1080) == std::optional<std::string>("u1080"));
  CHECK(pick_hls(full, Quality::P720) == std::optional<std::string>("u720"));
  CHECK(pick_hls(full, Quality::P480) == std::optional<std::string>("u480"));
  CHECK(pick_hls(full, Quality::Worst) == std::optional<std::string>("u480"));

  Episode low;  // only 480 offered: everything lands there.
  low.hls480 = "u480";
  CHECK(pick_hls(low, Quality::Best) == std::optional<std::string>("u480"));
  CHECK(pick_hls(low, Quality::P1080) == std::optional<std::string>("u480"));

  Episode mid;  // 720 missing: P720 steps DOWN before it steps up.
  mid.hls480 = "u480";
  mid.hls1080 = "u1080";
  CHECK(pick_hls(mid, Quality::P720) == std::optional<std::string>("u480"));

  CHECK(pick_hls(Episode{}, Quality::Best) == std::nullopt);
}

TEST_CASE("form_urlencode_bytes") {
  CHECK(form_urlencode("sousou no frieren") == "sousou+no+frieren");
  CHECK(form_urlencode("a-b_c.d*") == "a-b_c.d*");
  CHECK(form_urlencode("q&r=s") == "q%26r%3Ds");
  // Cyrillic goes byte-wise ("ф" = D1 84).
  CHECK(form_urlencode("\xD1\x84") == "%D1%84");
}

// ===========================================================================
// provider surface, offline
// ===========================================================================

TEST_CASE("canonical_key_is_none_because_the_api_has_no_canonical_ids") {
  auto p = AniLibria::with_endpoint("http://127.0.0.1:1");
  REQUIRE(p.has_value());
  Enrichment e;
  e.mal_id = 52991;
  e.anilist_id = 154587;
  CHECK(p->canonical_key(e) == std::nullopt);
  CHECK(p->supports_search());
}

TEST_CASE("episodes_and_resolve_reject_a_non_numeric_show_id") {
  auto p = AniLibria::with_endpoint("http://127.0.0.1:1");
  REQUIRE(p.has_value());
  CHECK(!p->episodes("../9542", Translation::Sub, std::nullopt).has_value());
  CHECK(!p->episodes("", Translation::Sub, std::nullopt).has_value());
  CHECK(!p->resolve("12a", "1", Translation::Sub, Quality::Best).has_value());
}

TEST_CASE("search_empty_query_and_later_pages_answer_offline") {
  auto p = AniLibria::with_endpoint("http://127.0.0.1:1");
  REQUIRE(p.has_value());
  SearchOptions o;
  o.page = 2;
  auto later = p->search("frieren", o);
  REQUIRE(later.has_value());
  CHECK(later->empty());
  o.page = 1;
  auto empty = p->search("", o);
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

TEST_CASE("cover_request_joins_relative_and_passes_absolute") {
  auto p = AniLibria::with_endpoint("https://api.test");
  REQUIRE(p.has_value());
  auto rel = p->cover_request("/storage/releases/posters/9542/x.webp");
  REQUIRE(rel.has_value());
  CHECK(rel->url == "https://api.test/storage/releases/posters/9542/x.webp");
  CHECK(rel->referer == std::nullopt);
  auto abs = p->cover_request("https://cdn.test/poster.jpg");
  REQUIRE(abs.has_value());
  CHECK(abs->url == "https://cdn.test/poster.jpg");
  CHECK(!p->cover_request("").has_value());
  CHECK(!p->cover_request("storage/relative-no-slash.jpg").has_value());
  CHECK(!p->cover_request("/storage/ctrl\r\nX: y").has_value());
  CHECK(!p->cover_request(std::string(3000, 'a')).has_value());
}

// ===========================================================================
// transport, over a loopback fixture server
// ===========================================================================

namespace {

struct RouteServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;
  std::shared_ptr<std::vector<std::string>> seen =
      std::make_shared<std::vector<std::string>>();
  std::shared_ptr<std::mutex> seen_mu = std::make_shared<std::mutex>();
  std::atomic<bool> stop{false};

  explicit RouteServer(std::vector<std::pair<std::string, std::vector<std::uint8_t>>> routes) {
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
    REQUIRE(::listen(listen_fd, 16) == 0);

    thread = std::thread([this, routes = std::move(routes)]() mutable {
      while (!stop.load()) {
        const int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;
        char buf[16384];
        const ssize_t n = ::read(cfd, buf, sizeof(buf));
        std::string path;
        if (n > 0) {
          // Request line: "GET <path> HTTP/1.1". Take the second token.
          std::string head(buf, static_cast<std::size_t>(n));
          const auto s = head.find(' ');
          if (s != std::string::npos) {
            const auto e = head.find(' ', s + 1);
            if (e != std::string::npos) path = head.substr(s + 1, e - s - 1);
          }
        }
        std::vector<std::uint8_t> body;
        bool found = false;
        for (const auto& r : routes) {
          if (r.first == path) {
            body = r.second;
            found = true;
            break;
          }
        }
        if (!found) {
          const std::string b = "nope";
          std::string head = "HTTP/1.1 404 Not Found\r\nContent-Length: " +
                             std::to_string(b.size()) + "\r\nConnection: close\r\n\r\n" + b;
          body.assign(head.begin(), head.end());
        }
        {
          std::lock_guard<std::mutex> lk(*seen_mu);
          seen->push_back(path);
        }
        ::write(cfd, body.data(), body.size());
        ::close(cfd);
      }
    });
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
  std::optional<std::string> last_seen() const {
    std::lock_guard<std::mutex> lk(*seen_mu);
    if (seen->empty()) return std::nullopt;
    return seen->back();
  }

  ~RouteServer() {
    stop.store(true);
    // Unblock accept() with a throwaway connection.
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (cfd >= 0) {
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      a.sin_port = htons(port);
      ::connect(cfd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
      ::close(cfd);
    }
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) ::close(listen_fd);
  }
};

std::vector<std::uint8_t> http_ok(std::string_view body) {
  std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

using Route = std::pair<std::string, std::vector<std::uint8_t>>;

// Build a provider pointed at a fresh RouteServer; the server is kept alive
// for the process (matches the sibling provider tests' leak-into-lifetime
// pattern).
struct Bound {
  AniLibria provider;
  RouteServer* server;
};
Bound against(std::vector<Route> routes) {
  static std::vector<std::unique_ptr<RouteServer>> keepalive;
  keepalive.push_back(std::make_unique<RouteServer>(std::move(routes)));
  RouteServer* srv = keepalive.back().get();
  auto p = AniLibria::with_endpoint(srv->url());
  REQUIRE(p.has_value());
  return Bound{std::move(*p), srv};
}

SearchOptions opts() {
  SearchOptions o;
  o.translation = Translation::Sub;
  o.limit = 26;
  o.page = 1;
  return o;
}

}  // namespace

TEST_CASE("transport_search_maps_hits") {
  auto b = against({{"/api/v1/app/search/releases?query=frieren", http_ok(kSearch)}});
  auto hits = b.provider.search("frieren", opts());
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  CHECK((*hits)[0].provider_id == "9542");
  CHECK((*hits)[0].title == "Провожающая в последний путь Фрирен");
  CHECK((*hits)[0].title_english == std::optional<std::string>("Sousou no Frieren"));
  CHECK((*hits)[0].year == std::optional<std::uint32_t>(2023));
  CHECK((*hits)[0].total_episodes == std::optional<std::uint32_t>(28));
  CHECK((*hits)[0].anilist_id == std::nullopt);
  CHECK((*hits)[0].mal_id == std::nullopt);
  CHECK((*hits)[1].provider_id == "413");
}

TEST_CASE("transport_search_percent_encodes_the_query") {
  // "фрирен" goes byte-wise; the route only answers the encoded form.
  auto b = against(
      {{"/api/v1/app/search/releases?query=%D1%84%D1%80%D0%B8%D1%80%D0%B5%D0%BD",
        http_ok("[]")}});
  auto hits = b.provider.search("фрирен", opts());
  REQUIRE(hits.has_value());
  CHECK(hits->empty());
  CHECK(b.server->last_seen() ==
        std::optional<std::string>(
            "/api/v1/app/search/releases?query=%D1%84%D1%80%D0%B8%D1%80%D0%B5%D0%BD"));
}

TEST_CASE("transport_search_truncates_to_the_requested_limit") {
  auto b = against({{"/api/v1/app/search/releases?query=frieren", http_ok(kSearch)}});
  SearchOptions o = opts();
  o.limit = 1;
  auto hits = b.provider.search("frieren", o);
  REQUIRE(hits.has_value());
  CHECK(hits->size() == 1);
}

TEST_CASE("transport_search_bad_body_is_an_error_not_empty") {
  auto b = against({{"/api/v1/app/search/releases?query=frieren", http_ok("{\"oops\":1}")}});
  CHECK(!b.provider.search("frieren", opts()).has_value());
}

TEST_CASE("transport_episodes_lists_formatted_labels_on_either_track") {
  auto b = against({{"/api/v1/anime/releases/9542", http_ok(kRelease)}});
  auto sub = b.provider.episodes("9542", Translation::Sub, std::nullopt);
  REQUIRE(sub.has_value());
  CHECK(*sub == std::vector<std::string>{"1", "2", "11.5"});
  // Single-track voiceover: the dub listing is the same claim.
  auto dub = b.provider.episodes("9542", Translation::Dub, std::nullopt);
  REQUIRE(dub.has_value());
  CHECK(*dub == *sub);
}

TEST_CASE("transport_episodes_missing_show_is_an_error_not_absence") {
  auto b = against({});
  CHECK(!b.provider.episodes("404404", Translation::Sub, std::nullopt).has_value());
}

TEST_CASE("transport_blocked_release_is_forbidden") {
  const char* blocked = R"({"is_blocked_by_geo":true,"episodes":[
    {"ordinal":1,"hls_480":"https://cdn.test/1/480.m3u8"}]})";
  auto b = against({{"/api/v1/anime/releases/9542", http_ok(blocked)}});
  auto eps = b.provider.episodes("9542", Translation::Sub, std::nullopt);
  REQUIRE(!eps.has_value());
  CHECK(eps.error().kind == ProviderError::Kind::Forbidden);
  auto link = b.provider.resolve("9542", "1", Translation::Sub, Quality::Best);
  REQUIRE(!link.has_value());
  CHECK(link.error().kind == ProviderError::Kind::Forbidden);
}

TEST_CASE("transport_resolve_picks_the_requested_quality") {
  auto b = against({{"/api/v1/anime/releases/9542", http_ok(kRelease)}});
  auto best = b.provider.resolve("9542", "2", Translation::Sub, Quality::Best);
  REQUIRE(best.has_value());
  CHECK(best->url == "https://cdn.test/2/1080.m3u8");
  CHECK(best->referer == std::nullopt);
  CHECK(!best->cloaked_segments);
  CHECK(!best->decloak_segments);
  auto p720 = b.provider.resolve("9542", "2", Translation::Sub, Quality::P720);
  REQUIRE(p720.has_value());
  CHECK(p720->url == "https://cdn.test/2/720.m3u8");
  auto worst = b.provider.resolve("9542", "2", Translation::Sub, Quality::Worst);
  REQUIRE(worst.has_value());
  CHECK(worst->url == "https://cdn.test/2/480.m3u8");
  // Episode 1 offers no 720: the pick steps down the ladder.
  auto down = b.provider.resolve("9542", "1", Translation::Sub, Quality::P720);
  REQUIRE(down.has_value());
  CHECK(down->url == "https://cdn.test/1/480.m3u8");
}

TEST_CASE("transport_resolve_matches_padded_and_fractional_labels") {
  auto b = against({{"/api/v1/anime/releases/9542", http_ok(kRelease)}});
  auto padded = b.provider.resolve("9542", "02", Translation::Sub, Quality::Best);
  REQUIRE(padded.has_value());
  CHECK(padded->url == "https://cdn.test/2/1080.m3u8");
  auto special = b.provider.resolve("9542", "11.5", Translation::Sub, Quality::Best);
  REQUIRE(special.has_value());
  CHECK(special->url == "https://cdn.test/s/480.m3u8");
}

TEST_CASE("transport_resolve_unknown_episode_is_a_clean_miss") {
  auto b = against({{"/api/v1/anime/releases/9542", http_ok(kRelease)}});
  CHECK(!b.provider.resolve("9542", "99", Translation::Sub, Quality::Best).has_value());
  CHECK(!b.provider.resolve("9542", "", Translation::Sub, Quality::Best).has_value());
}

TEST_CASE("transport_resolve_refuses_what_must_never_become_a_play_url") {
  // A private/metadata address or a relative path must end the resolve, not
  // reach the player.
  const char* evil = R"({"episodes":[
    {"ordinal":1,"hls_720":"http://169.254.169.254/latest/meta.m3u8"},
    {"ordinal":2,"hls_720":"videos/relative.m3u8"}]})";
  auto b = against({{"/api/v1/anime/releases/9542", http_ok(evil)}});
  CHECK(!b.provider.resolve("9542", "1", Translation::Sub, Quality::Best).has_value());
  CHECK(!b.provider.resolve("9542", "2", Translation::Sub, Quality::Best).has_value());
}
