// manga_mangadex_tests.cpp — golden tests for src/manga/mangadex.hpp/.cpp.
// Fully offline: URL builders exact-string, the three parsers over
// tests/fixtures/mangadex/{search,feed,at_home}.json (fixtures captured from
// live responses) plus synthetic edge cases, chapter_less / dedupe_chapters
// / next_offset pinned-rule cases. No fixture server (CMakeLists.txt: the
// transport loop's math is tested pure via detail::next_offset).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/manga/mangadex.hpp"

using namespace shigoku;
using namespace shigoku::manga;
using namespace shigoku::manga::detail;

namespace {

std::string read_fixture(const char* name) {
  const std::string path = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/" + name;
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "missing fixture: " << path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

MdChapter mk_chapter(const char* id, const char* chapter, const char* publish_at) {
  MdChapter c;
  c.id = id;
  c.chapter = chapter;
  c.publish_at = publish_at;
  return c;
}

}  // namespace

// ===========================================================================
// url_encode
// ===========================================================================

TEST_CASE("url_encode_space_percent20_unreserved_passthrough_plus_encoded") {
  CHECK(url_encode(" ") == "%20");
  CHECK(url_encode("chainsaw man") == "chainsaw%20man");
  // Unreserved set passes through untouched.
  CHECK(url_encode("AZaz09._~-") == "AZaz09._~-");
  // '+' is NOT the space escape here — always %XX, never form-encoding.
  CHECK(url_encode("a+b") == "a%2Bb");
  // Multi-byte UTF-8: each byte of U+65E5 ("日", e6 97 a5) escaped separately.
  CHECK(url_encode("\xe6\x97\xa5") == "%E6%97%A5");
}

// ===========================================================================
// URL builders (exact-string)
// ===========================================================================

TEST_CASE("search_url_exact_string") {
  CHECK(search_url(kApiHost, "chainsaw man", 25) ==
        "https://api.mangadex.org/manga?title=chainsaw%20man&limit=25&includes%5B%5D=cover_art");
  CHECK(search_url(kApiHost, "", 1) ==
        "https://api.mangadex.org/manga?title=&limit=1&includes%5B%5D=cover_art");
}

TEST_CASE("feed_url_exact_string_limit_and_offset") {
  CHECK(feed_url(kApiHost, "a77742b1-befd-49a4-bff5-1ad4e6b0ef7b", "en", 500, 0) ==
        "https://api.mangadex.org/manga/a77742b1-befd-49a4-bff5-1ad4e6b0ef7b/feed"
        "?translatedLanguage%5B%5D=en&order%5Bchapter%5D=asc&limit=500&offset=0");
  CHECK(feed_url(kApiHost, "a77742b1-befd-49a4-bff5-1ad4e6b0ef7b", "en", 20, 40) ==
        "https://api.mangadex.org/manga/a77742b1-befd-49a4-bff5-1ad4e6b0ef7b/feed"
        "?translatedLanguage%5B%5D=en&order%5Bchapter%5D=asc&limit=20&offset=40");
}

TEST_CASE("at_home_url_exact_string") {
  CHECK(at_home_url(kApiHost, "73af4d8d-1532-4a72-b1b9-8f4e5cd295c9") ==
        "https://api.mangadex.org/at-home/server/73af4d8d-1532-4a72-b1b9-8f4e5cd295c9");
}

TEST_CASE("cover_url_appends_512_thumb_suffix_to_a_filename_that_already_ends_jpg") {
  CHECK(cover_url(kUploadsHost, "a77742b1-befd-49a4-bff5-1ad4e6b0ef7b",
                  "6e518bd1-5f60-446b-8832-bfe6bf74834b.jpg") ==
        "https://uploads.mangadex.org/covers/a77742b1-befd-49a4-bff5-1ad4e6b0ef7b/"
        "6e518bd1-5f60-446b-8832-bfe6bf74834b.jpg.512.jpg");
}

TEST_CASE("page_url_data_and_data_saver_variants") {
  MdAtHome ah;
  ah.base_url = "https://cmdxd98sb0x3yprd.mangadex.network";
  ah.hash = "d828fa5fffd26b264ad400b3b0fdffe8";
  ah.data = {"A1-612f24d412cc157e7221bd8a051d5d564adcd539931b8c0bd58b691c07bf8c90.jpg",
            "A2-x.jpg"};
  ah.data_saver = {"A1-bf40bc4a7040ee983de15871349aef33f5314d96134e99c5d017384ff7ee316e.jpg",
                   "A2-y.jpg"};
  CHECK(page_url(ah, 0, /*data_saver=*/false) ==
        "https://cmdxd98sb0x3yprd.mangadex.network/data/d828fa5fffd26b264ad400b3b0fdffe8/"
        "A1-612f24d412cc157e7221bd8a051d5d564adcd539931b8c0bd58b691c07bf8c90.jpg");
  CHECK(page_url(ah, 0, /*data_saver=*/true) ==
        "https://cmdxd98sb0x3yprd.mangadex.network/data-saver/d828fa5fffd26b264ad400b3b0fdffe8/"
        "A1-bf40bc4a7040ee983de15871349aef33f5314d96134e99c5d017384ff7ee316e.jpg");
  CHECK(page_url(ah, 1, /*data_saver=*/false) ==
        "https://cmdxd98sb0x3yprd.mangadex.network/data/d828fa5fffd26b264ad400b3b0fdffe8/A2-x.jpg");
  // Out-of-range index never crashes: "" (implementation choice, not spec-pinned).
  CHECK(page_url(ah, 99, false).empty());
}

// ===========================================================================
// uuid_shaped / filename_safe
// ===========================================================================

TEST_CASE("uuid_shaped_positive_and_negative") {
  CHECK(uuid_shaped("a77742b1-befd-49a4-bff5-1ad4e6b0ef7b"));
  CHECK_FALSE(uuid_shaped(""));
  CHECK_FALSE(uuid_shaped("a77742b1-befd-49a4-bff5-1ad4e6b0ef7"));   // 35 chars.
  CHECK_FALSE(uuid_shaped("a77742b1-befd-49a4-bff5-1ad4e6b0ef7bb")); // 37 chars.
  CHECK_FALSE(uuid_shaped("A77742B1-befd-49a4-bff5-1ad4e6b0ef7b"));  // uppercase hex.
  CHECK_FALSE(uuid_shaped("a77742b1_befd_49a4_bff5_1ad4e6b0ef7b"));  // wrong separator.
  CHECK_FALSE(uuid_shaped("a77742b1-befd49a4-bff5-1ad4e6b0ef7bx"));  // dash in the wrong slot.
  CHECK_FALSE(uuid_shaped("g77742b1-befd-49a4-bff5-1ad4e6b0ef7b"));  // 'g' not hex.
}

TEST_CASE("filename_safe_positive_and_negative") {
  CHECK(filename_safe("6e518bd1-5f60-446b-8832-bfe6bf74834b.jpg"));
  CHECK(filename_safe("A1-x_y.9.png"));
  CHECK_FALSE(filename_safe(""));
  CHECK_FALSE(filename_safe(".hidden"));           // leading dot.
  CHECK_FALSE(filename_safe("../evil.jpg"));       // ".." run.
  CHECK_FALSE(filename_safe("a/b.jpg"));           // '/' not in the safe set.
  CHECK_FALSE(filename_safe("bad file.jpg"));      // space not in the safe set.
  CHECK_FALSE(filename_safe("a..b.jpg"));          // ".." run mid-string too.
}

// ===========================================================================
// scoped_id
// ===========================================================================

TEST_CASE("scoped_id_prefixes_md_colon") {
  CHECK(scoped_id("abc") == "md:abc");
  CHECK(scoped_id("a77742b1-befd-49a4-bff5-1ad4e6b0ef7b") ==
        "md:a77742b1-befd-49a4-bff5-1ad4e6b0ef7b");
}

// ===========================================================================
// parse_search — fixture (live capture) + synthetic edge cases.
// ===========================================================================

TEST_CASE("parse_search_over_the_fixture_pins_the_chainsaw_man_facts") {
  const auto raw = read_fixture("mangadex/search.json");
  auto got = parse_search(raw);
  REQUIRE(got.has_value());
  REQUIRE(got->size() == 5);

  const MdManga& m0 = (*got)[0];
  CHECK(m0.id == "a77742b1-befd-49a4-bff5-1ad4e6b0ef7b");
  // NO title.en on this row — arrives via the ja-ro fallback (the pinned case).
  CHECK(m0.title == "Chainsaw Man");
  CHECK(m0.year == std::optional<int>(2018));
  CHECK(m0.status == "completed");
  CHECK(m0.al_id == std::optional<std::int64_t>(105778));
  CHECK(m0.mal_id == std::optional<std::int64_t>(116778));
  CHECK(m0.cover_filename == "6e518bd1-5f60-446b-8832-bfe6bf74834b.jpg");
  CHECK(m0.description.rfind("Broke young man", 0) == 0);

  // Row 1: title.en IS present ("Chainsaw Man (Official Colored)") and links.al/
  // links.mal are absent on this row -> nullopt (fixture-derived).
  const MdManga& m1 = (*got)[1];
  CHECK(m1.id == "e896c48c-3150-437d-ba57-d8567eb399ae");
  CHECK(m1.title == "Chainsaw Man (Official Colored)");
  CHECK_FALSE(m1.al_id.has_value());
  CHECK_FALSE(m1.mal_id.has_value());

  // Rows 3/4: description object has no "en" key -> "" (fixture-derived).
  CHECK((*got)[3].description.empty());
  CHECK((*got)[4].description.empty());
}

TEST_CASE("parse_search_synthetic_bad_uuid_dropped_bad_cover_blanked_link_edge_cases") {
  const char* raw = R"({"data":[
    {"id":"not-a-uuid-at-all","type":"manga","attributes":{"title":{"en":"Ghost"}}},
    {
      "id":"11111111-1111-1111-1111-111111111111",
      "type":"manga",
      "attributes":{
        "title":{"en":"English Wins","ja-ro":"Romaji Loses"},
        "status":"ongoing",
        "links":{"al":"12a","mal":999}
      },
      "relationships":[{"id":"x","type":"cover_art","attributes":{"fileName":"../evil.jpg"}}]
    },
    {
      "id":"22222222-2222-2222-2222-222222222222",
      "type":"manga",
      "attributes":{"title":{"fr":"Only French"},"links":{}}
    }
  ]})";
  auto got = parse_search(raw);
  REQUIRE(got.has_value());
  // The bad-uuid row is dropped entirely, not defaulted.
  REQUIRE(got->size() == 2);

  const MdManga& a = (*got)[0];
  CHECK(a.id == "11111111-1111-1111-1111-111111111111");
  CHECK(a.title == "English Wins");  // en beats ja-ro when both present.
  // "12a" fails the all-digits rule; 999 is a JSON number, not a string —
  // both malformed link shapes degrade to nullopt, never fail the row.
  CHECK_FALSE(a.al_id.has_value());
  CHECK_FALSE(a.mal_id.has_value());
  // fileName contains ".." -> fails filename_safe -> blanked, not passed through.
  CHECK(a.cover_filename.empty());

  const MdManga& b = (*got)[1];
  CHECK(b.id == "22222222-2222-2222-2222-222222222222");
  CHECK(b.title == "Only French");  // neither en nor ja-ro -> first entry.
  CHECK_FALSE(b.al_id.has_value());  // links present but empty -> nullopt.
  CHECK_FALSE(b.mal_id.has_value());
  CHECK(b.cover_filename.empty());  // no relationships[] at all.
}

TEST_CASE("parse_search_malformed_json_never_crashes") {
  CHECK_FALSE(parse_search("not json").has_value());
  CHECK(parse_search("not json").error().kind == ProviderError::Kind::Decode);
  CHECK_FALSE(parse_search("{}").has_value());
  CHECK(parse_search("{}").error().kind == ProviderError::Kind::Decode);
}

// ===========================================================================
// parse_feed — fixture (live capture, limit=20) + synthetic edge cases.
// ===========================================================================

TEST_CASE("parse_feed_over_the_fixture_drops_externals_and_pins_the_survivor_set") {
  const auto raw = read_fixture("mangadex/feed.json");
  auto got = parse_feed(raw);
  REQUIRE(got.has_value());
  CHECK(got->total == 107);
  // 20 raw rows, 6 external (viz.com, pages==0) dropped -> 14 survive.
  // (fixture-derived via python3, see the launching prompt's live-capture notes)
  REQUIRE(got->chapters.size() == 14);

  const MdChapter& first = got->chapters.front();
  CHECK(first.id == "73af4d8d-1532-4a72-b1b9-8f4e5cd295c9");
  CHECK(first.chapter == "1");
  CHECK(first.pages == 53);
  CHECK(first.lang == "en");
  CHECK(first.publish_at == "2020-06-30T00:05:51+00:00");
  CHECK(first.title == "A Dog and a Chainsaw");

  const MdChapter& last = got->chapters.back();
  CHECK(last.id == "9aad14f4-2d71-4311-a7bb-455ba422e1cf");
  CHECK(last.chapter == "14");
  CHECK(last.pages == 19);

  // The un-deduped, filter-preserving order already lands 1..14 in this
  // fixture (fixture-derived): every kept row's raw position is monotonic in
  // chapter number even though externals are interleaved.
  for (std::size_t i = 0; i < got->chapters.size(); ++i) {
    CHECK(got->chapters[i].chapter == std::to_string(i + 1));
  }
}

TEST_CASE("parse_feed_synthetic_unavailable_and_zero_pages_dropped_null_fields_blanked") {
  const char* raw = R"({"total":4,"data":[
    {"id":"33333333-3333-3333-3333-333333333333","type":"chapter",
     "attributes":{"chapter":null,"title":null,"translatedLanguage":"en",
                   "publishAt":"2024-01-01T00:00:00+00:00","pages":5,
                   "externalUrl":null,"isUnavailable":false}},
    {"id":"44444444-4444-4444-4444-444444444444","type":"chapter",
     "attributes":{"chapter":"2","title":"Unavailable","translatedLanguage":"en",
                   "publishAt":"2024-01-02T00:00:00+00:00","pages":10,
                   "externalUrl":null,"isUnavailable":true}},
    {"id":"55555555-5555-5555-5555-555555555555","type":"chapter",
     "attributes":{"chapter":"3","title":"Zero Pages","translatedLanguage":"en",
                   "publishAt":"2024-01-03T00:00:00+00:00","pages":0,
                   "externalUrl":null,"isUnavailable":false}},
    {"id":"not-a-uuid","type":"chapter","attributes":{"chapter":"9","pages":5}}
  ]})";
  auto got = parse_feed(raw);
  REQUIRE(got.has_value());
  CHECK(got->total == 4);
  // isUnavailable=true, pages==0, and the bad-uuid row all drop; only the
  // null-chapter/null-title survivor remains.
  REQUIRE(got->chapters.size() == 1);
  CHECK(got->chapters[0].id == "33333333-3333-3333-3333-333333333333");
  CHECK(got->chapters[0].chapter.empty());
  CHECK(got->chapters[0].title.empty());
  CHECK(got->chapters[0].pages == 5);
}

TEST_CASE("parse_feed_malformed_json_never_crashes") {
  CHECK_FALSE(parse_feed("not json").has_value());
  CHECK(parse_feed("not json").error().kind == ProviderError::Kind::Decode);
  CHECK_FALSE(parse_feed("{}").has_value());
  CHECK(parse_feed("{}").error().kind == ProviderError::Kind::Decode);
}

// ===========================================================================
// parse_at_home — fixture + synthetic edge cases.
// ===========================================================================

TEST_CASE("parse_at_home_over_the_fixture_pins_base_url_hash_and_page_counts") {
  const auto raw = read_fixture("mangadex/at_home.json");
  auto got = parse_at_home(raw);
  REQUIRE(got.has_value());
  CHECK(got->base_url == "https://cmdxd98sb0x3yprd.mangadex.network");
  CHECK(got->hash == "d828fa5fffd26b264ad400b3b0fdffe8");
  REQUIRE(got->data.size() == 53);
  REQUIRE(got->data_saver.size() == 53);
  CHECK(got->data[0] ==
        "A1-612f24d412cc157e7221bd8a051d5d564adcd539931b8c0bd58b691c07bf8c90.jpg");
}

TEST_CASE("parse_at_home_synthetic_bad_filename_fails_the_whole_parse") {
  const char* raw = R"({"baseUrl":"https://node.example",
    "chapter":{"hash":"abc123","data":["ok1.jpg","../evil.jpg"],"dataSaver":["ok1.jpg"]}})";
  auto got = parse_at_home(raw);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_at_home_empty_data_is_decode") {
  const char* raw = R"({"baseUrl":"https://node.example",
    "chapter":{"hash":"abc123","data":[],"dataSaver":[]}})";
  auto got = parse_at_home(raw);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_at_home_malformed_json_never_crashes") {
  CHECK_FALSE(parse_at_home("not json").has_value());
  CHECK(parse_at_home("not json").error().kind == ProviderError::Kind::Decode);
  CHECK_FALSE(parse_at_home("{}").has_value());
  CHECK(parse_at_home("{}").error().kind == ProviderError::Kind::Decode);
}

// ===========================================================================
// chapter_less
// ===========================================================================

TEST_CASE("chapter_less_numeric_aware_ordering_law") {
  CHECK(chapter_less("2", "10"));
  CHECK_FALSE(chapter_less("10", "2"));

  CHECK(chapter_less("9", "9.5"));
  CHECK(chapter_less("9.5", "10"));
  CHECK(chapter_less("9", "10"));

  // Numeric sorts strictly before non-numeric.
  CHECK(chapter_less("10", "extra"));
  CHECK_FALSE(chapter_less("extra", "10"));

  // "" is non-numeric: a numeric always beats it, and among non-numerics it
  // is lexicographically smallest (the empty-prefix rule).
  CHECK(chapter_less("1", ""));
  CHECK_FALSE(chapter_less("", "1"));
  CHECK(chapter_less("", "extra"));
  CHECK_FALSE(chapter_less("extra", ""));

  // Equal strings are never less (irreflexive).
  CHECK_FALSE(chapter_less("5", "5"));
  CHECK_FALSE(chapter_less("", ""));

  // Distinct numeric values, one a prefix of the other's digits.
  CHECK(chapter_less("1", "1.5"));
  CHECK_FALSE(chapter_less("1.5", "1"));

  // Numeric tie (equal value, different string) -> lexicographic tie-break.
  CHECK(chapter_less("1", "1.0"));    // "1" is a strict prefix of "1.0".
  CHECK_FALSE(chapter_less("1.0", "1"));

  // Both non-numeric -> plain lexicographic.
  CHECK(chapter_less("extra", "extra2"));
  CHECK_FALSE(chapter_less("extra2", "extra"));
}

// ===========================================================================
// dedupe_chapters
// ===========================================================================

TEST_CASE("dedupe_chapters_keeps_newest_publish_at_ties_keep_first_seen_output_sorted") {
  std::vector<MdChapter> in = {
      mk_chapter("aaaaaaaa-0000-0000-0000-000000000000", "5", "2020-01-01T00:00:00+00:00"),
      mk_chapter("bbbbbbbb-0000-0000-0000-000000000000", "5", "2021-01-01T00:00:00+00:00"),
      // Ties the current incumbent (b) exactly -> b (first-seen at that
      // publish_at) keeps its slot, c is dropped.
      mk_chapter("cccccccc-0000-0000-0000-000000000000", "5", "2021-01-01T00:00:00+00:00"),
      mk_chapter("dddddddd-0000-0000-0000-000000000000", "10", "2020-06-01T00:00:00+00:00"),
      mk_chapter("eeeeeeee-0000-0000-0000-000000000000", "2", "2020-06-01T00:00:00+00:00"),
  };
  const auto out = dedupe_chapters(in);
  REQUIRE(out.size() == 3);
  // Sorted by chapter_less: "2" < "5" < "10" (numeric, not lexicographic).
  CHECK(out[0].chapter == "2");
  CHECK(out[1].chapter == "5");
  CHECK(out[1].id == "bbbbbbbb-0000-0000-0000-000000000000");
  CHECK(out[2].chapter == "10");
}

TEST_CASE("dedupe_chapters_exact_chapter_string_match_only") {
  // "1" and "1.0" are distinct chapter strings — never merged even though
  // chapter_less treats their numeric value as a tie.
  std::vector<MdChapter> in = {
      mk_chapter("11111111-0000-0000-0000-000000000000", "1", "2020-01-01T00:00:00+00:00"),
      mk_chapter("22222222-0000-0000-0000-000000000000", "1.0", "2020-01-02T00:00:00+00:00"),
  };
  const auto out = dedupe_chapters(in);
  REQUIRE(out.size() == 2);
  CHECK(out[0].chapter == "1");
  CHECK(out[1].chapter == "1.0");
}

TEST_CASE("dedupe_chapters_over_the_fixture_parsed_feed_is_a_stable_reorder") {
  const auto raw = read_fixture("mangadex/feed.json");
  auto page = parse_feed(raw);
  REQUIRE(page.has_value());
  // The fixture's 14 survivors already have unique chapter strings, so
  // dedupe is a pure sort here (fixture-derived).
  const auto out = dedupe_chapters(page->chapters);
  REQUIRE(out.size() == 14);
  for (std::size_t i = 0; i < out.size(); ++i) {
    CHECK(out[i].chapter == std::to_string(i + 1));
  }
  CHECK(out.front().id == "73af4d8d-1532-4a72-b1b9-8f4e5cd295c9");
  CHECK(out.back().id == "9aad14f4-2d71-4311-a7bb-455ba422e1cf");
}

// ===========================================================================
// next_offset
// ===========================================================================

TEST_CASE("next_offset_mid_loop_advance") {
  CHECK(next_offset(0, 500, 1200, 1) == std::optional<std::uint32_t>(500));
  CHECK(next_offset(500, 500, 1200, 2) == std::optional<std::uint32_t>(1000));
}

TEST_CASE("next_offset_exact_boundary_exhaustion") {
  // offset + limit == total -> exhausted, not one more page.
  CHECK_FALSE(next_offset(500, 500, 1000, 2).has_value());
}

TEST_CASE("next_offset_total_less_than_limit_is_immediate_nullopt") {
  // Mirrors the real feed_url production limit (500) against the fixture's
  // real total (107): a single page already exhausts it.
  CHECK_FALSE(next_offset(0, 500, 107, 1).has_value());
}

TEST_CASE("next_offset_flood_cap_at_kMaxFeedPages") {
  // Under the cap, still advancing regardless of how large total claims to be.
  CHECK(next_offset(9000, 500, 1000000, 19) == std::optional<std::uint32_t>(9500));
  // At the cap: nullopt even though offset+limit is nowhere near total.
  CHECK_FALSE(next_offset(9500, 500, 1000000, kMaxFeedPages).has_value());
  CHECK_FALSE(next_offset(9500, 500, 1000000, kMaxFeedPages + 1).has_value());
}

// ===========================================================================
// Transport surface reachable offline (no network: MangaDex::create() only
// builds an http::Client; cover_thumb_url is pure computation).
// ===========================================================================

TEST_CASE("cover_thumb_url_absolute_or_blank_without_a_cover") {
  auto p = MangaDex::create();
  REQUIRE(p.has_value());
  MdManga with_cover;
  with_cover.id = "a77742b1-befd-49a4-bff5-1ad4e6b0ef7b";
  with_cover.cover_filename = "6e518bd1-5f60-446b-8832-bfe6bf74834b.jpg";
  CHECK(p->cover_thumb_url(with_cover) ==
        "https://uploads.mangadex.org/covers/a77742b1-befd-49a4-bff5-1ad4e6b0ef7b/"
        "6e518bd1-5f60-446b-8832-bfe6bf74834b.jpg.512.jpg");

  MdManga no_cover;
  no_cover.id = "a77742b1-befd-49a4-bff5-1ad4e6b0ef7b";
  CHECK(p->cover_thumb_url(no_cover).empty());
}
