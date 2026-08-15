// aniskip_tests.cpp — P22 golden tests, ported 1:1 from sabigoku
// src/aniskip.rs's `mod tests` (skip_mode parse, times_from_body, build_opts,
// episode_number, ensure_script/prepare early-outs). jikan_mal_id has no Rust
// reference (shigoku-only addition, ROD-439) — covered by an offline-safe
// empty-title test only; the live HTTP path is exercised by hand, not ctest
// (same posture as the *_live_smoke binaries elsewhere in this tree).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/aniskip.hpp"

using namespace shigoku;
using namespace shigoku::aniskip;
using namespace shigoku::aniskip::detail;

namespace {

std::string temp_dir(const char* leaf) {
  const char* tmp = std::getenv("TMPDIR");
  std::string base = (tmp != nullptr && tmp[0] != '\0') ? tmp : "/tmp";
  return base + "/shigoku-" + leaf;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

}  // namespace

// ===========================================================================
// parse_skip_mode
// ===========================================================================

TEST_CASE("skip_mode_parses_with_typo_fallback_to_both") {
  CHECK(parse_skip_mode("none") == SkipMode::None);
  CHECK(parse_skip_mode("intro") == SkipMode::Intro);
  CHECK(parse_skip_mode("outro") == SkipMode::Outro);
  CHECK(parse_skip_mode("both") == SkipMode::Both);
  CHECK(parse_skip_mode("bth") == SkipMode::Both);
  CHECK(parse_skip_mode("") == SkipMode::Both);
}

// ===========================================================================
// times_from_body
// ===========================================================================

TEST_CASE("times_parse_real_api_shape_first_of_each_wins") {
  const std::string body = R"({"found":true,"results":[
      {"interval":{"startTime":3.221,"endTime":93.221},"skipType":"op","skipId":"x"},
      {"interval":{"startTime":1417.135,"endTime":1507.135},"skipType":"ed","skipId":"y"},
      {"interval":{"startTime":5,"endTime":9},"skipType":"op","skipId":"z"}]})";
  const SkipTimes t = times_from_body(body);
  REQUIRE(t.op.has_value());
  CHECK(t.op->first == doctest::Approx(3.221));
  CHECK(t.op->second == doctest::Approx(93.221));
  REQUIRE(t.ed.has_value());
  CHECK(t.ed->first == doctest::Approx(1417.135));
  CHECK(t.ed->second == doctest::Approx(1507.135));
}

TEST_CASE("times_leave_missing_segments_none_and_survive_garbage") {
  const std::string body =
      R"({"results":[{"interval":{"startTime":12.5,"endTime":84.3},"skipType":"op"}]})";
  const SkipTimes t = times_from_body(body);
  CHECK_FALSE(t.ed.has_value());
  CHECK(times_from_body("not json") == SkipTimes{});
  CHECK(times_from_body("{}") == SkipTimes{});
}

TEST_CASE("times_drop_degenerate_and_subsecond_intervals") {
  const std::string body = R"({"results":[
      {"interval":{"startTime":0,"endTime":0},"skipType":"op"},
      {"interval":{"startTime":100,"endTime":50},"skipType":"ed"},
      {"interval":{"startTime":10,"endTime":10.5},"skipType":"op"}]})";
  CHECK(times_from_body(body) == SkipTimes{});
}

// ===========================================================================
// build_opts
// ===========================================================================

TEST_CASE("build_opts_emits_all_keys_gated_by_mode") {
  SkipTimes both;
  both.op = std::make_pair(12.5, 84.3);
  both.ed = std::make_pair(1340.0, 1412.0);
  auto opts_both = build_opts(both, SkipMode::Both);
  REQUIRE(opts_both.has_value());
  CHECK(*opts_both ==
        "aniskip-op_start=12.5,aniskip-op_end=84.3,aniskip-ed_start=1340,aniskip-ed_end=1412,"
        "aniskip-mode=both");

  SkipTimes op_only;
  op_only.op = std::make_pair(12.5, 84.3);
  auto opts_intro = build_opts(op_only, SkipMode::Intro);
  REQUIRE(opts_intro.has_value());
  CHECK(*opts_intro ==
        "aniskip-op_start=12.5,aniskip-op_end=84.3,aniskip-ed_start=-1,aniskip-ed_end=-1,"
        "aniskip-mode=intro");
}

TEST_CASE("build_opts_none_when_no_relevant_interval_or_disabled") {
  SkipTimes op_only;
  op_only.op = std::make_pair(12.5, 84.3);
  CHECK_FALSE(build_opts(op_only, SkipMode::Outro).has_value());
  CHECK_FALSE(build_opts(SkipTimes{}, SkipMode::Both).has_value());
  CHECK_FALSE(build_opts(op_only, SkipMode::None).has_value());
}

// ===========================================================================
// episode_number
// ===========================================================================

TEST_CASE("episode_number_parses_label_falls_back_to_ordinal") {
  CHECK(episode_number("12", 5) == 12u);
  CHECK(episode_number(" 1 ", 9) == 1u);
  CHECK(episode_number("12.5", 7) == 7u);
  CHECK(episode_number("OVA", 3) == 3u);
}

// ===========================================================================
// prepare() early-outs
// ===========================================================================

TEST_CASE("prepare_early_outs_never_touch_the_network") {
  const std::string dir = temp_dir("aniskip-test");
  CHECK_FALSE(prepare(1, "t", 1, SkipMode::None, dir).has_value());
  CHECK_FALSE(prepare(std::nullopt, "", 1, SkipMode::Both, dir).has_value());
}

// ===========================================================================
// ensure_script
// ===========================================================================

TEST_CASE("ensure_script_writes_and_rewrites") {
  const std::string dir = temp_dir("aniskip-script-test");
  ::system(("rm -rf '" + dir + "'").c_str());
  auto path = ensure_script(dir);
  REQUIRE(path.has_value());
  {
    const std::string body = read_file(*path);
    CHECK(body.find(R"(read_options(o, "aniskip"))") != std::string::npos);
  }
  {
    std::ofstream stale(*path, std::ios::binary | std::ios::trunc);
    stale << "stale";
  }
  REQUIRE(ensure_script(dir).has_value());
  const std::string body = read_file(*path);
  CHECK(body.find("file-loaded") != std::string::npos);
  ::system(("rm -rf '" + dir + "'").c_str());
}

// ===========================================================================
// jikan_mal_id (shigoku addition, no Rust reference — offline-safe cases only)
// ===========================================================================

TEST_CASE("jikan_mal_id_empty_title_never_touches_the_network") {
  CHECK_FALSE(jikan_mal_id("").has_value());
}
