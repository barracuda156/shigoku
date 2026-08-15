// manga_nhentai_tests.cpp — golden tests for src/manga/nhentai.hpp/.cpp.
// Fully offline (the manga_dynasty_tests shape): URL builders exact-string,
// the three JSON parsers over tests/fixtures/nhentai/ (search/gallery/cdn
// fixtures captured from live responses) plus synthetic edge cases, the
// id/path vets, the gallery≡oneshot adapter, and the source attrs —
// including the one this source exists to prove: nsfw() true and the
// `nsfw_sources` gate making it invisible to pick_source/eligible_sources.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/manga/nhentai.hpp"

using namespace shigoku;
using namespace shigoku::manga;
using namespace shigoku::manga::nh;

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
// id_shaped / path_shaped (the parse-time trust boundary)
// ===========================================================================

TEST_CASE("id_shaped_accepts_digit_runs_and_refuses_everything_else") {
  CHECK(id_shaped("671903"));
  CHECK(id_shaped("1"));
  CHECK(id_shaped("4111399"));
  CHECK(id_shaped(std::string(kNhIdMax, '9')));
  CHECK_FALSE(id_shaped(""));
  CHECK_FALSE(id_shaped(std::string(kNhIdMax + 1, '9')));
  CHECK_FALSE(id_shaped("12a"));            // hex smuggling.
  CHECK_FALSE(id_shaped("-1"));             // sign.
  CHECK_FALSE(id_shaped("1/2"));            // path separator.
  CHECK_FALSE(id_shaped("../7"));           // traversal.
  CHECK_FALSE(id_shaped("1?x=1"));          // query smuggling.
  CHECK_FALSE(id_shaped(" 671903"));        // whitespace.
}

TEST_CASE("path_shaped_accepts_cdn_paths_and_refuses_metacharacters") {
  CHECK(path_shaped("galleries/4111399/1.webp"));
  CHECK(path_shaped("galleries/4109858/thumb.jpg.webp"));  // doubled ext, live.
  CHECK(path_shaped("galleries/4111399/cover.webp.webp"));
  CHECK(path_shaped("1.webp"));  // bare filename: charset-legal.
  CHECK_FALSE(path_shaped(""));
  CHECK_FALSE(path_shaped("/galleries/1/1.webp"));   // absolute.
  CHECK_FALSE(path_shaped("galleries/1/1.webp/"));   // trailing slash.
  CHECK_FALSE(path_shaped("galleries//1.webp"));     // empty segment.
  CHECK_FALSE(path_shaped("galleries/../1.webp"));   // traversal.
  CHECK_FALSE(path_shaped("galleries/1/1.webp?x"));  // query smuggling.
  CHECK_FALSE(path_shaped("galleries/1/a b.webp"));  // space.
  CHECK_FALSE(path_shaped("https://evil/1.webp"));   // scheme smuggling (':').
  CHECK_FALSE(path_shaped(std::string(kNhPathMax + 1, 'a')));
}

// ===========================================================================
// URL builders (exact-string, the probe pins)
// ===========================================================================

TEST_CASE("search_gallery_cdn_and_page_urls_exact_string") {
  CHECK(search_url(kNhHost, "kase san", 1) ==
        "https://nhentai.net/api/v2/search?query=kase%20san&page=1");
  CHECK(search_url(kNhHost, "", 3) ==
        "https://nhentai.net/api/v2/search?query=&page=3");
  CHECK(gallery_url(kNhHost, "671903") ==
        "https://nhentai.net/api/v2/galleries/671903");
  CHECK(cdn_url(kNhHost) == "https://nhentai.net/api/v2/cdn");
  CHECK(page_url("https://i1.nhentai.net", "galleries/4111399/1.webp") ==
        "https://i1.nhentai.net/galleries/4111399/1.webp");
  CHECK(gallery_url("http://127.0.0.1:8080", "7") ==
        "http://127.0.0.1:8080/api/v2/galleries/7");
}

// ===========================================================================
// iso_from_unix (publish_at spelling)
// ===========================================================================

TEST_CASE("iso_from_unix_utc_spelling_and_refusals") {
  // The fixture gallery's own upload_date.
  CHECK(iso_from_unix(1786385298) == "2026-08-10T18:08:18+00:00");
  CHECK(iso_from_unix(0).empty());
  CHECK(iso_from_unix(-5).empty());
}

// ===========================================================================
// parse_search (fixture + synthetic)
// ===========================================================================

TEST_CASE("parse_search_fixture_rows_ids_titles_thumbs") {
  const auto r = parse_search(read_fixture("nhentai/search_page1.json"));
  REQUIRE(r.has_value());
  CHECK(r->size() == 25);
  CHECK(r->front().id == "671903");
  CHECK(r->front().title ==
        "[Ricker Kai] Tsubo no Naka no Taikai (Dragon Ball Super)");
  CHECK(r->front().cover_filename == "galleries/4111399/thumb.webp");
  // Row 11's live doubled-extension thumb, taken verbatim (per-file
  // extensions vary; never assume .webp).
  CHECK((*r)[11].cover_filename == "galleries/4109858/thumb.jpg.webp");
  // A japanese_title:null row still carries its english title.
  CHECK((*r)[2].title == "Scarlet desire EX scene English");
  for (const auto& m : *r) {
    CHECK(m.year == std::nullopt);
    CHECK(m.status.empty());
    CHECK(m.description.empty());
    CHECK(m.al_id == std::nullopt);  // no tracker links here to sync.
    CHECK(m.mal_id == std::nullopt);
  }
}

TEST_CASE("parse_search_synthetic_degradation_rules") {
  // english_title null → japanese fallback; both null → row kept, title "".
  const auto ja = parse_search(
      R"({"result":[{"id":7,"english_title":null,"japanese_title":"日本語"}]})");
  REQUIRE(ja.has_value());
  REQUIRE(ja->size() == 1);
  CHECK(ja->front().title == "日本語");
  const auto none = parse_search(
      R"({"result":[{"id":7,"english_title":null,"japanese_title":null}]})");
  REQUIRE(none.has_value());
  CHECK(none->front().title.empty());

  // A bad/missing id drops the ROW (it would anchor a library dir); a bad
  // thumbnail only drops the COVER; duplicate ids collapse to the first.
  const auto rows = parse_search(R"({"result":[
      {"id":"671903","english_title":"stringly id"},
      {"id":1.5,"english_title":"float id"},
      {"english_title":"no id"},
      {"id":8,"english_title":"bad thumb","thumbnail":"../etc"},
      {"id":8,"english_title":"dupe"},
      {"id":9,"english_title":"ok","thumbnail":"galleries/9/thumb.webp"}]})");
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  CHECK((*rows)[0].id == "8");
  CHECK((*rows)[0].title == "bad thumb");
  CHECK((*rows)[0].cover_filename.empty());
  CHECK((*rows)[1].cover_filename == "galleries/9/thumb.webp");

  // `blacklisted` is IGNORED for now: the row stays.
  const auto bl = parse_search(
      R"({"result":[{"id":5,"english_title":"x","blacklisted":true}]})");
  REQUIRE(bl.has_value());
  CHECK(bl->size() == 1);

  // Envelope failures.
  CHECK(parse_search(R"({"result":[]})")->empty());  // no results = an answer.
  CHECK_FALSE(parse_search(R"({"num_pages":3})").has_value());
  CHECK_FALSE(parse_search(R"({"result":"nope"})").has_value());
  CHECK_FALSE(parse_search("not json").has_value());
}

// ===========================================================================
// parse_cdn (fixture + synthetic)
// ===========================================================================

TEST_CASE("parse_cdn_fixture_pools") {
  const auto c = parse_cdn(read_fixture("nhentai/cdn.json"));
  REQUIRE(c.has_value());
  REQUIRE(c->image_servers.size() == 4);
  REQUIRE(c->thumb_servers.size() == 4);
  CHECK(c->image_servers.front() == "https://i1.nhentai.net");
  CHECK(c->image_servers.back() == "https://i4.nhentai.net");
  CHECK(c->thumb_servers.front() == "https://t1.nhentai.net");
}

TEST_CASE("parse_cdn_normalization_and_refusals") {
  // Trailing slashes stripped (page_url joins with '/'); junk schemes dropped.
  const auto c = parse_cdn(R"({"image_servers":["https://i1.example/","ftp://x",
      "relative","http://i2.example"],"thumb_servers":[]})");
  REQUIRE(c.has_value());
  REQUIRE(c->image_servers.size() == 2);
  CHECK(c->image_servers[0] == "https://i1.example");
  CHECK(c->image_servers[1] == "http://i2.example");
  CHECK(c->thumb_servers.empty());  // covers degrade; not an error.

  // No usable image pool = nothing is fetchable.
  CHECK_FALSE(parse_cdn(R"({"image_servers":[],"thumb_servers":["https://t"]})")
                  .has_value());
  CHECK_FALSE(parse_cdn(R"({"image_servers":["ftp://x"]})").has_value());
  CHECK_FALSE(parse_cdn(R"({"thumb_servers":["https://t"]})").has_value());
  CHECK_FALSE(parse_cdn("not json").has_value());
}

// ===========================================================================
// parse_gallery (fixture + synthetic) + the oneshot adapter
// ===========================================================================

TEST_CASE("parse_gallery_fixture_fields_and_pages_verbatim") {
  const auto g = parse_gallery(read_fixture("nhentai/gallery.json"));
  REQUIRE(g.has_value());
  CHECK(g->id == "671903");
  CHECK(g->media_id == "4111399");
  CHECK(g->title == "Tsubo no Naka no Taikai");  // pretty wins.
  CHECK(g->lang == "ja");                        // "japanese" tag mapped.
  CHECK(g->num_pages == 11);
  CHECK(g->publish_at == "2026-08-10T18:08:18+00:00");
  CHECK(g->thumbnail == "galleries/4111399/thumb.webp");
  REQUIRE(g->pages.size() == 11);
  CHECK(g->pages.front() == "galleries/4111399/1.webp");
  CHECK(g->pages.back() == "galleries/4111399/11.webp");
}

TEST_CASE("parse_gallery_title_ladder_and_language_rules") {
  // pretty absent → english → japanese.
  const auto en = parse_gallery(
      R"({"id":7,"media_id":"1","title":{"english":"EN","japanese":"JA"}})");
  REQUIRE(en.has_value());
  CHECK(en->title == "EN");
  const auto ja = parse_gallery(
      R"({"id":7,"media_id":"1","title":{"japanese":"JA"}})");
  REQUIRE(ja.has_value());
  CHECK(ja->title == "JA");

  // "translated"/"rewrite" are release properties, never the language; the
  // first real language tag wins; unknown names pass verbatim; none → "".
  const auto lang = [](const char* tags) {
    std::string body = R"({"id":7,"media_id":"1","tags":)";
    body += tags;
    body += "}";
    const auto g = parse_gallery(body);
    REQUIRE(g.has_value());
    return g->lang;
  };
  CHECK(lang(R"([{"type":"language","name":"translated"},
                 {"type":"language","name":"english"}])") == "en");
  CHECK(lang(R"([{"type":"language","name":"chinese"}])") == "zh");
  CHECK(lang(R"([{"type":"language","name":"korean"}])") == "korean");
  CHECK(lang(R"([{"type":"tag","name":"japanese"}])").empty());  // wrong type.
  CHECK(lang(R"([])").empty());
}

TEST_CASE("parse_gallery_refusals_and_the_whole_chapter_law") {
  // id/media_id are the fetch + dir anchors: absent/ill-shaped → Decode.
  CHECK_FALSE(parse_gallery(R"({"media_id":"1"})").has_value());
  CHECK_FALSE(parse_gallery(R"({"id":"671903","media_id":"1"})").has_value());
  CHECK_FALSE(parse_gallery(R"({"id":7})").has_value());
  CHECK_FALSE(parse_gallery(R"({"id":7,"media_id":"x1"})").has_value());
  // ONE unsafe page path fails the WHOLE gallery (a page gap is worse than a
  // failed chapter — the parse_at_home posture).
  const auto bad = parse_gallery(R"({"id":7,"media_id":"1",
      "pages":[{"number":1,"path":"galleries/1/1.webp"},
               {"number":2,"path":"galleries/1/../2.webp"}]})");
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().kind == ProviderError::Kind::Decode);
  CHECK_FALSE(parse_gallery(R"({"id":7,"media_id":"1","pages":[{"number":1}]})")
                  .has_value());
  // Missing pages[] parses as empty — chapters() has no use for it.
  const auto nopages = parse_gallery(R"({"id":7,"media_id":"1"})");
  REQUIRE(nopages.has_value());
  CHECK(nopages->pages.empty());
  CHECK_FALSE(parse_gallery("not json").has_value());
}

TEST_CASE("gallery_chapter_synthesizes_the_oneshot_row") {
  const auto g = parse_gallery(read_fixture("nhentai/gallery.json"));
  REQUIRE(g.has_value());
  const MdChapter c = gallery_chapter(*g);
  CHECK(c.id == "671903");
  CHECK(c.chapter.empty());  // oneshot: the library dir spells it out.
  CHECK(c.title == "Oneshot");
  CHECK(c.pages == 11);
  CHECK(c.lang == "ja");
  CHECK(c.publish_at == "2026-08-10T18:08:18+00:00");

  // Without pages[] the count falls back to num_pages (the search/detail
  // surfaces know it before any pages fetch).
  NhGallery bare;
  bare.id = "9";
  bare.num_pages = 42;
  CHECK(gallery_chapter(bare).pages == 42);
}

// ===========================================================================
// Source attrs, the nsfw gate, and the transport's id re-vet
// ===========================================================================

TEST_CASE("nhentai_attrs_scoped_id_and_the_nsfw_gate") {
  auto nh = Nhentai::create();
  REQUIRE(nh.has_value());
  CHECK(nh->key() == "nh");
  CHECK(nh->name() == "nhentai");
  CHECK(nh->nsfw());
  CHECK(scoped_id(*nh, "671903") == "nh:671903");

  // With `nsfw_sources` off the source is invisible EVERYWHERE — it can
  // neither match nor fall back.
  const std::vector<const MangaSource*> reg = {&nh.value()};
  CHECK(pick_source(reg, "nh", /*allow_nsfw=*/false) == nullptr);
  CHECK(eligible_sources(reg, false).empty());
  CHECK(pick_source(reg, "nh", /*allow_nsfw=*/true) == &nh.value());
  REQUIRE(eligible_sources(reg, true).size() == 1);
}

TEST_CASE("nhentai_cover_thumb_url_answers_only_from_the_cache") {
  auto nh = Nhentai::create();
  REQUIRE(nh.has_value());
  MdManga m;
  m.id = "671903";
  m.cover_filename = "galleries/4111399/thumb.webp";
  // The cdn pool was never primed (no fetch in this offline test) — and this
  // call must NOT fetch (UI thread): "" is the only right answer.
  CHECK(nh->cover_thumb_url(m).empty());
  MdManga bare;
  bare.id = "671903";
  CHECK(nh->cover_thumb_url(bare).empty());  // no thumb path at all.
}

TEST_CASE("nhentai_transport_revets_ids_before_they_shape_a_path") {
  // Ids may later arrive from persisted store space rather than a fresh
  // parse; a corrupt one must never reach the network (these fail before any
  // fetch, so the test stays offline).
  auto nh = Nhentai::create();
  REQUIRE(nh.has_value());
  const auto ch = nh->chapters("../secrets", "en");
  REQUIRE_FALSE(ch.has_value());
  CHECK(ch.error().kind == ProviderError::Kind::Decode);
  const auto pg = nh->pages("671903x", false);
  REQUIRE_FALSE(pg.has_value());
  CHECK(pg.error().kind == ProviderError::Kind::Decode);
}
