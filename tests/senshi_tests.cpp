// senshi_tests.cpp — P4 golden tests, ported 1:1 from
// src/providers/senshi.rs's `mod tests` (PORT_CPP.md P4: ~20 tests, the
// golden contract). Fixtures copied verbatim from sabigoku's
// tests/fixtures/senshi_{filter,episodes}.json.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "../src/senshi.hpp"

using namespace shigoku;
using namespace shigoku::senshi;
using namespace shigoku::senshi::detail;

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
// map_anime / map_status / parse_leading_uint
// ===========================================================================

TEST_CASE("map_anime_maps_a_filter_row") {
  FilterRow row;
  row.id = 59708;
  row.title = "Youkoso Jitsuryoku Shijou Shugi no Kyoushitsu e 4th Season";
  row.title_english = "Classroom of the Elite 4th Season";
  row.ani_episodes = "16";
  row.ani_status = "Finished Airing";
  row.ani_year = 2026;

  const SearchHit h = map_anime(row);
  CHECK(h.provider_id == "59708");
  CHECK(h.mal_id == std::optional<std::int64_t>(59708));
  CHECK(h.title_english.value() == "Classroom of the Elite 4th Season");
  CHECK(h.total_episodes == std::optional<std::uint32_t>(16));
  CHECK(h.eps_sub == 16);
  CHECK(h.eps_dub == 0);
  CHECK(h.year == std::optional<std::uint32_t>(2026));
  CHECK_FALSE(h.title_native.has_value());
}

TEST_CASE("map_anime_withholds_total_while_airing") {
  FilterRow row;
  row.id = 1;
  row.title = "X";
  row.ani_episodes = "4";
  row.ani_status = "Currently Airing";
  row.ani_year = 2026;

  const SearchHit h = map_anime(row);
  CHECK_FALSE(h.total_episodes.has_value());
  CHECK(h.eps_sub == 4);
}

TEST_CASE("map_status_folds_senshi_wording") {
  CHECK(map_status("Finished Airing").value() == "FINISHED");
  CHECK_FALSE(is_still_airing(std::string_view("FINISHED")));
  CHECK(map_status("Currently Airing").value() == "RELEASING");
  CHECK(is_still_airing(std::string_view("RELEASING")));
  CHECK(map_status("Not yet aired").value() == "NOT_YET_RELEASED");
  CHECK(map_status("Weird Label").value() == "Weird Label");
  CHECK_FALSE(map_status(std::nullopt).has_value());
}

TEST_CASE("parse_leading_uint_takes_the_digit_prefix") {
  CHECK(parse_leading_uint("23 min per ep") == std::optional<std::uint32_t>(23));
  CHECK(parse_leading_uint("16") == std::optional<std::uint32_t>(16));
  CHECK_FALSE(parse_leading_uint("n/a").has_value());
  CHECK_FALSE(parse_leading_uint("").has_value());
}

// ===========================================================================
// ep_label / parse_episodes
// ===========================================================================

TEST_CASE("ep_label_drops_integral_decimal_keeps_fractional") {
  CHECK(ep_label(1.0) == "1");
  CHECK(ep_label(10.0) == "10");
  CHECK(ep_label(13.5) == "13.5");
  CHECK(ep_label(std::numeric_limits<double>::quiet_NaN()) == "0");
  CHECK(ep_label(-2.0) == "0");
}

TEST_CASE("parse_episodes_drops_phantom_zero_and_sorts") {
  const std::string_view raw =
      R"([{"ep_id":3},{"ep_id":1},{"ep_id":0},{"ep_id":2},{"ep_id":1.5},{"ep_id":10}])";
  auto r = parse_episodes(raw);
  REQUIRE(r.has_value());
  CHECK(*r == std::vector<std::string>{"1", "1.5", "2", "3", "10"});
}

TEST_CASE("parse_episodes_drops_hostile_negative_never_mislabels_to_zero") {
  const std::string_view raw = R"([{"ep_id":-5},{"ep_id":2},{"ep_id":-0.5}])";
  auto r = parse_episodes(raw);
  REQUIRE(r.has_value());
  CHECK(*r == std::vector<std::string>{"2"});
  CHECK_FALSE(std::find(r->begin(), r->end(), "0") != r->end());
}

// ===========================================================================
// pick_embed / match_score
// ===========================================================================

TEST_CASE("pick_embed_first_wins_on_a_score_tie") {
  const std::vector<Embed> embeds = {
      Embed{"first", "SoftSub", std::nullopt},
      Embed{"second", "SoftSub", std::nullopt},
  };
  auto picked = pick_embed(embeds, Translation::Sub);
  REQUIRE(picked.has_value());
  CHECK(picked->url.value() == "first");
}

TEST_CASE("match_score_ranks_sub_soft_over_hard_and_separates_dub") {
  CHECK(match_score(std::string_view("SoftSub"), Translation::Sub) == 3);
  CHECK(match_score(std::string_view("HardSub"), Translation::Sub) == 2);
  CHECK(match_score(std::string_view("Sub"), Translation::Sub) == 1);
  CHECK(match_score(std::string_view("Dub"), Translation::Sub) == 0);
  CHECK(match_score(std::string_view("Dub"), Translation::Dub) == 1);
  CHECK(match_score(std::string_view("SoftSub"), Translation::Dub) == 0);
  CHECK(match_score(std::nullopt, Translation::Sub) == 0);
}

TEST_CASE("pick_embed_takes_the_best_track_or_none") {
  const std::vector<Embed> embeds = {
      Embed{"hard", "HardSub", std::nullopt},
      Embed{"soft", "SoftSub", std::nullopt},
      Embed{std::nullopt, "SoftSub", std::nullopt},  // no url, skip.
  };
  auto picked = pick_embed(embeds, Translation::Sub);
  REQUIRE(picked.has_value());
  CHECK(picked->url.value() == "soft");

  const std::vector<Embed> dub_only = {Embed{"d", "Dub", std::nullopt}};
  CHECK_FALSE(pick_embed(dub_only, Translation::Sub).has_value());
}

// ===========================================================================
// sub_info_url / pick_sub_track / guarded_sub_track / percent_decode
// ===========================================================================

TEST_CASE("sub_info_url_extracts_and_percent_decodes") {
  const std::string_view fm = "host=cdn&sub.info=https%3A%2F%2Fcdn%2Finfo.json&x=1";
  CHECK(sub_info_url(fm).value() == "https://cdn/info.json");
  CHECK_FALSE(sub_info_url(std::string_view("no sidecar here")).has_value());
  CHECK_FALSE(sub_info_url(std::string_view("sub.info=")).has_value());
  CHECK_FALSE(sub_info_url(std::nullopt).has_value());
}

TEST_CASE("pick_sub_track_prefers_default_then_english_then_first") {
  const std::vector<SubTrack> tracks = {
      SubTrack{"jp", "Japanese", false},
      SubTrack{"en", "English", false},
  };
  CHECK(pick_sub_track(tracks).value() == "en");

  const std::vector<SubTrack> with_default = {
      SubTrack{"en", "English", false},
      SubTrack{"host", "Whatever", true},
  };
  CHECK(pick_sub_track(with_default).value() == "host");

  const std::vector<SubTrack> no_english = {SubTrack{"jp", "Japanese", false}};
  CHECK(pick_sub_track(no_english).value() == "jp");

  CHECK_FALSE(pick_sub_track({}).has_value());
}

TEST_CASE("guarded_sub_track_drops_a_private_or_unsafe_src") {
  const std::vector<SubTrack> ok = {SubTrack{"https://cdn.example/eng.vtt", "English", true}};
  CHECK(guarded_sub_track(ok).value() == "https://cdn.example/eng.vtt");

  for (const char* bad : {"http://169.254.169.254/latest/meta-data/", "http://127.0.0.1:9/pwn.vtt"}) {
    const std::vector<SubTrack> t = {SubTrack{std::string(bad), "English", true}};
    CAPTURE(bad);
    CHECK_FALSE(guarded_sub_track(t).has_value());
  }

  const std::vector<SubTrack> rel = {SubTrack{"//cdn.example/x.vtt", std::nullopt, true}};
  CHECK_FALSE(guarded_sub_track(rel).has_value());
}

TEST_CASE("percent_decode_keeps_malformed_and_plus_literal") {
  CHECK(percent_decode("a%2Fb") == "a/b");
  CHECK(percent_decode("a%2b") == "a+");
  CHECK(percent_decode("a+b") == "a+b");
  CHECK(percent_decode("a%zz") == "a%zz");
  CHECK(percent_decode("tail%") == "tail%");
}

// ===========================================================================
// guard_ep_label
// ===========================================================================

TEST_CASE("guard_ep_label_allows_one_dot") {
  CHECK(guard_ep_label("1").has_value());
  CHECK(guard_ep_label("13.5").has_value());
  CHECK_FALSE(guard_ep_label("").has_value());
  CHECK_FALSE(guard_ep_label("1.2.3").has_value());
  CHECK_FALSE(guard_ep_label("1/2").has_value());
  CHECK_FALSE(guard_ep_label("e1").has_value());
}

// ===========================================================================
// Golden fixtures (live capture, trimmed — copied from sabigoku).
// ===========================================================================

namespace {

std::vector<SearchHit> parse_search_fixture(const std::string& raw, std::uint32_t limit) {
  const auto j = nlohmann::json::parse(raw);
  std::vector<SearchHit> hits;
  for (const auto& row : j.at("data")) {
    FilterRow fr;
    fr.id = row.at("id").get<std::uint64_t>();
    if (row.contains("title") && row.at("title").is_string()) fr.title = row.at("title").get<std::string>();
    if (row.contains("title_english") && row.at("title_english").is_string())
      fr.title_english = row.at("title_english").get<std::string>();
    if (row.contains("ani_episodes") && row.at("ani_episodes").is_string())
      fr.ani_episodes = row.at("ani_episodes").get<std::string>();
    if (row.contains("ani_status") && row.at("ani_status").is_string())
      fr.ani_status = row.at("ani_status").get<std::string>();
    if (row.contains("ani_year") && row.at("ani_year").is_number())
      fr.ani_year = row.at("ani_year").get<std::uint32_t>();
    hits.push_back(map_anime(fr));
  }
  if (hits.size() > limit) hits.resize(limit);
  return hits;
}

}  // namespace

TEST_CASE("golden_filter_maps_rows_and_keys_by_mal") {
  const auto hits = parse_search_fixture(read_fixture("senshi_filter.json"), 26);
  REQUIRE(hits.size() == 3);
  auto it = std::find_if(hits.begin(), hits.end(), [](const SearchHit& h) { return h.provider_id == "52991"; });
  REQUIRE(it != hits.end());
  CHECK(it->title == "Sousou no Frieren");
  CHECK(it->mal_id == std::optional<std::int64_t>(52991));
  CHECK_FALSE(it->anilist_id.has_value());
  CHECK(it->total_episodes == std::optional<std::uint32_t>(28));
  CHECK(it->eps_sub == 28);
  CHECK(it->year == std::optional<std::uint32_t>(2023));
  CHECK(std::any_of(hits.begin(), hits.end(), [](const SearchHit& h) { return h.provider_id == "59978"; }));
}

TEST_CASE("golden_filter_respects_limit") {
  CHECK(parse_search_fixture(read_fixture("senshi_filter.json"), 1).size() == 1);
}

TEST_CASE("golden_episodes_sorts_and_drops_phantom_zero") {
  auto r = parse_episodes(read_fixture("senshi_episodes.json"));
  REQUIRE(r.has_value());
  CHECK(r->size() == 28);
  CHECK(r->front() == "1");
  CHECK((*r)[9] == "10");
  CHECK(r->back() == "28");
  CHECK_FALSE(std::find(r->begin(), r->end(), "0") != r->end());
}

// ===========================================================================
// Transport — against a one-shot loopback fixture server (same shape as
// http_tests.cpp / anilist_tests.cpp's OneShotServer).
// ===========================================================================

namespace {

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
      ssize_t n = ::read(cfd, buf, sizeof(buf));
      (void)n;
      ::write(cfd, response.data(), response.size());
      ::close(cfd);
    });
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

  ~OneShotServer() {
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

Senshi against(std::vector<std::uint8_t> response) {
  // The server is deliberately leaked into the returned Senshi's lifetime by
  // running exactly one request against it (matches serve_once's contract:
  // one connection, then the thread exits after writing the response).
  static std::vector<std::unique_ptr<OneShotServer>> keepalive;
  keepalive.push_back(std::make_unique<OneShotServer>(std::move(response)));
  auto p = Senshi::with_endpoint(keepalive.back()->url());
  REQUIRE(p.has_value());
  return std::move(*p);
}

}  // namespace

TEST_CASE("transport_search_parses_a_2xx_body") {
  auto p = against(response_with_body("201 Created", read_fixture("senshi_filter.json")));
  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = 26;
  opts.page = 1;
  auto hits = p.search("frieren", opts);
  REQUIRE(hits.has_value());
  CHECK(hits->size() == 3);
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

TEST_CASE("transport_episodes_rejects_a_non_numeric_show_id_before_fetch") {
  auto p = Senshi::create();
  REQUIRE(p.has_value());
  auto got = p->episodes("../7", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("canonical_key_is_the_stringified_mal_id") {
  auto p = Senshi::create();
  REQUIRE(p.has_value());
  Enrichment with_mal;
  with_mal.anilist_id = 1;
  with_mal.mal_id = 52991;
  CHECK(p->canonical_key(with_mal).value() == "52991");

  Enrichment no_mal;
  no_mal.anilist_id = 1;
  CHECK_FALSE(p->canonical_key(no_mal).has_value());
}

TEST_CASE("transport_resolve_builds_a_streamlink_from_an_embed") {
  const std::string_view embeds =
      R"([{"url":"https://cdn.test/master.m3u8","status":"HardSub"},{"url":"https://cdn.test/dub.m3u8","status":"Dub"}])";
  auto p = against(response_with_body("200 OK", embeds));
  auto sl = p.resolve("59708", "1", Translation::Dub, Quality::Best);
  REQUIRE(sl.has_value());
  CHECK(sl->url == "https://cdn.test/dub.m3u8");
  CHECK(sl->cloaked_segments);
  CHECK(sl->referer.value() == kStreamReferer);
  CHECK(sl->user_agent.value() == http::kBrowserUserAgent);
  CHECK_FALSE(sl->sub_url.has_value());
}

TEST_CASE("transport_resolve_track_miss_is_a_clean_error") {
  const std::string_view embeds = R"([{"url":"https://cdn.test/dub.m3u8","status":"Dub"}])";
  auto p = against(response_with_body("200 OK", embeds));
  auto got = p.resolve("59708", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("transport_resolve_rejects_a_non_absolute_embed_url") {
  const std::string_view embeds = R"([{"url":"//cdn.test/x.m3u8","status":"SoftSub"}])";
  auto p = against(response_with_body("200 OK", embeds));
  auto got = p.resolve("59708", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("resolve_guards_a_bad_episode_label_before_any_fetch") {
  auto p = Senshi::create();
  REQUIRE(p.has_value());
  auto got = p->resolve("59708", "1.2.3", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}
