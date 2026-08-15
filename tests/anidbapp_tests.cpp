// anidbapp_tests.cpp — P25 golden tests, ported 1:1 from
// src/providers/anidbapp.rs's `mod tests`. Slice 2 covers the pure parsers +
// guards (run offline); the transport cases (fixture server) land in Slice 3.

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

#include "../src/anidbapp.hpp"
#include "../src/resolver.hpp"  // best_id_match (the id-scorer contract).

using namespace shigoku;
using namespace shigoku::anidbapp;
using namespace shigoku::anidbapp::detail;

// ===========================================================================
// parse_cards / parse_card
// ===========================================================================

static const char* kCards = R"(
<a href="https://anidb.app/anime/frieren-beyond-journeys-end-season-2-1665"
   data-search-item
   class="flex items-center gap-3">
    <img src="https://cdn.test/1665.jpg" alt="Frieren: Beyond Journey&amp;#039;s End Season 2" class="w-9">
    <div class="min-w-0">
        <p class="text-sm">Frieren: Beyond Journey&#039;s End Season 2</p>
        <p class="text-xs">TV &middot; 2026</p>
    </div>
</a>
<a href="https://anidb.app/anime/frieren-beyond-journeys-end-1663"
   data-search-item
   class="flex items-center gap-3">
    <img src="https://cdn.test/1663.jpg" alt="Frieren" class="w-9">
    <div class="min-w-0">
        <p class="text-sm">Frieren: Beyond Journey&#039;s End</p>
        <p class="text-xs">TV &middot; 2023</p>
    </div>
</a>
)";

TEST_CASE("parse_cards_reads_slug_id_title_and_year") {
  auto cards = parse_cards(kCards);
  REQUIRE(cards.size() == 2);
  CHECK(cards[0].site_id == "1665");
  CHECK(cards[0].slug == "frieren-beyond-journeys-end-season-2-1665");
  CHECK(cards[0].title == "Frieren: Beyond Journey's End Season 2");
  CHECK(cards[0].year == std::optional<std::uint32_t>(2026));
  CHECK(cards[1].site_id == "1663");
  CHECK(cards[1].title == "Frieren: Beyond Journey's End");
  CHECK(cards[1].year == std::optional<std::uint32_t>(2023));
}

TEST_CASE("parse_cards_ignores_anchors_without_the_marker") {
  const char* html =
      R"(<a href="https://anidb.app/anime/other-99" class="nav"><p>Other</p></a>)";
  CHECK(parse_cards(html).empty());
  CHECK(parse_cards("<html>no results</html>").empty());
}

TEST_CASE("parse_cards_drops_a_card_with_no_title") {
  const char* html =
      R"(<a href="https://anidb.app/anime/x-12" data-search-item><img src="x"></a>)";
  CHECK(parse_cards(html).empty());
}

// ===========================================================================
// split_slug
// ===========================================================================

TEST_CASE("split_slug_takes_the_trailing_id") {
  auto a = split_slug("https://anidb.app/anime/one-piece-3880");
  REQUIRE(a.has_value());
  CHECK(a->first == "one-piece-3880");
  CHECK(a->second == "3880");

  auto b = split_slug("/anime/x_y-7?ref=a");
  REQUIRE(b.has_value());
  CHECK(b->first == "x_y-7");
  CHECK(b->second == "7");
}

TEST_CASE("split_slug_refuses_a_slug_that_could_escape_the_path") {
  CHECK_FALSE(split_slug("https://anidb.app/anime/..%2f..%2fetc-1").has_value());
  CHECK_FALSE(split_slug("https://anidb.app/anime/a.b-1").has_value());
  // Only the last segment survives, so traversal never rides along.
  auto c = split_slug("https://evil.test/anime/../../x-1");
  REQUIRE(c.has_value());
  CHECK(c->first == "x-1");
  CHECK(c->second == "1");
  CHECK_FALSE(split_slug("https://anidb.app/anime/no-id-here").has_value());
  CHECK_FALSE(split_slug("https://anidb.app/anime/-12").has_value());
  CHECK_FALSE(split_slug("https://anidb.app/about-1").has_value());
}

// ===========================================================================
// decode_entities
// ===========================================================================

TEST_CASE("decode_entities_handles_named_numeric_and_unknown") {
  CHECK(decode_entities("Journey&#039;s End") == "Journey's End");
  CHECK(decode_entities("Fate&#x2f;Zero") == "Fate/Zero");
  CHECK(decode_entities("A &amp; B &lt;c&gt;") == "A & B <c>");
  CHECK(decode_entities("100&percnt; sure") == "100&percnt; sure");
  CHECK(decode_entities("bare & loose") == "bare & loose");
  CHECK(decode_entities("plain") == "plain");
}

TEST_CASE("decode_entities_is_single_pass") {
  // The alt attribute is double-escaped; one pass must not over-decode a
  // literal that only looks like an entity.
  CHECK(decode_entities("Journey&amp;#039;s") == "Journey&#039;s");
}

// ===========================================================================
// trailing_year
// ===========================================================================

TEST_CASE("trailing_year_from_the_meta_line") {
  CHECK(trailing_year("TV · 2026") == std::optional<std::uint32_t>(2026));
  CHECK(trailing_year("Movie · 1998") == std::optional<std::uint32_t>(1998));
  CHECK_FALSE(trailing_year("ONA").has_value());
  CHECK_FALSE(trailing_year("TV · 12 eps").has_value());
}

// ===========================================================================
// parse_external_ids
// ===========================================================================

TEST_CASE("parse_external_ids_reads_both_links") {
  const char* html =
      R"(<a href="https://myanimelist.net/anime/52991/Sousou">MAL</a>
         <a href="https://anilist.co/anime/154587" rel="noopener">AniList</a>)";
  auto ids = parse_external_ids(html);
  CHECK(ids.first == std::optional<std::int64_t>(154587));
  CHECK(ids.second == std::optional<std::int64_t>(52991));
}

TEST_CASE("parse_external_ids_tolerates_a_missing_side") {
  auto a = parse_external_ids(R"(<a href="https://anilist.co/anime/999">AniList</a>)");
  CHECK(a.first == std::optional<std::int64_t>(999));
  CHECK_FALSE(a.second.has_value());
  auto b = parse_external_ids("<html>nothing</html>");
  CHECK_FALSE(b.first.has_value());
  CHECK_FALSE(b.second.has_value());
}

// ===========================================================================
// is_challenge
// ===========================================================================

TEST_CASE("is_challenge_ignores_the_script_a_good_page_ships") {
  // Live embed pages carry this on a 200. Treating it as a challenge would fail
  // every playback, so it must not be a marker.
  const char* good =
      R"(<script src='/cdn-cgi/challenge-platform/scripts/jsd/main.js'></script>
         <script>var setup = { sources: [{ file: 'https://h/m.m3u8' }] };</script>)";
  CHECK_FALSE(is_challenge(good));

  CHECK(is_challenge("<title>Just a moment...</title>"));
  CHECK(is_challenge("window._cf_chl_opt={cvId:'3'}"));
  CHECK(is_challenge("/cdn-cgi/challenge-platform/h/b/jsd/__cf_chl_f_tk"));
}

TEST_CASE("is_challenge_ignores_loading_copy_outside_the_title") {
  // "Just a moment" is ordinary body copy. Matching it anywhere would let a
  // synopsis or an app shell take the whole provider offline.
  CHECK_FALSE(is_challenge(
      R"(<title>Frieren</title><div id="app">Just a moment, loading...</div>)"));
  CHECK_FALSE(is_challenge("<p>Just a moment of silence.</p>"));
}

// ===========================================================================
// stream_url_ok
// ===========================================================================

TEST_CASE("stream_url_ok_refuses_what_must_never_become_a_play_url") {
  CHECK(stream_url_ok("https://hls.test/stream/master.m3u8"));
  // Private/loopback: under a quality cap we would fetch this ourselves.
  CHECK_FALSE(stream_url_ok("http://169.254.169.254/latest/meta-data/x.m3u8"));
  CHECK_FALSE(stream_url_ok("http://127.0.0.1:8080/x.m3u8"));
  CHECK_FALSE(stream_url_ok("http://localhost/x.m3u8"));
  CHECK_FALSE(stream_url_ok("http://10.0.0.5/x.m3u8"));
  CHECK_FALSE(stream_url_ok("file:///etc/passwd"));
  CHECK_FALSE(stream_url_ok("/relative/master.m3u8"));
  CHECK_FALSE(stream_url_ok("https://h.test/a b.m3u8"));
  CHECK_FALSE(stream_url_ok("https://h.test/x.m3u8\r\nX-Evil: 1"));
  CHECK_FALSE(stream_url_ok(""));
}

// ===========================================================================
// extract_hls
// ===========================================================================

TEST_CASE("extract_hls_prefers_the_player_source_over_an_earlier_m3u8") {
  const char* html =
      R"(<img data-preview="https://ads.test/decoy.m3u8">
         <script>var setup = { sources: [{ file: 'https://hls.test/real/master.m3u8' }] };</script>)";
  auto got = extract_hls(html);
  REQUIRE(got.has_value());
  CHECK(*got == "https://hls.test/real/master.m3u8");
}

TEST_CASE("extract_hls_falls_back_when_the_config_key_moves") {
  auto a = extract_hls(R"(var s = { src: "https://hls.test/only/master.m3u8" };)");
  REQUIRE(a.has_value());
  CHECK(*a == "https://hls.test/only/master.m3u8");
  // A `file` that is not a stream must not shadow a real one.
  auto b = extract_hls(R"({"file": "poster.jpg", "src": "https://hls.test/x.m3u8"})");
  REQUIRE(b.has_value());
  CHECK(*b == "https://hls.test/x.m3u8");
}

TEST_CASE("extract_hls_from_the_jwplayer_setup") {
  const char* html =
      R"(var setup = {
            sources: [{ file: 'https://hls.test/stream/abc/master.m3u8', type: 'hls' }],
            width: '100%',
        };)";
  auto got = extract_hls(html);
  REQUIRE(got.has_value());
  CHECK(*got == "https://hls.test/stream/abc/master.m3u8");
}

TEST_CASE("extract_hls_handles_double_quotes_and_refuses_junk") {
  auto a = extract_hls(R"({"file":"https://h/x.m3u8"})");
  REQUIRE(a.has_value());
  CHECK(*a == "https://h/x.m3u8");
  CHECK_FALSE(extract_hls("<html>no player</html>").has_value());
  // Relative: this url reaches mpv, so a non-absolute one is not usable.
  CHECK_FALSE(extract_hls("file: '/rel/master.m3u8'").has_value());
  // Unquoted: nothing to bound the url with.
  CHECK_FALSE(extract_hls("see master.m3u8 somewhere").has_value());
}

// ===========================================================================
// parse_episodes / base_offset / label
// ===========================================================================

static const char* kEpisodes = R"({"episodes":[
    {"id":3064,"number":3,"filler":false},
    {"id":3062,"number":1,"filler":false},
    {"id":3063,"number":2,"filler":true}
]})";

TEST_CASE("parse_episodes_sorts_and_filters") {
  auto eps = parse_episodes(kEpisodes);
  REQUIRE(eps.has_value());
  std::vector<std::uint32_t> nums;
  for (const auto& e : *eps) nums.push_back(e.number);
  CHECK(nums == std::vector<std::uint32_t>{1, 2, 3});
  CHECK((*eps)[0].id == 3062);

  const char* junk = R"({"episodes":[
      {"id":0,"number":1},{"id":5,"number":0},{"id":6,"number":null},
      {"number":9},{"id":7,"number":-2},{"id":8,"number":4}
  ]})";
  auto e2 = parse_episodes(junk);
  REQUIRE(e2.has_value());
  REQUIRE(e2->size() == 1);
  CHECK((*e2)[0] == Episode{8, 4});
}

TEST_CASE("parse_episodes_dedupes_repeated_numbers_keeping_the_first_listed") {
  // The surviving id must be deterministic, not sort-order roulette.
  const char* dupes = R"({"episodes":[
      {"id":11,"number":2},{"id":12,"number":2},{"id":13,"number":2},
      {"id":21,"number":1},{"id":22,"number":1}
  ]})";
  auto eps = parse_episodes(dupes);
  REQUIRE(eps.has_value());
  REQUIRE(eps->size() == 2);
  CHECK((*eps)[0] == Episode{21, 1});
  CHECK((*eps)[1] == Episode{11, 2});
}

TEST_CASE("parse_episodes_empty_and_malformed") {
  auto a = parse_episodes(R"({"episodes":[]})");
  REQUIRE(a.has_value());
  CHECK(a->empty());
  auto b = parse_episodes("{}");
  REQUIRE(b.has_value());
  CHECK(b->empty());
  auto c = parse_episodes("<html>404</html>");
  REQUIRE_FALSE(c.has_value());
  CHECK(c.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("base_offset_normalizes_absolute_season_numbering") {
  std::vector<Episode> one_based{{1, 1}, {2, 2}};
  CHECK(base_offset(one_based) == 0);
  CHECK(label(one_based[1], 0) == "2");

  // Frieren S2: the site numbers 29..38, its AniList entry numbers 1..10.
  std::vector<Episode> absolute{{26020, 29}, {26021, 30}};
  const std::uint32_t offset = base_offset(absolute);
  CHECK(offset == 28);
  CHECK(label(absolute[0], offset) == "1");
  CHECK(label(absolute[1], offset) == "2");

  CHECK(base_offset({}) == 0);
}

// ===========================================================================
// Seam (no network): canonical_key, supports_search, guard rejections.
// ===========================================================================

TEST_CASE("canonical_key_is_none_because_the_site_has_no_canonical_endpoint") {
  auto p = AniDbApp::create();
  REQUIRE(p.has_value());
  Enrichment show;
  show.anilist_id = 154587;
  show.mal_id = 52991;
  CHECK_FALSE(p->canonical_key(show).has_value());
}

TEST_CASE("supports_search_because_nothing_else_can_bind_it") {
  auto p = AniDbApp::create();
  REQUIRE(p.has_value());
  CHECK(p->supports_search());
  CHECK_FALSE(p->canonical_key(Enrichment{}).has_value());
}

TEST_CASE("episodes_rejects_a_non_numeric_show_id") {
  auto p = AniDbApp::create();
  REQUIRE(p.has_value());
  auto r = p->episodes("../7", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("resolve_rejects_bad_episode_labels") {
  auto p = AniDbApp::create();
  REQUIRE(p.has_value());
  for (const char* bad : {"0", "abc", "", "-1", "1.5"}) {
    auto r = p->resolve("1663", bad, Translation::Sub, Quality::Best);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == ProviderError::Kind::Decode);
  }
}

TEST_CASE("cover_request_takes_absolute_refs_only") {
  auto p = AniDbApp::create();
  REQUIRE(p.has_value());
  auto got = p->cover_request("https://cdn.test/poster.jpg");
  REQUIRE(got.has_value());
  CHECK(got->url == "https://cdn.test/poster.jpg");
  CHECK(got->referer == std::optional<std::string>("https://anidb.app/"));

  for (const char* bad : {"", "/poster.jpg", "https://cdn.test/a b.jpg"}) {
    CHECK_FALSE(p->cover_request(bad).has_value());
  }
  CHECK_FALSE(p->cover_request(std::string(kMaxCoverRefLen + 1, 'x')).has_value());
}

// ===========================================================================
// Transport: path-routed loopback fixture server (port of serve_routes).
// ===========================================================================
namespace {

// Answers each request by URL path from `routes` (404 otherwise) and records
// the paths it was asked for. Loops accepting connections until destroyed — the
// dub bisection makes the request count variable, so this is not a fixed
// sequence. Port of anidbapp.rs serve_routes.
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
  std::size_t seen_count() const {
    std::lock_guard<std::mutex> lk(*seen_mu);
    return seen->size();
  }
  std::optional<std::string> last_seen() const {
    std::lock_guard<std::mutex> lk(*seen_mu);
    if (seen->empty()) return std::nullopt;
    return seen->back();
  }
  std::optional<std::string> first_seen() const {
    std::lock_guard<std::mutex> lk(*seen_mu);
    if (seen->empty()) return std::nullopt;
    return seen->front();
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

// Embeds point at a private address on purpose: the SSRF guard then ends every
// resolve chain deterministically instead of a test reaching the live site.
std::vector<std::uint8_t> langs(const std::vector<std::string>& codes) {
  std::string rows;
  for (std::size_t i = 0; i < codes.size(); ++i) {
    if (i) rows += ",";
    rows += "{\"code\":\"" + codes[i] + "\",\"embed_url\":\"http://169.254.169.254/embed/" +
            codes[i] + "\"}";
  }
  return http_ok("{\"languages\":[" + rows + "]}");
}

// A show of `n` episodes numbered from `first`, dubbed through the first
// `dubbed` of them. Episode ids are 1000 + index.
std::vector<Route> show_routes(const std::string& site, std::uint32_t first,
                               std::uint32_t n, std::uint32_t dubbed) {
  std::string rows;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (i) rows += ",";
    rows += "{\"id\":" + std::to_string(1000 + i) + ",\"number\":" +
            std::to_string(first + i) + "}";
  }
  std::vector<Route> routes;
  routes.push_back({"/api/frontend/anime/" + site + "/episodes",
                    http_ok("{\"episodes\":[" + rows + "]}")});
  for (std::uint32_t i = 0; i < n; ++i) {
    std::vector<std::string> codes =
        (i < dubbed) ? std::vector<std::string>{"eng", "jpn"}
                     : std::vector<std::string>{"jpn"};
    routes.push_back({"/api/frontend/episode/" + std::to_string(1000 + i) + "/languages",
                      langs(codes)});
  }
  return routes;
}

// Build a provider pointed at a fresh RouteServer; the server is kept alive for
// the process (matches anibd_tests' leak-into-lifetime pattern).
struct Bound {
  AniDbApp provider;
  RouteServer* server;
};
Bound against(std::vector<Route> routes) {
  static std::vector<std::unique_ptr<RouteServer>> keepalive;
  keepalive.push_back(std::make_unique<RouteServer>(std::move(routes)));
  RouteServer* srv = keepalive.back().get();
  auto p = AniDbApp::with_endpoint(srv->url());
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

TEST_CASE("transport_sub_lists_every_episode_without_probing_languages") {
  auto b = against(show_routes("1663", 1, 5, 0));
  auto eps = b.provider.episodes("1663", Translation::Sub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(*eps == std::vector<std::string>{"1", "2", "3", "4", "5"});
  CHECK(b.server->seen_count() == 1);  // sub must not spend a language call.
}

TEST_CASE("transport_sub_labels_are_offset_to_the_canonical_numbering") {
  auto b = against(show_routes("1665", 29, 10, 0));
  auto eps = b.provider.episodes("1665", Translation::Sub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(eps->front() == "1");
  CHECK(eps->back() == "10");
}

TEST_CASE("transport_dub_absent_is_not_stocked_for_the_track") {
  auto b = against(show_routes("1663", 1, 12, 0));
  auto eps = b.provider.episodes("1663", Translation::Dub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(eps->empty());
  CHECK(b.server->seen_count() == 2);  // episodes call, then ep 1 settles it.
}

TEST_CASE("transport_dub_fully_dubbed_takes_two_probes") {
  auto b = against(show_routes("1663", 1, 28, 28));
  auto eps = b.provider.episodes("1663", Translation::Dub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(eps->size() == 28);
  CHECK(eps->back() == "28");
  CHECK(b.server->seen_count() == 3);  // first and last dubbed ends the search.
}

TEST_CASE("transport_dub_bisects_the_boundary") {
  // 64 episodes, dubbed through 40. Linear probing would cost 40+ calls.
  auto b = against(show_routes("3880", 1, 64, 40));
  auto eps = b.provider.episodes("3880", Translation::Dub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(eps->size() == 40);
  CHECK(eps->back() == "40");
  CHECK(b.server->seen_count() <= 10);  // bisect stays logarithmic.
}

TEST_CASE("transport_dub_boundary_holds_under_the_offset") {
  auto b = against(show_routes("1665", 29, 10, 4));
  auto eps = b.provider.episodes("1665", Translation::Dub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(*eps == std::vector<std::string>{"1", "2", "3", "4"});
}

TEST_CASE("transport_dub_single_episode_show") {
  auto b1 = against(show_routes("77", 1, 1, 1));
  auto e1 = b1.provider.episodes("77", Translation::Dub, std::nullopt);
  REQUIRE(e1.has_value());
  CHECK(*e1 == std::vector<std::string>{"1"});
  auto b2 = against(show_routes("78", 1, 1, 0));
  auto e2 = b2.provider.episodes("78", Translation::Dub, std::nullopt);
  REQUIRE(e2.has_value());
  CHECK(e2->empty());
}

TEST_CASE("transport_episodes_missing_show_is_an_error_not_absence") {
  // A 404 must not read as "not stocked", or it stamps a 7-day absence.
  auto b = against({});
  auto r = b.provider.episodes("1663", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ProviderError::Kind::Http);
  CHECK(r.error().status == 404);
}

TEST_CASE("transport_resolve_maps_the_label_through_the_offset") {
  // Label 1 on a 29-based show must ask for episode 29's languages. The embed is
  // loopback here, so the SSRF guard stops the chain right after; the requested
  // path is the proof.
  auto b = against(show_routes("1665", 29, 10, 0));
  auto got = b.provider.resolve("1665", "3", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
  // Third episode of the show: id 1002, site number 31.
  CHECK(b.server->last_seen() ==
        std::optional<std::string>("/api/frontend/episode/1002/languages"));
}

TEST_CASE("transport_resolve_refuses_a_private_embed_url") {
  // The embed url is provider-supplied and gets fetched, so it must clear the
  // SSRF guard before anything dials it.
  auto b = against(show_routes("1663", 1, 1, 0));
  auto got = b.provider.resolve("1663", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
  CHECK(got.error().detail == "blocked embed url");
}

TEST_CASE("transport_resolve_wrong_track_is_a_clean_miss") {
  auto b = against(show_routes("1663", 1, 3, 0));
  auto got = b.provider.resolve("1663", "1", Translation::Dub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("transport_resolve_unknown_episode_is_a_clean_miss") {
  auto b = against(show_routes("1663", 1, 3, 0));
  auto got = b.provider.resolve("1663", "99", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("transport_search_probes_each_card_for_its_ids") {
  auto detail_page = [](std::int64_t anilist, std::int64_t mal) {
    return http_ok(
        "<a href=\"https://myanimelist.net/anime/" + std::to_string(mal) +
        "/x\">MAL</a>\n<a href=\"https://anilist.co/anime/" + std::to_string(anilist) +
        "\">AniList</a>");
  };
  auto b = against({
      {"/search/suggestions?q=frieren", http_ok(kCards)},
      {"/anime/frieren-beyond-journeys-end-season-2-1665", detail_page(182255, 59978)},
      {"/anime/frieren-beyond-journeys-end-1663", detail_page(154587, 52991)},
  });

  auto hits = b.provider.search("frieren", opts());
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  CHECK((*hits)[0].provider_id == "1665");
  CHECK((*hits)[0].anilist_id == std::optional<std::int64_t>(182255));
  CHECK((*hits)[0].mal_id == std::optional<std::int64_t>(59978));
  CHECK((*hits)[0].year == std::optional<std::uint32_t>(2026));
  CHECK((*hits)[1].provider_id == "1663");
  CHECK((*hits)[1].anilist_id == std::optional<std::int64_t>(154587));
  CHECK(b.server->seen_count() == 3);

  // The ids let the scorer separate the two seasons a title match cannot.
  Enrichment s1;
  s1.anilist_id = 154587;
  s1.mal_id = 52991;
  CHECK(resolver::best_id_match(s1, *hits) == std::optional<std::size_t>(1));
}

TEST_CASE("transport_search_survives_a_detail_page_that_fails") {
  auto b = against({{"/search/suggestions?q=frieren", http_ok(kCards)}});
  // Both detail probes 404; the cards still ship for title matching.
  auto hits = b.provider.search("frieren", opts());
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  for (const auto& h : *hits) CHECK_FALSE(h.anilist_id.has_value());
  CHECK((*hits)[0].title == "Frieren: Beyond Journey's End Season 2");
}

TEST_CASE("transport_search_percent_encodes_the_query") {
  auto b = against({{"/search/suggestions?q=fate%2Fzero+%26+co", http_ok("")}});
  auto hits = b.provider.search("fate/zero & co", opts());
  REQUIRE(hits.has_value());
  CHECK(hits->empty());
  CHECK(b.server->first_seen() ==
        std::optional<std::string>("/search/suggestions?q=fate%2Fzero+%26+co"));
}

TEST_CASE("transport_search_challenge_page_is_a_block_not_an_empty_result") {
  auto b = against(
      {{"/search/suggestions?q=frieren", http_ok("<title>Just a moment...</title>")}});
  auto r = b.provider.search("frieren", opts());
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ProviderError::Kind::Forbidden);
  CHECK(r.error().status == 403);
}

TEST_CASE("transport_search_challenge_on_a_detail_page_stops_the_walk") {
  auto b = against({
      {"/search/suggestions?q=frieren", http_ok(kCards)},
      {"/anime/frieren-beyond-journeys-end-season-2-1665", http_ok("window._cf_chl_opt={}")},
  });
  auto r = b.provider.search("frieren", opts());
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ProviderError::Kind::Forbidden);
  CHECK(r.error().status == 403);
  CHECK(b.server->seen_count() == 2);  // a block stops the loop, not all 8.
}

TEST_CASE("search_short_query_and_later_pages_answer_offline") {
  auto p = AniDbApp::with_endpoint("http://127.0.0.1:1");
  REQUIRE(p.has_value());
  CHECK(p->search("a", opts())->empty());
  CHECK(p->search("", opts())->empty());
  SearchOptions page2 = opts();
  page2.page = 2;
  CHECK(p->search("frieren", page2)->empty());
}

TEST_CASE("transport_search_truncates_to_the_requested_limit") {
  auto b = against({{"/search/suggestions?q=frieren", http_ok(kCards)}});
  SearchOptions one = opts();
  one.limit = 1;
  auto hits = b.provider.search("frieren", one);
  REQUIRE(hits.has_value());
  CHECK(hits->size() == 1);
}
