// anibd_tests.cpp — P12a golden tests, ported 1:1 from
// src/providers/anibd.rs's `mod tests`. Pure parsers + guards run offline; the
// transport cases (SSRF skip of a private player URL, percent-encoded `data`
// param on the wire, forbidden -> taxonomy) run over a loopback fixture server,
// same shape as senshi_tests.cpp's OneShotServer.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

#include "../src/anibd.hpp"

using namespace shigoku;
using namespace shigoku::anibd;
using namespace shigoku::anibd::detail;

// ===========================================================================
// audio_kind / parse_ep_number (via parse_catalog) / find_link
// ===========================================================================

TEST_CASE("audio_kind_classifies_by_name") {
  CHECK(audio_kind(std::string_view("Sub Server")) == "sub");
  CHECK(audio_kind(std::string_view("English Dub")) == "dub");
  CHECK(audio_kind(std::string_view("DUB")) == "dub");
  CHECK(audio_kind(std::string_view("dubbed")) == "dub");
  CHECK(audio_kind(std::nullopt) == "sub");
  CHECK(audio_kind(std::string_view("")) == "sub");
}

static const char* kCatalogFixture = R"([
    {
        "server_name": "Sub Server",
        "server_data": [
            {"name": "01", "slug": "ep-1", "link": "link-sub-1"},
            {"name": "02", "slug": "ep-2", "link": "link-sub-2"},
            {"name": "03", "slug": "ep-3", "link": "link-sub-3"}
        ]
    },
    {
        "server_name": "English Dub",
        "server_data": [
            {"name": "1", "slug": "ep-1", "link": "link-dub-1"},
            {"name": "2", "slug": "ep-2", "link": "link-dub-2"}
        ]
    },
    {
        "server_name": "Empty Group",
        "server_data": []
    }
])";

TEST_CASE("parse_ep_number_from_name_and_slug") {
  // parse_ep_number is file-private in the Rust; exercise it via parse_catalog,
  // which is where it lives. name preferred over slug; "0" and absent rejected.
  const char* raw = R"([
    {"server_name":"Sub","server_data":[
      {"name":"01","slug":"ep-1","link":"a"},
      {"name":null,"slug":"3","link":"b"},
      {"name":"0","slug":null,"link":"c"},
      {"name":null,"slug":null,"link":"d"}
    ]}
  ])";
  auto groups = parse_catalog(raw);
  REQUIRE(groups.has_value());
  REQUIRE(groups->size() == 1);
  // Only "01" -> 1 and slug "3" -> 3 survive; "0" and the all-null row drop.
  REQUIRE((*groups)[0].episodes.size() == 2);
  CHECK((*groups)[0].episodes[0] == EpisodeLink{1, "a"});
  CHECK((*groups)[0].episodes[1] == EpisodeLink{3, "b"});
}

TEST_CASE("parse_catalog_maps_groups_and_drops_empty") {
  auto groups = parse_catalog(kCatalogFixture);
  REQUIRE(groups.has_value());
  REQUIRE(groups->size() == 2);
  CHECK((*groups)[0].audio == "sub");
  CHECK((*groups)[0].episodes.size() == 3);
  CHECK((*groups)[0].episodes[0] == EpisodeLink{1, "link-sub-1"});
  CHECK((*groups)[1].audio == "dub");
  CHECK((*groups)[1].episodes.size() == 2);
}

TEST_CASE("parse_catalog_empty_array_is_ok") {
  auto groups = parse_catalog("[]");
  REQUIRE(groups.has_value());
  CHECK(groups->empty());
}

TEST_CASE("episode_labels_union_sorted_deduped") {
  auto groups = parse_catalog(kCatalogFixture);
  REQUIRE(groups.has_value());
  const auto labels = episode_labels(*groups);
  CHECK(labels == std::vector<std::string>{"1", "2", "3"});
}

TEST_CASE("find_link_by_track_and_episode") {
  auto groups = parse_catalog(kCatalogFixture);
  REQUIRE(groups.has_value());
  CHECK(find_link(*groups, 2, Translation::Sub) == std::optional<std::string>("link-sub-2"));
  CHECK(find_link(*groups, 1, Translation::Dub) == std::optional<std::string>("link-dub-1"));
  CHECK_FALSE(find_link(*groups, 3, Translation::Dub).has_value());
  CHECK_FALSE(find_link(*groups, 99, Translation::Sub).has_value());
}

// ===========================================================================
// extract_video_url / extract_config_value / url_origin
// ===========================================================================

TEST_CASE("extract_video_url_absolute") {
  const char* html = R"(var player = { videoUrl: "https://cdn.example/master.m3u8", other: 1 })";
  CHECK(extract_video_url(html, "https://player.example/play.php") ==
        std::optional<std::string>("https://cdn.example/master.m3u8"));
}

TEST_CASE("extract_video_url_relative_slash") {
  const char* html = R"(videoUrl: "/r2/cache/abc/index.m3u8")";
  CHECK(extract_video_url(html, "https://player.example/play.php") ==
        std::optional<std::string>("https://player.example/r2/cache/abc/index.m3u8"));
}

TEST_CASE("extract_video_url_relative_bare_resolves_against_page_dir") {
  const char* html = R"(url: 'cache/ani2-154587ebd1.m3u8')";
  CHECK(extract_video_url(html, "https://playeng.example/r2/play.php?x=1") ==
        std::optional<std::string>("https://playeng.example/r2/cache/ani2-154587ebd1.m3u8"));
}

TEST_CASE("extract_video_url_falls_back_to_bare_url_key") {
  const char* html = "url: 'https://cdn.test/v.m3u8'";
  CHECK(extract_video_url(html, "https://x/play.php") ==
        std::optional<std::string>("https://cdn.test/v.m3u8"));
}

TEST_CASE("extract_video_url_prefers_video_url_over_bare") {
  const char* html = R"(url: 'https://wrong.test/x.m3u8', videoUrl: "https://right.test/y.m3u8")";
  CHECK(extract_video_url(html, "https://x/play.php") ==
        std::optional<std::string>("https://right.test/y.m3u8"));
}

TEST_CASE("extract_video_url_single_quotes") {
  const char* html = "videoUrl : 'https://cdn.test/v.m3u8'";
  CHECK(extract_video_url(html, "https://x/play.php") ==
        std::optional<std::string>("https://cdn.test/v.m3u8"));
}

TEST_CASE("extract_video_url_none_when_absent") {
  CHECK_FALSE(extract_video_url("<html>no player</html>", "https://x/play.php").has_value());
  CHECK_FALSE(extract_video_url(R"(videoUrl: "")", "https://x/play.php").has_value());
}

TEST_CASE("url_origin_extracts_scheme_and_host") {
  CHECK(url_origin("https://player.example/play2.php?x=1") == "https://player.example");
  CHECK(url_origin("http://cdn.test:8080/path") == "http://cdn.test:8080");
  CHECK(url_origin("https://bare.host") == "https://bare.host");
}

// ===========================================================================
// parse_subtitles / pick_subtitle / json_string_field
// ===========================================================================

TEST_CASE("parse_subtitles_from_playsub_page") {
  const char* html = R"(
      var player = new ArtPlayer({
          tracks: [
              {"label": "English", "file": "https://cdn.test/eng.vtt", "kind": "captions"},
              {"label": "Spanish", "file": "https://cdn.test/spa.vtt", "kind": "captions"},
              {"file": "https://cdn.test/thumbs.vtt", "kind": "thumbnails"}
          ],
          videoUrl: "/stream.m3u8"
      });
  )";
  const auto tracks = parse_subtitles(html);
  REQUIRE(tracks.size() == 2);
  CHECK(tracks[0].label == "English");
  CHECK(tracks[0].file == "https://cdn.test/eng.vtt");
  CHECK(tracks[1].label == "Spanish");
}

TEST_CASE("parse_subtitles_empty_when_no_tracks") {
  CHECK(parse_subtitles("<html>no tracks</html>").empty());
  CHECK(parse_subtitles("tracks: []").empty());
}

TEST_CASE("parse_subtitles_skips_non_http_files") {
  const char* html = R"(tracks: [{"label": "X", "file": "/local.vtt", "kind": "captions"}])";
  CHECK(parse_subtitles(html).empty());
}

TEST_CASE("pick_subtitle_prefers_english") {
  std::vector<SubTrack> tracks = {
      {"https://cdn/jp.vtt", "Japanese"},
      {"https://cdn/en.vtt", "English"},
  };
  const auto pick = pick_subtitle(tracks);
  REQUIRE(pick.has_value());
  CHECK(tracks[*pick].file == "https://cdn/en.vtt");
}

TEST_CASE("pick_subtitle_falls_back_to_first") {
  std::vector<SubTrack> tracks = {{"https://cdn/fr.vtt", "French"}};
  const auto pick = pick_subtitle(tracks);
  REQUIRE(pick.has_value());
  CHECK(tracks[*pick].file == "https://cdn/fr.vtt");
}

TEST_CASE("pick_subtitle_none_when_empty") {
  CHECK_FALSE(pick_subtitle({}).has_value());
}

TEST_CASE("json_string_field_extracts_value") {
  const char* obj = R"({"label": "English", "file": "https://cdn/x.vtt"})";
  CHECK(json_string_field(obj, "label") == std::optional<std::string>("English"));
  CHECK(json_string_field(obj, "file") == std::optional<std::string>("https://cdn/x.vtt"));
  CHECK_FALSE(json_string_field(obj, "missing").has_value());
}

// ===========================================================================
// canonical_key / episode + resolve guards
// ===========================================================================

TEST_CASE("canonical_key_is_the_anilist_id") {
  auto p = AniBd::create();
  REQUIRE(p.has_value());
  Enrichment show;
  show.anilist_id = 154587;
  show.mal_id = 52991;
  CHECK(p->canonical_key(show) == std::optional<std::string>("154587"));
}

TEST_CASE("canonical_key_works_without_mal") {
  auto p = AniBd::create();
  REQUIRE(p.has_value());
  Enrichment show;
  show.anilist_id = 100;
  CHECK(p->canonical_key(show) == std::optional<std::string>("100"));
}

TEST_CASE("search_is_unsupported") {
  auto p = AniBd::create();
  REQUIRE(p.has_value());
  CHECK_FALSE(p->supports_search());
  SearchOptions opts;
  auto got = p->search("frieren", opts);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Unsupported);
}

TEST_CASE("episodes_rejects_non_numeric_id") {
  auto p = AniBd::create();
  REQUIRE(p.has_value());
  auto got = p->episodes("../7", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("resolve_rejects_zero_episode") {
  auto p = AniBd::create();
  REQUIRE(p.has_value());
  auto got = p->resolve("154587", "0", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("resolve_rejects_non_numeric_episode") {
  auto p = AniBd::create();
  REQUIRE(p.has_value());
  auto got = p->resolve("154587", "abc", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("guarded_subtitle_drops_private_url") {
  // guarded_subtitle is private; exercise through a page that yields only a
  // private-IP track. Reuse resolve()'s subtitle path is heavier, so we assert
  // via parse+pick+guard directly on the parser output (the guard is what the
  // Rust test targets).
  const char* html =
      R"(tracks: [{"label": "English", "file": "http://169.254.169.254/meta", "kind": "captions"}])";
  const auto tracks = parse_subtitles(html);
  REQUIRE(tracks.size() == 1);
  const auto pick = pick_subtitle(tracks);
  REQUIRE(pick.has_value());
  const std::string& url = tracks[*pick].file;
  CHECK_FALSE(http::guard_fetch_url(url).has_value());
}

TEST_CASE("guarded_subtitle_accepts_public_url") {
  const char* html =
      R"(tracks: [{"label": "English", "file": "https://cdn.example/eng.vtt", "kind": "captions"}])";
  const auto tracks = parse_subtitles(html);
  REQUIRE(tracks.size() == 1);
  const auto pick = pick_subtitle(tracks);
  REQUIRE(pick.has_value());
  const std::string& url = tracks[*pick].file;
  CHECK(is_absolute_url(url));
  CHECK(clean_arg(url));
  CHECK(http::guard_fetch_url(url).has_value());
}

// ===========================================================================
// Transport — against a loopback fixture server that answers a fixed
// sequence of requests (catalog, apilink, ...), same shape as senshi_tests'.
// ===========================================================================

namespace {

// Serves a fixed list of raw HTTP responses, one per accepted connection, on a
// loopback ephemeral port. Optionally captures each request into `captured`.
struct SequenceServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;
  std::vector<std::string>* captured = nullptr;

  SequenceServer(std::vector<std::vector<std::uint8_t>> responses,
                 std::vector<std::string>* cap)
      : captured(cap) {
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
    REQUIRE(::listen(listen_fd, static_cast<int>(responses.size()) + 1) == 0);

    thread = std::thread([this, responses = std::move(responses)]() mutable {
      for (auto& resp : responses) {
        const int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;
        char buf[16384];
        const ssize_t n = ::read(cfd, buf, sizeof(buf));
        if (captured != nullptr && n > 0) {
          captured->emplace_back(buf, static_cast<std::size_t>(n));
        }
        ::write(cfd, resp.data(), resp.size());
        ::close(cfd);
      }
    });
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

  ~SequenceServer() {
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) ::close(listen_fd);
  }
};

std::vector<std::uint8_t> response_with_body(const std::string& status, std::string_view body) {
  std::string head = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n" +
                     "Content-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// A single-response provider (matches senshi_tests' against()), leaked into the
// returned provider's lifetime.
AniBd against(std::vector<std::uint8_t> response) {
  static std::vector<std::unique_ptr<SequenceServer>> keepalive;
  std::vector<std::vector<std::uint8_t>> one;
  one.push_back(std::move(response));
  keepalive.push_back(std::make_unique<SequenceServer>(std::move(one), nullptr));
  auto p = AniBd::with_endpoint(keepalive.back()->url());
  REQUIRE(p.has_value());
  return std::move(*p);
}

}  // namespace

TEST_CASE("transport_episodes_from_catalog") {
  auto p = against(response_with_body("200 OK", kCatalogFixture));
  auto eps = p.episodes("154587", Translation::Sub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(*eps == std::vector<std::string>{"1", "2", "3"});
}

TEST_CASE("transport_episodes_empty_catalog_is_not_stocked") {
  auto p = against(response_with_body("200 OK", "[]"));
  auto eps = p.episodes("154587", Translation::Sub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(eps->empty());
}

TEST_CASE("transport_episodes_forbidden_maps_to_taxonomy") {
  auto p = against(response_with_body("403 Forbidden", ""));
  auto got = p.episodes("154587", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Forbidden);
  CHECK(got.error().status == 403);
}

TEST_CASE("resolve_skips_private_player_url") {
  // apilink.php returns a loopback player link: the SSRF guard must skip it,
  // not fetch it. With no other candidates, resolve fails. Two hops answered
  // (catalog, apilink); the player URL itself must never be fetched.
  const char* catalog = R"([{"server_name":"Sub","server_data":[{"name":"1","link":"x"}]}])";
  const char* players = R"([{"link":"http://169.254.169.254/latest/meta-data/"}])";
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", catalog));
  responses.push_back(response_with_body("200 OK", players));
  SequenceServer srv(std::move(responses), nullptr);
  auto p = AniBd::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->resolve("154587", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("resolve_encodes_catalog_link_in_url") {
  // A catalog link with query metacharacters must be percent-encoded so it
  // stays inside the `data` param, not injected as siblings.
  const char* catalog =
      R"([{"server_name":"Sub","server_data":[{"name":"1","link":"evil&inject=1"}]}])";
  const char* players = "[]";  // empty players -> resolve fails; we assert the wire.
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", catalog));
  responses.push_back(response_with_body("200 OK", players));
  std::vector<std::string> captured;
  // Heap the server so we can destroy it (joining its thread, flushing
  // `captured`) before asserting, while resolve() has already returned.
  auto srv = std::make_unique<SequenceServer>(std::move(responses), &captured);
  auto p = AniBd::with_endpoint(srv->url());
  REQUIRE(p.has_value());
  (void)p->resolve("154587", "1", Translation::Sub, Quality::Best);
  srv.reset();  // join the server thread -> `captured` is now complete.
  REQUIRE(captured.size() >= 2);
  // The second request is apilink.php; the link must appear percent-encoded.
  CHECK(captured[1].find("data=evil%26inject%3D1") != std::string::npos);
}
