// mal_catalog_tests.cpp — golden tests for the MAL catalog: URL builders,
// the list/detail parsers and every field-mapper branch, the client-side
// filter table, and the offline id map. No fixture files (JSON inline), no
// network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../src/idmap.hpp"
#include "../src/mal_catalog.hpp"

using namespace shigoku;
using namespace shigoku::mal_catalog;
using namespace shigoku::mal_catalog::detail;

namespace {

// A bridge that makes the mapping visible: every id doubles.
std::int64_t doubling_bridge(std::int64_t mal_id) { return mal_id * 2; }

const char* kNode = R"({
  "id": 52991,
  "title": "Sousou no Frieren",
  "main_picture": {"medium": "https://cdn.myanimelist.net/images/anime/1015/138006.jpg",
                   "large": "https://cdn.myanimelist.net/images/anime/1015/138006l.jpg"},
  "alternative_titles": {"synonyms": ["Frieren at the Funeral"], "en": "Frieren: Beyond Journey's End",
                         "ja": "葬送のフリーレン"},
  "start_date": "2023-09-29",
  "synopsis": "During their decade-long quest to defeat the Demon King,\n\nthe members of the hero's party...\n\n[Written by MAL Rewrite]",
  "mean": 9.31,
  "rank": 1,
  "popularity": 156,
  "num_episodes": 28,
  "start_season": {"year": 2023, "season": "fall"},
  "source": "manga",
  "average_episode_duration": 1470,
  "studios": [{"id": 11, "name": "Madhouse"}],
  "genres": [{"id": 2, "name": "Adventure"}, {"id": 8, "name": "Drama"}, {"id": 10, "name": "Fantasy"},
             {"id": 82, "name": "Shounen"}],
  "media_type": "tv",
  "status": "finished_airing",
  "recommendations": [
    {"node": {"id": 21, "title": "One Piece",
              "main_picture": {"medium": "https://cdn.myanimelist.net/images/anime/6/73245.jpg"}},
     "num_recommendations": 12},
    {"node": {"title": "no id here"}, "num_recommendations": 1}
  ]
})";

}  // namespace

// ===========================================================================
// URL builders
// ===========================================================================

TEST_CASE("search_url encodes the query and pages by kSearchPageSize") {
  const std::string u1 = search_url("sousou no frieren", 1);
  CHECK(u1.rfind("https://api.myanimelist.net/v2/anime?q=sousou%20no%20frieren&limit=26&offset=0", 0) == 0);
  CHECK(u1.find("&nsfw=true") != std::string::npos);
  CHECK(u1.find("&fields=id,title,main_picture") != std::string::npos);
  const std::string u3 = search_url("x", 3);
  CHECK(u3.find("&offset=52") != std::string::npos);
  // page 0 is treated as page 1.
  CHECK(search_url("x", 0).find("&offset=0") != std::string::npos);
}

TEST_CASE("percent_encode leaves unreserved bytes and encodes the rest") {
  CHECK(percent_encode("abc-_.~09") == "abc-_.~09");
  CHECK(percent_encode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
}

TEST_CASE("query_too_short counts codepoints, not bytes") {
  CHECK(query_too_short(""));
  CHECK(query_too_short("ab"));
  CHECK_FALSE(query_too_short("abc"));
  CHECK(query_too_short("葬送"));       // 2 codepoints, 6 bytes
  CHECK_FALSE(query_too_short("葬送の"));  // 3 codepoints
}

TEST_CASE("ranking_type per axis; This Season has none") {
  CHECK(ranking_type(DiscoverAxis::Trending) == "airing");
  CHECK(ranking_type(DiscoverAxis::Popular) == "bypopularity");
  CHECK(ranking_type(DiscoverAxis::TopRated) == "all");
  CHECK_FALSE(ranking_type(DiscoverAxis::ThisSeason).has_value());
}

TEST_CASE("discover_url: ranking endpoints page by limit, This Season uses the cour") {
  const std::string trending = discover_url(DiscoverAxis::Trending, 2, 0, kDiscoverPageSize);
  CHECK(trending.rfind("https://api.myanimelist.net/v2/anime/ranking?ranking_type=airing&limit=20&offset=20", 0) == 0);
  const std::string filtered = discover_url(DiscoverAxis::Popular, 2, 0, kFilteredPageSize);
  CHECK(filtered.find("ranking_type=bypopularity&limit=100&offset=100") != std::string::npos);
  // 2023-10-15 UTC -> Fall 2023.
  const std::string season = discover_url(DiscoverAxis::ThisSeason, 1, 1697328000, kDiscoverPageSize);
  CHECK(season.rfind("https://api.myanimelist.net/v2/anime/season/2023/fall?sort=anime_num_list_users&limit=20&offset=0", 0) == 0);
  // December rolls into next-year Winter (current_cour): 2023-12-20.
  const std::string dec = discover_url(DiscoverAxis::ThisSeason, 1, 1703030400, kDiscoverPageSize);
  CHECK(dec.find("/anime/season/2024/winter?") != std::string::npos);
}

TEST_CASE("by_id_url") {
  CHECK(by_id_url(52991).rfind("https://api.myanimelist.net/v2/anime/52991?fields=id,title", 0) == 0);
}

// ===========================================================================
// parse_detail + the field mapper
// ===========================================================================

namespace {
const char* kListPage = R"({
  "data": [
    {"node": {"id": 52991, "title": "Sousou no Frieren",
              "main_picture": {"medium": "https://cdn.myanimelist.net/images/anime/1015/138006.jpg"},
              "num_episodes": 28, "status": "finished_airing"},
     "list_status": {"status": "completed", "score": 9, "num_episodes_watched": 28,
                     "is_rewatching": false, "updated_at": "2024-05-01T12:34:56+00:00"}},
    {"node": {"id": 21, "title": "One Piece"},
     "list_status": {"status": "watching", "score": 0, "num_episodes_watched": 1100}},
    {"node": {"title": "no id here"}, "list_status": {"status": "dropped"}}
  ],
  "paging": {"next": "https://api.myanimelist.net/v2/users/@me/animelist?offset=1000&limit=1000"}
})";
}  // namespace

TEST_CASE("parse_user_list_page maps rows, bridges ids, drops id-less nodes, keeps the link") {
  auto p = parse_user_list_page(kListPage, doubling_bridge);
  REQUIRE(p.has_value());
  REQUIRE(p->entries.size() == 2);
  const UserListEntry& a = p->entries[0];
  CHECK(a.seed.mal_id == 52991);
  CHECK(a.seed.anilist_id == 52991 * 2);
  CHECK(a.seed.title_romaji == "Sousou no Frieren");
  CHECK(a.seed.total_episodes == 28);
  CHECK(a.status == ListStatus::Completed);
  CHECK(a.progress == 28);
  CHECK(a.score == 9);
  CHECK(a.updated_at == 1714566896);
  const UserListEntry& b = p->entries[1];
  CHECK(b.status == ListStatus::Watching);
  CHECK(b.progress == 1100);
  CHECK(b.score == 0);
  CHECK(b.updated_at == 0);
  CHECK(p->next == "https://api.myanimelist.net/v2/users/@me/animelist?offset=1000&limit=1000");
}

TEST_CASE("parse_user_list_page: a last page carries no link; garbage is a decode error") {
  auto last = parse_user_list_page(R"({"data": [], "paging": {}})", doubling_bridge);
  REQUIRE(last.has_value());
  CHECK(last->entries.empty());
  CHECK(!last->next.has_value());
  auto bad = parse_user_list_page("nope", doubling_bridge);
  REQUIRE(!bad.has_value());
  CHECK(bad.error().kind == ProviderError::Kind::Decode);
  auto wrong_shape = parse_user_list_page("[]", doubling_bridge);
  REQUIRE(!wrong_shape.has_value());
  CHECK(wrong_shape.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_iso8601_utc: Z and offset forms; anything else is 0") {
  CHECK(parse_iso8601_utc("2024-05-01T12:34:56Z") == 1714566896);
  CHECK(parse_iso8601_utc("2024-05-01T12:34:56+00:00") == 1714566896);
  CHECK(parse_iso8601_utc("2024-05-01T12:34:56+09:00") == 1714566896 - 9 * 3600);
  CHECK(parse_iso8601_utc("2024-05-01T12:34:56-02:30") == 1714566896 + 2 * 3600 + 30 * 60);
  CHECK(parse_iso8601_utc("1970-01-01T00:00:00Z") == 0);
  CHECK(parse_iso8601_utc("2024-05-01") == 0);
  CHECK(parse_iso8601_utc("2024-13-01T00:00:00Z") == 0);
  CHECK(parse_iso8601_utc("2024-05-01T12:34:56") == 0);
  CHECK(parse_iso8601_utc("") == 0);
}

TEST_CASE("user_list_url asks for the list status and seed fields, a full page, nsfw included") {
  const std::string url = user_list_url();
  CHECK(url.rfind(std::string(kApiBase) + "/users/@me/animelist?", 0) == 0);
  CHECK(url.find("fields=list_status") != std::string::npos);
  CHECK(url.find("limit=1000") != std::string::npos);
  CHECK(url.find("nsfw=true") != std::string::npos);
}

TEST_CASE("parse_detail maps every field of a full node") {
  auto d = parse_detail(kNode, 0, doubling_bridge);
  REQUIRE(d.has_value());
  const Enrichment& e = d->show;
  CHECK(e.mal_id == 52991);
  CHECK(e.anilist_id == 52991 * 2);  // the bridge, since keep_anilist_id was 0
  CHECK(e.title_romaji == "Sousou no Frieren");
  CHECK(e.title_english == "Frieren: Beyond Journey's End");
  CHECK(e.title_native == "葬送のフリーレン");
  CHECK(e.cover_url == "https://cdn.myanimelist.net/images/anime/1015/138006l.jpg");
  CHECK(e.total_episodes == 28);
  CHECK(e.duration_minutes == 25);  // 1470 s -> 24.5 -> 25
  CHECK(e.year == 2023);
  CHECK(e.season == Season::Fall);
  REQUIRE(e.start_date.has_value());
  CHECK(e.start_date->year == 2023);
  CHECK(e.start_date->month == 9);
  CHECK(e.start_date->day == 29);
  CHECK(e.status == "FINISHED");
  CHECK(e.kind == "TV");
  CHECK(e.source_material == "MANGA");
  CHECK(e.score == 93);
  CHECK(e.rank == 1);
  CHECK(e.rank_type == "RATED");
  CHECK_FALSE(e.rank_year.has_value());
  CHECK(e.genres == std::vector<std::string>{"Adventure", "Drama", "Fantasy", "Shounen"});
  CHECK(e.studios == std::vector<std::string>{"Madhouse"});
  CHECK(e.description ==
        "During their decade-long quest to defeat the Demon King, the members of the hero's party...");
  CHECK_FALSE(e.next_airing_at.has_value());
  CHECK_FALSE(e.country.has_value());

  // Recommendations: sparse nodes, bridged; the id-less one is dropped.
  REQUIRE(d->recommendations.size() == 1);
  CHECK(d->recommendations[0].mal_id == 21);
  CHECK(d->recommendations[0].anilist_id == 42);
  CHECK(d->recommendations[0].title_romaji == "One Piece");
  CHECK(d->recommendations[0].cover_url == "https://cdn.myanimelist.net/images/anime/6/73245.jpg");
}

TEST_CASE("parse_detail pins keep_anilist_id over the bridge") {
  auto d = parse_detail(kNode, 154587, doubling_bridge);
  REQUIRE(d.has_value());
  CHECK(d->show.anilist_id == 154587);
  CHECK(d->show.mal_id == 52991);
  // Recommendations are not pinned — they are other shows.
  REQUIRE(d->recommendations.size() == 1);
  CHECK(d->recommendations[0].anilist_id == 42);
}

TEST_CASE("parse_detail: sparse node degrades field by field") {
  const char* raw = R"({"id": 7, "title": "Bare", "num_episodes": 0, "average_episode_duration": 0,
                        "alternative_titles": {"en": "", "ja": null}, "main_picture": {"medium": "m.jpg"},
                        "mean": 0, "rank": 0, "popularity": 300, "media_type": "unknown",
                        "start_date": "1998", "status": "currently_airing"})";
  auto d = parse_detail(raw, 0, doubling_bridge);
  REQUIRE(d.has_value());
  const Enrichment& e = d->show;
  CHECK(e.title_romaji == "Bare");
  CHECK_FALSE(e.title_english.has_value());  // blank -> absent
  CHECK_FALSE(e.title_native.has_value());
  CHECK(e.cover_url == "m.jpg");             // medium fallback
  CHECK_FALSE(e.total_episodes.has_value());  // 0 -> unknown
  CHECK_FALSE(e.duration_minutes.has_value());
  CHECK_FALSE(e.score.has_value());           // mean 0 -> unscored
  CHECK(e.rank == 300);                       // popularity fallback
  CHECK(e.rank_type == "POPULAR");
  CHECK_FALSE(e.kind.has_value());            // unknown -> absent
  CHECK(e.year == 1998);                      // from start_date, no start_season
  REQUIRE(e.start_date.has_value());
  CHECK_FALSE(e.start_date->month.has_value());
  CHECK(e.status == "RELEASING");
  CHECK_FALSE(e.description.has_value());
  CHECK(e.genres.empty());
  CHECK(d->recommendations.empty());
}

TEST_CASE("parse_detail refuses a body without an id, and non-JSON") {
  CHECK_FALSE(parse_detail(R"({"title":"x"})", 0, doubling_bridge).has_value());
  CHECK(parse_detail(R"({"title":"x"})", 0, doubling_bridge).error().kind ==
        ProviderError::Kind::Decode);
  CHECK_FALSE(parse_detail("<html>", 0, doubling_bridge).has_value());
  CHECK_FALSE(parse_detail("[1,2]", 0, doubling_bridge).has_value());
}

// ===========================================================================
// parse_page
// ===========================================================================

TEST_CASE("parse_page: search/ranking list shapes, paging.next -> has_next") {
  const char* raw = R"({"data": [
      {"node": {"id": 52991, "title": "Sousou no Frieren"}, "ranking": {"rank": 1}},
      {"node": {"id": 5114, "title": "Fullmetal Alchemist: Brotherhood"}, "ranking": {"rank": 2}},
      {"node": {"title": "no id"}},
      {"junk": true}
    ], "paging": {"next": "https://api.myanimelist.net/v2/anime/ranking?offset=20"}})";
  auto p = parse_page(raw, doubling_bridge);
  REQUIRE(p.has_value());
  REQUIRE(p->entries.size() == 2);
  CHECK(p->entries[0].mal_id == 52991);
  CHECK(p->entries[0].anilist_id == 52991 * 2);
  CHECK(p->entries[1].title_romaji == "Fullmetal Alchemist: Brotherhood");
  CHECK(p->has_next);

  auto last = parse_page(R"({"data": [{"node": {"id": 1, "title": "x"}}], "paging": {}})",
                         doubling_bridge);
  REQUIRE(last.has_value());
  CHECK_FALSE(last->has_next);
  auto nopaging = parse_page(R"({"data": []})", doubling_bridge);
  REQUIRE(nopaging.has_value());
  CHECK(nopaging->entries.empty());
  CHECK_FALSE(nopaging->has_next);
}

TEST_CASE("parse_page: a body without the list shape is Decode, never an empty page") {
  CHECK(parse_page(R"({"error":"forbidden"})", doubling_bridge).error().kind ==
        ProviderError::Kind::Decode);
  CHECK(parse_page(R"({"data": {"node": {}}})", doubling_bridge).error().kind ==
        ProviderError::Kind::Decode);
  CHECK(parse_page("not json", doubling_bridge).error().kind == ProviderError::Kind::Decode);
}

// ===========================================================================
// field mappers
// ===========================================================================

TEST_CASE("parse_date accepts Y, Y-M, Y-M-D and refuses the rest") {
  auto d = parse_date("2023-09-29");
  REQUIRE(d.has_value());
  CHECK(d->year == 2023);
  CHECK(d->month == 9);
  CHECK(d->day == 29);
  auto ym = parse_date("2023-09");
  REQUIRE(ym.has_value());
  CHECK(ym->month == 9);
  CHECK_FALSE(ym->day.has_value());
  auto y = parse_date("2023");
  REQUIRE(y.has_value());
  CHECK_FALSE(y->month.has_value());
  CHECK_FALSE(parse_date("").has_value());
  CHECK_FALSE(parse_date("0000").has_value());
  CHECK_FALSE(parse_date("2023-13").has_value());
  CHECK_FALSE(parse_date("2023-09-32").has_value());
  CHECK_FALSE(parse_date("2023-9-1").has_value());
  CHECK_FALSE(parse_date("2023-").has_value());
  CHECK_FALSE(parse_date("2023-09-29-1").has_value());
  CHECK_FALSE(parse_date("abcd").has_value());
}

TEST_CASE("map_status / map_media_type / map_source") {
  CHECK(map_status("currently_airing") == "RELEASING");
  CHECK(map_status("finished_airing") == "FINISHED");
  CHECK(map_status("not_yet_aired") == "NOT_YET_RELEASED");
  CHECK(map_status("something_new") == "SOMETHING_NEW");
  CHECK_FALSE(map_status("").has_value());

  CHECK(map_media_type("tv") == "TV");
  CHECK(map_media_type("movie") == "MOVIE");
  CHECK(map_media_type("ova") == "OVA");
  CHECK(map_media_type("ona") == "ONA");
  CHECK(map_media_type("special") == "SPECIAL");
  CHECK(map_media_type("tv_special") == "SPECIAL");
  CHECK(map_media_type("music") == "MUSIC");
  CHECK_FALSE(map_media_type("unknown").has_value());
  CHECK_FALSE(map_media_type("").has_value());

  CHECK(map_source("manga") == "MANGA");
  CHECK(map_source("web_manga") == "MANGA");
  CHECK(map_source("4_koma_manga") == "MANGA");
  CHECK(map_source("light_novel") == "LIGHT_NOVEL");
  CHECK(map_source("visual_novel") == "VISUAL_NOVEL");
  CHECK(map_source("game") == "VIDEO_GAME");
  CHECK(map_source("mixed_media") == "MULTIMEDIA_PROJECT");
  CHECK(map_source("original") == "ORIGINAL");
  CHECK_FALSE(map_source("").has_value());
}

TEST_CASE("score_from_mean rounds ×10 into AniList's 0..100; minutes_from_seconds rounds") {
  CHECK(score_from_mean(9.31) == 93);
  CHECK(score_from_mean(8.25) == 83);
  CHECK(score_from_mean(10.0) == 100);
  CHECK(score_from_mean(0.04) == 0);
  CHECK_FALSE(score_from_mean(0.0).has_value());
  CHECK_FALSE(score_from_mean(-1.0).has_value());
  CHECK_FALSE(score_from_mean(11.0).has_value());

  CHECK(minutes_from_seconds(1470) == 25);
  CHECK(minutes_from_seconds(1440) == 24);
  CHECK(minutes_from_seconds(89) == 1);
  CHECK(minutes_from_seconds(30) == 1);
  CHECK_FALSE(minutes_from_seconds(29).has_value());  // rounds to 0 -> unknown
  CHECK_FALSE(minutes_from_seconds(0).has_value());
  CHECK_FALSE(minutes_from_seconds(-5).has_value());
}

TEST_CASE("clean_synopsis drops the MAL trailer, collapses whitespace, strips controls") {
  CHECK(clean_synopsis("A.\n\nB.\t C.\n\n[Written by MAL Rewrite]") == "A. B. C.");
  CHECK(clean_synopsis("  only spaces  ") == "only spaces");
  CHECK_FALSE(clean_synopsis("[Written by MAL Rewrite]").has_value());
  CHECK_FALSE(clean_synopsis("").has_value());
  CHECK(clean_synopsis("esc\x1b[31m red") == "esc[31m red");
}

// ===========================================================================
// filters
// ===========================================================================

TEST_CASE("passes_filters: genres are all-of with the Thriller/Suspense fold") {
  Enrichment e;
  e.genres = {"Action", "Suspense"};
  e.year = 2021;
  e.status = "FINISHED";
  e.score = 80;
  DiscoverFilters f;
  CHECK(passes_filters(e, f));  // empty filters pass everything
  f.genres = {"Action"};
  CHECK(passes_filters(e, f));
  f.genres = {"Action", "Thriller"};
  CHECK(passes_filters(e, f));  // Thriller matches MAL's Suspense
  f.genres = {"Action", "Romance"};
  CHECK_FALSE(passes_filters(e, f));  // all-of, not any-of
}

TEST_CASE("passes_filters: year, status, min_score (strictly greater)") {
  Enrichment e;
  e.year = 2021;
  e.status = "FINISHED";
  e.score = 80;
  DiscoverFilters f;
  f.year = 2021;
  CHECK(passes_filters(e, f));
  f.year = 2020;
  CHECK_FALSE(passes_filters(e, f));
  f = {};
  f.status = "FINISHED";
  CHECK(passes_filters(e, f));
  f.status = "RELEASING";
  CHECK_FALSE(passes_filters(e, f));
  f = {};
  f.min_score = 79;
  CHECK(passes_filters(e, f));
  f.min_score = 80;
  CHECK_FALSE(passes_filters(e, f));  // averageScore_greater is strict
  Enrichment bare;
  CHECK_FALSE(passes_filters(bare, f));  // unknown score never passes a score filter
  f = {};
  f.year = 2021;
  CHECK_FALSE(passes_filters(bare, f));
}

TEST_CASE("genre_vocabulary is the AniList list the overlay speaks") {
  const auto v = genre_vocabulary();
  CHECK(v.size() == 19);
  CHECK(std::find(v.begin(), v.end(), "Thriller") != v.end());
  CHECK(std::find(v.begin(), v.end(), "Slice of Life") != v.end());
}

// ===========================================================================
// client id + bridge
// ===========================================================================

TEST_CASE("effective_client_id: override wins, else the baked id") {
  CHECK(effective_client_id("abc123") == "abc123");
  CHECK(effective_client_id("") == std::string(kClientId));
}

TEST_CASE("default_bridge: table hit -> real AniList id, miss -> -mal_id") {
  CHECK(default_bridge(1) == 1);        // Cowboy Bebop is 1 on both sites
  CHECK(default_bridge(20) == 20);      // Naruto
  CHECK(default_bridge(52991) == 154587);  // Sousou no Frieren
  CHECK(default_bridge(4000000) == -4000000);
}

TEST_CASE("search on a too-short query answers an empty page without a request") {
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  auto p = search(*client, "whatever-id", "ab", 1, doubling_bridge);
  REQUIRE(p.has_value());
  CHECK(p->entries.empty());
  CHECK_FALSE(p->has_next);
}

TEST_CASE("an empty client id refuses every call as Forbidden, without a request") {
  auto client = http::Client::create();
  REQUIRE(client.has_value());
  auto s = search(*client, "", "frieren", 1, doubling_bridge);
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().kind == ProviderError::Kind::Forbidden);
  auto d = discover(*client, "", DiscoverAxis::Popular, 1, 0, DiscoverFilters{}, doubling_bridge);
  REQUIRE_FALSE(d.has_value());
  CHECK(d.error().kind == ProviderError::Kind::Forbidden);
  auto b = by_id(*client, "", 1, 0, doubling_bridge);
  REQUIRE_FALSE(b.has_value());
  CHECK(b.error().kind == ProviderError::Kind::Forbidden);
}

// ===========================================================================
// idmap
// ===========================================================================

TEST_CASE("idmap: known pairs both ways, misses, and the table is sorted+unique") {
  CHECK(idmap::to_anilist(1) == 1);
  CHECK(idmap::to_mal(1) == 1);
  CHECK(idmap::to_anilist(52991) == 154587);
  CHECK(idmap::to_mal(154587) == 52991);
  CHECK_FALSE(idmap::to_anilist(0).has_value());
  CHECK_FALSE(idmap::to_anilist(-1).has_value());
  CHECK_FALSE(idmap::to_anilist(4000000).has_value());
  CHECK_FALSE(idmap::to_anilist(0x1FFFFFFFFLL).has_value());
  CHECK_FALSE(idmap::to_mal(0).has_value());
  CHECK_FALSE(idmap::to_mal(-154587).has_value());
  CHECK(idmap::size() > 15000);
  CHECK(std::string(idmap::release()) == "2026-27");

  for (std::size_t i = 1; i < idmap::data::kCount; ++i) {
    REQUIRE(idmap::data::kByMal[2 * i] > idmap::data::kByMal[2 * (i - 1)]);
  }
  // Round-trip a sample across the table.
  for (std::size_t i = 0; i < idmap::data::kCount; i += 997) {
    const std::int64_t m = idmap::data::kByMal[2 * i];
    const std::int64_t a = idmap::data::kByMal[2 * i + 1];
    CHECK(idmap::to_anilist(m) == a);
    CHECK(idmap::to_mal(a) == m);
  }
}
