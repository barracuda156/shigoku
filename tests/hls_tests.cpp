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

TEST_CASE("has_separate_audio: audio renditions with a URI, not labels of muxed audio") {
  // The shape senshi's CDN serves: video-only variants beside two audio
  // renditions.
  const std::string demuxed =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"group_audio\",NAME=\"Japanese\",DEFAULT=YES,"
      "LANGUAGE=\"ja\",URI=\"/i/x/p/audio-ja.jpg\"\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"group_audio\",NAME=\"English\",DEFAULT=NO,"
      "LANGUAGE=\"en\",URI=\"/i/x/p/audio-en.jpg\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=3476000,RESOLUTION=1920x1080,AUDIO=\"group_audio\"\n"
      "/i/x/p/video-1080.jpg\n";
  CHECK(has_separate_audio(demuxed));
  // TYPE=AUDIO with no URI only names audio muxed into the variants.
  const std::string labelled =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Japanese\",DEFAULT=YES\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480,AUDIO=\"aac\"\n"
      "480/index.m3u8\n";
  CHECK_FALSE(has_separate_audio(labelled));
  // Subtitle renditions are not audio; a plain ladder has none.
  CHECK_FALSE(has_separate_audio(
      "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",NAME=\"en\",URI=\"en.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1,RESOLUTION=1x480,SUBTITLES=\"s\"\nv.m3u8\n"));
  CHECK_FALSE(has_separate_audio(
      "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480\n480/index.m3u8\n"));
  // The words inside a quoted value are not attributes.
  CHECK_FALSE(has_separate_audio(
      "#EXT-X-MEDIA:TYPE=VIDEO,NAME=\"x TYPE=AUDIO y\",URI=\"v.m3u8\"\n"));
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

TEST_CASE("choose_variant: capped pick, media playlist, separate audio, unsafe url") {
  const std::string master =
      "#EXTM3U\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480\n"
      "480/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1400000,RESOLUTION=1280x720\n"
      "720/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1920x1080\n"
      "1080/index.m3u8\n";
  const std::string base = "https://cdn.test/hls/master.m3u8";
  CHECK(choose_variant(master, base, Quality::Best) ==
        std::optional<std::string>("https://cdn.test/hls/1080/index.m3u8"));
  CHECK(choose_variant(master, base, Quality::Worst) ==
        std::optional<std::string>("https://cdn.test/hls/480/index.m3u8"));
  CHECK(choose_variant(master, base, Quality::P720) ==
        std::optional<std::string>("https://cdn.test/hls/720/index.m3u8"));

  const std::string media =
      "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXTINF:9.0,\nseg0.ts\n#EXT-X-ENDLIST\n";
  CHECK_FALSE(choose_variant(media, base, Quality::Best).has_value());

  const std::string split_audio =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\",URI=\"audio/en.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480,AUDIO=\"a\"\n"
      "480/index.m3u8\n";
  CHECK_FALSE(choose_variant(split_audio, base, Quality::Best).has_value());

  const std::string spaced =
      "#EXTM3U\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480\n"
      "480/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1920x1080\n"
      "1080/with space.m3u8\n";
  CHECK(choose_variant(spaced, base, Quality::Best) ==
        std::optional<std::string>("https://cdn.test/hls/480/index.m3u8"));
}

TEST_CASE("join_url_absolute_rooted_and_relative") {
  const std::string base = "https://h.example/x/y/master.m3u8";
  CHECK(join_url(base, "https://cdn.other/v.ts").value() == "https://cdn.other/v.ts");
  CHECK(join_url(base, "/a/b.ts").value() == "https://h.example/a/b.ts");
  CHECK(join_url(base, "720/seg.ts").value() == "https://h.example/x/y/720/seg.ts");
  CHECK(join_url("no-scheme", "x.ts") == std::nullopt);
}

TEST_CASE("keep_audio_language: one rendition kept and made the default") {
  const std::string master =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",LANGUAGE=\"jpn\",NAME=\"Native\",DEFAULT=YES,"
      "AUTOSELECT=YES,URI=\"a/native.m3u8\"\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",LANGUAGE=\"eng\",NAME=\"English\",DEFAULT=NO,"
      "AUTOSELECT=YES,URI=\"a/english.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1024,RESOLUTION=854x480,AUDIO=\"audio\"\n"
      "video-480p.m3u8\n";

  const std::string dub = keep_audio_language(master, "eng");
  CHECK(dub.find("native.m3u8") == std::string::npos);
  CHECK(dub.find("LANGUAGE=\"eng\",NAME=\"English\",DEFAULT=YES,") != std::string::npos);
  CHECK(dub.find("video-480p.m3u8") != std::string::npos);
  CHECK(has_separate_audio(dub));

  const std::string sub = keep_audio_language(master, "ja");  // two-letter folds to jpn
  CHECK(sub.find("english.m3u8") == std::string::npos);
  CHECK(sub.find("NAME=\"Native\",DEFAULT=YES,") != std::string::npos);

  // No rendition in that language, or no language at all: unchanged.
  CHECK(keep_audio_language(master, "spa") == master);
  CHECK(keep_audio_language(master, "") == master);
  CHECK(keep_audio_language("#EXTM3U\nseg.ts\n", "eng") == "#EXTM3U\nseg.ts\n");
}

TEST_CASE("keep_audio_language: a group without the language stays whole") {
  const std::string master =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"hi\",LANGUAGE=\"en-US\",URI=\"hi/en.m3u8\"\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"hi\",LANGUAGE=\"jpn\",DEFAULT=YES,URI=\"hi/ja.m3u8\"\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"lo\",LANGUAGE=\"jpn\",DEFAULT=YES,URI=\"lo/ja.m3u8\"\n"
      "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",LANGUAGE=\"jpn\",URI=\"s/ja.m3u8\"\n";
  const std::string out = keep_audio_language(master, "eng");
  CHECK(out.find("hi/ja.m3u8") == std::string::npos);
  CHECK(out.find("LANGUAGE=\"en-US\",URI=\"hi/en.m3u8\",DEFAULT=YES") != std::string::npos);
  CHECK(out.find("lo/ja.m3u8") != std::string::npos);  // group "lo" has no English
  CHECK(out.find("s/ja.m3u8") != std::string::npos);   // subtitles are not audio
}

TEST_CASE("keep_audio_language: CRLF masters, a second track, AUTOSELECT") {
  const std::string master =
      "#EXTM3U\r\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",LANGUAGE=\"jpn\",DEFAULT=YES,URI=\"ja.m3u8\"\r\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",LANGUAGE=\"eng\",DEFAULT=NO,AUTOSELECT=NO,"
      "URI=\"en.m3u8\"\r\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",LANGUAGE=\"eng\",NAME=\"Commentary\",DEFAULT=YES,"
      "URI=\"en2.m3u8\"\r\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1,AUDIO=\"a\"\r\n"
      "v.m3u8\r\n";
  const std::string out = keep_audio_language(master, "eng");
  CHECK(out.find("ja.m3u8") == std::string::npos);
  CHECK(out.find("DEFAULT=YES,AUTOSELECT=YES,URI=\"en.m3u8\"") != std::string::npos);
  CHECK(out.find("NAME=\"Commentary\",DEFAULT=NO,URI=\"en2.m3u8\"") != std::string::npos);
  CHECK(out.find("v.m3u8") != std::string::npos);
  CHECK(has_separate_audio(out));
}
