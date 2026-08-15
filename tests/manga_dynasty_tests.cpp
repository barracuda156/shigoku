// manga_dynasty_tests.cpp — golden tests for src/manga/dynasty.hpp/.cpp.
// Fully offline (the manga_weebcentral_tests shape): URL builders
// exact-string, the two JSON parsers + the search HTML parser over
// tests/fixtures/dynasty/ (fixtures captured from live responses, re-probed
// with zero drift) plus synthetic edge cases, the slug vet, the
// permalink→chapter-number rules, and the source attrs + scoped-id round-trip.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/manga/dynasty.hpp"

using namespace shigoku;
using namespace shigoku::manga;
using namespace shigoku::manga::dy;

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
// slug_shaped (the parse-time trust boundary)
// ===========================================================================

TEST_CASE("slug_shaped_accepts_permalinks_and_refuses_path_metacharacters") {
  CHECK(slug_shaped("kase_san"));
  CHECK(slug_shaped("kase_san_ch17_2"));
  CHECK(slug_shaped("yamada_to_kase_san"));
  CHECK(slug_shaped("k-on-2"));
  CHECK_FALSE(slug_shaped(""));
  CHECK_FALSE(slug_shaped("kase san"));          // space.
  CHECK_FALSE(slug_shaped("kase/san"));          // path separator.
  CHECK_FALSE(slug_shaped("../etc/passwd"));     // traversal.
  CHECK_FALSE(slug_shaped(".hidden"));           // leading dot.
  CHECK_FALSE(slug_shaped("kase_san?x=1"));      // query smuggling.
  CHECK_FALSE(slug_shaped(std::string(kDySlugMax + 1, 'a')));
  CHECK(slug_shaped(std::string(kDySlugMax, 'a')));
}

// ===========================================================================
// URL builders (exact-string, the probe pins)
// ===========================================================================

TEST_CASE("search_series_and_chapter_urls_exact_string") {
  // The classes[] filter is the difference between series rows and a page of
  // chapter/author links (probe pin), and rides pre-encoded.
  CHECK(search_url(kDyHost, "kase san") ==
        "https://dynasty-scans.com/search?q=kase%20san&classes%5B%5D=Series");
  CHECK(search_url(kDyHost, "") ==
        "https://dynasty-scans.com/search?q=&classes%5B%5D=Series");
  CHECK(series_url(kDyHost, "kase_san") ==
        "https://dynasty-scans.com/series/kase_san.json");
  CHECK(chapter_url(kDyHost, "kase_san_ch01") ==
        "https://dynasty-scans.com/chapters/kase_san_ch01.json");
  CHECK(series_url("http://127.0.0.1:8080", "x") ==
        "http://127.0.0.1:8080/series/x.json");
}

// ===========================================================================
// chapter_number / chapter_title
// ===========================================================================

TEST_CASE("chapter_number_permalink_wins_over_title") {
  // The permalink wins. Dynasty reuses one displayed number
  // across release variants, so the title alone would collide.
  CHECK(chapter_number("kase_san_ch01", "Chapter 1: Morning Glory") == "1");
  CHECK(chapter_number("kase_san_ch5_5", "Chapter 5.5: Chocolate") == "5.5");
  CHECK(chapter_number("kase_san_ch17", "Chapter 17: Crepe (Part 1)") == "17");
  CHECK(chapter_number("kase_san_ch17_1", "Chapter 17: Crepe (Part 2)") == "17.1");
  CHECK(chapter_number("kase_san_ch17_2", "Chapter 17: Crepe (Part 3)") == "17.2");
  // Leading zeros never survive: chapter_less breaks value ties on the full
  // string, so "06" and "6" must not be two different chapters.
  CHECK(chapter_number("kase_san_ch06_1", "Chapter 6: Bento") == "6.1");
  CHECK(chapter_number("x_ch000", "") == "0");
}

TEST_CASE("chapter_number_falls_back_to_title_then_oneshot") {
  // No "_ch" run at all: the extras.
  CHECK(chapter_number("kase_san_rival_and_kase_san", "Rival and Kase-san").empty());
  CHECK(chapter_number("kase_san_rival_and_kase_san", "Chapter 42: Rival") == "42");
  // A tail that is not a clean number run is refused, title tried next.
  CHECK(chapter_number("some_ch01v2", "Chapter 1: X") == "1");
  CHECK(chapter_number("some_ch01_", "Chapter 1: X") == "1");
  CHECK(chapter_number("some_chocolate", "Chapter 3.5: X") == "3.5");
  CHECK(chapter_number("some_chocolate", "Chapter: X").empty());
  CHECK(chapter_number("some_chocolate", "Extra: X").empty());
  CHECK(chapter_number("", "").empty());
  // A slug whose series name contains "_ch": only the LAST run is read.
  CHECK(chapter_number("witch_hunt_ch12", "") == "12");
}

TEST_CASE("chapter_title_drops_the_redundant_chapter_prefix") {
  CHECK(chapter_title("Chapter 1: Morning Glory and Kase-san") ==
        "Morning Glory and Kase-san");
  CHECK(chapter_title("Chapter 17: Crepe (Part 2)  (Magazine ver.)") ==
        "Crepe (Part 2)  (Magazine ver.)");
  CHECK(chapter_title("Rival and Kase-san") == "Rival and Kase-san");
  CHECK(chapter_title("Chapter 5 no colon") == "Chapter 5 no colon");
  CHECK(chapter_title("  padded  ") == "padded");
  CHECK(chapter_title("").empty());
}

// ===========================================================================
// parse_search (the one HTML surface)
// ===========================================================================

TEST_CASE("parse_search_fixture_two_series_rows") {
  const auto r = parse_search(read_fixture("dynasty/search_kase_series.html"));
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 2);
  CHECK((*r)[0].id == "kase_san");
  CHECK((*r)[0].title == "Kase-san");
  CHECK((*r)[1].id == "yamada_to_kase_san");
  CHECK((*r)[1].title == "Yamada to Kase-san");
  // Header pins: the search page carries none of these.
  CHECK_FALSE((*r)[0].year.has_value());
  CHECK((*r)[0].status.empty());
  CHECK((*r)[0].description.empty());
  CHECK((*r)[0].cover_filename.empty());
  CHECK_FALSE((*r)[0].al_id.has_value());
  CHECK_FALSE((*r)[0].mal_id.has_value());
}

TEST_CASE("parse_search_entities_dupes_and_bad_slugs") {
  const std::string html =
      "<li><a href=\"/series\">Directory</a></li>"          // nav: no slug.
      "<dd><a href=\"/series/fish_and_chips\" class=\"name\">Fish &amp; Chips</a>"
      " by <a href=\"/authors/x\">X</a></dd>"
      "<dd><a href=\"/series/its_mine\" class=\"name\">It&#39;s Mine</a></dd>"
      "<dd><a href=\"/series/fish_and_chips\" class=\"name\">Dupe</a></dd>"
      "<dd><a href=\"/series/bad slug\" class=\"name\">Dropped</a></dd>"
      "<dd><a href=\"/chapters/some_ch01\" class=\"name\">Not a series</a></dd>";
  const auto r = parse_search(html);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 2);
  CHECK((*r)[0].id == "fish_and_chips");
  CHECK((*r)[0].title == "Fish & Chips");  // Rails escapes; the UI must not.
  CHECK((*r)[1].id == "its_mine");
  CHECK((*r)[1].title == "It's Mine");
}

TEST_CASE("parse_search_no_results_is_ok_empty") {
  const auto empty = parse_search("");
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
  const auto alien = parse_search("<html><body>nothing here</body></html>");
  REQUIRE(alien.has_value());
  CHECK(alien->empty());
}

// ===========================================================================
// parse_series
// ===========================================================================

TEST_CASE("parse_series_fixture_30_chapters_in_reading_order") {
  const auto r = parse_series(read_fixture("dynasty/series_kase_san.json"));
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 30);  // 4 volume headers + the null one all skipped.

  const MdChapter& first = r->front();
  CHECK(first.id == "kase_san_ch01");
  CHECK(first.chapter == "1");
  CHECK(first.title == "Morning Glory and Kase-san");
  CHECK(first.lang == "en");
  CHECK(first.pages == 0);
  CHECK(first.publish_at == "2012-06-08");

  // The variant run: three rows Dynasty displays as "Chapter 17", kept
  // distinct by the permalink (md's dedupe_chapters would eat two of them).
  const std::vector<std::string> want_head = {"1",  "2",    "3",    "4",  "5",
                                              "5.5", "6.1", "7",    "8",  "9",
                                              "10.1", "11", "12",   "13", "14",
                                              "15", "16.1", "17", "17.1", "17.2",
                                              "18", "18.1", "21", "22", "23", "24"};
  for (std::size_t i = 0; i < want_head.size(); ++i) {
    CHECK((*r)[i].chapter == want_head[i]);
  }
  CHECK((*r)[17].id == "kase_san_ch17");
  CHECK((*r)[18].id == "kase_san_ch17_1");
  CHECK((*r)[19].id == "kase_san_ch17_2");

  // The unnumbered extras trail the numbered run, ordered by release date.
  CHECK((*r)[26].id == "kase_san_sandy_beach_and_kase_san_magazine_version");
  CHECK((*r)[26].chapter.empty());
  CHECK((*r)[26].title == "Sandy Beach and Kase-san (Magazine version)");
  CHECK((*r)[27].id == "kase_san_christmas_and_kase_san");
  CHECK((*r)[28].id == "kase_san_rival_and_kase_san");
  CHECK((*r)[29].id == "kase_san_going_to_tokyo_and_kase_san");
  CHECK((*r)[29].publish_at == "2018-02-03");
}

TEST_CASE("parse_series_skips_headers_and_bad_slugs_degrades_fields") {
  const std::string js = R"({"name":"X","taggings":[
    {"header":"Volume 1"},
    {"header":null},
    {"title":"Chapter 2: Two","permalink":"x_ch02","released_on":"2020-01-02"},
    {"title":"Bad","permalink":"x/../etc","released_on":"2020-01-03"},
    {"title":null,"permalink":"x_ch01"},
    "not an object",
    {"released_on":"2020-01-04"}
  ]})";
  const auto r = parse_series(js);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 2);
  CHECK((*r)[0].id == "x_ch01");
  CHECK((*r)[0].chapter == "1");
  CHECK((*r)[0].title.empty());
  CHECK((*r)[0].publish_at.empty());  // absent field degrades, never drops.
  CHECK((*r)[1].id == "x_ch02");
  CHECK((*r)[1].title == "Two");
}

TEST_CASE("parse_series_invalid_shapes_are_decode") {
  CHECK_FALSE(parse_series("not json").has_value());
  CHECK(parse_series("not json").error().kind == ProviderError::Kind::Decode);
  CHECK_FALSE(parse_series(R"({"name":"X"})").has_value());
  CHECK_FALSE(parse_series(R"({"taggings":"nope"})").has_value());
  // A series with no chapters is a legit answer, not an error.
  const auto empty = parse_series(R"({"taggings":[]})");
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

// ===========================================================================
// parse_chapter (pages)
// ===========================================================================

TEST_CASE("parse_chapter_fixture_27_webp_pages_joined_to_host") {
  const auto r =
      parse_chapter(read_fixture("dynasty/chapter_kase_san_ch01.json"), kDyHost);
  REQUIRE(r.has_value());
  REQUIRE(r->units.size() == 27);
  CHECK(r->units.front().url ==
        "https://dynasty-scans.com/system/releases/000/004/288/hirari_2.webp");
  CHECK(r->units.front().name_hint == "hirari_2.webp");
  CHECK(r->units.front().referer == kDyHost);
  CHECK(r->units[2].url ==
        "https://dynasty-scans.com/system/releases/000/004/288/kase-san_ch01_01.webp");
  CHECK(r->units.back().name_hint == "kase-san_ch01_credits.webp");
}

TEST_CASE("parse_chapter_host_is_injectable_and_absolute_urls_pass_through") {
  const std::string js = R"({"pages":[
    {"name":"01","url":"/system/releases/a.webp"},
    {"name":"02","url":"https://cdn.example/b.webp?v=2"}
  ]})";
  const auto r = parse_chapter(js, "http://127.0.0.1:9000");
  REQUIRE(r.has_value());
  REQUIRE(r->units.size() == 2);
  CHECK(r->units[0].url == "http://127.0.0.1:9000/system/releases/a.webp");
  CHECK(r->units[0].referer == "http://127.0.0.1:9000");
  CHECK(r->units[1].url == "https://cdn.example/b.webp?v=2");
  CHECK(r->units[1].name_hint == "b.webp");  // the query never names the file.
}

TEST_CASE("parse_chapter_malformed_rows_fail_the_whole_chapter") {
  // Unlike a scraped <img> list, every pages[] row IS a page — skipping one
  // would leave a hole, so the whole chapter fails (the parse_at_home law).
  const auto unsafe = parse_chapter(R"({"pages":[{"url":"/x/..evil"}]})", kDyHost);
  REQUIRE_FALSE(unsafe.has_value());
  CHECK(unsafe.error().kind == ProviderError::Kind::Decode);
  CHECK_FALSE(parse_chapter(R"({"pages":[{"name":"01"}]})", kDyHost).has_value());
  CHECK_FALSE(parse_chapter(R"({"pages":[{"url":"relative.webp"}]})", kDyHost)
                  .has_value());
  CHECK_FALSE(parse_chapter(R"({"pages":[]})", kDyHost).has_value());
  CHECK_FALSE(parse_chapter(R"({"title":"x"})", kDyHost).has_value());
  CHECK_FALSE(parse_chapter("not json", kDyHost).has_value());
}

// ===========================================================================
// Source attrs + id scoping + the transport's id re-vet
// ===========================================================================

TEST_CASE("dynasty_attrs_scoped_id_and_no_cover") {
  auto dyn = Dynasty::create();
  REQUIRE(dyn.has_value());
  CHECK(dyn->key() == "dy");
  CHECK(dyn->name() == "Dynasty Reader");
  CHECK_FALSE(dyn->nsfw());
  CHECK(scoped_id(*dyn, "kase_san") == "dy:kase_san");
  MdManga m;
  m.id = "kase_san";
  CHECK(dyn->cover_thumb_url(m).empty());  // no cover source in v1.
}

TEST_CASE("dynasty_transport_revets_ids_before_they_shape_a_path") {
  // Ids may later arrive from persisted store space rather than a fresh
  // parse; a corrupt one must never reach the network (these fail before any
  // fetch, so the test stays offline).
  auto dyn = Dynasty::create();
  REQUIRE(dyn.has_value());
  const auto ch = dyn->chapters("../secrets", "en");
  REQUIRE_FALSE(ch.has_value());
  CHECK(ch.error().kind == ProviderError::Kind::Decode);
  const auto pg = dyn->pages("kase san", false);
  REQUIRE_FALSE(pg.has_value());
  CHECK(pg.error().kind == ProviderError::Kind::Decode);
}
