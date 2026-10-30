// manga_mangahub_tests.cpp — golden tests for src/manga/mangahub.hpp/.cpp:
// the vets, chapter ids, request bodies exact-string, the three parsers over
// tests/fixtures/mangahub/ (live captures) plus synthetic edge cases, and
// the source attrs. Fully offline.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/manga/mangahub.hpp"

using namespace shigoku;
using namespace shigoku::manga;
using namespace shigoku::manga::mh;

namespace {

std::string read_fixture(const char* name) {
  const std::string path = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/mangahub/" + name;
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "missing fixture: " << path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("slug_shaped") {
  CHECK(slug_shaped("sousou-no-frieren"));
  CHECK(slug_shaped("dr-stone_111"));
  CHECK(slug_shaped("a"));
  CHECK(slug_shaped(std::string(kMhSlugMax, 'a')));
  CHECK_FALSE(slug_shaped(""));
  CHECK_FALSE(slug_shaped(std::string(kMhSlugMax + 1, 'a')));
  CHECK_FALSE(slug_shaped("-frieren"));
  CHECK_FALSE(slug_shaped("frieren_"));
  CHECK_FALSE(slug_shaped("Frieren"));
  CHECK_FALSE(slug_shaped("a/b"));
  CHECK_FALSE(slug_shaped("a\"b"));
  CHECK_FALSE(slug_shaped("a b"));
  CHECK_FALSE(slug_shaped("../x"));
}

TEST_CASE("number_shaped") {
  CHECK(number_shaped("1"));
  CHECK(number_shaped("93.5"));
  CHECK(number_shaped("3862"));
  CHECK(number_shaped("430.6"));
  CHECK_FALSE(number_shaped(""));
  CHECK_FALSE(number_shaped("."));
  CHECK_FALSE(number_shaped("1."));
  CHECK_FALSE(number_shaped(".5"));
  CHECK_FALSE(number_shaped("1.2.3"));
  CHECK_FALSE(number_shaped("-1"));
  CHECK_FALSE(number_shaped("1e+21"));
  CHECK_FALSE(number_shaped("1,5"));
  CHECK_FALSE(number_shaped(std::string(kMhNumberMax + 1, '9')));
}

TEST_CASE("prefix_and_cover_shapes") {
  CHECK(prefix_shaped("dr-stone/1/"));
  CHECK(prefix_shaped("sousou-no-frieren/93.5/"));
  CHECK_FALSE(prefix_shaped("dr-stone/1"));
  CHECK_FALSE(prefix_shaped("/dr-stone/1/"));
  CHECK_FALSE(prefix_shaped("dr-stone//"));
  CHECK_FALSE(prefix_shaped("../1/"));
  CHECK_FALSE(prefix_shaped("https://evil/1/"));
  CHECK_FALSE(prefix_shaped("a b/1/"));
  CHECK_FALSE(prefix_shaped(""));

  CHECK(cover_shaped("md/sousou-no-frieren.jpg"));
  CHECK(cover_shaped("mbd/x.png"));
  CHECK(cover_shaped("plain.jpg"));
  CHECK_FALSE(cover_shaped(""));
  CHECK_FALSE(cover_shaped("md/"));
  CHECK_FALSE(cover_shaped("/md/x.jpg"));
  CHECK_FALSE(cover_shaped("md/../x.jpg"));
  CHECK_FALSE(cover_shaped("md/.hidden"));
  CHECK_FALSE(cover_shaped("https://evil/x.jpg"));
}

TEST_CASE("chapter_id_round_trip") {
  CHECK(chapter_id("sousou-no-frieren", "93.5") == "sousou-no-frieren_93.5");
  const auto a = split_chapter_id("sousou-no-frieren_93.5");
  REQUIRE(a.has_value());
  CHECK(a->first == "sousou-no-frieren");
  CHECK(a->second == "93.5");
  // A slug carrying '_' splits at the last one.
  const auto b = split_chapter_id(chapter_id("dr-stone_111", "1"));
  REQUIRE(b.has_value());
  CHECK(b->first == "dr-stone_111");
  CHECK(b->second == "1");
  CHECK_FALSE(split_chapter_id("no-separator").has_value());
  CHECK_FALSE(split_chapter_id("_1").has_value());
  CHECK_FALSE(split_chapter_id("frieren_").has_value());
  CHECK_FALSE(split_chapter_id("frieren_1x").has_value());
  CHECK_FALSE(split_chapter_id("../x_1").has_value());
}

TEST_CASE("chapter_title_drops_the_filler") {
  CHECK(chapter_title("Chapter 146", "146").empty());
  CHECK(chapter_title("  chapter   93.5 ", "93.5").empty());
  CHECK(chapter_title("End of Adventure", "1") == "End of Adventure");
  CHECK(chapter_title("Chapter 146", "147") == "Chapter 146");
  CHECK(chapter_title("  Spaced  ", "1") == "Spaced");
  CHECK(chapter_title("", "1").empty());
}

TEST_CASE("request_bodies_exact_string") {
  CHECK(search_body("frieren", 30) ==
        R"({"query":"query($q:String!){search(x:m01,q:$q,limit:30){rows{title,slug,image,status,isSafe}}}","variables":{"q":"frieren"}})");
  // Punctuation and quotes ride the variables, JSON-escaped, never the query.
  CHECK(search_body("Re:Zero \"x\"", 5) ==
        R"({"query":"query($q:String!){search(x:m01,q:$q,limit:5){rows{title,slug,image,status,isSafe}}}","variables":{"q":"Re:Zero \"x\""}})");
  CHECK(manga_body("sousou-no-frieren") ==
        R"({"query":"query($s:String!){manga(x:m01,slug:$s){chapters{number,title,date}}}","variables":{"s":"sousou-no-frieren"}})");
  CHECK(chapter_body("sousou-no-frieren", "93.5") ==
        R"({"query":"query($s:String!,$n:Float!){chapter(x:m01,slug:$s,number:$n){pages}}","variables":{"n":93.5,"s":"sousou-no-frieren"}})");
  CHECK(chapter_body("dr-stone_111", "1") ==
        R"({"query":"query($s:String!,$n:Float!){chapter(x:m01,slug:$s,number:$n){pages}}","variables":{"n":1,"s":"dr-stone_111"}})");
}

TEST_CASE("page_and_thumb_urls") {
  CHECK(page_url(kMhImageHost, "dr-stone/1/", "2.jpg") ==
        "https://imgx.mghcdn.com/dr-stone/1/2.jpg");
  CHECK(thumb_url(kMhThumbHost, "md/sousou-no-frieren.jpg") ==
        "https://thumb.mghcdn.com/md/sousou-no-frieren.jpg");
}

TEST_CASE("parse_search_fixture") {
  const auto rows = parse_search(read_fixture("search_frieren.json"), false);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 3);
  const MdManga& first = rows->front();
  CHECK(first.id == "sousou-no-frieren");
  CHECK(first.title == "Sousou no Frieren");
  CHECK(first.status == "ongoing");
  CHECK(first.cover_filename == "md/sousou-no-frieren.jpg");
  CHECK_FALSE(first.al_id.has_value());
  CHECK_FALSE(first.mal_id.has_value());
  CHECK((*rows)[1].id == "kmtakpk-s-frieren-koma");
}

TEST_CASE("parse_search_safety_gate_and_degradation") {
  const std::string raw = R"({"data":{"search":{"rows":[
      {"title":"Safe","slug":"safe-one","image":"md/safe.jpg","status":"ongoing","isSafe":true},
      {"title":"Adult","slug":"adult-one","image":"md/adult.jpg","status":"ongoing","isSafe":false},
      {"title":"Unknown","slug":"unknown-one","image":"","status":"ongoing"},
      {"title":"","slug":"no-title","image":"../x.jpg","isSafe":true},
      {"title":"Bad","slug":"Bad Slug","isSafe":true},
      {"title":"Dup","slug":"safe-one","isSafe":true},
      7
  ]}}})";
  const auto gated = parse_search(raw, false);
  REQUIRE(gated.has_value());
  REQUIRE(gated->size() == 2);
  CHECK((*gated)[0].id == "safe-one");
  CHECK((*gated)[0].title == "Safe");
  CHECK((*gated)[1].id == "no-title");
  CHECK((*gated)[1].title == "no-title");
  CHECK((*gated)[1].cover_filename.empty());

  const auto open = parse_search(raw, true);
  REQUIRE(open.has_value());
  REQUIRE(open->size() == 4);
  CHECK((*open)[1].id == "adult-one");
  CHECK((*open)[2].id == "unknown-one");
  CHECK((*open)[2].cover_filename.empty());

  const auto empty = parse_search(R"({"data":{"search":{"rows":[]}}})", false);
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

TEST_CASE("parse_search_refusals") {
  const auto schema = parse_search(read_fixture("search_schema_error.json"), false);
  REQUIRE_FALSE(schema.has_value());
  CHECK(schema.error().kind == ProviderError::Kind::Decode);
  CHECK(schema.error().detail.find("Cannot query field") != std::string::npos);
  CHECK_FALSE(parse_search("not json", false).has_value());
  CHECK_FALSE(parse_search(R"({"data":{"search":{}}})", false).has_value());
  const auto null_search = parse_search(R"({"data":{"search":null}})", false);
  REQUIRE_FALSE(null_search.has_value());
  CHECK(null_search.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_manga_fixture") {
  const auto chapters =
      parse_manga(read_fixture("manga_sousou_no_frieren.json"), "sousou-no-frieren");
  REQUIRE(chapters.has_value());
  REQUIRE(chapters->size() == 158);
  const MdChapter& first = chapters->front();
  CHECK(first.id == "sousou-no-frieren_1");
  CHECK(first.chapter == "1");
  CHECK(first.title == "End of Adventure");
  CHECK(first.lang == "en");
  CHECK(first.publish_at == "2020-05-13T10:45:23.000Z");
  CHECK(chapters->back().chapter == "147");
  CHECK(chapters->back().title.empty());  // "Chapter 147" filler.

  std::vector<std::string> numbers;
  for (const auto& c : *chapters) numbers.push_back(c.chapter);
  const auto at = [&](const std::string& n) {
    return std::find(numbers.begin(), numbers.end(), n) - numbers.begin();
  };
  REQUIRE(at("93.5") < static_cast<long>(numbers.size()));
  CHECK(at("93") < at("93.1"));
  CHECK(at("93.1") < at("93.5"));
  CHECK(at("93.5") < at("94"));
  CHECK(at("110.5") < at("111"));
}

TEST_CASE("parse_manga_synthetic_rows") {
  const std::string raw = R"({"data":{"manga":{"chapters":[
      {"number":2,"title":"Two","date":"2021-01-02T00:00:00.000Z"},
      {"number":1.0,"title":"One","date":"2021-01-01T00:00:00.000Z"},
      {"number":10.5,"title":"Ten and a half","date":"bad date <script>"},
      {"number":-1,"title":"Negative"},
      {"number":"x","title":"Text"},
      {"title":"No number"}
  ]}}})";
  const auto chapters = parse_manga(raw, "x-y");
  REQUIRE(chapters.has_value());
  REQUIRE(chapters->size() == 3);
  CHECK((*chapters)[0].chapter == "1");
  CHECK((*chapters)[0].id == "x-y_1");
  CHECK((*chapters)[1].chapter == "2");
  CHECK((*chapters)[2].chapter == "10.5");
  CHECK((*chapters)[2].publish_at.empty());
}

TEST_CASE("parse_manga_missing_and_malformed") {
  const auto missing = parse_manga(read_fixture("manga_missing.json"), "no-such-title-zzz");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().kind == ProviderError::Kind::Http);
  CHECK(missing.error().status == 404);
  const auto no_chapters = parse_manga(R"({"data":{"manga":{}}})", "x");
  REQUIRE_FALSE(no_chapters.has_value());
  CHECK(no_chapters.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_chapter_fixtures") {
  const auto frieren =
      parse_chapter(read_fixture("chapter_sousou_no_frieren_93_5.json"), kMhImageHost);
  REQUIRE(frieren.has_value());
  REQUIRE(frieren->units.size() == 5);
  CHECK(frieren->units[0].url == "https://imgx.mghcdn.com/sousou-no-frieren/93.5/1.jpg");
  CHECK(frieren->units[0].referer == kMhReferer);
  CHECK(frieren->units[0].name_hint == "1.jpg");
  CHECK(frieren->units[4].url == "https://imgx.mghcdn.com/sousou-no-frieren/93.5/5.jpg");

  // The prefix is the image path, not the slug.
  const auto stone = parse_chapter(read_fixture("chapter_dr_stone_1.json"), kMhImageHost);
  REQUIRE(stone.has_value());
  REQUIRE_FALSE(stone->units.empty());
  CHECK(stone->units[0].url == "https://imgx.mghcdn.com/dr-stone/1/1.jpg");
}

TEST_CASE("parse_chapter_missing_and_throttled") {
  const auto missing = parse_chapter(read_fixture("chapter_missing.json"), kMhImageHost);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().kind == ProviderError::Kind::Http);
  CHECK(missing.error().status == 404);

  const auto throttled =
      parse_chapter(read_fixture("chapter_rate_limited.json"), kMhImageHost);
  REQUIRE_FALSE(throttled.has_value());
  CHECK(throttled.error().kind == ProviderError::Kind::RateLimited);
}

TEST_CASE("parse_chapter_whole_chapter_law") {
  const auto wrap = [](const std::string& pages) {
    return std::string(R"({"data":{"chapter":{"pages":)") + pages + "}}}";
  };
  CHECK(parse_chapter(wrap(R"("{\"p\":\"x/1/\",\"i\":[\"1.webp\",\"2.png\"]}")"), kMhImageHost)
            .has_value());
  CHECK_FALSE(parse_chapter(wrap(R"("{\"p\":\"x/1/\",\"i\":[\"1.jpg\",\"../2.jpg\"]}")"),
                            kMhImageHost)
                  .has_value());
  CHECK_FALSE(parse_chapter(wrap(R"("{\"p\":\"x/1/\",\"i\":[\"1.jpg\",7]}")"), kMhImageHost)
                  .has_value());
  CHECK_FALSE(
      parse_chapter(wrap(R"("{\"p\":\"/etc/\",\"i\":[\"1.jpg\"]}")"), kMhImageHost).has_value());
  CHECK_FALSE(
      parse_chapter(wrap(R"("{\"p\":\"x/1\",\"i\":[\"1.jpg\"]}")"), kMhImageHost).has_value());
  CHECK_FALSE(parse_chapter(wrap(R"("{\"p\":\"x/1/\",\"i\":[]}")"), kMhImageHost).has_value());
  CHECK_FALSE(parse_chapter(wrap(R"("not json")"), kMhImageHost).has_value());
  CHECK_FALSE(parse_chapter(wrap(R"({"p":"x/1/","i":["1.jpg"]})"), kMhImageHost).has_value());
}

TEST_CASE("mangahub_attrs_and_cover") {
  auto mh = MangaHub::create(false);
  REQUIRE(mh.has_value());
  CHECK(mh->key() == "mh");
  CHECK(mh->name() == "MangaHub");
  CHECK_FALSE(mh->nsfw());
  CHECK(scoped_id(*mh, "sousou-no-frieren") == "mh:sousou-no-frieren");

  MdManga m;
  m.id = "sousou-no-frieren";
  m.cover_filename = "md/sousou-no-frieren.jpg";
  CHECK(mh->cover_thumb_url(m) == "https://thumb.mghcdn.com/md/sousou-no-frieren.jpg");
  m.cover_filename = "../x.jpg";
  CHECK(mh->cover_thumb_url(m).empty());
  m.cover_filename.clear();
  CHECK(mh->cover_thumb_url(m).empty());
}

TEST_CASE("mangahub_transport_revets_ids_before_any_request") {
  auto mh = MangaHub::create(false);
  REQUIRE(mh.has_value());
  const auto ch = mh->chapters("../secrets", "en");
  REQUIRE_FALSE(ch.has_value());
  CHECK(ch.error().kind == ProviderError::Kind::Decode);
  const auto pg = mh->pages("sousou-no-frieren_1x", false);
  REQUIRE_FALSE(pg.has_value());
  CHECK(pg.error().kind == ProviderError::Kind::Decode);
  const auto pg2 = mh->pages("Bad Slug_1", false);
  REQUIRE_FALSE(pg2.has_value());
  CHECK(pg2.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("ranking_body_and_count") {
  CHECK(ranking_body(FeedKind::Popular, 30) ==
        std::optional<std::string>(
            R"({"query":"query($o:Int){search(x:m01,mod:POPULAR,count:true,offset:$o){rows{title,slug,image,status,isSafe},count}}","variables":{"o":30}})"));
  CHECK(ranking_body(FeedKind::Latest, 0)->find("mod:LATEST") != std::string::npos);
  CHECK(ranking_body(FeedKind::Newest, 0)->find("mod:NEW,") != std::string::npos);
  CHECK_FALSE(ranking_body(FeedKind::TopRated, 0).has_value());

  CHECK(ranking_count(R"({"data":{"search":{"rows":[],"count":79479}}})") == 79479);
  CHECK(ranking_count(R"({"data":{"search":{"rows":[]}}})") == 0);
  CHECK(ranking_count(R"({"errors":[{"message":"x"}]})") == 0);

  // The rows parse like search's, isSafe gate included.
  const auto rows = parse_search(
      R"({"data":{"search":{"rows":[{"title":"A","slug":"a-1","isSafe":true},)"
      R"({"title":"B","slug":"b-2","isSafe":false}],"count":2}}})",
      false);
  REQUIRE(rows.has_value());
  CHECK(rows->size() == 1);
}
