// hls_tests.cpp — P12 seed + P23. Ported 1:1 from
// sabigoku src/providers/hls.rs's `mod tests`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/hls.hpp"

using namespace shigoku;
using namespace shigoku::hls;

namespace {

StreamLink mk(std::optional<std::uint32_t> res) {
  StreamLink link;
  link.url = "https://cdn.test/v.m3u8";
  link.resolution = res;
  return link;
}

}  // namespace

TEST_CASE("parse_master_playlist_extracts_uris_and_resolutions") {
  const std::string playlist =
      "#EXTM3U\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480\n"
      "480/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1400000,RESOLUTION=1280x720\n"
      "720/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1920x1080\n"
      "1080/index.m3u8\n";
  const auto vs = parse_master_playlist(playlist);
  REQUIRE(vs.size() == 3);
  CHECK(vs[0].url == "480/index.m3u8");
  CHECK(vs[0].resolution == std::optional<std::uint32_t>(480));
  CHECK(vs[1].resolution == std::optional<std::uint32_t>(720));
  CHECK(vs[2].resolution == std::optional<std::uint32_t>(1080));
}

TEST_CASE("parse_master_playlist_media_playlist_yields_empty") {
  const std::string media =
      "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXTINF:9.0,\nseg0.ts\n#EXTINF:9.0,\nseg1.ts\n#EXT-X-ENDLIST\n";
  CHECK(parse_master_playlist(media).empty());
}

TEST_CASE("parse_master_playlist_bandwidth_only_variant_has_no_resolution") {
  const std::string playlist = "#EXT-X-STREAM-INF:BANDWIDTH=800000\nv.m3u8\n";
  const auto vs = parse_master_playlist(playlist);
  REQUIRE(vs.size() == 1);
  CHECK(vs[0].resolution == std::nullopt);
}

TEST_CASE("select_variant_cap_policy_picks_the_right_rung") {
  CHECK(select_variant({}, Quality::Best) == nullptr);

  const std::vector<StreamLink> full = {mk(480), mk(1080), mk(720)};
  auto res = [&](Quality q) { return select_variant(full, q)->resolution; };
  CHECK(res(Quality::Best) == std::optional<std::uint32_t>(1080));
  CHECK(res(Quality::Worst) == std::optional<std::uint32_t>(480));
  CHECK(res(Quality::P480) == std::optional<std::uint32_t>(480));
  CHECK(res(Quality::P720) == std::optional<std::uint32_t>(720));
  CHECK(res(Quality::P1080) == std::optional<std::uint32_t>(1080));

  // Requested rung absent -> highest at or below it.
  const std::vector<StreamLink> gap = {mk(480), mk(1080)};
  CHECK(select_variant(gap, Quality::P720)->resolution == std::optional<std::uint32_t>(480));

  // Every variant exceeds the cap -> the smallest available.
  const std::vector<StreamLink> over = {mk(720), mk(1080)};
  CHECK(select_variant(over, Quality::P480)->resolution == std::optional<std::uint32_t>(720));

  // Known beats unknown in every mode (rung-cap landmine).
  const std::vector<StreamLink> withnull = {mk(std::nullopt), mk(720)};
  for (const Quality q : {Quality::Best, Quality::Worst, Quality::P480}) {
    CHECK(select_variant(withnull, q)->resolution == std::optional<std::uint32_t>(720));
  }

  // All unknown: still return one (a stream exists; do not error out).
  const std::vector<StreamLink> allnull = {mk(std::nullopt), mk(std::nullopt)};
  CHECK(select_variant(allnull, Quality::P720) != nullptr);
}

TEST_CASE("join_url_absolute_rooted_and_relative") {
  const std::string base = "https://h.example/x/y/master.m3u8";
  CHECK(join_url(base, "https://cdn.other/v.ts").value() == "https://cdn.other/v.ts");
  CHECK(join_url(base, "/a/b.ts").value() == "https://h.example/a/b.ts");
  CHECK(join_url(base, "720/seg.ts").value() == "https://h.example/x/y/720/seg.ts");
  CHECK(join_url("no-scheme", "x.ts") == std::nullopt);
}
