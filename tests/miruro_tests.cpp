// miruro_tests.cpp — the Miruro page readers against captured pages
// (a search for "flcl", the FLCL episode 1 watch page), the candidate order
// and the guards, then the provider over a loopback fixture server.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/select.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cmath>
#include <csignal>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/miruro.hpp"

using namespace shigoku;
using namespace shigoku::miruro;
using namespace shigoku::miruro::detail;

namespace {

std::string read_fixture(const std::string& name) {
  const std::string path = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/miruro/" + name;
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "missing fixture: " << path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

void replace_all(std::string& s, std::string_view from, std::string_view to) {
  for (auto at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
    s.replace(at, from.size(), to);
  }
}

// The watch fixture without its sources part.
std::string watch_without_sources() {
  std::string html = read_fixture("watch.html");
  const auto at = html.find("<script>__sveltekit_1bfmtb1.resolve(5,");
  REQUIRE(at != std::string::npos);
  const auto end = html.find("</script>", at);
  REQUIRE(end != std::string::npos);
  html.erase(at, end + 9 - at);
  return html;
}

// The watch fixture with its dub track renamed out of the way.
std::string watch_without_dub() {
  std::string html = read_fixture("watch.html");
  REQUIRE(contains(html, "track:\"dub\""));
  replace_all(html, "track:\"dub\"", "track:\"raw\"");
  return html;
}

std::string page_of(const std::string& kit_data, const std::vector<std::string>& parts) {
  std::string html =
      "<!doctype html><html><head><title>t - Miruro</title></head><body><script>\n"
      "Promise.all([]).then(([kit, app]) => { kit.start(app, element, { node_ids: [0, 1], "
      "data: " +
      kit_data + ", form: null, error: null }); });\n</script>\n";
  int n = 1;
  for (const auto& p : parts) {
    html += "<script>__sveltekit_t.resolve(" + std::to_string(n++) + ", () => " + p + ")</script>\n";
  }
  return html + "</body></html>";
}

std::string search_of(const std::string& items) {
  return page_of(R"([{type:"data",data:{country:"DE"},uses:{}},{type:"data",data:{mode:"anime",items:)" +
                     items + R"(,hasMore:false},uses:{}}])",
                 {});
}

constexpr const char* kWatchKit =
    R"([{type:"data",data:{},uses:{}},{type:"data",data:{id:"abcdefgh",)"
    R"(episodesData:__sveltekit_t.defer(1),sourcesData:__sveltekit_t.defer(2)},uses:{}}])";

std::string watch_of(const std::string& episodes, const std::string& tracks) {
  std::vector<std::string> parts{"[{episodes:" + episodes + "}]"};
  if (!tracks.empty()) parts.push_back("[{episodeNumber:1,tracks:" + tracks + "}]");
  return page_of(kWatchKit, parts);
}

constexpr const char* kChallengePage =
    "<!DOCTYPE html><html lang=\"en-US\"><head><title>Just a moment...</title></head>"
    "<body><noscript>Enable JavaScript and cookies to continue</noscript></body></html>";

constexpr const char* kShowId = "40IfvwRLNelF3a9pLjVxPWquVU0gEwA3";

constexpr const char* kSunSub0 = "https://calmgrove.crystal-61.top/m/ASx0CLBlKVqlgaO1SWpJg2d9.m3u8";
constexpr const char* kSunSub1 = "https://coralarch.coastowl.top/m/AdlnjjhRjH7BmDRuaW5puKEl.m3u8";
constexpr const char* kSunSub2 = "https://vault-6.keeply.top/m/AUF59PfjpdZcrFcnonXKrSsS.m3u8";
constexpr const char* kSunSub3 = "https://vault-7.keeply.top/m/AQh3VnNBtwc27L5oZS0nELlC.m3u8";
constexpr const char* kSunSubtitle = "https://calmgrove.crystal-61.top/a/43843e4200687721e6448896.jpeg";
constexpr const char* kSunReferer = "https://ultracloud.cc/";
constexpr const char* kSunOrigin = "https://ultracloud.cc";

#define SUB_HASH "218a0aefd1d1a4be65601cc6ddc1520e/2fd95cc7c0f17a2870bb6a4ec5eb0bb6"
#define DUB_HASH "218a0aefd1d1a4be65601cc6ddc1520e/fa021fb8dfe2edbe87cf5b5a97dcd30b"
#define SUB_TOKEN "?token=MTc5NTIyMDI5MnwyMThhMGFl.KPDEM-lobZhOHY2PSiHOeCcFHnn0xD-XjPWzvOTyAxo"
#define DUB_TOKEN "?token=MTc5NTIyMDI5MnwyMThhMGFl.B25OrIwObKKhzZX8uaw_hO_pvX9mOpSkM1f6wkoHLyA"

constexpr const char* kBeeSubShared = "https://fetch.nexabloom.top/anime/" SUB_HASH "/master.m3u8" SUB_TOKEN;
constexpr const char* kBeeSubHd2 = "https://ncdn.imgnex.top/anime/" SUB_HASH "/master.m3u8" SUB_TOKEN;
constexpr const char* kBeeSubHd1 = "https://megap.shiora.top/" SUB_HASH "/master.m3u8" SUB_TOKEN;
constexpr const char* kBeeSubVidPlay =
    "https://0arx3.crystaldrift.world/anime/" SUB_HASH "/master.m3u8" SUB_TOKEN;
constexpr const char* kBeeSubtitle0 =
    "https://4gmkc.mysubsplay.com/anime/" SUB_HASH "/subtitles/13197d906c7b3a91347431ba4bc4f482.vtt";
constexpr const char* kBeeSubtitle1 =
    "https://0arx3.crystaldrift.world/anime/" SUB_HASH "/subtitles/13197d906c7b3a91347431ba4bc4f482.vtt";
constexpr const char* kKnobSub = "https://hlsxst2.winghoster.site/cdn/092e3d2d14736a0ad5386790?t.m3u8&v.m3u8";

constexpr const char* kSunDub0 = "https://calmgrove.crystal-61.top/m/ARUHljObJtWyoEgFUbFWhnBN.m3u8";
constexpr const char* kSunDub1 = "https://coralarch.coastowl.top/m/ATl9CMt_j_QkTc9uqr6Sme0B.m3u8";
constexpr const char* kSunDub2 = "https://vault-6.keeply.top/m/AScvaM5gsYmWjBX0AcWt1lGi.m3u8";
constexpr const char* kSunDub3 = "https://vault-7.keeply.top/m/AZ_CCLf22sVT69xap8CasB5N.m3u8";
constexpr const char* kBeeDub0 = "https://fetch.nexabloom.top/anime/" DUB_HASH "/master.m3u8" DUB_TOKEN;
constexpr const char* kBeeDub1 = "https://ncdn.imgnex.top/anime/" DUB_HASH "/master.m3u8" DUB_TOKEN;
constexpr const char* kBeeDub2 = "https://megap.shiora.top/" DUB_HASH "/master.m3u8" DUB_TOKEN;
constexpr const char* kBeeDub3 = "https://0arx3.crystaldrift.world/anime/" DUB_HASH "/master.m3u8" DUB_TOKEN;
constexpr const char* kKnobDub = "https://st2.dpopdrop89.store/cdn/092e3d2d14736a0ad5386790?t.m3u8&v.m3u8";

constexpr const char* kMediaPlaylist =
    "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n#EXTINF:10.0,\nseg-0.jpg\n"
    "#EXT-X-ENDLIST\n";

constexpr const char* kTwoLanguageMaster =
    "#EXTM3U\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"Japanese\",LANGUAGE=\"jpn\",DEFAULT=YES,"
    "AUTOSELECT=YES,URI=\"ja/index.m3u8\"\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"English\",LANGUAGE=\"eng\",DEFAULT=NO,"
    "AUTOSELECT=YES,URI=\"en/index.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,AUDIO=\"aud\"\n"
    "720/index.m3u8\n";

Stream stream(std::string url, std::optional<std::string> referer = std::nullopt,
              std::optional<std::string> origin = std::nullopt, bool hls = true) {
  Stream s;
  s.server = "s";
  s.url = std::move(url);
  s.hls = hls;
  s.referer = std::move(referer);
  s.origin = std::move(origin);
  return s;
}

Subtitle subtitle(std::string url, std::string language, std::string label, bool is_default) {
  return Subtitle{std::move(url), std::move(language), std::move(label), is_default};
}

std::vector<std::string> urls_of(const std::vector<Candidate>& list) {
  std::vector<std::string> out;
  for (const auto& c : list) out.push_back(c.stream.url);
  return out;
}

}  // namespace

// ===========================================================================
// Guards and small helpers
// ===========================================================================

TEST_CASE("valid_show_id") {
  CHECK(valid_show_id(kShowId));
  CHECK(valid_show_id("g7k9EC7-5D51wyjaPksr8vz1v6PqlAE_"));
  CHECK(valid_show_id("abcdefgh"));
  CHECK(valid_show_id(std::string(64, 'a')));
  CHECK_FALSE(valid_show_id("abcdefg"));
  CHECK_FALSE(valid_show_id(std::string(65, 'a')));
  CHECK_FALSE(valid_show_id(""));
  CHECK_FALSE(valid_show_id("../abcdefgh"));
  CHECK_FALSE(valid_show_id("abcd efgh"));
  CHECK_FALSE(valid_show_id("abcdefgh?ep=1"));
  CHECK_FALSE(valid_show_id("abcdefgh/x"));
  CHECK_FALSE(valid_show_id("abcd%2Fefgh"));
}

TEST_CASE("valid_episode") {
  for (const char* ok : {"1", "7", "0", "12.5", "1.25", "1234", "007"}) {
    CAPTURE(ok);
    CHECK(valid_episode(ok));
  }
  for (const char* bad :
       {"", "12345", ".5", "1.", "1.234", "-1", "+1", "1a", "1.5.5", " 1", "1 ", "1,5", "1e2"}) {
    CAPTURE(bad);
    CHECK_FALSE(valid_episode(bad));
  }
}

TEST_CASE("episode_label") {
  CHECK(episode_label(7) == "7");
  CHECK(episode_label(12.5) == "12.5");
  CHECK(episode_label(1.25) == "1.25");
  CHECK(episode_label(1.10) == "1.1");
  CHECK(episode_label(0.29) == "0.29");
  CHECK(episode_label(0) == "0");
  CHECK(episode_label(-0.0) == "0");
  CHECK(episode_label(2.999) == "3");
  CHECK(episode_label(9999.99) == "9999.99");
  CHECK(episode_label(1234) == "1234");
  CHECK_FALSE(episode_label(-1).has_value());
  CHECK_FALSE(episode_label(-0.5).has_value());
  CHECK_FALSE(episode_label(std::numeric_limits<double>::quiet_NaN()).has_value());
  CHECK_FALSE(episode_label(std::numeric_limits<double>::infinity()).has_value());
  CHECK_FALSE(episode_label(10000).has_value());
  CHECK_FALSE(episode_label(9999.996).has_value());
  CHECK_FALSE(episode_label(1e300).has_value());
}

TEST_CASE("is_playlist") {
  CHECK(is_playlist("#EXTM3U\n#EXT-X-VERSION:3\n"));
  CHECK(is_playlist("#EXTM3U"));
  CHECK(is_playlist("\xEF\xBB\xBF#EXTM3U\n"));
  CHECK(is_playlist(" \r\n\t#EXTM3U\n"));
  CHECK(is_playlist("\xEF\xBB\xBF \n#EXTM3U"));
  CHECK_FALSE(is_playlist(""));
  CHECK_FALSE(is_playlist("<html><body>blocked</body></html>"));
  CHECK_FALSE(is_playlist("#EXTINF:10,\nseg.ts"));
  CHECK_FALSE(is_playlist("x#EXTM3U"));
  CHECK_FALSE(is_playlist("\n\xEF\xBB\xBF#EXTM3U"));
  CHECK_FALSE(is_playlist("#EXTM3"));
}

TEST_CASE("dub_audio_language") {
  CHECK(dub_audio_language(kTwoLanguageMaster) == "eng");
  const std::string tagged =
      "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"JP\",LANGUAGE=\"ja\",URI=\"j.m3u8\"\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"EN\",LANGUAGE=\"en-US\",URI=\"e.m3u8\"\n";
  CHECK(dub_audio_language(tagged) == "eng");
  const std::string english_only =
      "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"EN\",LANGUAGE=\"eng\",URI=\"e.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1,AUDIO=\"a\"\nv.m3u8\n";
  CHECK_FALSE(dub_audio_language(english_only).has_value());
  const std::string japanese_only =
      "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"JP\",LANGUAGE=\"jpn\",URI=\"j.m3u8\"\n";
  CHECK_FALSE(dub_audio_language(japanese_only).has_value());
  const std::string unlabelled =
      "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"EN\",LANGUAGE=\"eng\",URI=\"e.m3u8\"\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"Other\",URI=\"o.m3u8\"\n";
  CHECK_FALSE(dub_audio_language(unlabelled).has_value());
  CHECK_FALSE(dub_audio_language(kMediaPlaylist).has_value());
  CHECK_FALSE(dub_audio_language("").has_value());
}

TEST_CASE("pick_subtitle") {
  CHECK_FALSE(pick_subtitle({}).has_value());
  CHECK(pick_subtitle({subtitle("https://s.example.com/fr.vtt", "fr", "French", true),
                       subtitle("https://s.example.com/en1.vtt", "en", "English", false),
                       subtitle("https://s.example.com/en2.vtt", "en-US", "English (2)", true)}) ==
        "https://s.example.com/en2.vtt");
  CHECK(pick_subtitle({subtitle("https://s.example.com/en1.vtt", "en", "English", false),
                       subtitle("https://s.example.com/en2.vtt", "eng", "English", false)}) ==
        "https://s.example.com/en1.vtt");
  for (const char* lang : {"en", "EN", "eng", "English", "ENGLISH", "en-GB", "EN-us"}) {
    CAPTURE(lang);
    CHECK(pick_subtitle({subtitle("https://s.example.com/x.vtt", lang, "", false)}) ==
          "https://s.example.com/x.vtt");
  }
  for (const char* lang : {"es", "enm", "en_US", "fr", "ja"}) {
    CAPTURE(lang);
    CHECK_FALSE(pick_subtitle({subtitle("https://s.example.com/x.vtt", lang, "English", true)})
                    .has_value());
  }
  CHECK(pick_subtitle({subtitle("https://s.example.com/x.vtt", "", "English (CC)", false)}) ==
        "https://s.example.com/x.vtt");
  CHECK(pick_subtitle({subtitle("https://s.example.com/x.vtt", "", "english", false)}) ==
        "https://s.example.com/x.vtt");
  CHECK_FALSE(
      pick_subtitle({subtitle("https://s.example.com/x.vtt", "", "Spanish", true)}).has_value());
  CHECK(pick_subtitle({subtitle("https://127.0.0.1/en.vtt", "en", "English", true),
                       subtitle("https://s.example.com/a b.vtt", "en", "English", true),
                       subtitle("/subs/en.vtt", "en", "English", true),
                       subtitle("ftp://s.example.com/en.vtt", "en", "English", true),
                       subtitle("https://s.example.com/ok.vtt", "en", "English", false)}) ==
        "https://s.example.com/ok.vtt");
  CHECK(pick_subtitle({subtitle(kSunSubtitle, "en", "English", true)}) == kSunSubtitle);
}

// ===========================================================================
// Page readers
// ===========================================================================

TEST_CASE("parse_search_fixture_golden") {
  auto hits = parse_search(read_fixture("search.html"));
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 5);
  struct Want {
    const char* id;
    const char* title;
    std::int64_t anilist;
    std::int64_t mal;
    std::uint32_t episodes;
    std::uint32_t year;
  };
  const Want want[] = {
      {kShowId, "FLCL", 227, 227, 6, 2000},
      {"xZlBegnY4dQIvxyi9Ym5HJmmYRUyU7id", "FLCL Progressive", 21746, 33010, 6, 2018},
      {"jbJYkqmI5eAfxZZ354CRo84QcD5aqA0h", "FLCL Alternative", 21748, 35842, 6, 2018},
      {"g7k9EC7-5D51wyjaPksr8vz1v6PqlAE_", "FLCL: Grunge", 146472, 51304, 3, 2023},
      {"tHzpIqguLqPa-6qVtj1i9JSKV-8mRVh0", "FLCL: Shoegaze", 146473, 51305, 3, 2023},
  };
  for (std::size_t i = 0; i < 5; ++i) {
    CAPTURE(i);
    const SearchHit& h = (*hits)[i];
    CHECK(h.provider_id == want[i].id);
    CHECK(h.title == want[i].title);
    CHECK(h.title_english == want[i].title);
    CHECK(h.anilist_id == want[i].anilist);
    CHECK(h.mal_id == want[i].mal);
    CHECK(h.total_episodes == want[i].episodes);
    CHECK(h.eps_sub == want[i].episodes);
    CHECK(h.eps_dub == want[i].episodes);
    CHECK(h.year == want[i].year);
  }
  CHECK((*hits)[0].title_native == "\xE3\x83\x95\xE3\x83\xAA\xE3\x82\xAF\xE3\x83\xAA");
}

TEST_CASE("parse_search_item_rules") {
  const std::string html = search_of(
      R"([{id:"short",title:{romaji:"Dropped"}},)"
      R"({id:"aaaaaaaa",title:{romaji:"",english:null}},)"
      R"({id:"bbbbbbbb",title:{english:"Only English",native:"N"},external_ids:{anilist:["0"],mal:["012"]},)"
      R"(episode_count:0,episode_counts:{sub:-1,dub:2}},)"
      R"({id:"cccccccc",title:{native:"Native"},external_ids:{anilist:[42],mal:["x1"]},season_year:-5},)"
      R"("not an object",)"
      R"({id:12345678,title:{romaji:"Numeric id"}},)"
      R"({id:"dddddddd",title:{romaji:"R",english:"E"},external_ids:{anilist:[],mal:"7"},)"
      R"(episode_count:12.5,episode_counts:{sub:"3"},season_year:2024}])");
  auto hits = parse_search(html);
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 3);
  const SearchHit& b = (*hits)[0];
  CHECK(b.provider_id == "bbbbbbbb");
  CHECK(b.title == "Only English");
  CHECK(b.title_english == "Only English");
  CHECK(b.title_native == "N");
  CHECK_FALSE(b.anilist_id.has_value());
  CHECK_FALSE(b.mal_id.has_value());
  CHECK_FALSE(b.total_episodes.has_value());
  CHECK(b.eps_sub == 0);
  CHECK(b.eps_dub == 2);
  CHECK_FALSE(b.year.has_value());
  const SearchHit& c = (*hits)[1];
  CHECK(c.title == "Native");
  CHECK_FALSE(c.title_english.has_value());
  CHECK(c.anilist_id == 42);
  CHECK_FALSE(c.mal_id.has_value());
  CHECK_FALSE(c.year.has_value());
  const SearchHit& d = (*hits)[2];
  CHECK(d.title == "R");
  CHECK(d.title_english == "E");
  CHECK_FALSE(d.anilist_id.has_value());
  CHECK_FALSE(d.mal_id.has_value());
  CHECK_FALSE(d.total_episodes.has_value());
  CHECK(d.eps_sub == 0);
  CHECK(d.year == 2024u);

  auto empty = parse_search(search_of("[]"));
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

TEST_CASE("challenge_page_is_forbidden_403") {
  auto s = parse_search(kChallengePage);
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().kind == ProviderError::Kind::Forbidden);
  CHECK(s.error().status == 403);
  auto e = parse_episodes(kChallengePage);
  REQUIRE_FALSE(e.has_value());
  CHECK(e.error() == ProviderError::forbidden(403));
  auto t = parse_tracks(kChallengePage);
  REQUIRE_FALSE(t.has_value());
  CHECK(t.error() == ProviderError::forbidden(403));
}

TEST_CASE("page_without_data_is_decode") {
  for (const std::string& html :
       {std::string("<html><body>nothing here</body></html>"), std::string(""),
        page_of("[{type:\"data\",data:{},uses:{}}]", {}),
        std::string("<script>kit.start(app, element, {data: [unterminated</script>")}) {
    CAPTURE(html);
    auto s = parse_search(html);
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().kind == ProviderError::Kind::Decode);
    auto e = parse_episodes(html);
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error().kind == ProviderError::Kind::Decode);
    auto t = parse_tracks(html);
    REQUIRE_FALSE(t.has_value());
    CHECK(t.error().kind == ProviderError::Kind::Decode);
  }
  // A key that holds something other than an array does not count.
  auto s = parse_search(search_of("\"x\""));
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_episodes_fixture") {
  auto eps = parse_episodes(read_fixture("watch.html"));
  REQUIRE(eps.has_value());
  CHECK(*eps == std::vector<std::string>{"1", "2", "3", "4", "5", "6"});
}

TEST_CASE("parse_episodes_sorts_dedupes_and_skips") {
  const std::string html = watch_of(
      R"([{number:3},{number:1},{number:2.5},{number:1},{number:-1},{number:"4"},5,)"
      R"({title:"no number"},{number:12.50},{number:10000},{number:0},{number:1.0}])",
      "");
  auto eps = parse_episodes(html);
  REQUIRE(eps.has_value());
  CHECK(*eps == std::vector<std::string>{"0", "1", "2.5", "3", "12.5"});
  auto none = parse_episodes(watch_of("[]", ""));
  REQUIRE(none.has_value());
  CHECK(none->empty());
}

TEST_CASE("a_part_that_does_not_parse_is_skipped") {
  std::string html = page_of(kWatchKit, {"[{episodes:[{number:9}", "[{episodes:[{number:2}]}]"});
  auto eps = parse_episodes(html);
  REQUIRE(eps.has_value());
  CHECK(*eps == std::vector<std::string>{"2"});
}

TEST_CASE("the_page_node_budget_bounds_the_parts_read") {
  std::string params, args, body;
  // Each part reads (under the reader's own limit) as about 25k nodes.
  for (int k = 0; k <= 13; ++k) {
    const std::string p = "p" + std::to_string(k);
    const std::string q = "p" + std::to_string(k - 1);
    params += (k == 0 ? "" : ",") + p;
    args += (k == 0 ? "{}" : ",{}");
    if (k > 0) body += p + ".x=[" + q + "," + q + "];";
  }
  const std::string heavy = "(function(" + params + "){" + body + "return [p13]}(" + args + "))";
  const std::string listing = "[{episodes:[{number:1}]}]";
  auto one = parse_episodes(page_of(kWatchKit, {heavy, listing}));
  REQUIRE(one.has_value());
  CHECK(*one == std::vector<std::string>{"1"});
  auto many =
      parse_episodes(page_of(kWatchKit, {heavy, heavy, heavy, heavy, heavy, heavy, listing}));
  REQUIRE_FALSE(many.has_value());
  CHECK(many.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("parse_tracks_fixture_golden") {
  auto tracks = parse_tracks(read_fixture("watch.html"));
  REQUIRE(tracks.has_value());
  REQUIRE(tracks->size() == 3);
  CHECK((*tracks)[0].kind == "ssub");
  CHECK((*tracks)[1].kind == "dub");
  CHECK((*tracks)[2].kind == "sub");

  const Track& ssub = (*tracks)[0];
  REQUIRE(ssub.sources.size() == 3);
  const Source& sun = ssub.sources[0];
  CHECK(sun.name == "sun");
  REQUIRE(sun.streams.size() == 4);
  CHECK(sun.streams[0] == Stream{"icarus-2-1", kSunSub0, true, kSunReferer, kSunOrigin});
  CHECK(sun.streams[1].server == "cf01a14a6b812a74acacee7e2bf7b3f60f-cloudflare-2");
  CHECK(sun.streams[1].url == kSunSub1);
  CHECK(sun.streams[2].url == kSunSub2);
  CHECK(sun.streams[3] == Stream{"vault-7-direct-2", kSunSub3, true, kSunReferer, kSunOrigin});
  CHECK(sun.subtitles == std::vector<Subtitle>{subtitle(kSunSubtitle, "en", "English", true)});

  const Source& bee = ssub.sources[1];
  CHECK(bee.name == "bee");
  REQUIRE(bee.streams.size() == 5);
  CHECK(bee.streams[0] ==
        Stream{"Vidstream-1", kBeeSubShared, true, std::string("https://megaplay.buzz/"), std::nullopt});
  CHECK(bee.streams[1].server == "Vidstream-2");
  CHECK(bee.streams[1].url == kBeeSubShared);
  CHECK(bee.streams[2].url == kBeeSubHd2);
  CHECK(bee.streams[3].url == kBeeSubHd1);
  CHECK(bee.streams[4] ==
        Stream{"VidPlay", kBeeSubVidPlay, true, std::string("https://vidtube.site/"), std::nullopt});
  CHECK(bee.subtitles == std::vector<Subtitle>{subtitle(kBeeSubtitle0, "en", "English", true),
                                               subtitle(kBeeSubtitle1, "en", "English (2)", false)});

  const Source& bun = ssub.sources[2];
  CHECK(bun.name == "bun");
  CHECK(bun.streams.empty());
  CHECK(bun.subtitles.size() == 2);

  const Track& dub = (*tracks)[1];
  REQUIRE(dub.sources.size() == 4);
  CHECK(dub.sources[0].name == "sun");
  CHECK(dub.sources[0].streams.size() == 4);
  CHECK(dub.sources[1].name == "bee");
  CHECK(dub.sources[1].streams.size() == 4);
  CHECK(dub.sources[1].subtitles.size() == 2);
  CHECK(dub.sources[2].name == "knob");
  CHECK(dub.sources[2].streams ==
        std::vector<Stream>{Stream{"Vidplay", kKnobDub, true, std::string("https://play.vhscdn.to/"),
                                   std::nullopt}});
  CHECK(dub.sources[2].subtitles.empty());
  CHECK(dub.sources[3].name == "bun");
  CHECK(dub.sources[3].streams.empty());

  const Track& sub = (*tracks)[2];
  REQUIRE(sub.sources.size() == 1);
  CHECK(sub.sources[0].name == "knob");
  CHECK(sub.sources[0].streams ==
        std::vector<Stream>{Stream{"Vidplay", kKnobSub, true, std::string("https://play.vhscdn.to/"),
                                   std::nullopt}});
}

TEST_CASE("parse_tracks_shape_rules") {
  const std::string tracks =
      R"([{providers:[]},"junk",{track:"sub",providers:["junk",{servers:[)"
      R"({server:"n",headers:null,streams:[{url:"https://n.example.com/1.m3u8",format:"hls"}]},)"
      R"({server:"lc",headers:{referer:"https://r.example.com/",ORIGIN:"https://r.example.com",)"
      R"(Accept:"x"},streams:[{url:"https://r.example.com/1.m3u8",format:"HLS"},{url:5},)"
      R"({format:"hls"},"junk",{url:"https://r.example.com/v.mp4",format:"mp4"}]},)"
      R"({server:"blank",headers:{Referer:"",Origin:7},streams:[{url:"https://b.example.com/1.m3u8"}]},)"
      R"({server:"nostreams",headers:{},embed:{url:"https://e.example.com/"}}],)"
      R"(subtitles:[{file:"https://s.example.com/a.vtt"},{language:"en",label:"English"},)"
      R"({file:7},{file:"https://s.example.com/b.vtt",language:"en",label:"English",default:"yes"}]}]}])";
  auto parsed = parse_tracks(watch_of("[]", tracks));
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->size() == 1);
  const Track& t = (*parsed)[0];
  CHECK(t.kind == "sub");
  REQUIRE(t.sources.size() == 1);
  const Source& src = t.sources[0];
  CHECK(src.name.empty());
  CHECK(src.streams ==
        std::vector<Stream>{
            Stream{"n", "https://n.example.com/1.m3u8", true, std::nullopt, std::nullopt},
            Stream{"lc", "https://r.example.com/1.m3u8", true, std::string("https://r.example.com/"),
                   std::string("https://r.example.com")},
            Stream{"lc", "https://r.example.com/v.mp4", false, std::string("https://r.example.com/"),
                   std::string("https://r.example.com")},
            Stream{"blank", "https://b.example.com/1.m3u8", false, std::nullopt, std::nullopt},
        });
  CHECK(src.subtitles ==
        std::vector<Subtitle>{subtitle("https://s.example.com/a.vtt", "", "", false),
                              subtitle("https://s.example.com/b.vtt", "en", "English", false)});
}

TEST_CASE("watch_page_without_its_sources_part") {
  const std::string html = watch_without_sources();
  auto eps = parse_episodes(html);
  REQUIRE(eps.has_value());
  CHECK(eps->size() == 6);
  auto tracks = parse_tracks(html);
  REQUIRE_FALSE(tracks.has_value());
  CHECK(tracks.error().kind == ProviderError::Kind::Decode);
}

// ===========================================================================
// Candidates
// ===========================================================================

TEST_CASE("candidates_sub_fixture") {
  auto tracks = parse_tracks(read_fixture("watch.html"));
  REQUIRE(tracks.has_value());
  const auto list = candidates(*tracks, Translation::Sub);
  CHECK(urls_of(list) == std::vector<std::string>{kSunSub0, kSunSub1, kSunSub2, kSunSub3,
                                                  kBeeSubShared, kBeeSubHd2, kBeeSubHd1,
                                                  kBeeSubVidPlay, kKnobSub});
  REQUIRE(list.size() == 9);
  for (std::size_t i = 0; i < 4; ++i) CHECK(list[i].sub_url == kSunSubtitle);
  for (std::size_t i = 4; i < 8; ++i) CHECK(list[i].sub_url == kBeeSubtitle0);
  CHECK_FALSE(list[8].sub_url.has_value());
  CHECK(list[0].stream == Stream{"icarus-2-1", kSunSub0, true, kSunReferer, kSunOrigin});
  CHECK(list[4].stream.server == "Vidstream-1");
  CHECK(list[7].stream.referer == "https://vidtube.site/");
  CHECK(list[8].stream.referer == "https://play.vhscdn.to/");
}

TEST_CASE("candidates_dub_fixture") {
  auto tracks = parse_tracks(read_fixture("watch.html"));
  REQUIRE(tracks.has_value());
  const auto list = candidates(*tracks, Translation::Dub);
  CHECK(urls_of(list) == std::vector<std::string>{kSunDub0, kSunDub1, kSunDub2, kSunDub3, kBeeDub0,
                                                  kBeeDub1, kBeeDub2, kBeeDub3, kKnobDub});
  for (const auto& c : list) CHECK_FALSE(c.sub_url.has_value());
  REQUIRE_FALSE(list.empty());
  CHECK(list[0].stream.origin == kSunOrigin);
}

TEST_CASE("candidates_group_order_and_dedupe") {
  const Subtitle en = subtitle("https://subs.example.com/en.vtt", "en", "English", false);
  const Subtitle fr = subtitle("https://subs.example.com/fr.vtt", "fr", "French", true);
  const std::vector<Track> tracks = {
      Track{"ssub", {Source{"a", {stream("https://a.example.com/1.m3u8")}, {fr}}}},
      Track{"sub",
            {Source{"b", {stream("https://b.example.com/1.m3u8"), stream("https://c.example.com/2.m3u8")},
                    {en}}}},
      Track{"ssub",
            {Source{"c", {stream("https://c.example.com/1.m3u8"), stream("https://c.example.com/2.m3u8")},
                    {en}},
             Source{"e", {stream("https://e.example.com/1.m3u8")}, {}}}},
      Track{"dub", {Source{"d", {stream("https://d.example.com/1.m3u8")}, {en}}}},
      Track{"raw", {Source{"r", {stream("https://r.example.com/1.m3u8")}, {en}}}},
  };
  const auto sub = candidates(tracks, Translation::Sub);
  CHECK(urls_of(sub) == std::vector<std::string>{"https://c.example.com/1.m3u8",
                                                 "https://c.example.com/2.m3u8",
                                                 "https://b.example.com/1.m3u8",
                                                 "https://a.example.com/1.m3u8",
                                                 "https://e.example.com/1.m3u8"});
  REQUIRE(sub.size() == 5);
  CHECK(sub[0].sub_url == en.url);
  CHECK(sub[1].sub_url == en.url);
  CHECK_FALSE(sub[2].sub_url.has_value());
  CHECK_FALSE(sub[3].sub_url.has_value());
  CHECK_FALSE(sub[4].sub_url.has_value());

  const auto dub = candidates(tracks, Translation::Dub);
  REQUIRE(dub.size() == 1);
  CHECK(dub[0].stream.url == "https://d.example.com/1.m3u8");
  CHECK_FALSE(dub[0].sub_url.has_value());

  CHECK(candidates({}, Translation::Sub).empty());
  CHECK(candidates({Track{"sub", {Source{"x", {}, {}}}}}, Translation::Sub).empty());
}

TEST_CASE("candidates_ssub_without_english_from_a_page") {
  const std::string tracks =
      R"([{track:"ssub",providers:[{provider:"a",servers:[{server:"a1",)"
      R"(headers:{Referer:"https://a.example.com/"},streams:[{url:"https://a.example.com/1.m3u8",)"
      R"(format:"hls"}]}],subtitles:[{language:"fr",label:"French",file:"https://a.example.com/fr.vtt",)"
      R"(default:true}]}]},{track:"sub",providers:[{provider:"b",servers:[{server:"b1",headers:null,)"
      R"(streams:[{url:"https://b.example.com/1.m3u8",format:"hls"}]}],subtitles:[]}]}])";
  auto parsed = parse_tracks(watch_of("[{number:1}]", tracks));
  REQUIRE(parsed.has_value());
  const auto list = candidates(*parsed, Translation::Sub);
  CHECK(urls_of(list) ==
        std::vector<std::string>{"https://b.example.com/1.m3u8", "https://a.example.com/1.m3u8"});
  REQUIRE(list.size() == 2);
  CHECK_FALSE(list[0].sub_url.has_value());
  CHECK_FALSE(list[1].sub_url.has_value());
  CHECK(list[1].stream.referer == "https://a.example.com/");
}

TEST_CASE("candidates_stream_rules") {
  const std::vector<Track> tracks = {Track{
      "sub",
      {Source{"x",
              {
                  stream("http://plain.example.com/1.m3u8"),
                  stream("https://127.0.0.1/1.m3u8"),
                  stream("https://10.0.0.1/1.m3u8"),
                  stream("https://ok.example.com/a b.m3u8"),
                  stream("https://r.example.com/1.m3u8", std::string("https://r.example.com/ x")),
                  stream("https://r.example.com/2.m3u8", std::string("")),
                  stream("https://o.example.com/1.m3u8", std::string("https://o.example.com/"),
                         std::string("https://o.example.com\n")),
                  stream("https://m.example.com/1.m3u8", std::string("https://m.example.com/"),
                         std::string("https://m.example.com/")),
                  stream("https://p.example.com/1.m3u8", std::nullopt,
                         std::string("https://p.example.com:8443")),
                  stream("https://q.example.com/1.m3u8", std::nullopt, std::string("q.example.com")),
                  stream("https://q.example.com/2.m3u8", std::nullopt,
                         std::string("https://q.example.com:port")),
                  stream("https://q.example.com/3.m3u8", std::nullopt,
                         std::string("https://q.example.com?x")),
                  stream("https://u.example.com/ep.mp4", std::string("https://u.example.com/"),
                         std::nullopt, false),
                  stream("https://m.example.com/1.m3u8"),
              },
              {}}}}};
  const auto list = candidates(tracks, Translation::Sub);
  CHECK(urls_of(list) == std::vector<std::string>{
                             "https://m.example.com/1.m3u8", "https://p.example.com/1.m3u8",
                             "https://q.example.com/1.m3u8", "https://q.example.com/2.m3u8",
                             "https://q.example.com/3.m3u8", "https://u.example.com/ep.mp4"});
  REQUIRE(list.size() == 6);
  CHECK(list[0].stream.referer == "https://m.example.com/");
  CHECK_FALSE(list[0].stream.origin.has_value());
  CHECK(list[1].stream.origin == "https://p.example.com:8443");
  CHECK_FALSE(list[2].stream.origin.has_value());
  CHECK_FALSE(list[3].stream.origin.has_value());
  CHECK_FALSE(list[4].stream.origin.has_value());
  CHECK_FALSE(list[5].stream.hls);
}

// ===========================================================================
// Provider surface
// ===========================================================================

TEST_CASE("provider_surface") {
  auto p = Miruro::create();
  REQUIRE(p.has_value());
  CHECK(p->name() == "miruro");
  CHECK(p->display_name() == "Miruro");
  CHECK(p->supports_search());
  Enrichment show;
  show.anilist_id = 227;
  show.mal_id = 227;
  CHECK_FALSE(p->canonical_key(show).has_value());
  auto cover = p->cover_request("https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/x.jpg");
  REQUIRE(cover.has_value());
  CHECK(cover->url == "https://s4.anilist.co/file/anilistcdn/media/anime/cover/large/x.jpg");
  CHECK_FALSE(p->cover_request("").has_value());
  CHECK_FALSE(p->cover_request("/relative.jpg").has_value());
  CHECK_FALSE(p->cover_request("https://x.example.com/a b.jpg").has_value());
}

namespace {

// Answers one connection per scripted response, in order, and keeps each
// request's head.
struct Server {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;
  std::mutex mu;
  std::vector<std::string> seen;

  explicit Server(std::vector<std::string> responses) {
    std::signal(SIGPIPE, SIG_IGN);
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
    REQUIRE(::listen(listen_fd, 16) == 0);

    thread = std::thread([this, responses = std::move(responses)]() {
      for (const auto& resp : responses) {
        if (!readable(listen_fd, 15)) return;
        const int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;
        std::string head;
        char buf[8192];
        while (head.find("\r\n\r\n") == std::string::npos && head.size() < 65536 &&
               readable(cfd, 5)) {
          const ssize_t n = ::read(cfd, buf, sizeof(buf));
          if (n <= 0) break;
          head.append(buf, static_cast<std::size_t>(n));
        }
        {
          std::lock_guard<std::mutex> lock(mu);
          seen.push_back(head);
        }
        std::size_t off = 0;
        while (off < resp.size()) {
          const ssize_t n = ::write(cfd, resp.data() + off, resp.size() - off);
          if (n <= 0) break;
          off += static_cast<std::size_t>(n);
        }
        ::close(cfd);
      }
    });
  }

  static bool readable(int fd, long secs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{};
    tv.tv_sec = secs;
    return ::select(fd + 1, &set, nullptr, nullptr, &tv) > 0;
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

  // Every request served, once the script has run out.
  std::vector<std::string> requests() {
    if (thread.joinable()) thread.join();
    std::lock_guard<std::mutex> lock(mu);
    return seen;
  }

  // A connection waiting that the script had no answer for.
  bool pending() const { return readable(listen_fd, 0); }

  ~Server() {
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) ::close(listen_fd);
  }
};

std::string response(int status, std::string_view body,
                     std::string_view type = "text/html; charset=utf-8") {
  const char* reason = status == 200   ? "OK"
                       : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found"
                                       : "Service Unavailable";
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " +
         std::string(type) + "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + std::string(body);
}

std::string playlist(std::string_view body) {
  return response(200, body, "application/vnd.apple.mpegurl");
}

std::string redirect(int status, std::string_view location) {
  return "HTTP/1.1 " + std::to_string(status) + " Moved\r\nLocation: " + std::string(location) +
         "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

bool has_header(const std::string& head, const std::string& line) {
  return contains(head, "\r\n" + line + "\r\n");
}

}  // namespace

TEST_CASE("transport_search") {
  Server srv({response(200, read_fixture("search.html"))});
  auto p = Miruro::with_endpoints(srv.url() + "/", "");
  REQUIRE(p.has_value());
  auto hits = p->search("flcl", SearchOptions{});
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 5);
  CHECK((*hits)[0].provider_id == kShowId);
  CHECK((*hits)[0].anilist_id == 227);
  const auto reqs = srv.requests();
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].starts_with("GET /search?q=flcl HTTP/1.1\r\n"));
  CHECK(has_header(reqs[0], "Referer: " + srv.url() + "/"));
  CHECK(has_header(reqs[0], "Accept: text/html,application/xhtml+xml,*/*;q=0.8"));
}

TEST_CASE("transport_search_encodes_the_query_and_honours_the_limit") {
  Server srv({response(200, read_fixture("search.html"))});
  auto p = Miruro::with_endpoints(srv.url(), "");
  REQUIRE(p.has_value());
  SearchOptions opts;
  opts.limit = 2;
  auto hits = p->search("fooly cooly/2&x", opts);
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  CHECK((*hits)[1].title == "FLCL Progressive");
  const auto reqs = srv.requests();
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].starts_with("GET /search?q=fooly%20cooly%2F2%26x HTTP/1.1\r\n"));
}

TEST_CASE("transport_search_page_two_is_empty_without_a_request") {
  Server srv({});
  auto p = Miruro::with_endpoints(srv.url(), "");
  REQUIRE(p.has_value());
  SearchOptions opts;
  opts.page = 2;
  auto hits = p->search("flcl", opts);
  REQUIRE(hits.has_value());
  CHECK(hits->empty());
  CHECK(srv.requests().empty());
  CHECK_FALSE(srv.pending());
}

TEST_CASE("transport_challenge_and_403_are_forbidden") {
  Server srv({response(200, kChallengePage), response(403, kChallengePage)});
  auto p = Miruro::with_endpoints(srv.url(), "");
  REQUIRE(p.has_value());
  auto first = p->search("flcl", SearchOptions{});
  REQUIRE_FALSE(first.has_value());
  CHECK(first.error() == ProviderError::forbidden(403));
  auto second = p->episodes(kShowId, Translation::Sub, std::nullopt);
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error() == ProviderError::forbidden(403));
  CHECK(srv.requests().size() == 2);
}

TEST_CASE("transport_episodes_sub") {
  Server srv({response(200, read_fixture("watch.html"))});
  auto p = Miruro::with_endpoints(srv.url(), "");
  REQUIRE(p.has_value());
  auto eps = p->episodes(kShowId, Translation::Sub, 99);
  REQUIRE(eps.has_value());
  CHECK(*eps == std::vector<std::string>{"1", "2", "3", "4", "5", "6"});
  const auto reqs = srv.requests();
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].starts_with(std::string("GET /watch/") + kShowId + "/x?ep=1 HTTP/1.1\r\n"));
  CHECK(has_header(reqs[0], "Referer: " + srv.url() + "/"));
}

TEST_CASE("watch_slug") {
  const std::string id = kShowId;
  CHECK(watch_slug("https://miruro.cx/watch/" + id + "/flcl", id) == "flcl");
  CHECK(watch_slug("https://miruro.cx/watch/" + id + "/bocchi-the-rock?ep=3", id) ==
        "bocchi-the-rock");
  CHECK(watch_slug("http://127.0.0.1:8080/watch/" + id + "/a.b_c~%E3%81%82#top", id) ==
        "a.b_c~%E3%81%82");
  CHECK_FALSE(watch_slug("https://miruro.cx/watch/" + id + "/", id).has_value());
  CHECK_FALSE(watch_slug("https://miruro.cx/watch/" + id, id).has_value());
  CHECK_FALSE(watch_slug("https://miruro.cx/watch/" + id + "/a/b", id).has_value());
  CHECK_FALSE(watch_slug("https://miruro.cx/watch/otherotherother/flcl", id).has_value());
  CHECK_FALSE(watch_slug("https://miruro.cx/search?q=flcl", id).has_value());
  CHECK_FALSE(watch_slug("/watch/" + id + "/flcl", id).has_value());
  CHECK_FALSE(
      watch_slug("https://miruro.cx/watch/" + id + "/" + std::string(201, 'a'), id).has_value());
}

TEST_CASE("transport_watch_page_follows_the_site_to_the_shows_own_slug") {
  const std::string id = kShowId;
  const std::string page = read_fixture("watch.html");
  // A relative Location, then an absolute one on the same origin.
  Server first({redirect(307, "/watch/" + id + "/flcl?ep=1"), response(200, page)});
  auto p = Miruro::with_endpoints(first.url(), "");
  REQUIRE(p.has_value());
  auto eps = p->episodes(id, Translation::Sub, std::nullopt);
  REQUIRE(eps.has_value());
  CHECK(eps->size() == 6);
  const auto reqs = first.requests();
  REQUIRE(reqs.size() == 2);
  CHECK(reqs[0].starts_with("GET /watch/" + id + "/x?ep=1 HTTP/1.1\r\n"));
  CHECK(reqs[1].starts_with("GET /watch/" + id + "/flcl?ep=1 HTTP/1.1\r\n"));
  CHECK(has_header(reqs[1], "Referer: " + first.url() + "/"));
}

TEST_CASE("transport_watch_page_remembers_the_slug_for_the_run") {
  const std::string id = kShowId;
  const std::string page = read_fixture("watch.html");
  Server srv({redirect(308, "/watch/" + id + "/flcl?ep=1"), response(200, page),
              response(200, page)});
  auto p = Miruro::with_endpoints(srv.url(), "");
  REQUIRE(p.has_value());
  REQUIRE(p->episodes(id, Translation::Sub, std::nullopt).has_value());
  REQUIRE(p->episodes(id, Translation::Sub, std::nullopt).has_value());
  const auto reqs = srv.requests();
  REQUIRE(reqs.size() == 3);
  CHECK(reqs[2].starts_with("GET /watch/" + id + "/flcl?ep=1 HTTP/1.1\r\n"));
}

TEST_CASE("transport_redirect_off_the_site_is_not_followed") {
  const std::string id = kShowId;
  Server srv({redirect(302, "https://elsewhere.example.com/watch/" + id + "/flcl?ep=1")});
  auto p = Miruro::with_endpoints(srv.url(), "");
  REQUIRE(p.has_value());
  auto eps = p->episodes(id, Translation::Sub, std::nullopt);
  REQUIRE_FALSE(eps.has_value());
  CHECK(eps.error().kind == ProviderError::Kind::Http);
  CHECK(eps.error().status == 302);
  CHECK(srv.requests().size() == 1);
}

TEST_CASE("transport_redirects_are_bounded") {
  const std::string id = kShowId;
  std::vector<std::string> script;
  for (int k = 0; k <= miruro::kMaxRedirects; ++k) {
    script.push_back(redirect(307, "/watch/" + id + "/hop" + std::to_string(k) + "?ep=1"));
  }
  Server srv(std::move(script));
  auto p = Miruro::with_endpoints(srv.url(), "");
  REQUIRE(p.has_value());
  auto eps = p->episodes(id, Translation::Sub, std::nullopt);
  REQUIRE_FALSE(eps.has_value());
  CHECK(eps.error().status == 307);
  CHECK(srv.requests().size() == static_cast<std::size_t>(miruro::kMaxRedirects) + 1);
}

TEST_CASE("transport_episodes_dub") {
  {
    Server srv({response(200, read_fixture("watch.html"))});
    auto p = Miruro::with_endpoints(srv.url(), "");
    REQUIRE(p.has_value());
    auto eps = p->episodes(kShowId, Translation::Dub, std::nullopt);
    REQUIRE(eps.has_value());
    CHECK(eps->size() == 6);
  }
  {
    Server srv({response(200, watch_without_dub())});
    auto p = Miruro::with_endpoints(srv.url(), "");
    REQUIRE(p.has_value());
    auto eps = p->episodes(kShowId, Translation::Dub, std::nullopt);
    REQUIRE(eps.has_value());
    CHECK(eps->empty());
  }
  {
    Server srv({response(200, watch_without_sources())});
    auto p = Miruro::with_endpoints(srv.url(), "");
    REQUIRE(p.has_value());
    auto eps = p->episodes(kShowId, Translation::Dub, std::nullopt);
    REQUIRE(eps.has_value());
    CHECK(eps->size() == 6);
  }
}

TEST_CASE("transport_resolve_takes_the_first_candidate_that_answers") {
  Server srv({response(200, read_fixture("watch.html")), playlist(kMediaPlaylist)});
  auto p = Miruro::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto link = p->resolve(kShowId, "1", Translation::Sub, Quality::P480);
  REQUIRE(link.has_value());
  CHECK(link->url == kSunSub0);
  CHECK(link->referer == kSunReferer);
  CHECK(link->origin == kSunOrigin);
  CHECK(link->user_agent == http::kBrowserUserAgent);
  CHECK(link->cloaked_segments);
  CHECK_FALSE(link->decloak_segments);
  CHECK(link->sub_url == kSunSubtitle);
  CHECK_FALSE(link->audio_language.has_value());
  CHECK_FALSE(link->playlist_cipher.has_value());
  CHECK_FALSE(link->resolution.has_value());

  const auto reqs = srv.requests();
  REQUIRE(reqs.size() == 2);
  CHECK(reqs[0].starts_with(std::string("GET /watch/") + kShowId + "/x?ep=1 HTTP/1.1\r\n"));
  CHECK(reqs[1].starts_with("GET /m/ASx0CLBlKVqlgaO1SWpJg2d9.m3u8 HTTP/1.1\r\n"));
  CHECK(has_header(reqs[1], "Referer: https://ultracloud.cc/"));
  CHECK(has_header(reqs[1], "Origin: https://ultracloud.cc"));
  CHECK(has_header(reqs[1], std::string("User-Agent: ") + http::kBrowserUserAgent));
}

TEST_CASE("transport_resolve_falls_through_dead_and_non_playlist_streams") {
  Server srv({response(200, read_fixture("watch.html")), response(403, "no"),
              response(200, "<html>a parked domain</html>"), playlist("\xEF\xBB\xBF\n#EXTM3U\n")});
  auto p = Miruro::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto link = p->resolve(kShowId, "1", Translation::Sub, Quality::Best);
  REQUIRE(link.has_value());
  CHECK(link->url == kSunSub2);
  CHECK(link->sub_url == kSunSubtitle);
  const auto reqs = srv.requests();
  REQUIRE(reqs.size() == 4);
  CHECK(reqs[1].starts_with("GET /m/ASx0CLBlKVqlgaO1SWpJg2d9.m3u8 "));
  CHECK(reqs[2].starts_with("GET /m/AdlnjjhRjH7BmDRuaW5puKEl.m3u8 "));
  CHECK(reqs[3].starts_with("GET /m/AUF59PfjpdZcrFcnonXKrSsS.m3u8 "));
}

TEST_CASE("transport_resolve_gives_up_after_the_probe_budget") {
  std::vector<std::string> script{response(200, read_fixture("watch.html"))};
  for (std::size_t i = 0; i + 1 < kMaxProbes; ++i) script.push_back(response(404, "gone"));
  script.push_back(response(503, "down"));
  Server srv(std::move(script));
  auto p = Miruro::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto link = p->resolve(kShowId, "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(link.has_value());
  CHECK(link.error() == ProviderError::server(503));
  const auto reqs = srv.requests();
  CHECK(reqs.size() == 1 + kMaxProbes);
  REQUIRE(reqs.size() >= 6);
  CHECK(reqs[5].starts_with("GET /anime/" SUB_HASH "/master.m3u8" SUB_TOKEN " "));
  CHECK(has_header(reqs[5], "Referer: https://megaplay.buzz/"));
  CHECK_FALSE(contains(reqs[5], "\r\nOrigin:"));
  CHECK_FALSE(srv.pending());
}

TEST_CASE("transport_resolve_dub_reads_the_audio_language") {
  {
    Server srv({response(200, read_fixture("watch.html")), playlist(kTwoLanguageMaster)});
    auto p = Miruro::with_endpoints(srv.url(), srv.url());
    REQUIRE(p.has_value());
    auto link = p->resolve(kShowId, "1", Translation::Dub, Quality::Best);
    REQUIRE(link.has_value());
    CHECK(link->url == kSunDub0);
    CHECK(link->audio_language == "eng");
    CHECK_FALSE(link->sub_url.has_value());
    CHECK(link->origin == kSunOrigin);
    const auto reqs = srv.requests();
    REQUIRE(reqs.size() == 2);
    CHECK(reqs[1].starts_with("GET /m/ARUHljObJtWyoEgFUbFWhnBN.m3u8 "));
  }
  {
    Server srv({response(200, read_fixture("watch.html")), playlist(kMediaPlaylist)});
    auto p = Miruro::with_endpoints(srv.url(), srv.url());
    REQUIRE(p.has_value());
    auto link = p->resolve(kShowId, "1", Translation::Dub, Quality::Best);
    REQUIRE(link.has_value());
    CHECK_FALSE(link->audio_language.has_value());
  }
}

TEST_CASE("transport_resolve_takes_a_non_hls_stream_unprobed") {
  const std::string tracks =
      R"([{track:"sub",providers:[{provider:"v",servers:[{server:"file",)"
      R"(headers:{Referer:"https://v.example.com/"},streams:[{url:"https://v.example.com/ep.mp4",)"
      R"(format:"mp4"}]}],subtitles:[]}]}])";
  Server srv({response(200, watch_of("[{number:2.5}]", tracks))});
  auto p = Miruro::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto link = p->resolve("abcdefgh", "2.5", Translation::Sub, Quality::Best);
  REQUIRE(link.has_value());
  CHECK(link->url == "https://v.example.com/ep.mp4");
  CHECK(link->referer == "https://v.example.com/");
  CHECK_FALSE(link->origin.has_value());
  CHECK_FALSE(link->cloaked_segments);
  const auto reqs = srv.requests();
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].starts_with("GET /watch/abcdefgh/x?ep=2.5 HTTP/1.1\r\n"));
}

TEST_CASE("transport_resolve_without_candidates_or_tracks") {
  Server srv({response(200, watch_without_dub()), response(200, watch_without_sources())});
  auto p = Miruro::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  auto none = p->resolve(kShowId, "1", Translation::Dub, Quality::Best);
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error() == ProviderError::decode("no stream for this track"));
  auto missing = p->resolve(kShowId, "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().kind == ProviderError::Kind::Decode);
  CHECK(srv.requests().size() == 2);
}

TEST_CASE("transport_bad_input_is_rejected_before_any_request") {
  Server srv({});
  auto p = Miruro::with_endpoints(srv.url(), srv.url());
  REQUIRE(p.has_value());
  for (const char* id : {"short", "", "../../etc/passwd", "abcdefgh/x", "abcd efgh"}) {
    CAPTURE(id);
    auto r = p->resolve(id, "1", Translation::Sub, Quality::Best);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == ProviderError::decode("invalid show id"));
    auto e = p->episodes(id, Translation::Sub, std::nullopt);
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error() == ProviderError::decode("invalid show id"));
  }
  for (const char* ep : {"", "1a", "-1", ".5", "1.234", "123456", "1&x=2", "1 "}) {
    CAPTURE(ep);
    auto r = p->resolve(kShowId, ep, Translation::Sub, Quality::Best);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == ProviderError::decode("invalid episode"));
  }
  CHECK(srv.requests().empty());
  CHECK_FALSE(srv.pending());
}
