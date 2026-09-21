// hianime_tests.cpp — P47 golden tests: pure parsers/guards offline (search
// card scrape + main-sidebar cut, episode-list/servers JSON-envelope unwrap,
// data-hash base64, the `__P` XOR+JSON decode, entity decode, url encode)
// plus transport cases over loopback fixture servers. Fixtures under
// tests/fixtures/hianime/ were captured live against hianime.at and the
// ZokoAnime embed host on 2026-09-21 (search "frieren", site id 481,
// episode 1) — PROVIDER_INTEL.md §6 has the walk record.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "../src/crypto.hpp"
#include "../src/hianime.hpp"

using namespace shigoku;
using namespace shigoku::hianime;
using namespace shigoku::hianime::detail;

namespace {

std::string read_fixture(const char* name) {
  const std::string path = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/" + name;
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "missing fixture: " << path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

// ===========================================================================
// Pure parsers/guards — offline.
// ===========================================================================

TEST_CASE("decode_entities_handles_named_and_numeric_escapes_once") {
  CHECK(decode_entities("Journey&#039;s End") == "Journey's End");
  CHECK(decode_entities("Fire &amp; Ice") == "Fire & Ice");
  CHECK(decode_entities("&#x41;&#66;") == "AB");
  // Unknown entity stays literal; no double-decode.
  CHECK(decode_entities("Journey&amp;#039;s") == "Journey&#039;s");
  CHECK(decode_entities("no entities here") == "no entities here");
}

TEST_CASE("trailing_id_reads_the_slugs_numeric_tail") {
  CHECK(trailing_id("https://hianime.at/frieren-beyond-journeys-end-481").value() == "481");
  CHECK(trailing_id("/one-piece-1?ep=2").value() == "1");
  CHECK_FALSE(trailing_id("https://hianime.at/no-trailing-digits").has_value());
  CHECK_FALSE(trailing_id("https://hianime.at/-").has_value());
  CHECK_FALSE(trailing_id("").has_value());
}

TEST_CASE("parse_search_cards_stops_at_the_main_sidebar_marker") {
  auto html = read_fixture("hianime/search.html");
  auto cards = parse_search_cards(html);
  REQUIRE(cards.size() == 4);
  CHECK(cards[0].id == "481");
  CHECK(cards[0].title == "Frieren: Beyond Journey's End");
  CHECK(cards[1].id == "808");
  CHECK(cards[1].title == "Frieren: Beyond Journey's End Season 2");
  CHECK(cards[2].id == "2851");
  CHECK(cards[3].id == "7220");
  CHECK(cards[3].title == "Sousou no Frieren 3rd Season");
  // The sidebar's own trending rail (after the marker) must not leak in.
  for (const auto& c : cards) CHECK(c.title != "One Piece");
}

TEST_CASE("parse_search_cards_scans_the_whole_page_with_no_sidebar_marker") {
  const char* html = R"(<h3 class="film-name"><a href="https://hianime.at/x-1">X</a></h3>)";
  auto cards = parse_search_cards(html);
  REQUIRE(cards.size() == 1);
  CHECK(cards[0].id == "1");
  CHECK(cards[0].title == "X");
}

TEST_CASE("parse_episode_list_reads_number_and_id_pairs_off_the_real_capture") {
  auto raw = read_fixture("hianime/episodes.json");
  auto rows = parse_episode_list(raw);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 28);
  CHECK(rows->front().number == 1);
  CHECK(rows->front().id == "9227");
  CHECK(rows->back().number == 28);
}

TEST_CASE("parse_episode_list_rejects_a_body_that_isnt_the_json_envelope") {
  auto got = parse_episode_list("<html>404</html>");
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
  auto no_html_field = parse_episode_list(R"({"status":false})");
  REQUIRE_FALSE(no_html_field.has_value());
}

TEST_CASE("parse_episode_list_sorts_ascending_and_dedups_keeping_the_first_row") {
  const char* raw =
      R"({"html":"<a data-number=\"2\" data-id=\"20\"></a><a data-number=\"1\" data-id=\"10\">)"
      R"(</a><a data-number=\"1\" data-id=\"11\"></a>"})";
  auto rows = parse_episode_list(raw);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  CHECK((*rows)[0].number == 1);
  CHECK((*rows)[0].id == "10");
  CHECK((*rows)[1].number == 2);
}

TEST_CASE("find_zokoanime_hash_reads_sub_and_dub_off_the_real_capture") {
  auto raw = read_fixture("hianime/servers.json");
  auto sub = find_zokoanime_hash(raw, Translation::Sub);
  REQUIRE(sub.has_value());
  REQUIRE(sub->has_value());
  CHECK(**sub == "aHR0cHM6Ly96b2tvYW5pbWUudmlkZW8vc3RyZWFtL21hbC81Mjk5MS8xL3N1Yg==");

  auto dub = find_zokoanime_hash(raw, Translation::Dub);
  REQUIRE(dub.has_value());
  REQUIRE(dub->has_value());
  CHECK(**dub == "aHR0cHM6Ly96b2tvYW5pbWUudmlkZW8vc3RyZWFtL21hbC81Mjk5MS8xL2R1Yg==");
}

TEST_CASE("find_zokoanime_hash_misses_a_track_cleanly_not_an_error") {
  const char* raw =
      R"({"html":"<div class=\"item server-item\" data-type=\"sub\" data-server-name=\"HD-1\" )"
      R"(data-hash=\"xyz\"></div>"})";
  auto miss = find_zokoanime_hash(raw, Translation::Sub);
  REQUIRE(miss.has_value());
  CHECK_FALSE(miss->has_value());
}

TEST_CASE("decode_hash_reads_the_real_captured_hashes") {
  auto sub = decode_hash("aHR0cHM6Ly96b2tvYW5pbWUudmlkZW8vc3RyZWFtL21hbC81Mjk5MS8xL3N1Yg==");
  REQUIRE(sub.has_value());
  CHECK(*sub == "https://zokoanime.video/stream/mal/52991/1/sub");

  auto dub = decode_hash("aHR0cHM6Ly96b2tvYW5pbWUudmlkZW8vc3RyZWFtL21hbC81Mjk5MS8xL2R1Yg==");
  REQUIRE(dub.has_value());
  CHECK(*dub == "https://zokoanime.video/stream/mal/52991/1/dub");

  CHECK_FALSE(decode_hash("not base64!!").has_value());
  CHECK_FALSE(decode_hash("bm9wZQ==").has_value());  // "nope": valid base64, not a url.
}

TEST_CASE("extract_p_blob_finds_the_window_P_assignment") {
  auto html = read_fixture("hianime/embed.html");
  auto blob = extract_p_blob(html);
  REQUIRE(blob.has_value());
  CHECK(blob->substr(0, 8) == "FFYFBAJD");
  CHECK_FALSE(extract_p_blob("<html></html>").has_value());
  CHECK_FALSE(extract_p_blob("window.__P=\"unterminated").has_value());
}

TEST_CASE("xor_repeat_is_its_own_inverse") {
  const std::vector<std::uint8_t> data{1, 2, 3, 4, 5, 6, 7, 8, 9};
  auto once = xor_repeat(data, "ab");
  CHECK(once != data);
  auto twice = xor_repeat(once, "ab");
  CHECK(twice == data);
}

TEST_CASE("parse_embed_reads_src_and_subtitles") {
  auto s = parse_embed(
      R"({"src":"https://cdn.test/master.m3u8","subtitles":[{"lang":"en","label":"English",)"
      R"("default":true,"src":"https://cdn.test/en.vtt"}],"chapters":[]})");
  REQUIRE(s.has_value());
  REQUIRE(s->src.has_value());
  CHECK(*s->src == "https://cdn.test/master.m3u8");
  REQUIRE(s->subtitles.size() == 1);
  CHECK(s->subtitles[0].is_default);
  CHECK(s->subtitles[0].src == "https://cdn.test/en.vtt");

  auto bare = parse_embed(R"({"subtitles":[]})");
  REQUIRE(bare.has_value());
  CHECK_FALSE(bare->src.has_value());
  CHECK(bare->subtitles.empty());

  CHECK_FALSE(parse_embed("nope").has_value());
  CHECK_FALSE(parse_embed("[]").has_value());
}

TEST_CASE("embed_decode_end_to_end_over_the_real_captured_payload") {
  // base64 -> XOR -> JSON, over a real hianime.at/ZokoAnime capture: a
  // regression on the whole embed chain, not just its pieces.
  auto html = read_fixture("hianime/embed.html");
  auto blob = extract_p_blob(html);
  REQUIRE(blob.has_value());
  auto raw = crypto::base64_decode(*blob);
  REQUIRE(raw.has_value());
  auto plain = xor_repeat(*raw, kEmbedKey);
  auto embed = parse_embed(
      std::string_view(reinterpret_cast<const char*>(plain.data()), plain.size()));
  REQUIRE(embed.has_value());
  REQUIRE(embed->src.has_value());
  CHECK(*embed->src ==
        "https://hls.1embed.buzz/v/scxqicy/55ejtphmn9/e38a115ezb/dnfinmtr3wdbax/master.m3u8");
  REQUIRE(embed->subtitles.size() == 1);
  CHECK(embed->subtitles[0].is_default);
  CHECK(embed->subtitles[0].src ==
        "https://hls.1embed.buzz/v/scxqicy/55ejtphmn9/e38a115ezb/dnfinmtr3wdbax/subs/"
        "nzo9j4s07a98qkl1.vtt");
}

TEST_CASE("stream_url_ok_gates_absolute_clean_urls_only") {
  CHECK(stream_url_ok("https://cdn.test/x.m3u8"));
  CHECK_FALSE(stream_url_ok("not-a-url"));
  CHECK_FALSE(stream_url_ok("https://cdn.test/x y.m3u8"));
  CHECK_FALSE(stream_url_ok(""));
}

TEST_CASE("url_encode_percent_encodes_reserved_bytes") {
  CHECK(url_encode("attack on titan") == "attack%20on%20titan");
  CHECK(url_encode("a-b_c.d~e") == "a-b_c.d~e");
  CHECK(url_encode("50%") == "50%25");
}

TEST_CASE("origin_of_strips_the_path_down_to_the_origin") {
  CHECK(origin_of("https://zokoanime.video/stream/mal/52991/1/sub").value() ==
        "https://zokoanime.video/");
  CHECK(origin_of("https://host.example").value() == "https://host.example/");
  CHECK_FALSE(origin_of("/relative/path").has_value());
}

TEST_CASE("provider_identity_and_capabilities") {
  auto p = Hianime::create();
  REQUIRE(p.has_value());
  CHECK(p->name() == "hianime");
  CHECK(p->display_name() == "HiAnime");
  CHECK(p->supports_search());
  CHECK_FALSE(p->localized_titles());
  Enrichment show;
  show.anilist_id = 1;
  show.mal_id = 52991;
  CHECK_FALSE(p->canonical_key(show).has_value());
}

TEST_CASE("cover_request_wraps_an_absolute_url_rejects_the_rest") {
  auto p = Hianime::create();
  REQUIRE(p.has_value());
  auto ok = p->cover_request("https://cdn.anipixcdn.co/thumbnail/x.jpg");
  REQUIRE(ok.has_value());
  CHECK(ok->url == "https://cdn.anipixcdn.co/thumbnail/x.jpg");
  CHECK(ok->referer.value() == kReferer);
  CHECK_FALSE(p->cover_request("/relative.jpg").has_value());
  CHECK_FALSE(p->cover_request("").has_value());
  CHECK_FALSE(p->cover_request("https://cdn/x y.jpg").has_value());
}

// ===========================================================================
// Transport — against loopback fixture servers that answer a fixed sequence
// of requests (same shape as megaplay_tests' SequenceServer).
// ===========================================================================

namespace {

struct SequenceServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;

  explicit SequenceServer(std::vector<std::vector<std::uint8_t>> responses) {
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
        (void)::read(cfd, buf, sizeof(buf));
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

// A single-response provider, leaked into the returned provider's lifetime.
Hianime against(std::vector<std::uint8_t> response) {
  static std::vector<std::unique_ptr<SequenceServer>> keepalive;
  std::vector<std::vector<std::uint8_t>> one;
  one.push_back(std::move(response));
  keepalive.push_back(std::make_unique<SequenceServer>(std::move(one)));
  auto p = Hianime::with_endpoint(keepalive.back()->url());
  REQUIRE(p.has_value());
  return std::move(*p);
}

}  // namespace

TEST_CASE("transport_search_parses_a_2xx_body") {
  auto html = read_fixture("hianime/search.html");
  auto p = against(response_with_body("200 OK", html));
  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = 26;
  opts.page = 1;
  auto hits = p.search("frieren", opts);
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 4);
  CHECK((*hits)[0].provider_id == "481");
  CHECK((*hits)[0].title == "Frieren: Beyond Journey's End");
}

TEST_CASE("transport_search_forbidden_maps_to_taxonomy") {
  auto p = against(response_with_body("403 Forbidden", ""));
  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = 26;
  opts.page = 1;
  auto got = p.search("frieren", opts);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Forbidden);
  CHECK(got.error().status == 403);
}

TEST_CASE("episodes_rejects_a_non_numeric_show_id_before_any_fetch") {
  auto p = Hianime::create();
  REQUIRE(p.has_value());
  auto got = p->episodes("../7", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("transport_episodes_lists_every_number_for_sub_off_the_real_capture") {
  auto raw = read_fixture("hianime/episodes.json");
  auto p = against(response_with_body("200 OK", raw));
  auto eps = p.episodes("481", Translation::Sub, std::nullopt);
  REQUIRE(eps.has_value());
  REQUIRE(eps->size() == 28);
  CHECK(eps->front() == "1");
  CHECK(eps->back() == "28");
}

TEST_CASE("transport_episodes_dub_empty_when_the_first_episode_has_no_dub") {
  const char* eplist =
      R"({"html":"<a data-number=\"1\" data-id=\"1\"></a><a data-number=\"2\" data-id=\"2\">)"
      R"(</a>"})";
  const char* no_dub_row =
      R"({"html":"<div class=\"item server-item\" data-type=\"sub\" )"
      R"(data-server-name=\"ZokoAnime\" data-hash=\"x\"></div>"})";
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", eplist));
  responses.push_back(response_with_body("200 OK", no_dub_row));
  SequenceServer srv(std::move(responses));
  auto p = Hianime::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto eps = p->episodes("481", Translation::Dub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(eps->empty());
}

TEST_CASE("transport_episodes_dub_single_episode_shortcut") {
  const char* eplist = R"({"html":"<a data-number=\"1\" data-id=\"1\"></a>"})";
  const char* dub_row =
      R"({"html":"<div class=\"item server-item\" data-type=\"dub\" )"
      R"(data-server-name=\"ZokoAnime\" data-hash=\"x\"></div>"})";
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", eplist));
  responses.push_back(response_with_body("200 OK", dub_row));
  SequenceServer srv(std::move(responses));
  auto p = Hianime::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto eps = p->episodes("481", Translation::Dub, std::nullopt);
  REQUIRE(eps.has_value());
  REQUIRE(eps->size() == 1);
  CHECK(eps->front() == "1");
}

TEST_CASE("transport_episodes_dub_prefix_bisection_over_four_episodes") {
  // 4 episodes; dub covers 1-2 only. episode-list, then has_dub(ep1)=yes,
  // has_dub(ep4)=no, has_dub(ep2, mid of 0..3)=yes, has_dub(ep3, mid of
  // 1..3)=no -> lo=1 -> episodes "1","2".
  const char* eplist =
      R"({"html":"<a data-number=\"1\" data-id=\"1\"></a><a data-number=\"2\" data-id=\"2\">)"
      R"(</a><a data-number=\"3\" data-id=\"3\"></a><a data-number=\"4\" data-id=\"4\"></a>"})";
  const char* dub_yes =
      R"({"html":"<div class=\"item server-item\" data-type=\"dub\" )"
      R"(data-server-name=\"ZokoAnime\" data-hash=\"x\"></div>"})";
  const char* dub_no =
      R"({"html":"<div class=\"item server-item\" data-type=\"sub\" )"
      R"(data-server-name=\"ZokoAnime\" data-hash=\"x\"></div>"})";
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", eplist));
  responses.push_back(response_with_body("200 OK", dub_yes));  // ep 1
  responses.push_back(response_with_body("200 OK", dub_no));   // ep 4
  responses.push_back(response_with_body("200 OK", dub_yes));  // ep 2 (mid)
  responses.push_back(response_with_body("200 OK", dub_no));   // ep 3 (mid)
  SequenceServer srv(std::move(responses));
  auto p = Hianime::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto eps = p->episodes("481", Translation::Dub, std::nullopt);
  REQUIRE(eps.has_value());
  REQUIRE(eps->size() == 2);
  CHECK(eps->front() == "1");
  CHECK(eps->back() == "2");
}

TEST_CASE("resolve_rejects_a_non_numeric_show_id_before_any_fetch") {
  auto p = Hianime::create();
  REQUIRE(p.has_value());
  auto got = p->resolve("../7", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("resolve_guards_a_bad_episode_label_before_any_fetch") {
  auto p = Hianime::create();
  REQUIRE(p.has_value());
  for (const char* bad : {"1.2.3", "0", ""}) {
    auto got = p->resolve("481", bad, Translation::Sub, Quality::Best);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().kind == ProviderError::Kind::Decode);
  }
}

TEST_CASE("resolve_no_such_episode_is_a_clean_error_before_the_servers_hop") {
  auto p = against(response_with_body("200 OK", R"({"html":"<a data-number=\"1\" data-id=\"9227\"></a>"})"));
  auto got = p.resolve("481", "99", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("resolve_track_miss_is_a_clean_error") {
  const std::string eplist = R"({"html":"<a data-number=\"1\" data-id=\"9227\"></a>"})";
  const std::string servers_sub_only =
      R"({"html":"<div class=\"item server-item\" data-type=\"sub\" )"
      R"(data-server-name=\"ZokoAnime\" data-hash=\"x\"></div>"})";
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", eplist));
  responses.push_back(response_with_body("200 OK", servers_sub_only));
  SequenceServer srv(std::move(responses));
  auto p = Hianime::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->resolve("481", "1", Translation::Dub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("transport_resolve_refuses_a_private_embed_url_after_the_two_api_hops") {
  // The "two-server" resolve, anidbapp-shaped (transport_resolve_refuses_a_
  // private_embed_url): the api server answers episode-list then servers off
  // ONE connection sequence — two real hops — and the ZokoAnime data-hash it
  // hands back decodes to a DIFFERENT origin (a second, private-looking
  // server the real chain would cross to zokoanime.video for). That decoded
  // url is provider-supplied, so it must clear the SSRF guard before
  // anything dials it; a loopback target never does, and the chain stops
  // right there instead of ever making a third request.
  const std::string embed_url = "http://127.0.0.1:1/e";
  const std::string hash_b64 = crypto::base64_encode(
      reinterpret_cast<const std::uint8_t*>(embed_url.data()), embed_url.size());
  const std::string eplist = R"({"html":"<a data-number=\"1\" data-id=\"9227\"></a>"})";
  const std::string servers =
      R"({"html":"<div class=\"item server-item\" data-type=\"sub\" )"
      R"(data-server-name=\"ZokoAnime\" data-hash=\")" +
      hash_b64 + R"(\"></div>"})";

  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", eplist));
  responses.push_back(response_with_body("200 OK", servers));
  SequenceServer srv(std::move(responses));

  auto p = Hianime::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->resolve("481", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
  CHECK(got.error().detail == "blocked embed url");
}
