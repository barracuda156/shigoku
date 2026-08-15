// manga_weebcentral_tests.cpp — golden tests for src/manga/weebcentral.hpp/
// .cpp. Fully offline (the manga_mangadex_tests shape): URL builders
// exact-string, the three fragment parsers over tests/fixtures/weebcentral/
// (fixtures captured from live responses) plus synthetic edge cases, the
// ULID vet / entity unescape / challenge sniff, and the source attrs +
// scoped-id round-trip.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/manga/weebcentral.hpp"

using namespace shigoku;
using namespace shigoku::manga;
using namespace shigoku::manga::wc;

namespace {

std::string read_fixture(const char* name) {
  const std::string path = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/" + name;
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "missing fixture: " << path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

constexpr const char* kAynikUlid = "01J76XY9Y7BB76XK8YYBCFYKVF";

}  // namespace

// ===========================================================================
// ulid_shaped (the parse-time trust boundary)
// ===========================================================================

TEST_CASE("ulid_shaped_crockford_base32_26_chars_only") {
  CHECK(ulid_shaped(kAynikUlid));
  CHECK(ulid_shaped("01J76XYY670GZSEJQG395N25HM"));
  CHECK_FALSE(ulid_shaped(""));
  CHECK_FALSE(ulid_shaped("01J76XY9Y7BB76XK8YYBCFYKV"));    // 25 chars.
  CHECK_FALSE(ulid_shaped("01J76XY9Y7BB76XK8YYBCFYKVFX"));  // 27 chars.
  CHECK_FALSE(ulid_shaped("01j76xy9y7bb76xk8yybcfykvf"));   // lowercase.
  // The four letters Crockford base32 excludes.
  CHECK_FALSE(ulid_shaped("I1J76XY9Y7BB76XK8YYBCFYKVF"));
  CHECK_FALSE(ulid_shaped("L1J76XY9Y7BB76XK8YYBCFYKVF"));
  CHECK_FALSE(ulid_shaped("O1J76XY9Y7BB76XK8YYBCFYKVF"));
  CHECK_FALSE(ulid_shaped("U1J76XY9Y7BB76XK8YYBCFYKVF"));
  // Path metacharacters can never pass.
  CHECK_FALSE(ulid_shaped("01J76XY9Y7BB76XK8YYBCFYK/F"));
  CHECK_FALSE(ulid_shaped("../76XY9Y7BB76XK8YYBCFYKVF"));
}

// ===========================================================================
// html_unescape
// ===========================================================================

TEST_CASE("html_unescape_named_numeric_and_passthrough") {
  CHECK(html_unescape("Kaguya-sama &amp; Friends") == "Kaguya-sama & Friends");
  CHECK(html_unescape("&lt;b&gt;&quot;x&quot;&apos;") == "<b>\"x\"'");
  CHECK(html_unescape("it&#39;s") == "it's");
  CHECK(html_unescape("it&#x27;s") == "it's");
  // Multi-byte codepoint: U+65E5 (日) as decimal.
  CHECK(html_unescape("&#26085;") == "\xE6\x97\xA5");
  // Unknown / malformed entities pass through verbatim.
  CHECK(html_unescape("a &unknown; b") == "a &unknown; b");
  CHECK(html_unescape("fish & chips") == "fish & chips");
  CHECK(html_unescape("&#;") == "&#;");
  CHECK(html_unescape("&#0;") == "&#0;");         // hostile NUL refused.
  CHECK(html_unescape("&#99999999;") == "&#99999999;");  // out of range.
  CHECK(html_unescape("") == "");
}

// ===========================================================================
// looks_like_challenge
// ===========================================================================

TEST_CASE("looks_like_challenge_markers_case_insensitive") {
  CHECK(looks_like_challenge("<html><title>Just a moment...</title></html>"));
  CHECK(looks_like_challenge("<div id=\"cf-browser-verification\"></div>"));
  CHECK(looks_like_challenge("src=\"/cdn-cgi/challenge-platform/x.js\""));
  CHECK(looks_like_challenge("<title>Redirecting...</title>"));
  CHECK(looks_like_challenge("<body class=\"AdBlockingDetected\">"));
  CHECK(looks_like_challenge("window.parkLogic.go()"));
  CHECK_FALSE(looks_like_challenge(""));
  CHECK_FALSE(looks_like_challenge("<article>perfectly normal manga</article>"));
}

TEST_CASE("looks_like_challenge_negative_on_all_three_fixtures") {
  CHECK_FALSE(looks_like_challenge(read_fixture("weebcentral/search_data.html")));
  CHECK_FALSE(looks_like_challenge(read_fixture("weebcentral/full_chapter_list.html")));
  CHECK_FALSE(looks_like_challenge(read_fixture("weebcentral/chapter_images.html")));
}

// ===========================================================================
// URL builders (exact-string, the probe pins)
// ===========================================================================

TEST_CASE("search_url_exact_string") {
  CHECK(search_url(kWcHost, "all you need", 32) ==
        "https://weebcentral.com/search/data?limit=32&offset=0"
        "&text=all%20you%20need&sort=Best+Match&order=Ascending&official=Any"
        "&display_mode=Full+Display");
  CHECK(search_url(kWcHost, "", 1) ==
        "https://weebcentral.com/search/data?limit=1&offset=0&text="
        "&sort=Best+Match&order=Ascending&official=Any&display_mode=Full+Display");
}

TEST_CASE("chapter_list_and_images_and_cover_urls_exact_string") {
  CHECK(chapter_list_url(kWcHost, kAynikUlid) ==
        "https://weebcentral.com/series/01J76XY9Y7BB76XK8YYBCFYKVF/full-chapter-list");
  CHECK(images_url(kWcHost, "01J76XYY670GZSEJQG395N25HM") ==
        "https://weebcentral.com/chapters/01J76XYY670GZSEJQG395N25HM/images"
        "?is_prev=False&current_page=1&reading_style=long_strip");
  CHECK(cover_url(kWcCoverHost, kAynikUlid) ==
        "https://temp.compsci88.com/cover/fallback/01J76XY9Y7BB76XK8YYBCFYKVF.jpg");
}

// ===========================================================================
// parse_search
// ===========================================================================

TEST_CASE("parse_search_fixture_pins_the_one_result_row") {
  const auto r = parse_search(read_fixture("weebcentral/search_data.html"));
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 1);
  const MdManga& m = (*r)[0];
  CHECK(m.id == kAynikUlid);
  CHECK(m.title == "All You Need Is Kill");
  REQUIRE(m.year.has_value());
  CHECK(*m.year == 2014);
  CHECK(m.status == "Complete");
  // Header pins: not in the fragment / not this source's business.
  CHECK(m.description.empty());
  CHECK(m.cover_filename.empty());
  CHECK_FALSE(m.al_id.has_value());
  CHECK_FALSE(m.mal_id.has_value());
}

TEST_CASE("parse_search_synthetic_drop_bad_ulid_fallback_title_entities") {
  const std::string frag =
      // Row 1: no line-clamp anchor -> the overlay-div fallback; entity in
      // the title; no Year/Status rows.
      "<article class=\"bg-base-300 flex\">"
      "<a href=\"https://weebcentral.com/series/01J76XYY670GZSEJQG395N25HM/X\">"
      "<div class=\"text-ellipsis truncate text-white\">Fish &amp; Chips</div>"
      "</a></article>"
      // Row 2: lowercase id fails the vet -> dropped whole.
      "<article class=\"bg-base-300 flex\">"
      "<a href=\"https://weebcentral.com/series/01j76xyy670gzsejqg395n25hm/Y\">"
      "<div class=\"text-ellipsis truncate\">Dropped</div></a></article>"
      // Row 3: the desktop anchor + year/status.
      "<article class=\"bg-base-300 flex\">"
      "<a href=\"https://weebcentral.com/series/01J76XY9Y7BB76XK8YYBCFYKVF/Z\">z</a>"
      "<a href=\"...\" class=\"line-clamp-1 link link-hover\">It&#39;s Mine</a>"
      "<div><strong>Year:</strong>\n <span>1999</span></div>"
      "<div><strong>Status:</strong>\n <span>Ongoing</span></div>"
      "</article>";
  const auto r = parse_search(frag);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 2);
  CHECK((*r)[0].id == "01J76XYY670GZSEJQG395N25HM");
  CHECK((*r)[0].title == "Fish & Chips");
  CHECK_FALSE((*r)[0].year.has_value());
  CHECK((*r)[0].status.empty());
  CHECK((*r)[1].id == kAynikUlid);
  CHECK((*r)[1].title == "It's Mine");
  REQUIRE((*r)[1].year.has_value());
  CHECK(*(*r)[1].year == 1999);
  CHECK((*r)[1].status == "Ongoing");
}

TEST_CASE("parse_search_empty_or_alien_body_is_ok_empty") {
  // A no-results fragment is a legit answer, never an error.
  const auto empty = parse_search("");
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
  const auto alien = parse_search("<html><body>not the fragment</body></html>");
  REQUIRE(alien.has_value());
  CHECK(alien->empty());
}

// ===========================================================================
// parse_chapters
// ===========================================================================

TEST_CASE("parse_chapters_fixture_17_rows_ascending_en") {
  const auto r = parse_chapters(read_fixture("weebcentral/full_chapter_list.html"));
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 17);
  // The site lists newest-first; output is reading order.
  CHECK(r->front().id == "01J76XYY670GZSEJQG395N25HM");
  CHECK(r->front().chapter == "1");
  CHECK(r->back().id == "01J76XYY67FJMNM12XA6R35PFA");
  CHECK(r->back().chapter == "17");
  for (std::size_t i = 0; i < r->size(); ++i) {
    const MdChapter& c = (*r)[i];
    CHECK(c.chapter == std::to_string(i + 1));
    CHECK(c.title.empty());
    CHECK(c.lang == "en");
    CHECK(c.pages == 0);
    CHECK(c.publish_at == "2024-09-07T17:04:15.717Z");
  }
}

TEST_CASE("parse_chapters_synthetic_specials_and_bad_ids") {
  const std::string frag =
      "<a href=\"/chapters/01J76XYY67FJMNM12XA6R35PFA\">"
      "<span class=\"\">Special Oneshot</span>"
      "<time datetime=\"2024-01-02T00:00:00.000Z\">x</time></a>"
      "<a href=\"/chapters/01j00000000000000000000000\">"  // bad id: dropped.
      "<span class=\"\">Chapter 9</span></a>"
      "<a href=\"/chapters/01J76XYY670GZSEJQG395N25HM\">"
      "<span class=\"\">Chapter 10.5</span>"
      "<time datetime=\"2024-01-01T00:00:00.000Z\">x</time></a>";
  const auto r = parse_chapters(frag);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 2);
  // Numbered rows sort before unnumbered specials (chapter_less law).
  CHECK((*r)[0].chapter == "10.5");
  CHECK((*r)[0].title.empty());
  CHECK((*r)[0].publish_at == "2024-01-01T00:00:00.000Z");
  CHECK((*r)[1].chapter.empty());
  CHECK((*r)[1].title == "Special Oneshot");
}

TEST_CASE("parse_chapters_missing_label_and_datetime_degrade_to_empty") {
  const std::string frag = "<a href=\"/chapters/01J76XYY670GZSEJQG395N25HM\">bare</a>";
  const auto r = parse_chapters(frag);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 1);
  CHECK(r->front().chapter.empty());
  CHECK(r->front().title.empty());
  CHECK(r->front().publish_at.empty());
}

// ===========================================================================
// parse_images
// ===========================================================================

TEST_CASE("parse_images_fixture_69_pages_verbatim_urls_referer_hint") {
  const auto r = parse_images(read_fixture("weebcentral/chapter_images.html"));
  REQUIRE(r.has_value());
  REQUIRE(r->units.size() == 69);
  CHECK(r->units.front().url ==
        "https://official.lowee.us/manga/All-You-Need-Is-Kill/0001-001.png");
  CHECK(r->units.front().name_hint == "0001-001.png");
  CHECK(r->units.front().referer == kWcHost);
  CHECK(r->units.back().url ==
        "https://official.lowee.us/manga/All-You-Need-Is-Kill/0001-069.png");
  CHECK(r->units.back().name_hint == "0001-069.png");
}

TEST_CASE("parse_images_skips_relative_chrome_but_keeps_order") {
  const std::string frag =
      "<img src=\"/static/images/badge.svg\">"
      "<img\n  src=\"https://scans-hot.planeptune.us/manga/X/0001-001.png\"\n>"
      "<img src=\"https://scans-hot.planeptune.us/manga/X/0001-002.png?v=2\">";
  const auto r = parse_images(frag);
  REQUIRE(r.has_value());
  REQUIRE(r->units.size() == 2);
  CHECK(r->units[0].url == "https://scans-hot.planeptune.us/manga/X/0001-001.png");
  // The query rides the URL but never the on-disk name.
  CHECK(r->units[1].url == "https://scans-hot.planeptune.us/manga/X/0001-002.png?v=2");
  CHECK(r->units[1].name_hint == "0001-002.png");
}

TEST_CASE("parse_images_unsafe_filename_fails_whole_chapter") {
  // ".." in the would-be disk name: a page gap is worse than a failed
  // chapter, so the WHOLE parse errors (the parse_at_home posture).
  const auto bad = parse_images("<img src=\"https://h.example/manga/..evil\">");
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_images_zero_pages_is_decode") {
  const auto r = parse_images("<section>no imgs at all</section>");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ProviderError::Kind::Decode);
}

// ===========================================================================
// Source attrs + id scoping + cover URL
// ===========================================================================

TEST_CASE("weebcentral_attrs_scoped_id_and_cover_thumb") {
  auto wc = WeebCentral::create();
  REQUIRE(wc.has_value());
  CHECK(wc->key() == "wc");
  CHECK(wc->name() == "WeebCentral");
  CHECK_FALSE(wc->nsfw());
  CHECK(scoped_id(*wc, kAynikUlid) == "wc:01J76XY9Y7BB76XK8YYBCFYKVF");
  MdManga m;
  m.id = kAynikUlid;
  CHECK(wc->cover_thumb_url(m) ==
        "https://temp.compsci88.com/cover/fallback/01J76XY9Y7BB76XK8YYBCFYKVF.jpg");
  m.id = "not-a-ulid";
  CHECK(wc->cover_thumb_url(m).empty());
}
