// hls_tests.cpp — P12 seed + P23. Ported 1:1 from
// sabigoku src/providers/hls.rs's `mod tests`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

Variant lv(std::string url, std::optional<std::uint32_t> res, std::optional<std::uint64_t> bw) {
  Variant v;
  v.url = std::move(url);
  v.resolution = res;
  v.bandwidth = bw;
  return v;
}

std::string pick_url(const std::vector<Variant>& variants, const Rung& rung) {
  const Variant* v = pick_variant(variants, rung);
  return v == nullptr ? "(none)" : v->url;
}

Variant in_group(std::string group) {
  Variant v;
  v.url = "v.m3u8";
  v.audio_group = std::move(group);
  return v;
}

// tests/fixtures/reanime/master.m3u8: video-only variants beside two audio renditions.
constexpr std::string_view kReanimeMaster =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",LANGUAGE=\"jpn\",NAME=\"Native\",DEFAULT=YES,"
    "AUTOSELECT=YES,"
    "URI=\"../0CVzLUG5XzCIowwXsx_8nnH7KUSpQ1CDu7yJrtfxO8laTerqNicUJwjk/audio/native.m3u8\"\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",LANGUAGE=\"eng\",NAME=\"English\",DEFAULT=NO,"
    "AUTOSELECT=YES,"
    "URI=\"../0CVzLUG5XzCIowwXsx_8nnH7KUSpQ1CDu7yJrtfxO8laTerqNicUJwjk/audio/english.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=5184,RESOLUTION=1920x1080,AUDIO=\"audio\","
    "CODECS=\"avc1.64001f,mp4a.40.2\"\n"
    "../0CVzLUG5XzCIowwXsx_8nnH7KUSpQ1CDu7yJrtfxO8laTerqNicUJwjk/video.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1024,RESOLUTION=854x480,AUDIO=\"audio\","
    "CODECS=\"avc1.64001f,mp4a.40.2\"\n"
    "../0CVzLUG5XzCIowwXsx_8nnH7KUSpQ1CDu7yJrtfxO8laTerqNicUJwjk/video-480p.m3u8\n";

constexpr std::string_view kReanimeDir =
    "../0CVzLUG5XzCIowwXsx_8nnH7KUSpQ1CDu7yJrtfxO8laTerqNicUJwjk/";

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

TEST_CASE("parse_master_playlist: width, bandwidth, codecs and audio group") {
  const std::string master =
      "#EXTM3U\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=5184,RESOLUTION=1920x1080,AUDIO=\"audio\","
      "CODECS=\"avc1.64001f,mp4a.40.2\"\n"
      "v1080.m3u8\n"
      "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=900,BANDWIDTH=1200,RESOLUTION=1280x720\n"
      "v720.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=700,CODECS=avc1.4d401e,AVERAGE-BANDWIDTH=600,"
      "RESOLUTION=854x480\n"
      "v480.m3u8\n"
      "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=300,RESOLUTION=640x360\n"
      "v360.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=150000,CODECS=\"mp4a.40.2\"\n"
      "audio-only.m3u8\n";
  const auto vs = parse_master_playlist(master);
  REQUIRE(vs.size() == 5);

  CHECK(vs[0].url == "v1080.m3u8");
  CHECK(vs[0].resolution == std::optional<std::uint32_t>(1080));
  CHECK(vs[0].width == std::optional<std::uint32_t>(1920));
  CHECK(vs[0].bandwidth == std::optional<std::uint64_t>(5184));
  CHECK(vs[0].codecs == "avc1.64001f,mp4a.40.2");
  CHECK(vs[0].audio_group == "audio");

  // AVERAGE-BANDWIDTH is not BANDWIDTH, before it, after it, or alone.
  CHECK(vs[1].bandwidth == std::optional<std::uint64_t>(1200));
  CHECK(vs[1].resolution == std::optional<std::uint32_t>(720));
  CHECK(vs[1].width == std::optional<std::uint32_t>(1280));
  CHECK(vs[1].codecs.empty());
  CHECK(vs[1].audio_group.empty());
  CHECK(vs[2].bandwidth == std::optional<std::uint64_t>(700));
  CHECK(vs[2].codecs == "avc1.4d401e");  // unquoted: up to the next comma
  CHECK(vs[2].width == std::optional<std::uint32_t>(854));
  CHECK(vs[3].bandwidth == std::nullopt);
  CHECK(vs[3].resolution == std::optional<std::uint32_t>(360));
  CHECK(vs[3].width == std::optional<std::uint32_t>(640));

  // No RESOLUTION.
  CHECK(vs[4].url == "audio-only.m3u8");
  CHECK(vs[4].resolution == std::nullopt);
  CHECK(vs[4].width == std::nullopt);
  CHECK(vs[4].bandwidth == std::optional<std::uint64_t>(150000));
  CHECK(vs[4].codecs == "mp4a.40.2");
  CHECK(vs[4].audio_group.empty());
}

TEST_CASE("parse_master_playlist: quoted attribute names, empty and overflowing numbers") {
  const std::string master =
      "#EXT-X-STREAM-INF:CODECS=\"mp4a.40.2,BANDWIDTH=9,AUDIO=x\",BANDWIDTH=5,"
      "RESOLUTION=640x360\n"
      "a.m3u8\n"
      "#EXT-X-STREAM-INF:CODECS=\"BANDWIDTH=9\",RESOLUTION=x480,BANDWIDTH=\n"
      "b.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=18446744073709551615,RESOLUTION=4294967295x720\n"
      "c.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=18446744073709551616,RESOLUTION=4294967296x720\n"
      "d.m3u8\n";
  const auto vs = parse_master_playlist(master);
  REQUIRE(vs.size() == 4);

  // The names inside a quoted value are not attributes.
  CHECK(vs[0].codecs == "mp4a.40.2,BANDWIDTH=9,AUDIO=x");
  CHECK(vs[0].bandwidth == std::optional<std::uint64_t>(5));
  CHECK(vs[0].audio_group.empty());
  CHECK(vs[0].width == std::optional<std::uint32_t>(640));
  CHECK(vs[0].resolution == std::optional<std::uint32_t>(360));

  CHECK(vs[1].codecs == "BANDWIDTH=9");
  CHECK(vs[1].bandwidth == std::nullopt);
  CHECK(vs[1].width == std::nullopt);
  CHECK(vs[1].resolution == std::optional<std::uint32_t>(480));

  constexpr std::uint64_t kMaxBandwidth = std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint32_t kMaxWidth = std::numeric_limits<std::uint32_t>::max();
  CHECK(vs[2].bandwidth == std::optional<std::uint64_t>(kMaxBandwidth));
  CHECK(vs[2].width == std::optional<std::uint32_t>(kMaxWidth));
  CHECK(vs[3].bandwidth == std::nullopt);
  CHECK(vs[3].width == std::nullopt);
  CHECK(vs[3].resolution == std::optional<std::uint32_t>(720));
}

TEST_CASE("parse_audio_renditions: the audio entries of a master, in order") {
  const std::string dir(kReanimeDir);
  const std::vector<AudioRendition> reanime = {
      {"audio", "Native", "jpn", dir + "audio/native.m3u8", true},
      {"audio", "English", "eng", dir + "audio/english.m3u8", false},
  };
  CHECK(parse_audio_renditions(kReanimeMaster) == reanime);

  const std::string mixed =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"English\",LANGUAGE=\"eng\","
      "URI=\"subs/en.m3u8\"\n"
      "#EXT-X-MEDIA:GROUP-ID=\"s\",NAME=\"x,TYPE=AUDIO\",TYPE=SUBTITLES,URI=\"s.m3u8\"\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Japanese, original\",DEFAULT=YES\r\n"
      "  #EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Commentary\",LANGUAGE=\"en-US\","
      "URI=\"c.m3u8\"\t\n"
      "#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID=\"cam\",NAME=\"Angle\",URI=\"cam.m3u8\"\n"
      "#EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS,GROUP-ID=\"cc\",NAME=\"CC1\",INSTREAM-ID=\"CC1\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480,AUDIO=\"aac\"\n"
      "480/index.m3u8";
  const std::vector<AudioRendition> audio = {
      {"aac", "Japanese, original", "", "", true},
      {"aac", "Commentary", "en-US", "c.m3u8", false},
  };
  CHECK(parse_audio_renditions(mixed) == audio);

  CHECK(parse_audio_renditions("#EXTM3U\n#EXTINF:9.0,\nseg0.ts\n").empty());
}

TEST_CASE("pick_variant: one variant per height") {
  CHECK(pick_variant({}, Rung()) == nullptr);
  CHECK(pick_variant({}, Quality::Worst) == nullptr);

  const std::vector<Variant> ladder = {lv("480", 480, 800), lv("1080", 1080, 2800),
                                       lv("720", 720, 1400)};
  CHECK(pick_variant(ladder, Rung()) == &ladder[1]);
  CHECK(pick_url(ladder, Quality::Worst) == "480");
  CHECK(pick_url(ladder, Quality::Best) == "1080");
  CHECK(pick_url(ladder, Quality::P720) == "720");
  CHECK(pick_url(ladder, Quality::P1080) == "1080");
  CHECK(pick_url(ladder, Quality::P480) == "480");
  CHECK(pick_url(ladder, Rung(1000)) == "720");
  // Every variant above the ceiling: the lowest.
  CHECK(pick_url(ladder, Rung(360)) == "480");
  const std::vector<Variant> over = {lv("1080", 1080, 2800), lv("720", 720, 1400)};
  CHECK(pick_url(over, Quality::P480) == "720");

  // A variant without a resolution is not taken while another names one.
  const std::vector<Variant> mixed = {lv("unknown-hi", std::nullopt, 9000), lv("480", 480, 800),
                                      lv("unknown-lo", std::nullopt, 100)};
  for (const Rung& rung : {Rung(), Rung(Quality::Worst), Rung(Quality::P1080), Rung(360),
                           Rung(0, 100), Rung(0, 9000)}) {
    CHECK(pick_url(mixed, rung) == "480");
  }
}

TEST_CASE("pick_variant: variants of one height") {
  const std::vector<Variant> tall = {
      lv("720-lo", 720, 1000),  lv("1080-a", 1080, 3000), lv("1080-b", 1080, 6000),
      lv("1080-c", 1080, 4500), lv("720-hi", 720, 2000),  lv("360-x", 360, std::nullopt),
      lv("360-y", 360, 500)};
  // No rung bandwidth: the highest.
  CHECK(pick_url(tall, Rung()) == "1080-b");
  CHECK(pick_url(tall, Quality::P1080) == "1080-b");
  CHECK(pick_url(tall, Quality::P720) == "720-hi");
  // A rung bandwidth: the nearest.
  CHECK(pick_url(tall, Rung(1080, 4000)) == "1080-c");
  CHECK(pick_url(tall, Rung(0, 4000)) == "1080-c");
  CHECK(pick_url(tall, Rung(1080, 1)) == "1080-a");
  CHECK(pick_url(tall, Rung(720, 900)) == "720-lo");
  // Reaching for the lowest height takes its lowest bandwidth; a missing one counts as 0.
  CHECK(pick_url(tall, Quality::Worst) == "360-x");
  CHECK(pick_url(tall, Rung(240)) == "360-x");
  CHECK(pick_url(tall, Rung(240, 400)) == "360-y");
  CHECK(pick_url(tall, Rung(0, 600, true)) == "360-y");
  // A ceiling that lands on the lowest height is not reaching for it.
  CHECK(pick_url(tall, Quality::P480) == "360-y");

  // Still tied: the earlier one.
  const std::vector<Variant> tied = {lv("a", 720, 1000), lv("b", 720, 2000), lv("c", 720, 2000),
                                     lv("d", 720, 1000)};
  CHECK(pick_url(tied, Rung()) == "b");
  CHECK(pick_url(tied, Quality::Worst) == "a");
  CHECK(pick_url(tied, Rung(720, 1500)) == "a");
  CHECK(pick_url(tied, Rung(720, 2000)) == "b");
}

TEST_CASE("pick_variant: a ladder that names no resolution") {
  const std::vector<Variant> bare = {lv("mid", std::nullopt, 1500), lv("hi", std::nullopt, 3000),
                                     lv("none", std::nullopt, std::nullopt),
                                     lv("lo", std::nullopt, 800)};
  // lowest: the lowest bandwidth, a missing one counting as 0.
  CHECK(pick_url(bare, Quality::Worst) == "none");
  CHECK(pick_url(bare, Rung(0, 600, true)) == "none");
  // A rung bandwidth: the highest at or under it.
  CHECK(pick_url(bare, Rung(0, 2000)) == "mid");
  CHECK(pick_url(bare, Rung(0, 3000)) == "hi");
  CHECK(pick_url(bare, Rung(720, 2000)) == "mid");
  // The best: the highest bandwidth.
  CHECK(pick_url(bare, Rung()) == "hi");
  // A height alone: the first listed.
  CHECK(pick_url(bare, Quality::P480) == "mid");
  CHECK(pick_url(bare, Quality::P1080) == "mid");

  // Nothing at or under the rung bandwidth: the lowest.
  const std::vector<Variant> priced = {lv("mid", std::nullopt, 1500),
                                       lv("hi", std::nullopt, 3000), lv("lo", std::nullopt, 800)};
  CHECK(pick_url(priced, Rung(0, 500)) == "lo");
  CHECK(pick_url(priced, Quality::Worst) == "lo");
}

TEST_CASE("audio_for: the rendition a variant's audio group plays") {
  const std::vector<AudioRendition> renditions = {
      {"aud", "English", "eng", "en.m3u8", false},
      {"other", "Other", "jpn", "other.m3u8", true},
      {"aud", "Japanese", "jpn", "ja.m3u8", true},
      {"muxed", "Japanese", "jpn", "", true},
      {"nodef", "Label", "", "", true},
      {"nodef", "First", "eng", "first.m3u8", false},
      {"nodef", "Second", "jpn", "second.m3u8", false},
      {"", "Stray", "", "stray.m3u8", true},
  };
  // DEFAULT=YES over an earlier entry; another group's default is not this group's.
  CHECK(audio_for(in_group("aud"), renditions) == &renditions[2]);
  // No default with a URI: the first with one.
  CHECK(audio_for(in_group("nodef"), renditions) == &renditions[5]);
  // The group carries no URI: the audio is muxed in.
  CHECK(audio_for(in_group("muxed"), renditions) == nullptr);
  CHECK(audio_for(in_group("missing"), renditions) == nullptr);
  // The variant names no group.
  CHECK(audio_for(in_group(""), renditions) == nullptr);
  CHECK(audio_for(in_group("aud"), {}) == nullptr);
}

TEST_CASE("choose_pair: a video variant and its audio rendition, both joined") {
  const std::string base = "https://cdn.example/a/b/master.m3u8";
  const std::string dir = "https://cdn.example/a/b/" + std::string(kReanimeDir);

  const auto sd = choose_pair(kReanimeMaster, base, Rung(Quality::P480));
  REQUIRE(sd.has_value());
  CHECK(sd->video_url == dir + "video-480p.m3u8");
  CHECK(sd->audio_url == std::optional<std::string>(dir + "audio/native.m3u8"));
  Variant want;
  want.url = dir + "video-480p.m3u8";
  want.resolution = 480;
  want.width = 854;
  want.bandwidth = 1024;
  want.codecs = "avc1.64001f,mp4a.40.2";
  want.audio_group = "audio";
  CHECK(sd->variant == want);

  const auto hd = choose_pair(kReanimeMaster, base, Quality::Best);
  REQUIRE(hd.has_value());
  CHECK(hd->video_url == dir + "video.m3u8");
  CHECK(hd->audio_url == std::optional<std::string>(dir + "audio/native.m3u8"));
}

TEST_CASE("choose_pair: muxed audio, media playlists and unsafe urls") {
  const std::string base = "https://cdn.test/hls/master.m3u8";

  // Muxed audio: a pair without an audio url, whether or not a group labels it.
  const std::string muxed =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"Japanese\",DEFAULT=YES\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480,AUDIO=\"aac\"\n"
      "480/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1920x1080\n"
      "1080/index.m3u8\n";
  const auto best = choose_pair(muxed, base, Rung());
  REQUIRE(best.has_value());
  CHECK(best->video_url == "https://cdn.test/hls/1080/index.m3u8");
  CHECK(best->variant.url == best->video_url);
  CHECK_FALSE(best->audio_url.has_value());
  const auto sd = choose_pair(muxed, base, Quality::P480);
  REQUIRE(sd.has_value());
  CHECK(sd->video_url == "https://cdn.test/hls/480/index.m3u8");
  CHECK_FALSE(sd->audio_url.has_value());

  const std::string media =
      "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXTINF:9.0,\nseg0.ts\n#EXT-X-ENDLIST\n";
  CHECK_FALSE(choose_pair(media, base, Rung()).has_value());

  // The picked variant's audio url fails the guard, or does not join: keep the master.
  const std::string spaced_audio =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\",DEFAULT=YES,"
      "URI=\"audio/with space.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480,AUDIO=\"a\"\n"
      "480/index.m3u8\n";
  CHECK_FALSE(choose_pair(spaced_audio, base, Rung()).has_value());
  const std::string unjoinable =
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\",URI=\"audio/en.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480,AUDIO=\"a\"\n"
      "https://cdn.test/hls/480/index.m3u8\n";
  CHECK_FALSE(choose_pair(unjoinable, "no-scheme", Rung()).has_value());

  // A variant url that fails the guard is left out; the next best is picked.
  const std::string spaced =
      "#EXTM3U\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480\n"
      "480/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1920x1080\n"
      "1080/with space.m3u8\n";
  const auto fallback = choose_pair(spaced, base, Rung());
  REQUIRE(fallback.has_value());
  CHECK(fallback->video_url == "https://cdn.test/hls/480/index.m3u8");
  CHECK(fallback->variant.resolution == std::optional<std::uint32_t>(480));
  CHECK_FALSE(
      choose_pair("#EXT-X-STREAM-INF:BANDWIDTH=1\nwith space.m3u8\n", base, Rung()).has_value());
}

TEST_CASE("Rung: from each Quality, and the best") {
  CHECK(Rung(Quality::Best) == Rung());
  CHECK(Rung(Quality::P1080) == Rung(1080));
  CHECK(Rung(Quality::P720) == Rung(720));
  CHECK(Rung(Quality::P480) == Rung(480));
  CHECK(Rung(Quality::Worst) == Rung(0, 0, true));

  CHECK(Rung().best());
  CHECK(Rung(Quality::Best).best());
  CHECK_FALSE(Rung(Quality::P1080).best());
  CHECK_FALSE(Rung(Quality::P720).best());
  CHECK_FALSE(Rung(Quality::P480).best());
  CHECK_FALSE(Rung(Quality::Worst).best());
  CHECK_FALSE(Rung(0, 1).best());
}
