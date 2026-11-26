// uneval_tests.cpp — the reader for the JavaScript value expressions a
// SvelteKit page ships its data as: literals, escapes, numbers, call forms,
// the hoisting function, the limits, and the Miruro fixtures' payloads.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "../src/uneval.hpp"

using shigoku::uneval::kMaxDepth;
using shigoku::uneval::parse;
using json = nlohmann::json;

namespace {

json read(std::string_view text) {
  auto v = parse(text);
  REQUIRE_MESSAGE(v.has_value(), "did not parse: " << text);
  return *v;
}

json J(const char* text) { return json::parse(text); }

std::string read_fixture(const std::string& name) {
  const std::string path = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/miruro/" + name;
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "missing fixture: " << path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The kit.start data, then each streamed part, as the page lays them out.
std::vector<std::string_view> payload_texts(std::string_view html) {
  std::vector<std::string_view> out;
  const auto kit = html.find("kit.start(");
  if (kit != std::string_view::npos) {
    const auto d = html.find("data:", kit);
    if (d != std::string_view::npos) out.push_back(html.substr(d + 5));
  }
  for (auto at = html.find(".resolve("); at != std::string_view::npos;
       at = html.find(".resolve(", at + 1)) {
    const auto arrow = html.find("() =>", at);
    if (arrow == std::string_view::npos) break;
    out.push_back(html.substr(arrow + 5));
  }
  return out;
}

std::string param(int k) { return "p" + std::to_string(k); }

}  // namespace

TEST_CASE("objects_bare_quoted_and_numeric_keys") {
  CHECK(read(R"({a:1,"b c":2,'d':3,7:4,1.5:5,$x:6,_y:7})") ==
        J(R"({"a":1,"b c":2,"d":3,"7":4,"1.5":5,"$x":6,"_y":7})"));
  CHECK(read("{}") == json::object());
  CHECK(read("{ a : { b : [ ] } , }") == J(R"({"a":{"b":[]}})"));
  CHECK(read(R"({default:true,"__proto__":1})") == J(R"({"default":true,"__proto__":1})"));
}

TEST_CASE("arrays_with_holes_and_a_trailing_comma") {
  CHECK(read("[1,,3]") == J("[1,null,3]"));
  CHECK(read("[,]") == J("[null]"));
  CHECK(read("[,1]") == J("[null,1]"));
  CHECK(read("[1,2,]") == J("[1,2]"));
  CHECK(read("[1,,]") == J("[1,null]"));
  CHECK(read("[ ]") == json::array());
  CHECK(read("[[1],[2,[3]]]") == J("[[1],[2,[3]]]"));
}

TEST_CASE("strings_and_every_escape") {
  CHECK(read(R"("a\nb\tc\rd\be\ff\vg")") == "a\nb\tc\rd\be\ff\vg");
  CHECK(read(R"("q\"q\\q\'q\/q")") == "q\"q\\q'q/q");
  CHECK(read(R"("\u003Cscript\u003E</")") == "<script></");
  CHECK(read(R"("\x41\xe9")") == "A\xc3\xa9");
  CHECK(read(R"("\uD83D\uDE00")") == "\xF0\x9F\x98\x80");
  CHECK(read(R"("\u{1F600}\u{41}")") == "\xF0\x9F\x98\x80" "A");
  CHECK(read(R"("\uD83Dx")") == "\xEF\xBF\xBDx");
  CHECK(read(R"("\uDE00")") == "\xEF\xBF\xBD");
  CHECK(read("\"a\\\nb\"") == "ab");
  CHECK(read(R"('single "quoted"')") == "single \"quoted\"");
  CHECK(read(R"("\q")") == "q");
  CHECK(read(R"("a\0b")").get<std::string>() == std::string("a\0b", 3));
  CHECK(read("\"\xE3\x83\x95\xE3\x83\xAA\"") == "\xE3\x83\x95\xE3\x83\xAA");
  CHECK(read(R"("")") == "");
}

TEST_CASE("numbers") {
  CHECK(read("12") == 12);
  CHECK(read("12").is_number_integer());
  CHECK(read("-7") == -7);
  CHECK(read("-0") == 0);
  CHECK(read("+3") == 3);
  CHECK(read(".5") == 0.5);
  CHECK(read("-.25") == -0.25);
  CHECK(read("0.3") == 0.3);
  CHECK(read("0.1") == 0.1);
  CHECK(read("12.5") == 12.5);
  CHECK(read("12.5").is_number_float());
  CHECK(read("1e21") == 1e21);
  CHECK(read("2.5E-3") == 0.0025);
  CHECK(read("1e+2") == 100.0);
  CHECK(read("1e-7") == 1e-7);
  CHECK(read("123.456e2") == 12345.6);
  CHECK(read("5n") == 5);
  CHECK(read("9223372036854775807") == INT64_MAX);
  CHECK(read("-9223372036854775807") == -INT64_MAX);
  const json big = read("18446744073709551616");
  CHECK(big.is_number_float());
  CHECK(big.get<double>() > 1.8e19);
  CHECK(big.get<double>() < 1.9e19);
  CHECK(read("-9223372036854775808").is_number_float());
  CHECK(read("1e999").is_null());
  CHECK_FALSE(parse("[1e]").has_value());
}

TEST_CASE("words") {
  CHECK(read("true") == true);
  CHECK(read("false") == false);
  CHECK(read("null").is_null());
  CHECK(read("void 0").is_null());
  CHECK(read("undefined").is_null());
  CHECK(read("NaN").is_null());
  CHECK(read("Infinity").is_null());
  CHECK(read("-Infinity").is_null());
  CHECK(read("[true,void 0,-Infinity,NaN]") == J("[true,null,null,null]"));
}

TEST_CASE("call_forms") {
  CHECK(read("new Date(5)") == 5);
  CHECK(read("new Date(1700000000000)") == 1700000000000LL);
  CHECK(read("new Set([1,2])") == J("[1,2]"));
  CHECK(read(R"(new Map([["a",1]]))") == J(R"([["a",1]])"));
  CHECK(read("Object(1)") == 1);
  CHECK(read("Object(\"s\")") == "s");
  CHECK(read("a.b.defer(4)") == 4);
  CHECK(read("__sveltekit_x.defer(12)") == 12);
  CHECK(read("Array(2)") == J("[null,null]"));
  CHECK(read("Array(0)") == json::array());
  CHECK(read("Array(5000)") == json::array());
  CHECK(read("Object.create(null)") == json::object());
  CHECK(read("f()").is_null());
  CHECK(read("location").is_null());
  CHECK(read("{a:new Date(1),b:x.y.z(1,2,3)}") == J(R"({"a":1,"b":3})"));
}

TEST_CASE("hoisting_with_index_and_key_assignments") {
  CHECK(read(R"((function(a,b){a[0]=1;a[2]="x";b.k=a;b["q r"]=2;return [a,b]}(Array(3),{})))") ==
        J(R"([[1,null,"x"],{"k":[1,null,"x"],"q r":2}])"));
  CHECK(read(R"((function(a){a[3]=1;return a}(Array(1))))") == J("[null,null,null,1]"));
  CHECK(read(R"((function(a){a.x=1;a.x=2;return a}({})))") == J(R"({"x":2})"));
  CHECK(read(R"((function f(a){a.x=1;return a}({})))") == J(R"({"x":1})"));
  CHECK(read(R"(( function ( a , b ) { a . x = b ; return [ a ] } ( { } , 2 ) ))") ==
        J(R"([{"x":2}])"));
  CHECK(read(R"((function(a,b){return [a,b]}({})))") == J("[{},null]"));
  CHECK(read(R"((function(a){return a}({},{})))") == json::object());
}

TEST_CASE("hoisting_parameter_that_is_a_string") {
  CHECK(read(R"((function(a,b){a.s=b;return [a,b,{t:b}]}({},"str")))") ==
        J(R"([{"s":"str"},"str",{"t":"str"}])"));
}

TEST_CASE("hoisting_forward_references_and_a_cycle") {
  CHECK(read(R"((function(a,b){a.next=b;b.v=2;return a}({},{})))") ==
        J(R"({"next":{"v":2}})"));
  CHECK(read(R"((function(a,b){a.b=b;b.c=[a];return [b,a]}({},{})))") ==
        J(R"([{"c":[{"b":null}]},{"b":null}])"));
  CHECK(read(R"((function(a){a.self=a;a.n=1;return a}({})))") == J(R"({"self":null,"n":1})"));
}

TEST_CASE("hoisting_alternative_spelling") {
  CHECK(read(R"((function(a){a.x=1;return [a,a]})({}))") == J(R"([{"x":1},{"x":1}])"));
}

TEST_CASE("hoisting_unknown_statement_is_skipped") {
  CHECK(read(R"((function(a){var q=1;"use strict";;a.x=q;return a}({})))") ==
        J(R"({"x":null})"));
  CHECK(read(R"((function(a){console.log("a;b",[1,2]);a.y=3;return a}({})))") ==
        J(R"({"y":3})"));
  CHECK(read(R"((function(a){a==1;a.y=3;return a}({})))") == J(R"({"y":3})"));
}

TEST_CASE("trailing_text_ignored_and_leading_whitespace") {
  CHECK(read("[1,2])</script>") == J("[1,2]"));
  CHECK(read(" \n\t\r {a:1} trailing") == J(R"({"a":1})"));
  CHECK(read("(1)") == 1);
}

TEST_CASE("malformed_inputs") {
  for (const char* s :
       {"", "   ", "\"abc", "'abc", "\"abc\\", "[1,2", "[,,1", "[1 2]", "{a:1", "{a 1}",
        "{:1}", "{a:}", "@", "}", "]", "(1", "(function(a){a.x=1;", "(function(a){return a}",
        "(function(a){a[]=1;return a}({}))", "(function(a){a.x=1 return a}({}))",
        "\"\\u12\"", "\"\\x4\"", "\"\\u{}\"", "\"\\u{1234567}\"", "-", "1e5x", "12abc",
        "function(){}", "new 5"}) {
    CAPTURE(s);
    CHECK_FALSE(parse(s).has_value());
  }
}

TEST_CASE("depth_past_the_limit_fails") {
  const auto nested = [](int n) {
    return std::string(static_cast<std::size_t>(n), '[') + std::string(static_cast<std::size_t>(n), ']');
  };
  CHECK(parse(nested(kMaxDepth)).has_value());
  CHECK_FALSE(parse(nested(kMaxDepth + 2)).has_value());
  std::string objects;
  for (int k = 0; k < kMaxDepth + 2; ++k) objects += "{a:";
  objects += "1";
  for (int k = 0; k < kMaxDepth + 2; ++k) objects += "}";
  CHECK_FALSE(parse(objects).has_value());
}

TEST_CASE("hostile_expansion_fails_instead_of_blowing_up") {
  const auto doubling = [](int levels) {
    std::string params, args, body;
    for (int k = 0; k < levels; ++k) {
      params += (k == 0 ? "" : ",") + param(k);
      args += (k == 0 ? "{}" : ",{}");
    }
    for (int k = 1; k < levels; ++k) {
      body += param(k) + ".x=[" + param(k - 1) + "," + param(k - 1) + "];";
    }
    return "(function(" + params + "){" + body + "return " + param(levels - 1) + "}(" + args + "))";
  };
  const json small = read(doubling(4));
  CHECK(small["x"][1]["x"][0]["x"][1] == json::object());
  CHECK_FALSE(parse(doubling(40)).has_value());

  const auto chain = [](int levels) {
    std::string params, args, body;
    for (int k = 0; k < levels; ++k) {
      params += (k == 0 ? "" : ",") + param(k);
      args += (k == 0 ? "{}" : ",{}");
    }
    for (int k = 1; k < levels; ++k) body += param(k) + ".x=" + param(k - 1) + ";";
    return "(function(" + params + "){" + body + "return " + param(levels - 1) + "}(" + args + "))";
  };
  CHECK(parse(chain(20)).has_value());
  CHECK_FALSE(parse(chain(400)).has_value());

  // The same chain read shallowest first: each value is finished before the
  // next one takes it in, and the result still may not nest without end.
  const auto ladder = [&](int levels) {
    const std::string inner = chain(levels);
    std::string listed;
    for (int k = 0; k < levels; ++k) listed += (k == 0 ? "" : ",") + param(k);
    const std::string from = "return " + param(levels - 1) + "}";
    const auto at = inner.find(from);
    REQUIRE(at != std::string::npos);
    return inner.substr(0, at) + "return [" + listed + "]}" + inner.substr(at + from.size());
  };
  const json rungs = read(ladder(4));
  CHECK(rungs[3]["x"]["x"]["x"] == json::object());
  CHECK(parse(ladder(200)).has_value());
  CHECK_FALSE(parse(ladder(300)).has_value());
}

TEST_CASE("search_fixture_payload") {
  const std::string html = read_fixture("search.html");
  const auto texts = payload_texts(html);
  REQUIRE(texts.size() == 1);
  const json data = read(texts[0]);
  REQUIRE(data.is_array());
  REQUIRE(data.size() == 2);
  CHECK(data[0]["data"]["href"].is_null());
  CHECK(data[0]["data"]["baseUrl"] == "https://miruro.cx");
  const json& items = data[1]["data"]["items"];
  REQUIRE(items.size() == 5);
  CHECK(items[0]["title"]["native"] == "\xE3\x83\x95\xE3\x83\xAA\xE3\x82\xAF\xE3\x83\xAA");
  CHECK(items[0]["external_ids"]["anilist"] == J(R"(["227"])"));
  CHECK(items[4]["id"] == "tHzpIqguLqPa-6qVtj1i9JSKV-8mRVh0");
  CHECK(data[1]["data"]["hasMore"] == true);
}

TEST_CASE("watch_fixture_payloads") {
  const std::string html = read_fixture("watch.html");
  const auto texts = payload_texts(html);
  REQUIRE(texts.size() == 6);
  std::vector<json> all;
  for (const auto t : texts) all.push_back(read(t));

  CHECK(all[0][1]["data"]["episodesData"] == 4);
  CHECK(all[0][1]["data"]["sourcesData"] == 5);

  const json& eps = all[1][0]["episodes"];
  REQUIRE(eps.size() == 6);
  CHECK(eps[0]["title"] == "Fooly Cooly");
  CHECK(eps[1]["number"] == 2);
  CHECK(eps[1]["description"].get<std::string>().find("\r\n\r\nMeanwhile") != std::string::npos);
  CHECK(eps[5]["title"] == "FLCLimax");
  CHECK(all[1][0]["currentEpisode"]["id"] == "1");
  CHECK(all[1][0]["nextEpisode"]["number"] == 2);
  CHECK(all[1][0]["prevEpisode"].is_null());

  CHECK(all[2][0].size() == 1);
  CHECK(all[3] == J("[[]]"));
  CHECK(all[4][0].size() == 5);

  const json& part = all[5][0];
  CHECK(part["episodeNumber"] == 1);
  const json& bee = part["tracks"][0]["providers"][1];
  CHECK(bee["provider"] == "bee");
  CHECK(bee["servers"][0]["streams"][0]["url"] ==
        "https://fetch.nexabloom.top/anime/218a0aefd1d1a4be65601cc6ddc1520e/"
        "2fd95cc7c0f17a2870bb6a4ec5eb0bb6/master.m3u8?token=MTc5NTIyMDI5MnwyMThhMGFl."
        "KPDEM-lobZhOHY2PSiHOeCcFHnn0xD-XjPWzvOTyAxo");
  CHECK(bee["servers"][1]["streams"][0]["url"] == bee["servers"][0]["streams"][0]["url"]);
  REQUIRE(bee["subtitles"].size() == 2);
  CHECK(bee["subtitles"][0]["default"] == true);
  CHECK(bee["subtitles"][1]["file"] ==
        "https://0arx3.crystaldrift.world/anime/218a0aefd1d1a4be65601cc6ddc1520e/"
        "2fd95cc7c0f17a2870bb6a4ec5eb0bb6/subtitles/13197d906c7b3a91347431ba4bc4f482.vtt");
  CHECK(bee["skipTimes"] == J(R"([{"kind":"outro","start":1311,"end":1449}])"));
  CHECK(bee["downloads"] == json::array());
  CHECK(bee["thumbnails"]["sprite"].is_null());
  CHECK(part["tracks"][1]["track"] == "dub");
  CHECK(part["tracks"][1]["providers"][1]["subtitles"][1]["file"] == bee["subtitles"][1]["file"]);
  CHECK(part["config"]["bee"]["hls"]["segments"]["origin"] == "https://qx-01.quavex.top");
  CHECK(part["config"]["sun"]["hls"].is_null());
  CHECK(part["providerOrder"].size() == 11);
}
