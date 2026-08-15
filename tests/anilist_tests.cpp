// anilist_tests.cpp — P3 golden tests, ported from src/anilist.rs's `mod
// tests` (search path only). Same fixture as sabigoku's
// tests/fixtures/anilist_search.json (copied verbatim) so the golden values
// agree with the Rust port's asserts.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "../src/anilist.hpp"

using namespace shigoku;
using namespace shigoku::anilist;
using json = nlohmann::json;

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
// classify_page (anilist.rs classify_page / golden_search_page)
// ===========================================================================

TEST_CASE("golden_search_page") {
  const std::string raw = read_fixture("anilist_search.json");
  auto r = detail::classify_page(raw);
  REQUIRE(r.has_value());
  const CatalogPage& page = *r;

  CHECK(page.entries.size() == 3);
  CHECK(page.has_next);

  CHECK(page.entries[0].anilist_id == 154587);
  CHECK(page.entries[0].mal_id == std::optional<std::int64_t>(52991));
  CHECK(page.entries[0].title_romaji == "Sousou no Frieren");
  CHECK(page.entries[0].title_english.value() == "Frieren: Beyond Journey’s End");
  CHECK(page.entries[0].total_episodes == std::optional<std::uint32_t>(28));
  CHECK(page.entries[0].year == std::optional<std::uint32_t>(2023));
  CHECK(page.entries[0].season == std::optional<Season>(Season::Fall));
  CHECK(page.entries[0].status.value() == "FINISHED");
  CHECK(page.entries[0].score == std::optional<std::uint32_t>(91));
  CHECK(page.entries[0].kind.value() == "TV");
  CHECK(page.entries[0].genres == std::vector<std::string>{"Adventure", "Drama", "Fantasy"});
  CHECK(page.entries[0].cover_url.has_value());
  CHECK(page.entries[0].cover_url->find("154587") != std::string::npos);

  CHECK(page.entries[1].title_romaji == "Sousou no Frieren 3rd Season");
  // Unaired sequel: sparse record must map, not fail (episodes null).
  CHECK_FALSE(page.entries[1].total_episodes.has_value());
}

// ===========================================================================
// classify_by_id (P21, anilist.rs classify_by_id / golden_by_id_full_mapping)
// ===========================================================================

TEST_CASE("golden_by_id_full_mapping") {
  const std::string raw = read_fixture("anilist_by_id.json");
  auto r = detail::classify_by_id(raw);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  const Enrichment& e = **r;

  CHECK(e.anilist_id == 154587);
  CHECK(e.mal_id == std::optional<std::int64_t>(52991));
  CHECK(e.title_romaji == "Sousou no Frieren");
  CHECK(e.title_english == std::optional<std::string>("Frieren: Beyond Journey’s End"));
  CHECK(e.title_native == std::optional<std::string>("葬送のフリーレン"));
  CHECK(e.total_episodes == std::optional<std::uint32_t>(28));
  CHECK(e.duration_minutes == std::optional<std::uint32_t>(24));
  CHECK(e.year == std::optional<std::uint32_t>(2023));
  CHECK(e.season == std::optional<Season>(Season::Fall));
  CHECK(e.status == std::optional<std::string>("FINISHED"));
  CHECK(e.score == std::optional<std::uint32_t>(91));
  CHECK(e.kind == std::optional<std::string>("TV"));
  CHECK(e.start_date == std::optional<Date>(Date{2023, std::optional<std::uint32_t>(9),
                                                 std::optional<std::uint32_t>(29)}));
  CHECK(e.genres == std::vector<std::string>{"Adventure", "Drama", "Fantasy"});
  CHECK(e.studios == std::vector<std::string>{"MADHOUSE"});
  CHECK(e.source_material == std::optional<std::string>("MANGA"));
  // Contextual RATED 2023 outranks the all-time rank 1 (ROD-261 §5.3a).
  CHECK(e.rank == std::optional<std::uint32_t>(1));
  CHECK(e.rank_type == std::optional<std::string>("RATED"));
  CHECK(e.rank_year == std::optional<std::uint32_t>(2023));
  CHECK_FALSE(e.next_airing_at.has_value());
  CHECK_FALSE(e.next_airing_episode.has_value());
  CHECK(e.country == std::optional<std::string>("JP"));
  REQUIRE(e.cover_url.has_value());
  CHECK(e.cover_url->rfind("https://", 0) == 0);
  CHECK(e.cover_url->find("154587") != std::string::npos);
  REQUIRE(e.description.has_value());
  // Sanitized: tags stripped, entities decoded, newlines collapsed, trimmed.
  const std::string& desc = *e.description;
  const std::string suffix = "(Source: Crunchyroll)";
  REQUIRE(desc.size() >= suffix.size());
  CHECK(desc.compare(desc.size() - suffix.size(), suffix.size(), suffix) == 0);
  CHECK(desc.find('<') == std::string::npos);
  CHECK(desc.find('\n') == std::string::npos);
}

TEST_CASE("by_id_media_null_is_confirmed_no_match") {
  const std::string_view raw = R"({"data":{"Media":null}})";
  auto r = detail::classify_by_id(raw);
  REQUIRE(r.has_value());
  CHECK_FALSE(r->has_value());  // Ok(nullopt): a true negative, not an error.
}

TEST_CASE("by_id_data_null_is_no_answer") {
  {
    const std::string_view raw = R"({"data":null})";
    auto r = detail::classify_by_id(raw);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().kind == ProviderError::Kind::Decode);
  }
  {
    const std::string_view raw = "not json";
    auto r = detail::classify_by_id(raw);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().kind == ProviderError::Kind::Decode);
  }
}

TEST_CASE("by_id_body binds the id variable") {
  const std::string body = detail::by_id_body(154587);
  const json j = json::parse(body);
  CHECK(j.at("variables").at("id") == 154587);
  CHECK(j.at("query").get<std::string>().find("Media(id:$id,type:ANIME)") !=
        std::string::npos);
}

// ===========================================================================
// classify_characters_recs (P36)
// ===========================================================================

TEST_CASE("golden_characters_recs_full_mapping") {
  const std::string raw = read_fixture("anilist_characters_recs.json");
  auto r = detail::classify_characters_recs(raw);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  const CharactersAndRecommendations& cr = **r;

  // Characters: the null-name row is dropped, the VA-less row keeps role +
  // empty VA, order preserved.
  REQUIRE(cr.characters.size() == 3);
  CHECK(cr.characters[0].name == "Frieren");
  CHECK(cr.characters[0].role == "MAIN");
  CHECK(cr.characters[0].va_name == std::optional<std::string>("Atsumi Tanezaki"));
  CHECK(cr.characters[1].name == "Fern");
  CHECK(cr.characters[1].va_name == std::optional<std::string>("Kana Ichinose"));
  CHECK(cr.characters[2].name == "Sein");
  CHECK(cr.characters[2].role == "SUPPORTING");
  CHECK_FALSE(cr.characters[2].va_name.has_value());

  // Recommendations: the null mediaRecommendation node is skipped, and so is
  // the cross-type MANGA rec (only ANIME rows can ever be played — P36
  // review); the surviving two map through the shared media_to_enrichment
  // (kMediaFields), so a rec row is already a full Enrichment ready for
  // catalog_cache.
  REQUIRE(cr.recommendations.size() == 2);
  CHECK(cr.recommendations[0].anilist_id == 101922);
  CHECK(cr.recommendations[0].title_romaji == "Kimetsu no Yaiba");
  CHECK(cr.recommendations[0].score == std::optional<std::uint32_t>(84));
  CHECK(cr.recommendations[0].mal_id == std::optional<std::int64_t>(39535));
  CHECK(cr.recommendations[1].anilist_id == 21);
  CHECK(cr.recommendations[1].title_romaji == "One Piece");
  CHECK_FALSE(cr.recommendations[1].total_episodes.has_value());
}

TEST_CASE("characters_recs_media_null_is_confirmed_no_match") {
  const std::string_view raw = R"({"data":{"Media":null}})";
  auto r = detail::classify_characters_recs(raw);
  REQUIRE(r.has_value());
  CHECK_FALSE(r->has_value());  // Ok(nullopt): a true negative, not an error.
}

TEST_CASE("characters_recs_data_null_is_no_answer") {
  {
    const std::string_view raw = R"({"data":null})";
    auto r = detail::classify_characters_recs(raw);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().kind == ProviderError::Kind::Decode);
  }
  {
    const std::string_view raw = "not json";
    auto r = detail::classify_characters_recs(raw);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().kind == ProviderError::Kind::Decode);
  }
}

TEST_CASE("characters_recs_empty_lists_are_still_ok_some") {
  // A quiet show (no characters/recs staffed yet) is a present Media with
  // empty arrays — Ok(some), NOT a negative answer.
  const std::string_view raw =
      R"({"data":{"Media":{"characters":{"edges":[]},"recommendations":{"nodes":[]}}}})";
  auto r = detail::classify_characters_recs(raw);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  CHECK((*r)->characters.empty());
  CHECK((*r)->recommendations.empty());
}

TEST_CASE("characters_recs_body binds the id variable") {
  const std::string body = detail::characters_recs_body(154587);
  const json j = json::parse(body);
  CHECK(j.at("variables").at("id") == 154587);
  CHECK(j.at("query").get<std::string>().find("Media(id:$id,type:ANIME)") !=
        std::string::npos);
}

TEST_CASE("characters_recs_query requests characters and recommendations") {
  const std::string q = detail::characters_recs_query();
  CHECK(q.find("characters(page:1,perPage:12") != std::string::npos);
  CHECK(q.find("recommendations(perPage:10") != std::string::npos);
  // `type` must ride the rec node so the parser can drop cross-type recs.
  CHECK(q.find("mediaRecommendation{type ") != std::string::npos);
}

TEST_CASE("page_without_pageinfo_is_exhausted") {
  const std::string_view raw = R"({"data":{"Page":{"media":[]}}})";
  auto r = detail::classify_page(raw);
  REQUIRE(r.has_value());
  CHECK(r->entries.empty());
  CHECK_FALSE(r->has_next);
}

TEST_CASE("missing_page_is_exhausted") {
  const std::string_view raw = R"({"data":{}})";
  auto r = detail::classify_page(raw);
  REQUIRE(r.has_value());
  CHECK(r->entries.empty());
  CHECK_FALSE(r->has_next);
}

TEST_CASE("data_null_is_no_answer") {
  {
    const std::string_view raw = R"({"data":null})";
    auto r = detail::classify_page(raw);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().kind == ProviderError::Kind::Decode);
  }
  {
    const std::string_view raw = "not json";
    auto r = detail::classify_page(raw);
    CHECK_FALSE(r.has_value());
    CHECK(r.error().kind == ProviderError::Kind::Decode);
  }
}

// ===========================================================================
// search_body (anilist.rs search_body_binds_browse_page_size)
// ===========================================================================

TEST_CASE("search_body_binds_browse_page_size") {
  const std::string body = detail::search_body("frieren", 1);
  CHECK(body.find("\"search\":\"frieren\"") != std::string::npos);
  CHECK(body.find("\"perPage\":" + std::to_string(kSearchPageSize)) != std::string::npos);
  CHECK(body.find("\"page\":1") != std::string::npos);
}

TEST_CASE("search_query_requests_the_full_MEDIA_FIELDS set") {
  const std::string q = detail::search_query();
  // The full shared fieldset (P21, 06 §8b): by-id/search/discover never drift.
  for (const char* field :
       {"id", "idMal", "romaji", "english", "native", "episodes", "duration",
        "averageScore", "status", "season", "seasonYear", "startDate", "format",
        "source", "countryOfOrigin", "genres", "studios", "isMain", "rankings",
        "allTime", "nextAiringEpisode", "airingAt", "description", "coverImage",
        "large", "SEARCH_MATCH", "hasNextPage"}) {
    CAPTURE(field);
    CHECK(q.find(field) != std::string::npos);
  }
}

// ===========================================================================
// discover_body (P17, ported from anilist.rs discover_body_* tests). The
// omitted-vs-null variable rule (§1 R5) is the byte-exact serde-parity trap.
// ===========================================================================

// 2026-07-18: summer 2026 (the exact instant the Rust tests use).
constexpr std::int64_t kSummer2026 = 1'784'332'800;

TEST_CASE("discover_body omits season vars off This Season") {
  const std::string body = detail::discover_body(DiscoverAxis::Trending, 1, kSummer2026);
  const json j = json::parse(body);
  const json& vars = j.at("variables");
  // OMITTED, not null — the serde-parity trap: an explicit null would filter
  // season==null (06 §8b). The keys must be entirely absent.
  CHECK(!vars.contains("season"));
  CHECK(!vars.contains("seasonYear"));
  CHECK(vars.at("sort") == json::array({"TRENDING_DESC", "POPULARITY_DESC"}));
  CHECK(vars.at("perPage") == kDiscoverPageSize);
  CHECK(vars.at("page") == 1);
}

TEST_CASE("discover_body This Season binds the current cour") {
  const std::string body = detail::discover_body(DiscoverAxis::ThisSeason, 2, kSummer2026);
  const json j = json::parse(body);
  const json& vars = j.at("variables");
  CHECK(vars.at("season") == "SUMMER");
  CHECK(vars.at("seasonYear") == 2026);
  CHECK(vars.at("sort") == json::array({"POPULARITY_DESC", "ID_DESC"}));
  CHECK(vars.at("page") == 2);
}

TEST_CASE("discover_body sort keys match each axis") {
  auto sort_of = [](DiscoverAxis a) {
    return json::parse(detail::discover_body(a, 1, kSummer2026)).at("variables").at("sort");
  };
  CHECK(sort_of(DiscoverAxis::Trending) == json::array({"TRENDING_DESC", "POPULARITY_DESC"}));
  CHECK(sort_of(DiscoverAxis::Popular) == json::array({"POPULARITY_DESC", "ID_DESC"}));
  CHECK(sort_of(DiscoverAxis::TopRated) == json::array({"SCORE_DESC", "ID_DESC"}));
  CHECK(sort_of(DiscoverAxis::ThisSeason) == json::array({"POPULARITY_DESC", "ID_DESC"}));
}

TEST_CASE("discover_query requests the full MEDIA_FIELDS set + discover binds") {
  const std::string q = detail::discover_query();
  for (const char* field : {"MediaSort", "MediaSeason", "seasonYear", "hasNextPage",
                            "coverImage", "averageScore", "genres", "duration",
                            "startDate", "rankings", "nextAiringEpisode", "description",
                            "studios", "source", "countryOfOrigin"}) {
    CAPTURE(field);
    CHECK(q.find(field) != std::string::npos);
  }
  // discover NEVER uses the search-only SEARCH_MATCH sort constant.
  CHECK(q.find("SEARCH_MATCH") == std::string::npos);
}

// ===========================================================================
// P38 (§9 — no Rust precedent): discover_body filter vars. Same omitted-vs-
// null discipline as season/seasonYear above — every filter test below
// pins that an inactive field is ABSENT, never null.
// ===========================================================================

TEST_CASE("discover_query declares the P38 filter variables") {
  const std::string q = detail::discover_query();
  for (const char* field :
       {"$genre_in", "$status", "$averageScore_greater", "genre_in:$genre_in",
        "status:$status", "averageScore_greater:$averageScore_greater"}) {
    CAPTURE(field);
    CHECK(q.find(field) != std::string::npos);
  }
}

TEST_CASE("discover_body omits every filter var when filters is empty") {
  const std::string body =
      detail::discover_body(DiscoverAxis::Trending, 1, kSummer2026, DiscoverFilters{});
  const json vars = json::parse(body).at("variables");
  CHECK(!vars.contains("genre_in"));
  CHECK(!vars.contains("status"));
  CHECK(!vars.contains("averageScore_greater"));
  // Unchanged from the pre-P38 body (byte-for-byte the same call shape).
  CHECK(!vars.contains("season"));
  CHECK(!vars.contains("seasonYear"));
}

TEST_CASE("discover_body genre_in present only when genres is non-empty") {
  DiscoverFilters f;
  f.genres = {"Action", "Comedy"};
  const std::string body = detail::discover_body(DiscoverAxis::Trending, 1, kSummer2026, f);
  const json vars = json::parse(body).at("variables");
  CHECK(vars.at("genre_in") == json::array({"Action", "Comedy"}));
  CHECK(!vars.contains("status"));
  CHECK(!vars.contains("averageScore_greater"));
}

TEST_CASE("discover_body status present only when set") {
  DiscoverFilters f;
  f.status = "RELEASING";
  const std::string body = detail::discover_body(DiscoverAxis::Popular, 1, kSummer2026, f);
  const json vars = json::parse(body).at("variables");
  CHECK(vars.at("status") == "RELEASING");
  CHECK(!vars.contains("genre_in"));
  CHECK(!vars.contains("averageScore_greater"));
}

TEST_CASE("discover_body averageScore_greater present only when min_score is set") {
  DiscoverFilters f;
  f.min_score = 70;
  const std::string body = detail::discover_body(DiscoverAxis::TopRated, 1, kSummer2026, f);
  const json vars = json::parse(body).at("variables");
  CHECK(vars.at("averageScore_greater") == 70);
  CHECK(!vars.contains("genre_in"));
  CHECK(!vars.contains("status"));
}

TEST_CASE("discover_body year filter binds seasonYear off This Season") {
  DiscoverFilters f;
  f.year = 2019;
  const std::string body = detail::discover_body(DiscoverAxis::Trending, 1, kSummer2026, f);
  const json vars = json::parse(body).at("variables");
  CHECK(vars.at("seasonYear") == 2019);
  CHECK(!vars.contains("season"));  // season itself stays unbound off This Season.
}

TEST_CASE("discover_body year filter overrides This Season's own cour binding") {
  DiscoverFilters f;
  f.year = 2019;
  const std::string body = detail::discover_body(DiscoverAxis::ThisSeason, 1, kSummer2026, f);
  const json vars = json::parse(body).at("variables");
  // Last write wins: the explicit filter year replaces the cour's seasonYear,
  // but season (WINTER/SPRING/SUMMER/FALL) still reflects the live cour —
  // there is no independent season filter in P38.
  CHECK(vars.at("seasonYear") == 2019);
  CHECK(vars.at("season") == "SUMMER");
}

TEST_CASE("discover_body composes every filter dimension together") {
  DiscoverFilters f;
  f.genres = {"Fantasy"};
  f.year = 2024;
  f.status = "FINISHED";
  f.min_score = 80;
  const std::string body = detail::discover_body(DiscoverAxis::Popular, 3, kSummer2026, f);
  const json vars = json::parse(body).at("variables");
  CHECK(vars.at("genre_in") == json::array({"Fantasy"}));
  CHECK(vars.at("seasonYear") == 2024);
  CHECK(vars.at("status") == "FINISHED");
  CHECK(vars.at("averageScore_greater") == 80);
  CHECK(vars.at("page") == 3);
}

// ===========================================================================
// P38: genre_collection_query / genre_collection_body / classify_genre_
// collection — a bare, argument-free query (mirrors viewer_body's shape).
// ===========================================================================

TEST_CASE("genre_collection_query is argument-free") {
  const std::string q = detail::genre_collection_query();
  CHECK(q.find("GenreCollection") != std::string::npos);
  CHECK(q.find('$') == std::string::npos);  // no variables at all.
}

TEST_CASE("genre_collection_body carries no variables key") {
  const json j = json::parse(detail::genre_collection_body());
  CHECK(j.contains("query"));
  CHECK(!j.contains("variables"));
}

TEST_CASE("classify_genre_collection parses the flat genre list") {
  const std::string raw = R"({"data":{"GenreCollection":["Action","Comedy","Drama"]}})";
  const auto res = detail::classify_genre_collection(raw);
  REQUIRE(res.has_value());
  CHECK(*res == std::vector<std::string>{"Action", "Comedy", "Drama"});
}

TEST_CASE("classify_genre_collection: data null is Err (no answer)") {
  const std::string raw = R"({"data":null})";
  const auto res = detail::classify_genre_collection(raw);
  CHECK(!res.has_value());
}

TEST_CASE("classify_genre_collection: malformed JSON is Err") {
  const auto res = detail::classify_genre_collection("not json");
  CHECK(!res.has_value());
}

// ===========================================================================
// viewer_body / classify_viewer (P34 slice 2, 06 §4.2)
// ===========================================================================

// The push wire golden (P34 review): prove the CONVERTED number reaches the
// request body — a dropped to_anilist_score call or a typo'd variable name
// would otherwise pass the whole suite (only the domain-layer math is pinned
// elsewhere).
TEST_CASE("save_entry_body binds the converted wire score") {
  const std::string body = detail::save_entry_body(
      1007, ListStatus::Watching, 3, to_anilist_score(75, ScoreFormat::Point5));
  const json j = json::parse(body);
  CHECK(j.at("variables").at("mediaId") == 1007);
  CHECK(j.at("variables").at("status") == "CURRENT");
  CHECK(j.at("variables").at("progress") == 3);
  CHECK(j.at("variables").at("score") == 4.0);  // raw 75 on Point5 -> wire 4.
  CHECK(j.at("query").get<std::string>().find("score:$score") != std::string::npos);
}

// And the read half: the wire value converts back to the raw 0..=100 scale.
TEST_CASE("classify_entry converts the wire score back to raw") {
  const std::string body =
      R"({"data":{"Page":{"mediaList":[{"status":"CURRENT","progress":7,"score":4}]}}})";
  auto r = detail::classify_entry(body, ScoreFormat::Point5);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  CHECK((*r)->status == ListStatus::Watching);
  CHECK((*r)->progress == 7);
  CHECK((*r)->score == 80);  // Point5 wire 4 -> raw 80.
}

TEST_CASE("viewer_body requests id name and mediaListOptions.scoreFormat") {
  const std::string body = detail::viewer_body();
  CHECK(body.find("Viewer") != std::string::npos);
  CHECK(body.find("mediaListOptions") != std::string::npos);
  CHECK(body.find("scoreFormat") != std::string::npos);
}

TEST_CASE("classify_viewer maps scoreFormat to the domain enum") {
  const char* raw =
      R"({"data":{"Viewer":{"id":7,"name":"rod",)"
      R"("mediaListOptions":{"scoreFormat":"POINT_10_DECIMAL"}}}})";
  auto v = detail::classify_viewer(raw);
  REQUIRE(v.has_value());
  REQUIRE(v->has_value());
  CHECK((*v)->id == 7);
  CHECK((*v)->name == "rod");
  CHECK((*v)->score_format == ScoreFormat::Point10Decimal);
}

TEST_CASE("classify_viewer missing mediaListOptions defaults score_format to Point100") {
  const char* raw = R"({"data":{"Viewer":{"id":7,"name":"rod"}}})";
  auto v = detail::classify_viewer(raw);
  REQUIRE(v.has_value());
  REQUIRE(v->has_value());
  CHECK((*v)->score_format == ScoreFormat::Point100);
}

TEST_CASE("classify_viewer unrecognized scoreFormat string defaults to Point100") {
  const char* raw =
      R"({"data":{"Viewer":{"id":7,"name":"rod",)"
      R"("mediaListOptions":{"scoreFormat":"SOMETHING_NEW"}}}})";
  auto v = detail::classify_viewer(raw);
  REQUIRE(v.has_value());
  REQUIRE(v->has_value());
  CHECK((*v)->score_format == ScoreFormat::Point100);
}

// ===========================================================================
// search() over http::Client — HTTP-error path (empty result / error class).
// ===========================================================================

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <thread>

namespace {

// One-shot fixture server (same shape as http_tests.cpp's OneShotServer).
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

TEST_CASE("search_http_error_maps_to_provider_error") {
  // search() targets the fixed AniList endpoint, so the HTTP-error mapping is
  // exercised here by driving Client::fetch at the fixture server with the
  // same request shape search() builds internally.
  OneShotServer srv(response_with_body("503 Service Unavailable", "oops"));
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  http::Request req;
  req.method = http::Method::Post;
  req.url = srv.url();
  req.content_type = "application/json";
  const std::string body = detail::search_body("frieren", 1);
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;

  auto resp = client->fetch(req);
  REQUIRE_FALSE(resp.has_value());
  CHECK(resp.error().kind == ProviderError::Kind::Server);
  CHECK(resp.error().status == 503);
}

TEST_CASE("search_empty_result_is_not_an_error") {
  const std::string body = R"({"data":{"Page":{"pageInfo":{"hasNextPage":false},"media":[]}}})";
  OneShotServer srv(response_with_body("200 OK", body));
  auto client = http::Client::create();
  REQUIRE(client.has_value());

  http::Request req;
  req.method = http::Method::Post;
  req.url = srv.url();
  req.content_type = "application/json";
  const std::string reqbody = detail::search_body("zzz-no-such-show", 1);
  req.body.assign(reqbody.begin(), reqbody.end());
  req.accept = http::Accept::Any2xx;

  auto resp = client->fetch(req);
  REQUIRE(resp.has_value());
  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  auto page = detail::classify_page(raw);
  REQUIRE(page.has_value());
  CHECK(page->entries.empty());
  CHECK_FALSE(page->has_next);
}

// --- MediaKind (MANGA_PLAN MG-5) ---------------------------------------------
//
// The fence first: every builder the manga side parameterized must emit the
// exact same document it emitted before when nobody passes a kind. That is
// what keeps the anime app's wire bytes — and every golden above — unchanged.

TEST_CASE("the default kind is Anime at every parameterized builder") {
  CHECK(detail::media_fields() == detail::media_fields(MediaKind::Anime));
  CHECK(detail::search_query() == detail::search_query(MediaKind::Anime));
  CHECK(detail::discover_query() == detail::discover_query(MediaKind::Anime));
  CHECK(detail::by_id_query() == detail::by_id_query(MediaKind::Anime));
  CHECK(detail::characters_recs_query() == detail::characters_recs_query(MediaKind::Anime));
  CHECK(detail::search_body("frieren", 1) ==
        detail::search_body("frieren", 1, MediaKind::Anime));
  CHECK(detail::by_id_body(154587) == detail::by_id_body(154587, MediaKind::Anime));
  CHECK(detail::discover_body(DiscoverAxis::Trending, 1, 0) ==
        detail::discover_body(DiscoverAxis::Trending, 1, 0, DiscoverFilters{},
                              MediaKind::Anime));
  CHECK(detail::list_collection_body(7) == detail::list_collection_body(7, MediaKind::Anime));
  CHECK(detail::list_entry_body(7, 42) == detail::list_entry_body(7, 42, MediaKind::Anime));
}

TEST_CASE("every anime document still says type:ANIME and carries no manga fields") {
  for (const std::string& doc :
       {detail::search_query(), detail::discover_query(), detail::by_id_query(),
        detail::characters_recs_query()}) {
    CHECK(doc.find("type:ANIME") != std::string::npos);
    CHECK(doc.find("type:MANGA") == std::string::npos);
    CHECK(doc.find("chapters") == std::string::npos);
    CHECK(doc.find("volumes") == std::string::npos);
  }
  CHECK(detail::list_collection_body(7).find("type:ANIME") != std::string::npos);
  CHECK(detail::list_entry_body(7, 42).find("type:ANIME") != std::string::npos);
}

TEST_CASE("the manga fieldset is the anime one plus chapters and volumes") {
  const std::string anime = detail::media_fields(MediaKind::Anime);
  const std::string manga = detail::media_fields(MediaKind::Manga);
  CHECK(manga == anime + " chapters volumes");
  CHECK(manga.rfind(anime, 0) == 0);  // a strict extension, never a rewrite.
}

TEST_CASE("manga builders emit type:MANGA and the widened fieldset") {
  const std::string manga_fields = detail::media_fields(MediaKind::Manga);
  for (const std::string& doc :
       {detail::search_query(MediaKind::Manga), detail::discover_query(MediaKind::Manga),
        detail::by_id_query(MediaKind::Manga),
        detail::characters_recs_query(MediaKind::Manga)}) {
    CHECK(doc.find("type:MANGA") != std::string::npos);
    CHECK(doc.find("type:ANIME") == std::string::npos);
    CHECK(doc.find(manga_fields) != std::string::npos);
  }
}

TEST_CASE("manga list bodies scope both the collection and the single entry") {
  const json coll = json::parse(detail::list_collection_body(7, MediaKind::Manga));
  const std::string coll_q = coll.at("query").get<std::string>();
  CHECK(coll_q.find("MediaListCollection(userId:$userId,type:MANGA)") != std::string::npos);
  CHECK(coll.at("variables").at("userId") == 7);

  const json one = json::parse(detail::list_entry_body(7, 42, MediaKind::Manga));
  const std::string one_q = one.at("query").get<std::string>();
  // The Page(perPage:1) form survives the widening: the push guard's whole
  // absence/failure distinction rests on it (classify_entry's invariant).
  CHECK(one_q.find("Page(perPage:1)") != std::string::npos);
  CHECK(one_q.find("mediaList(userId:$userId,mediaId:$mediaId,type:MANGA)") !=
        std::string::npos);
  CHECK(one.at("variables").at("mediaId") == 42);
}

TEST_CASE("the save mutation is type-free: the media id picks the list") {
  // No MediaKind parameter exists on save_entry_body by design — widening it
  // would imply the mutation takes a type it does not have.
  const json body = json::parse(detail::save_entry_body(42, ListStatus::Watching, 10, 0.0));
  const std::string q = body.at("query").get<std::string>();
  CHECK(q.find("type:") == std::string::npos);
  CHECK(body.at("variables").at("progress") == 10);
}

TEST_CASE("cross-type recommendations are dropped against the queried kind") {
  const std::string raw = R"({"data":{"Media":{"characters":{"edges":[]},
    "recommendations":{"nodes":[
      {"mediaRecommendation":{"type":"MANGA","id":1,"title":{"romaji":"a manga"}}},
      {"mediaRecommendation":{"type":"ANIME","id":2,"title":{"romaji":"an anime"}}}
    ]}}}})";
  auto anime = detail::classify_characters_recs(raw, MediaKind::Anime);
  REQUIRE(anime.has_value());
  REQUIRE(anime->has_value());
  REQUIRE((*anime)->recommendations.size() == 1);
  CHECK((*anime)->recommendations[0].title_romaji == "an anime");

  auto manga = detail::classify_characters_recs(raw, MediaKind::Manga);
  REQUIRE(manga.has_value());
  REQUIRE(manga->has_value());
  REQUIRE((*manga)->recommendations.size() == 1);
  CHECK((*manga)->recommendations[0].title_romaji == "a manga");
}
