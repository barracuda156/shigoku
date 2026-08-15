// megaplay_tests.cpp — P13 golden tests, ported 1:1 from
// src/providers/megaplay.rs's `mod tests`. Pure parsers + guards run offline;
// the transport cases (episodes probe, private-host sub drop) run over a
// loopback fixture server, same SequenceServer shape as anibd_tests.cpp.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../src/megaplay.hpp"

using namespace shigoku;
using namespace shigoku::megaplay;
using namespace shigoku::megaplay::detail;

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
  auto s = map_sources(raw, Translation::Sub);
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
  // A dub resolve never loads a softsub.
  auto d = map_sources(raw, Translation::Dub);
  REQUIRE(d.has_value());
  CHECK_FALSE(d->link.sub_url.has_value());
}

TEST_CASE("map_sources_missing_or_unsafe_stream_url_is_a_clean_error") {
  CHECK_FALSE(map_sources("{}", Translation::Sub).has_value());
  CHECK_FALSE(map_sources(R"({"sources":{}})", Translation::Sub).has_value());
  CHECK_FALSE(map_sources(R"({"sources":{"file":"/x/master.m3u8"}})", Translation::Sub).has_value());
  CHECK_FALSE(
      map_sources(R"({"sources":{"file":"https://cdn/x master.m3u8"}})", Translation::Sub).has_value());
  // An unsafe track is dropped, not fatal, and cannot become the sub pick.
  auto s = map_sources(
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[{"file":"/relative.vtt","kind":"captions"}]})",
      Translation::Sub);
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
    auto s = map_sources(raw, Translation::Sub);
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
    CHECK_FALSE(map_sources(raw, Translation::Sub).has_value());
  }
  // But absent/null optionals still degrade cleanly (serde default), not error:
  // null sources.file -> "no stream source"; a null default -> false.
  CHECK_FALSE(map_sources(R"({"sources":{"file":null}})", Translation::Sub).has_value());
  auto ok = map_sources(
      R"({"sources":{"file":"https://cdn/ok.m3u8"},"tracks":[{"file":"https://c/e.vtt","kind":null,"default":null}]})",
      Translation::Sub);
  REQUIRE(ok.has_value());
  CHECK(ok->tracks.size() == 1);
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
