// megaplay_tests.cpp — P13 golden tests, ported 1:1 from
// src/providers/megaplay.rs's `mod tests`, plus the `enc` envelope cases
// (shigoku-only). Pure parsers + guards run offline;
// the transport cases (episodes probe, private-host sub drop, the two/three-
// hop resolve, the envelope cache) run over a loopback fixture server, same
// SequenceServer shape as anibd_tests.cpp.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/crypto.hpp"
#include "../src/megaplay.hpp"

using namespace shigoku;
using namespace shigoku::megaplay;
using namespace shigoku::megaplay::detail;

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
// parse_data_id
// ===========================================================================

TEST_CASE("parse_data_id_scrapes_the_first_numeric_id_any_quoting") {
  CHECK(parse_data_id(R"(<div id="megaplay-player" data-id="13458" data-lang="sub">)") == "13458");
  CHECK(parse_data_id("<div data-id='13452'>") == "13452");
  CHECK(parse_data_id("<div data-id=7 >") == "7");
  CHECK(parse_data_id(R"(<a data-id="11"></a><a data-id="22"></a>)") == "11");
  // data-id wins over sibling data-realid / data-mediaid.
  CHECK(parse_data_id(
            R"(<div id="megaplay-player" data-id="13461" data-realid="107257" data-mediaid="672">)") ==
        "13461");
}

TEST_CASE("parse_data_id_skips_empty_nonnumeric_and_bounds_the_id") {
  CHECK(parse_data_id(R"(<a data-id=""></a><b data-id="x9"></b><c data-id="42"></c>)") == "42");
  CHECK_FALSE(parse_data_id("<html>no ids here</html>").has_value());
  CHECK_FALSE(parse_data_id("").has_value());
  // Over-long digit runs rejected, not spliced into a URL.
  CHECK_FALSE(parse_data_id(R"(data-id="123456789012345678901")").has_value());
}

// ===========================================================================
// embed_url / labels
// ===========================================================================

TEST_CASE("embed_url_splices_mal_episode_and_track") {
  CHECK(embed_url(kHost, "52991", "28", Translation::Sub) ==
        "https://megaplay.buzz/stream/mal/52991/28/sub");
  CHECK(embed_url(kHost, "21", "1100", Translation::Dub) ==
        "https://megaplay.buzz/stream/mal/21/1100/dub");
}

TEST_CASE("labels_mints_positional_and_degrades_hintless_to_one") {
  const auto eps = labels(28);
  CHECK(eps.size() == 28);
  CHECK(eps.front() == "1");
  CHECK(eps.back() == "28");
  // Zero -> one (empty means not-stocked). Hostile count clamps.
  CHECK(labels(0).size() == 1);
  CHECK(labels(0xFFFFFFFFu).size() == static_cast<std::size_t>(kMaxEpisodeHint));
}

// ===========================================================================
// map_sources
// ===========================================================================

TEST_CASE("map_sources_maps_a_live_shaped_body") {
  const char* raw = R"({"sources":{"file":"https://cdn.mewstream.buzz/x/master.m3u8"},
      "tracks":[
        {"file":"https://1oe.lostproject.club/eng.vtt","label":"English","kind":"captions","default":true},
        {"file":"https://1oe.lostproject.club/thumbs.vtt","kind":"thumbnails"},
        {"label":"ghost-no-file","kind":"captions"}],
      "intro":{"start":100,"end":190},"server":2})";
  auto s = map_sources(raw, Translation::Sub, baked_envelope());
  REQUIRE(s.has_value());
  CHECK(s->link.url == "https://cdn.mewstream.buzz/x/master.m3u8");
  CHECK(s->link.referer == std::string(kStreamReferer));
  CHECK(s->link.user_agent == std::string(kUserAgent));
  CHECK(s->link.cloaked_segments);
  CHECK(s->link.decloak_segments);
  // Two vetted tracks (the file-less ghost dropped); thumbnails kept as a track
  // but never the sub pick.
  CHECK(s->tracks.size() == 2);
  CHECK(s->link.sub_url == "https://1oe.lostproject.club/eng.vtt");
  // The legacy shape still carries intro/outro; only outro is absent here.
  REQUIRE(s->skip.op.has_value());
  CHECK(s->skip.op->first == doctest::Approx(100));
  CHECK(s->skip.op->second == doctest::Approx(190));
  CHECK_FALSE(s->skip.ed.has_value());
  // A dub resolve never loads a softsub.
  auto d = map_sources(raw, Translation::Dub, baked_envelope());
  REQUIRE(d.has_value());
  CHECK_FALSE(d->link.sub_url.has_value());
}

TEST_CASE("map_sources_missing_or_unsafe_stream_url_is_a_clean_error") {
  CHECK_FALSE(map_sources("{}", Translation::Sub, baked_envelope()).has_value());
  CHECK_FALSE(map_sources(R"({"sources":{}})", Translation::Sub, baked_envelope()).has_value());
  CHECK_FALSE(map_sources(R"({"sources":{"file":"/x/master.m3u8"}})", Translation::Sub, baked_envelope())
                  .has_value());
  CHECK_FALSE(map_sources(R"({"sources":{"file":"https://cdn/x master.m3u8"}})", Translation::Sub,
                          baked_envelope())
                  .has_value());
  // An unsafe track is dropped, not fatal, and cannot become the sub pick.
  auto s = map_sources(
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[{"file":"/relative.vtt","kind":"captions"}]})",
      Translation::Sub, baked_envelope());
  REQUIRE(s.has_value());
  CHECK(s->tracks.empty());
  CHECK_FALSE(s->link.sub_url.has_value());
}

TEST_CASE("map_sources_drops_a_sub_url_aimed_at_a_private_host") {
  // The picked sub_url bypasses the proxy and reaches mpv --sub-file; a default
  // track pointing at cloud metadata / loopback must not survive the SSRF
  // guard, while the stream itself (proxy-guarded) still plays.
  for (const char* host : {"http://169.254.169.254/latest/meta-data/", "http://127.0.0.1:9/pwn.vtt"}) {
    const std::string raw =
        std::string(R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[{"file":")") + host +
        R"(","label":"English","kind":"captions","default":true}]})";
    auto s = map_sources(raw, Translation::Sub, baked_envelope());
    REQUIRE(s.has_value());
    CHECK_FALSE(s->link.sub_url.has_value());
    CHECK(s->link.url == "https://cdn/ok.m3u8");
  }
}

TEST_CASE("map_sources_rejects_type_mismatched_fields_like_serde") {
  // Not in megaplay.rs's `mod tests`, but pins the serde-parity contract: the
  // Rust DTOs (#[derive(Deserialize)]) reject a present-but-wrong-typed KNOWN
  // field with a hard error, so the C++ nlohmann walk must too (a hostile
  // getSources body must not coerce a bad type into a playable stream). Each of
  // these makes serde_json fail the struct -> Decode; the C++ mirrors it.
  const char* bad[] = {
      // sources / sources.file wrong type.
      R"({"sources":"notobj"})",
      R"({"sources":{"file":123}})",
      // tracks not an array; a non-object track element.
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":"notarray"})",
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[123]})",
      // a track's default is a string, kind is a number, label is a bool.
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[{"file":"https://c/e.vtt","default":"true"}]})",
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[{"file":"https://c/e.vtt","kind":7}]})",
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[{"file":"https://c/e.vtt","label":true}]})",
  };
  for (const char* raw : bad) {
    CHECK_FALSE(map_sources(raw, Translation::Sub, baked_envelope()).has_value());
  }
  // But absent/null optionals still degrade cleanly (serde default), not error:
  // null sources.file -> "no stream source"; a null default -> false.
  CHECK_FALSE(map_sources(R"({"sources":{"file":null}})", Translation::Sub, baked_envelope()).has_value());
  auto ok = map_sources(
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[{"file":"https://c/e.vtt","kind":null,"default":null}]})",
      Translation::Sub, baked_envelope());
  REQUIRE(ok.has_value());
  CHECK(ok->tracks.size() == 1);
}

// ===========================================================================
// The getSources `enc` envelope — baked_envelope / scrape_envelope /
// newclient_path / map_sources' enc branch.
// ===========================================================================

TEST_CASE("baked_envelope_is_the_site_literals_zero_padded_and_as_is") {
  const auto e = baked_envelope();
  REQUIRE(e.key.size() == 32);
  REQUIRE(e.iv.size() == 16);
  const std::string_view key_lit = "i?LMTAx0Q6,:}50U";
  const std::string_view iv_lit = "W0;27ToaUpl_P%'c";
  CHECK(std::equal(key_lit.begin(), key_lit.end(), e.key.begin()));
  CHECK(std::all_of(e.key.begin() + static_cast<long>(key_lit.size()), e.key.end(),
                    [](std::uint8_t b) { return b == 0; }));
  CHECK(std::equal(iv_lit.begin(), iv_lit.end(), e.iv.begin()));
}

TEST_CASE("scrape_envelope_reads_the_captured_newclient_slice_and_misses_cleanly") {
  const std::string js = read_fixture("megaplay_newclient.js");
  auto scraped = scrape_envelope(js);
  REQUIRE(scraped.has_value());
  // The live literals haven't rotated: the scrape lands on the same pair as
  // the baked fallback.
  CHECK(*scraped == baked_envelope());

  CHECK_FALSE(scrape_envelope("no anchors here at all").has_value());
  CHECK_FALSE(scrape_envelope(R"(..["trustAesKey","TRUST_AES_KEY"],"only-the-key")..)").has_value());
  // An over-long key literal (> 32 bytes) is a miss, not a silent truncation.
  const std::string oversize =
      std::string(R"(["trustAesKey","TRUST_AES_KEY"],")") + std::string(40, 'x') + R"(")";
  CHECK_FALSE(scrape_envelope(oversize).has_value());
  // A backslash inside the literal is refused outright.
  CHECK_FALSE(scrape_envelope(R"(["trustAesKey","TRUST_AES_KEY"],"a\"b"))").has_value());
}

TEST_CASE("newclient_path_reads_the_first_matching_script_tag") {
  // The live-captured embed page's actual head.
  const char* html =
      R"(<script src="https://megaplay.buzz/lib/newclient.min.js?v=4.17"></script><script>const x=1;</script>)";
  CHECK(newclient_path(html) == "/lib/newclient.min.js?v=4.17");
  // A relative src works the same way, untouched.
  CHECK(newclient_path(R"(<script src="/lib/newclient.min.js?v=1"></script>)") ==
        "/lib/newclient.min.js?v=1");
  // A different script, or no script at all, is a clean miss.
  CHECK_FALSE(newclient_path(R"(<script src="https://megaplay.buzz/lib/other.js"></script>)").has_value());
  CHECK_FALSE(newclient_path("<html>no scripts</html>").has_value());
}

TEST_CASE("map_sources_opens_the_enc_envelope_under_the_baked_pair") {
  // Live-captured `enc` (One Piece 21, ep1 sub),
  // decrypting under the baked pair to a real captured master URL.
  const std::string raw =
      R"({"tracks":[{"file":"https://1oe.club/eng.vtt","label":"English","kind":"captions","default":true}],)"
      R"("t":1,"intro":{"start":31,"end":111},"outro":{"start":1376,"end":1447},"server":4,)"
      R"("enc":"wdeBruh3qqn_i5wUNnyaPcXqidp1UWP84FfPHzGyKXA2hDZBfMCmZ4FLvs7_pQuH549Eptax8UOjAJyRIZfrRhUGUKy9O)"
      R"(BeGh2yB_-m_JLAlLnWTLzYZC3_C5I4ltveYoiaU66Do9RgI9bCetmk_o87-sd66brXnWV1MbjhLnjw="})";
  auto s = map_sources(raw, Translation::Sub, baked_envelope());
  REQUIRE(s.has_value());
  CHECK(s->link.url == "https://fetch.nexabloom.top/anime/f899139df5e1059396431415e770c6dd/"
                       "61b87186ab260d05003427e16ccf5657/master.m3u8");
  CHECK(s->link.cloaked_segments);
  CHECK(s->link.decloak_segments);
  CHECK(s->link.sub_url == "https://1oe.club/eng.vtt");
  REQUIRE(s->skip.op.has_value());
  CHECK(s->skip.op->first == doctest::Approx(31));
  CHECK(s->skip.op->second == doctest::Approx(111));
  REQUIRE(s->skip.ed.has_value());
  CHECK(s->skip.ed->first == doctest::Approx(1376));
  CHECK(s->skip.ed->second == doctest::Approx(1447));
}

namespace {

// base64 -> base64url, so aes256cbc_seal + base64_encode (crypto_tests'
// vocabulary) can build a fresh `enc` value in-test without a second fixture.
std::string to_b64url(std::string b64) {
  for (char& c : b64) {
    if (c == '+') c = '-';
    if (c == '/') c = '_';
  }
  return b64;
}

}  // namespace

TEST_CASE("map_sources_enc_rejections_never_fall_through_to_a_playable_link") {
  const auto env = baked_envelope();
  // A non-string enc is the same hard type error as every other known field.
  CHECK_FALSE(map_sources(R"({"enc":123})", Translation::Sub, env).has_value());
  // A present-but-unopenable enc (garbage, wrong key, truncated) is a distinct
  // "bad envelope" — never silently "no stream source".
  auto garbage = map_sources(R"({"enc":"not-valid-base64url!!"})", Translation::Sub, env);
  REQUIRE_FALSE(garbage.has_value());
  CHECK(garbage.error().kind == ProviderError::Kind::Decode);
  CHECK_FALSE(map_sources(R"({"enc":""})", Translation::Sub, env).has_value());

  // Opens fine under the pair but decrypts to something that isn't
  // `{"file":"..."}`: still "bad envelope", not a crash or a bogus link.
  const std::string wrong_shape = R"({"nope":true})";
  auto sealed = crypto::aes256cbc_seal(env.key, env.iv,
                                       reinterpret_cast<const std::uint8_t*>(wrong_shape.data()),
                                       wrong_shape.size());
  REQUIRE(sealed.has_value());
  const std::string enc = to_b64url(crypto::base64_encode(sealed->data(), sealed->size()));
  const std::string raw = R"({"enc":")" + enc + R"("})";
  auto bad_shape = map_sources(raw, Translation::Sub, env);
  REQUIRE_FALSE(bad_shape.has_value());
  CHECK(bad_shape.error().kind == ProviderError::Kind::Decode);

  // Neither `sources` nor `enc` -> the existing "no stream source" verdict.
  CHECK_FALSE(map_sources("{}", Translation::Sub, env).has_value());
  CHECK_FALSE(map_sources(R"({"enc":null})", Translation::Sub, env).has_value());
}

// ===========================================================================
// pick_subtitle / is_subtitle_track / english_captions
// ===========================================================================

namespace {

Track track(const char* file, std::optional<std::string> label, std::optional<std::string> kind,
            bool is_default) {
  Track t;
  t.file = file;
  t.label = std::move(label);
  t.kind = std::move(kind);
  t.is_default = is_default;
  return t;
}

// pick_subtitle returns an index; deref to the picked file (or nullopt).
std::optional<std::string> picked_file(const std::vector<Track>& tracks) {
  const auto i = pick_subtitle(tracks);
  if (!i.has_value()) return std::nullopt;
  return tracks[*i].file;
}

}  // namespace

TEST_CASE("pick_subtitle_default_wins_english_next_first_fallback_thumbnails_never") {
  const Track thumbs = track("https://c/thumbs.vtt", std::nullopt, "thumbnails", true);
  const Track spanish = track("https://c/spa.vtt", "Spanish", "captions", false);
  const Track english = track("https://c/eng.vtt", "English - CR", "captions", false);
  const Track eng_default = track("https://c/eng2.vtt", "English", "captions", true);
  const Track bare = track("https://c/bare.vtt", std::nullopt, std::nullopt, false);

  // Host default beats an earlier english; english beats first; thumbnails never
  // qualify.
  CHECK(picked_file({thumbs, english, eng_default}) == "https://c/eng2.vtt");
  CHECK(picked_file({spanish, english}) == "https://c/eng.vtt");
  CHECK(picked_file({thumbs, spanish}) == "https://c/spa.vtt");
  CHECK(picked_file({bare}) == "https://c/bare.vtt");
  CHECK_FALSE(picked_file({track("https://c/t.vtt", std::nullopt, "thumbnails", true)}).has_value());
  CHECK_FALSE(picked_file({}).has_value());
  // Unknown kind fails safe (never a wrong --sub-file).
  CHECK_FALSE(picked_file({track("https://c/alien.vtt", std::nullopt, "chapters", true)}).has_value());
}

TEST_CASE("english_captions_only_english_labeled_captions_host_order_capped") {
  const std::vector<Track> tracks = {
      track("https://c/eng-3.vtt", "English", "captions", true),
      track("https://c/spa.vtt", "Spanish", "captions", false),
      track("https://c/eng-4.vtt", "English 2", "captions", false),
      track("https://c/thumbs.vtt", "English", "thumbnails", false),
      track("https://c/eng-bare.vtt", "English", std::nullopt, false),
      track("https://c/bare.vtt", std::nullopt, "captions", false),
  };
  // Kind-less "English" qualifies; Spanish, thumbnails (even labeled English),
  // and unlabeled do not.
  const auto got = english_captions(tracks);
  CHECK(got == std::vector<std::string_view>{"https://c/eng-3.vtt", "https://c/eng-4.vtt",
                                             "https://c/eng-bare.vtt"});
  CHECK(english_captions({}).empty());

  std::vector<Track> flood;
  for (int i = 0; i < 12; ++i) flood.push_back(track("https://c/e.vtt", "English", "captions", false));
  CHECK(english_captions(flood).size() == kMaxSubtitleProbes);
}

// ===========================================================================
// provider surface (no network: guards fire before the wire)
// ===========================================================================

TEST_CASE("canonical_key_is_the_stringified_mal_id") {
  auto p = MegaPlay::create();
  REQUIRE(p.has_value());
  Enrichment with_mal;
  with_mal.anilist_id = 1;
  with_mal.mal_id = 52991;
  CHECK(p->canonical_key(with_mal) == "52991");
  Enrichment no_mal;
  no_mal.anilist_id = 1;
  CHECK_FALSE(p->canonical_key(no_mal).has_value());
}

TEST_CASE("search_is_structurally_unsupported") {
  auto p = MegaPlay::create();
  REQUIRE(p.has_value());
  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = 26;
  opts.page = 1;
  auto got = p->search("frieren", opts);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Unsupported);
  CHECK_FALSE(p->supports_search());
}

TEST_CASE("resolve_rejects_foreign_or_corrupt_episode_labels_before_any_network") {
  auto p = MegaPlay::create();
  REQUIRE(p.has_value());
  for (const char* bad : {"13.5", "0", ""}) {
    auto got = p->resolve("52991", bad, Translation::Sub, Quality::Best);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().kind == ProviderError::Kind::Decode);
  }
  // A path-smuggling show id is rejected by the show-id guard.
  auto got = p->resolve("../x", "1", Translation::Sub, Quality::Best);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("cover_request_absolute_passes_relative_and_unsafe_reject") {
  auto p = MegaPlay::create();
  REQUIRE(p.has_value());
  auto abs = p->cover_request("https://s4.anilist.co/file/cover.jpg");
  REQUIRE(abs.has_value());
  CHECK(abs->url == "https://s4.anilist.co/file/cover.jpg");
  CHECK_FALSE(abs->referer.has_value());
  CHECK_FALSE(p->cover_request("/posters/x.webp").has_value());
  CHECK_FALSE(p->cover_request("").has_value());
  CHECK_FALSE(p->cover_request("https://cdn/x y.jpg").has_value());
}

// ===========================================================================
// Transport — against a loopback fixture server that answers a fixed sequence
// of requests (embed, getSources, ...), same shape as anibd_tests'.
// ===========================================================================

namespace {

struct SequenceServer {
  int listen_fd = -1;
  std::uint16_t port = 0;
  std::thread thread;

  explicit SequenceServer(std::vector<std::vector<std::uint8_t>> responses) {
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
    REQUIRE(::listen(listen_fd, static_cast<int>(responses.size()) + 1) == 0);

    thread = std::thread([this, responses = std::move(responses)]() mutable {
      for (auto& resp : responses) {
        const int cfd = ::accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) return;
        char buf[16384];
        (void)::read(cfd, buf, sizeof(buf));
        ::write(cfd, resp.data(), resp.size());
        ::close(cfd);
      }
    });
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

  ~SequenceServer() {
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) ::close(listen_fd);
  }
};

std::vector<std::uint8_t> response_with_body(const std::string& status, std::string_view body) {
  std::string head = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n" +
                     "Content-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n";
  std::vector<std::uint8_t> out(head.begin(), head.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// A single-response provider (matches anibd_tests' against()), leaked into the
// returned provider's lifetime.
MegaPlay against(std::vector<std::uint8_t> response) {
  static std::vector<std::unique_ptr<SequenceServer>> keepalive;
  std::vector<std::vector<std::uint8_t>> one;
  one.push_back(std::move(response));
  keepalive.push_back(std::make_unique<SequenceServer>(std::move(one)));
  auto p = MegaPlay::with_host(keepalive.back()->url());
  REQUIRE(p.has_value());
  return std::move(*p);
}

}  // namespace

TEST_CASE("episodes_empty_when_embed_has_no_data_id") {
  // 200 with no data-id = authoritative not stocked (one embed GET).
  auto p = against(response_with_body("200 OK", "<html>no player here</html>"));
  auto eps = p.episodes("52991", Translation::Sub, std::optional<std::uint32_t>(12));
  REQUIRE(eps.has_value());
  CHECK(eps->empty());
}

TEST_CASE("episodes_mints_from_hint_when_embed_is_stocked") {
  auto p = against(response_with_body("200 OK", R"(<div id="megaplay-player" data-id="13458">)"));
  auto eps = p.episodes("52991", Translation::Sub, std::optional<std::uint32_t>(12));
  REQUIRE(eps.has_value());
  CHECK(eps->size() == 12);
  CHECK(eps->front() == "1");
  CHECK(eps->back() == "12");
}

TEST_CASE("episodes_rejects_a_non_numeric_show_id_before_fetch") {
  auto p = MegaPlay::create();
  REQUIRE(p.has_value());
  auto got = p->episodes("../7", Translation::Sub, std::nullopt);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind == ProviderError::Kind::Decode);
}

TEST_CASE("resolve_two_hop_embed_then_get_sources") {
  // Full resolve: embed (scrape data-id) then getSources (map the master +
  // softsub). Two hops answered in order.
  const char* embed = R"(<div id="megaplay-player" data-id="13458" data-lang="sub">)";
  const char* sources = R"({"sources":{"file":"https://cdn.mewstream.buzz/x/master.m3u8"},
      "tracks":[{"file":"https://1oe.club/eng.vtt","label":"English","kind":"captions","default":true}]})";
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", embed));
  responses.push_back(response_with_body("200 OK", sources));
  SequenceServer srv(std::move(responses));
  auto p = MegaPlay::with_host(srv.url());
  REQUIRE(p.has_value());
  auto link = p->resolve("52991", "1", Translation::Sub, Quality::Best);
  REQUIRE(link.has_value());
  CHECK(link->url == "https://cdn.mewstream.buzz/x/master.m3u8");
  CHECK(link->cloaked_segments);
  CHECK(link->decloak_segments);
  CHECK(link->sub_url == "https://1oe.club/eng.vtt");
}

TEST_CASE("resolve_scrapes_the_envelope_on_the_first_enc_body_then_reuses_the_cache") {
  // Fresh key/iv (NOT the baked pair) so success can only come from the
  // scrape, not a baked-fallback coincidence. Sealed offline (see
  // crypto_tests) under key "ShigokuTestKey12" (zero-padded to
  // 32) and iv "ShigokuTestIV123".
  const char* embed1 =
      R"(<div id="megaplay-player" data-id="500"><script src="/lib/newclient.min.js?v=9"></script>)";
  const char* sources1 =
      R"({"tracks":[],"enc":"jA6ZrULA2dedir8n_H0iOupouKH9ZpqwuUp-tYPKUi3fNARJE_s9m60voM1ZJPzcJwIgR7zTguJ4M)"
      R"(0xjiyGU2Q=="})";
  const char* script =
      R"(var a=String(e.pick(["trustAesKey","TRUST_AES_KEY"],"ShigokuTestKey12")),)"
      R"(c=String(e.pick(["trustAesIv","TRUST_AES_IV"],"ShigokuTestIV123"));)";
  const char* embed2 =
      R"(<div id="megaplay-player" data-id="501"><script src="/lib/newclient.min.js?v=9"></script>)";
  // Same fresh key (cached from resolve #1); a different plaintext proves this
  // body was actually opened, not just a stale link reused.
  const char* sources2 =
      R"({"tracks":[],"enc":"jA6ZrULA2dedir8n_H0iOupouKH9ZpqwuUp-tYPKUi0fLHUbd_jowcwS3tCU_IM56PDYV9JRpza)"
      R"(QOWIoOrRkmQ=="})";

  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", embed1));
  responses.push_back(response_with_body("200 OK", sources1));
  responses.push_back(response_with_body("200 OK", script));
  responses.push_back(response_with_body("200 OK", embed2));
  responses.push_back(response_with_body("200 OK", sources2));
  // Exactly 5 responses queued: a regression that re-fetches the script on
  // resolve #2 finds no 6th response waiting and times out, failing the
  // REQUIRE below instead of silently passing.
  SequenceServer srv(std::move(responses));
  auto p = MegaPlay::with_host(srv.url());
  REQUIRE(p.has_value());

  auto link1 = p->resolve("52991", "1", Translation::Sub, Quality::Best);
  REQUIRE(link1.has_value());
  CHECK(link1->url == "https://cdn.scraped-test.example/x/master.m3u8");

  auto link2 = p->resolve("52991", "2", Translation::Sub, Quality::Best);
  REQUIRE(link2.has_value());
  CHECK(link2->url == "https://cdn.scraped-test.example/y/master2.m3u8");
}

TEST_CASE("resolve_falls_back_to_the_baked_pair_when_the_script_404s") {
  const char* embed =
      R"(<div id="megaplay-player" data-id="500"><script src="/lib/newclient.min.js?v=9"></script>)";
  // The real captured `enc` (One Piece 21 ep1 sub), sealed under the BAKED
  // pair — this only opens if the 404'd script GET correctly falls back.
  const char* sources =
      R"({"tracks":[],"enc":"wdeBruh3qqn_i5wUNnyaPcXqidp1UWP84FfPHzGyKXA2hDZBfMCmZ4FLvs7_pQuH549Eptax8UOjA)"
      R"(JyRIZfrRhUGUKy9OBeGh2yB_-m_JLAlLnWTLzYZC3_C5I4ltveYoiaU66Do9RgI9bCetmk_o87-sd66brXnWV1MbjhLnjw="})";
  std::vector<std::vector<std::uint8_t>> responses;
  responses.push_back(response_with_body("200 OK", embed));
  responses.push_back(response_with_body("200 OK", sources));
  responses.push_back(response_with_body("404 Not Found", ""));
  SequenceServer srv(std::move(responses));
  auto p = MegaPlay::with_host(srv.url());
  REQUIRE(p.has_value());

  auto link = p->resolve("52991", "1", Translation::Sub, Quality::Best);
  REQUIRE(link.has_value());
  CHECK(link->url == "https://fetch.nexabloom.top/anime/f899139df5e1059396431415e770c6dd/"
                     "61b87186ab260d05003427e16ccf5657/master.m3u8");
}
