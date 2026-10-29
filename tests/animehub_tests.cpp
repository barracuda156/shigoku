// animehub_tests.cpp — golden tests: pure parsers/guards offline (search
// card scrape scoped off the sidebar, dub-twin folding, query sanitising, the
// episode-grid envelope, label order, the embed's zrpart2, the player page's
// sources JSON) plus transport cases over loopback fixture servers. Fixtures
// under tests/fixtures/animehub/ are live captures with the embed token and
// media urls replaced by synthetic values.

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

#include "../src/animehub.hpp"

using namespace shigoku;
using namespace shigoku::animehub;
using namespace shigoku::animehub::detail;

namespace {

std::string read_fixture(const char* name) {
  const std::string path = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/" + name;
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "missing fixture: " << path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A port nothing listens on: a fetch there fails as Network, so a Decode
// error proves the guard fired before any request.
constexpr const char* kDeadEndpoint = "http://127.0.0.1:1";

}  // namespace

// ===========================================================================
// Pure parsers/guards — offline.
// ===========================================================================

TEST_CASE("decode_entities_handles_named_and_numeric_escapes_once") {
  CHECK(decode_entities("Journey&#039;s End") == "Journey's End");
  CHECK(decode_entities("Fire &amp; Ice") == "Fire & Ice");
  CHECK(decode_entities("&#x41;&#66;") == "AB");
  CHECK(decode_entities("Journey&amp;#039;s") == "Journey&#039;s");
  CHECK(decode_entities("no entities here") == "no entities here");
}

TEST_CASE("search_query_turns_punctuation_into_single_spaces") {
  CHECK(search_query("Re:Zero") == "Re Zero");
  CHECK(search_query("Dr. Stone") == "Dr Stone");
  CHECK(search_query("Fate/Zero") == "Fate Zero");
  CHECK(search_query("Frieren: Beyond Journey's End") == "Frieren Beyond Journey s End");
  CHECK(search_query("Kaguya-sama wa Kokurasetai") == "Kaguya-sama wa Kokurasetai");
  CHECK(search_query("  86  ") == "86");
  CHECK(search_query("\xe8\x91\xac\xe9\x80\x81") == "");
  CHECK(search_query("") == "");
}

TEST_CASE("valid_slug_takes_the_sites_lowercase_dashed_shape_only") {
  CHECK(valid_slug("sousou-no-frieren"));
  CHECK(valid_slug("86-2nd-season"));
  CHECK_FALSE(valid_slug(""));
  CHECK_FALSE(valid_slug("../etc"));
  CHECK_FALSE(valid_slug("Sousou-no-frieren"));
  CHECK_FALSE(valid_slug("a/b"));
  CHECK_FALSE(valid_slug("-lead"));
  CHECK_FALSE(valid_slug("trail-"));
  CHECK_FALSE(valid_slug("a b"));
  CHECK_FALSE(valid_slug(std::string(kMaxSlugLen + 1, 'a')));
}

TEST_CASE("slug_title_title_cases_each_dashed_word") {
  CHECK(slug_title("kusuriya-no-hitorigoto") == "Kusuriya No Hitorigoto");
  CHECK(slug_title("sousou-no-frieren-2nd-season") == "Sousou No Frieren 2nd Season");
  CHECK(slug_title("86") == "86");
}

TEST_CASE("track_slug_appends_the_dub_twin_suffix_for_dub_only") {
  CHECK(track_slug("sousou-no-frieren", Translation::Sub) == "sousou-no-frieren");
  CHECK(track_slug("sousou-no-frieren", Translation::Dub) == "sousou-no-frieren-dub");
}

TEST_CASE("parse_search_cards_reads_the_result_list_and_stops_at_the_sidebar") {
  const auto cards = parse_search_cards(read_fixture("animehub/search.html"));
  REQUIRE(cards.size() == 6);
  CHECK(cards[0] == Card{"sousou-no-frieren-2nd-season-dub",
                         "Frieren: Beyond Journey's End Season 2 (Dub)", Translation::Dub, 10});
  CHECK(cards[1] == Card{"sousou-no-frieren-2nd-season",
                         "Frieren: Beyond Journey's End Season 2", Translation::Sub, 10});
  CHECK(cards[4] == Card{"sousou-no-frieren", "Frieren: Beyond Journey's End",
                         Translation::Sub, 28});
  CHECK(cards[5].slug == "kouryaku-wanted-isekai-sukuimasu");
  // The sidebar's ranking rail reuses class="name" and must not leak in.
  for (const auto& c : cards) {
    CHECK(c.slug != "youkoso-jitsuryoku-shijou-shugi-no-kyoushitsu-e-3rd-season-dub");
    CHECK(c.slug != "kimi-no-suizou-wo-tabetai-dub");
  }
}

TEST_CASE("parse_search_cards_finds_nothing_on_the_no_results_page") {
  CHECK(parse_search_cards(read_fixture("animehub/no_results.html")).empty());
}

TEST_CASE("parse_search_cards_without_a_film_list_still_stops_at_the_sidebar") {
  const std::string html =
      R"(<div class="item"><a href="/anime/one-piece" class="name">One Piece</a></div>)"
      R"(<div id="sidebar"><div class="item"><a href="/anime/naruto" class="name">Naruto</a></div></div>)";
  const auto cards = parse_search_cards(html);
  REQUIRE(cards.size() == 1);
  CHECK(cards[0].slug == "one-piece");
  CHECK(cards[0].episodes == 0);
}

TEST_CASE("parse_search_cards_skips_foreign_hrefs_and_takes_absolute_site_ones") {
  const std::string html =
      R"(<div class="film-list">)"
      R"(<div class="item"><a href="/genere/action" class="name">Action</a></div>)"
      R"(<div class="item"><a href="https://123animehub.cc/anime/death-note" class="name">Death Note</a></div>)"
      R"(<div class="item"><a href="/anime/Bad Slug" class="name">Bad</a></div>)"
      R"(</div>)";
  const auto cards = parse_search_cards(html);
  REQUIRE(cards.size() == 1);
  CHECK(cards[0].slug == "death-note");
}

TEST_CASE("merge_cards_folds_dub_twins_into_one_hit_per_title") {
  const auto hits = merge_cards(parse_search_cards(read_fixture("animehub/search.html")));
  REQUIRE(hits.size() == 4);
  CHECK(hits[0].provider_id == "sousou-no-frieren-2nd-season");
  CHECK(hits[0].title == "Sousou No Frieren 2nd Season");
  CHECK(hits[0].title_english.value() == "Frieren: Beyond Journey's End Season 2");
  CHECK(hits[0].eps_sub == 10);
  CHECK(hits[0].eps_dub == 10);
  CHECK(hits[1].provider_id == "sousou-no-frieren-no-mahou");
  CHECK(hits[1].eps_dub == 0);
  CHECK(hits[2].provider_id == "sousou-no-frieren");
  CHECK(hits[2].title_english.value() == "Frieren: Beyond Journey's End");
  CHECK(hits[2].eps_sub == 28);
  CHECK(hits[2].eps_dub == 28);
  // A card already named in romaji repeats the slug title: no alternate.
  CHECK(hits[3].provider_id == "kouryaku-wanted-isekai-sukuimasu");
  CHECK_FALSE(hits[3].title_english.has_value());
}

TEST_CASE("merge_cards_names_a_dub_only_title_from_its_dub_card") {
  const std::vector<Card> cards = {
      Card{"kimi-no-suizou-wo-tabetai-dub", "I Want to Eat Your Pancreas (Dub)",
           Translation::Dub, 1},
      Card{"kimi-no-suizou-wo-tabetai-dub", "Kimi No Suizou Wo Tabetai Dub", Translation::Dub, 1},
  };
  const auto hits = merge_cards(cards);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].provider_id == "kimi-no-suizou-wo-tabetai");
  CHECK(hits[0].title_english.value() == "I Want to Eat Your Pancreas");
  CHECK(hits[0].eps_sub == 0);
  CHECK(hits[0].eps_dub == 1);
}

TEST_CASE("valid_label_takes_integers_and_one_decimal_part") {
  CHECK(valid_label("1"));
  CHECK(valid_label("1128.5"));
  CHECK_FALSE(valid_label(""));
  CHECK_FALSE(valid_label(".5"));
  CHECK_FALSE(valid_label("5."));
  CHECK_FALSE(valid_label("1.2.3"));
  CHECK_FALSE(valid_label("12a"));
  CHECK_FALSE(valid_label("../1"));
  CHECK_FALSE(valid_label(std::string(17, '1')));
}

TEST_CASE("label_less_orders_numerically_with_fractions_between_neighbours") {
  CHECK(label_less("2", "10"));
  CHECK(label_less("10", "10.5"));
  CHECK(label_less("10.5", "11"));
  CHECK(label_less("10.25", "10.5"));
  CHECK_FALSE(label_less("10", "10"));
  CHECK_FALSE(label_less("10", "10.0"));
  CHECK_FALSE(label_less("11", "10.5"));
}

TEST_CASE("parse_film_reads_episodes_and_servers_off_the_real_grid") {
  auto film = parse_film(read_fixture("animehub/film_sv.json"), "sousou-no-frieren");
  REQUIRE(film.has_value());
  REQUIRE(film->episodes.size() == 28);
  CHECK(film->episodes.front() == "1");
  CHECK(film->episodes.back() == "28");
  CHECK(film->servers == std::vector<std::string>{"0", "10"});
}

TEST_CASE("parse_film_matches_only_its_own_track_slug") {
  auto film = parse_film(read_fixture("animehub/film_sv.json"), "sousou-no-frieren-dub");
  REQUIRE(film.has_value());
  CHECK(film->episodes.empty());
}

TEST_CASE("parse_film_sorts_chunked_ranges_and_dedups_labels") {
  const std::string raw =
      R"({"html":"<ul class=\"episodes range\" data-range-id=\"1\">)"
      R"(<li><a data-id=\"one-piece/1129\">1129</a></li>)"
      R"(<li><a data-id=\"one-piece/1128.5\">1128.5</a></li></ul>)"
      R"(<ul class=\"episodes range\" data-range-id=\"0\">)"
      R"(<li><a data-id=\"one-piece/2\">2</a></li>)"
      R"(<li><a data-id=\"one-piece/1128\">1128</a></li>)"
      R"(<li><a data-id=\"one-piece/2\">2</a></li>)"
      R"(<li><a data-id=\"one-piece/x\">x</a></li></ul>"})";
  auto film = parse_film(raw, "one-piece");
  REQUIRE(film.has_value());
  CHECK(film->episodes == std::vector<std::string>{"2", "1128", "1128.5", "1129"});
  CHECK(film->servers.empty());
}

TEST_CASE("parse_film_rejects_a_body_that_isnt_the_json_envelope") {
  auto bad = parse_film("<html>error</html>", "x");
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().kind == ProviderError::Kind::Decode);
  CHECK_FALSE(parse_film(R"({"status":200})", "x").has_value());
}

TEST_CASE("parse_target_reads_the_embed_url") {
  CHECK(parse_target(R"({"grabber":"g","target":"https://play.example/embed-3/abc","type":"iframe"})")
            .value() == "https://play.example/embed-3/abc");
  CHECK_FALSE(parse_target(R"({"target":""})").has_value());
  CHECK_FALSE(parse_target(R"({"target":7})").has_value());
  CHECK_FALSE(parse_target("not json").has_value());
}

TEST_CASE("extract_zrpart2_reads_the_token_off_the_real_embed") {
  CHECK(extract_zrpart2(read_fixture("animehub/embed.html")).value() ==
        "QUJDREVGR0hJSktMTU5PUA==");
  CHECK(extract_zrpart2(R"(var zrpart2="ab+/=_-";)").value() == "ab+/=_-");
  CHECK_FALSE(extract_zrpart2("var zlangSnd = 'sub';").has_value());
  CHECK_FALSE(extract_zrpart2("var zrpart2 = '';").has_value());
  CHECK_FALSE(extract_zrpart2("var zrpart2 = 'a\"b';").has_value());
  CHECK_FALSE(extract_zrpart2("var zrpart2 = 'a?b';").has_value());
}

TEST_CASE("parse_sources_reads_the_master_off_the_player_page") {
  CHECK(parse_sources(read_fixture("animehub/sources.html")).value() ==
        "https://eu-cdn1.echovideo.to/cdn/0123456789abcdef0123456789abcdef");
  CHECK(parse_sources(R"(<div id="sources">{"sources":[{"file":"https://a/m.m3u8"}]}</div>)")
            .value() == "https://a/m.m3u8");
  CHECK(parse_sources(R"(<div id="sources">{"sources":["https://a/m.m3u8"]}</div>)").value() ==
        "https://a/m.m3u8");
  CHECK_FALSE(parse_sources(R"(<div id="sources">{"sources":[]}</div>)").has_value());
  CHECK_FALSE(parse_sources(R"(<div id="sources">not json</div>)").has_value());
  CHECK_FALSE(parse_sources("<div id=\"other\"></div>").has_value());
}

TEST_CASE("stream_url_ok_gates_absolute_clean_public_urls_only") {
  CHECK(stream_url_ok("https://eu-cdn1.echovideo.to/cdn/abc"));
  CHECK_FALSE(stream_url_ok("/cdn/abc"));
  CHECK_FALSE(stream_url_ok("https://a/b c"));
  CHECK_FALSE(stream_url_ok("http://127.0.0.1:8080/e"));
}

TEST_CASE("url_encode_percent_encodes_reserved_bytes") {
  CHECK(url_encode("Re Zero") == "Re%20Zero");
  CHECK(url_encode("a-b_c.d~") == "a-b_c.d~");
  CHECK(url_encode("a&b") == "a%26b");
}

TEST_CASE("origin_of_keeps_scheme_host_and_port") {
  CHECK(origin_of("https://play2.echovideo.ru/embed-3/xyz?a=1").value() ==
        "https://play2.echovideo.ru");
  CHECK(origin_of("http://h:8080").value() == "http://h:8080");
  CHECK(origin_of("https://h?q").value() == "https://h");
  CHECK_FALSE(origin_of("no-scheme").has_value());
  CHECK_FALSE(origin_of("https://").has_value());
}

TEST_CASE("provider_identity_and_capabilities") {
  auto p = AnimeHub::create();
  REQUIRE(p.has_value());
  CHECK(p->name() == "animehub");
  CHECK(p->display_name() == "AnimeHub");
  CHECK(p->supports_search());
  CHECK_FALSE(p->localized_titles());
  Enrichment show;
  show.anilist_id = 154587;
  show.mal_id = 52991;
  CHECK_FALSE(p->canonical_key(show).has_value());
}

TEST_CASE("cover_request_wraps_an_absolute_url_rejects_the_rest") {
  auto p = AnimeHub::create();
  REQUIRE(p.has_value());
  auto ok = p->cover_request("https://123animehub.cc/imgs/poster/sousou-no-frieren.jpg");
  REQUIRE(ok.has_value());
  CHECK(ok->referer.value() == kReferer);
  CHECK_FALSE(p->cover_request("/imgs/poster/x.jpg").has_value());
  CHECK_FALSE(p->cover_request("").has_value());
  CHECK_FALSE(p->cover_request("https://h/x y.jpg").has_value());
}

// ===========================================================================
// Transport — against loopback fixture servers that answer a fixed sequence
// of requests (same shape as hianime_tests' SequenceServer).
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
  std::string head = "HTTP/1.1 " + status + "\r\nContent-Type: text/html; charset=UTF-8\r\n" +
                     "Content-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

SearchOptions sub_page_one() {
  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = kSearchPageSize;
  opts.page = 1;
  return opts;
}

}  // namespace

TEST_CASE("transport_search_folds_the_real_result_page") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", read_fixture("animehub/search.html")));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto hits = p->search("Sousou no Frieren", sub_page_one());
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 4);
  CHECK((*hits)[2].provider_id == "sousou-no-frieren");
}

TEST_CASE("transport_search_honours_the_limit") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", read_fixture("animehub/search.html")));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto opts = sub_page_one();
  opts.limit = 2;
  auto hits = p->search("frieren", opts);
  REQUIRE(hits.has_value());
  CHECK(hits->size() == 2);
}

TEST_CASE("transport_search_with_nothing_searchable_skips_the_request") {
  auto p = AnimeHub::with_endpoint(kDeadEndpoint);
  REQUIRE(p.has_value());
  auto hits = p->search("\xe8\x91\xac\xe9\x80\x81 :/", sub_page_one());
  REQUIRE(hits.has_value());
  CHECK(hits->empty());
}

TEST_CASE("transport_search_forbidden_maps_to_taxonomy") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("403 Forbidden", ""));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->search("frieren", sub_page_one());
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Forbidden);
  CHECK(got.error().status == 403);
}

TEST_CASE("episodes_rejects_a_bad_show_id_before_any_fetch") {
  auto p = AnimeHub::with_endpoint(kDeadEndpoint);
  REQUIRE(p.has_value());
  auto got = p->episodes("../x", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("transport_episodes_lists_the_real_grid_for_sub") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", read_fixture("animehub/film_sv.json")));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto eps = p->episodes("sousou-no-frieren", Translation::Sub, std::nullopt);
  REQUIRE(eps.has_value());
  REQUIRE(eps->size() == 28);
  CHECK(eps->front() == "1");
  CHECK(eps->back() == "28");
}

TEST_CASE("transport_episodes_dub_twin_missing_is_an_authoritative_empty") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("500 Internal Server Error", ""));
  responses.push_back(response_with_body("404 Not Found", "<html>not found</html>"));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto eps = p->episodes("kouryaku-wanted-isekai-sukuimasu", Translation::Dub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(eps->empty());
}

TEST_CASE("transport_episodes_dub_500_with_a_live_show_page_stays_an_outage") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("500 Internal Server Error", ""));
  responses.push_back(response_with_body("200 OK", "<html>show</html>"));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->episodes("sousou-no-frieren", Translation::Dub, std::nullopt);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Server);
  CHECK(got.error().status == 500);
}

TEST_CASE("transport_episodes_sub_500_is_an_error_without_the_page_probe") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("500 Internal Server Error", ""));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->episodes("no-such-show", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Server);
}

TEST_CASE("resolve_guards_the_show_id_and_episode_label_before_any_fetch") {
  auto p = AnimeHub::with_endpoint(kDeadEndpoint);
  REQUIRE(p.has_value());
  auto bad_id = p->resolve("a/b", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(bad_id.has_value());
  CHECK(bad_id.error().detail == "invalid show id");
  auto bad_ep = p->resolve("sousou-no-frieren", "1/0", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(bad_ep.has_value());
  CHECK(bad_ep.error().detail == "invalid episode");
}

TEST_CASE("resolve_no_such_episode_is_a_clean_error_before_the_info_hop") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", read_fixture("animehub/film_sv.json")));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->resolve("sousou-no-frieren", "99", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().detail == "no such episode");
}

TEST_CASE("transport_resolve_refuses_a_private_embed_on_every_server") {
  // The grid names two servers; each info hop hands back a loopback target.
  // That url is provider-supplied, so the SSRF guard refuses it before any
  // embed fetch, and the walk moves to the next server, then gives up.
  const std::string info =
      R"({"grabber":"","params":[],"backup":1,"target":"http://127.0.0.1:1/embed-3/x","type":"iframe","name":"Full","subtitle":""})";
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", read_fixture("animehub/film_sv.json")));
  responses.push_back(response_with_body("200 OK", info));
  responses.push_back(response_with_body("200 OK", info));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->resolve("sousou-no-frieren", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
  CHECK(got.error().detail == "blocked embed url");
}

TEST_CASE("transport_resolve_info_without_a_target_falls_through_the_servers") {
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", read_fixture("animehub/film_sv.json")));
  responses.push_back(response_with_body("200 OK", R"({"status":false})"));
  responses.push_back(response_with_body("503 Service Unavailable", ""));
  SequenceServer srv(std::move(responses));
  auto p = AnimeHub::with_endpoint(srv.url());
  REQUIRE(p.has_value());
  auto got = p->resolve("sousou-no-frieren", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Server);
  CHECK(got.error().status == 503);
}
