// proxy_tests.cpp — P12 golden tests, ported 1:1 from src/proxy.rs's `mod
// tests`. The pure codec/scan/rewrite helpers run offline; fetch_upstream's
// guard/injection cases run with no network (the SSRF guard rejects before
// send); the lifecycle + token-gate cases drive a real loopback socket against
// a live `engage`d proxy, the same shape as the Rust tests.
//
// The proxy's SSRF guard blocks a loopback upstream, so — exactly as the Rust
// module notes at respond()'s doc — the full wire path can only be exercised
// piecewise: `respond` and `serve` (token gate → dies at the guard) are tested
// separately, never over a real loopback upstream fetch.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../src/proxy.hpp"

using namespace shigoku;
using namespace shigoku::proxy;
using namespace shigoku::proxy::detail;

// ---------------------------------------------------------------------------
// Small helpers bridging the Rust slice/BufRead idioms to the C++ signatures.
// ---------------------------------------------------------------------------
static std::vector<std::uint8_t> bytes(std::string_view s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}
static bool is_playlist_s(std::string_view s) {
  auto b = bytes(s);
  return is_playlist(b.data(), b.size());
}
static bool is_playlist_b(const std::vector<std::uint8_t>& b) {
  return is_playlist(b.data(), b.size());
}
// decloak(&[u8]) -> &[u8], as a copied output vector for length/first-byte
// assertions.
static std::vector<std::uint8_t> decloak(const std::vector<std::uint8_t>& body) {
  const std::size_t off = decloak_offset(body.data(), body.size());
  return std::vector<std::uint8_t>(body.begin() + off, body.end());
}
static bool looks_like_fmp4_s(std::string_view s) {
  auto b = bytes(s);
  return looks_like_fmp4(b.data(), b.size());
}

// ===========================================================================
// percent_encode / percent_decode
// ===========================================================================
TEST_CASE("percent_round_trips_the_full_upstream_url") {
  const std::string url =
      "https://cdn.nekostream.site/x/master.m3u8?token=ab.cd_ef&exp=1720000000";
  const std::string enc = percent_encode(url);
  // Reserved bytes are escaped; only alnum - _ ~ stay literal. Dots too, so the
  // loopback url's sole extension is the synthetic `.ts`.
  for (char reserved : {':', '/', '?', '&', '.'}) {
    CHECK(enc.find(reserved) == std::string::npos);
  }
  auto dec = percent_decode(enc);
  REQUIRE(dec.has_value());
  CHECK(*dec == bytes(url));
}

TEST_CASE("percent_decode_rejects_truncated_or_invalid_escape") {
  CHECK(!percent_decode("abc%").has_value());
  CHECK(!percent_decode("abc%2").has_value());
  CHECK(!percent_decode("abc%zz").has_value());
  auto ok = percent_decode("a%20b");
  REQUIRE(ok.has_value());
  CHECK(*ok == bytes("a b"));
}

// ===========================================================================
// is_playlist
// ===========================================================================
TEST_CASE("is_playlist_extm3u_past_bom_ws_true_ts_bytes_false") {
  CHECK(is_playlist_s("#EXTM3U\n#EXT-X-VERSION:3\n"));
  CHECK(is_playlist_b({0xEF, 0xBB, 0xBF, '#', 'E', 'X', 'T', 'M', '3', 'U', '\n'}));
  CHECK(is_playlist_s("  \n#EXTM3U"));
  CHECK(!is_playlist_b({0x47, 0x40, 0x00, 0x10}));
  CHECK(!is_playlist_b({}));
  CHECK(!is_playlist_b({0x89, 'P', 'N', 'G', '\r', '\n'}));
}

TEST_CASE("is_playlist_rejects_binary_segment_wearing_extm3u_prefix") {
  CHECK(is_playlist_s("#EXTM3U\n#EXT-X-VERSION:3\nseg0\n"));
  // A TS segment prefixed with the magic bytes but carrying NULs must NOT be
  // treated as a playlist (would be shredded through the rewriter); it falls to
  // the de-cloak path instead.
  std::vector<std::uint8_t> spoof = bytes("#EXTM3U\n");
  for (std::uint32_t i = 0; i < 504u; ++i) {
    spoof.push_back(i % 7 == 0 ? 0x00 : 0x47);
  }
  CHECK(!is_playlist_b(spoof));
}

// ===========================================================================
// decloak
// ===========================================================================
TEST_CASE("decloak_strips_a_decoy_prefix_to_the_first_ts_sync_triple") {
  for (std::size_t prefix : {std::size_t(70), std::size_t(252)}) {
    std::vector<std::uint8_t> buf(prefix + 3 * kTsPacket, 0);
    for (std::size_t i = 0; i < buf.size(); ++i) {
      buf[i] = static_cast<std::uint8_t>(i % 251);
    }
    buf[0] = 0x89;  // definitely not 0x47 at byte 0
    buf[prefix] = 0x47;
    buf[prefix + kTsPacket] = 0x47;
    buf[prefix + 2 * kTsPacket] = 0x47;
    auto out = decloak(buf);
    CHECK(out.size() == 3 * kTsPacket);
    CHECK(out[0] == 0x47);
  }
}

TEST_CASE("decloak_passes_clean_and_unrecognized_streams_through_untouched") {
  // Clean TS from byte 0 matches at i=0 -> unchanged.
  std::vector<std::uint8_t> clean(3 * kTsPacket, 0);
  for (std::size_t i = 0; i < clean.size(); ++i) {
    clean[i] = static_cast<std::uint8_t>(i % 251);
  }
  clean[0] = 0x47;
  clean[kTsPacket] = 0x47;
  clean[2 * kTsPacket] = 0x47;
  CHECK(decloak(clean).size() == clean.size());
  CHECK(decloak(clean)[0] == 0x47);

  // No sync triple anywhere (fMP4-ish) -> pass through, do not corrupt.
  std::vector<std::uint8_t> fmp4;
  for (int r = 0; r < 8; ++r) {
    const std::uint8_t chunk[] = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y',
                                  'p', 'm', 'p', '4', '2'};
    fmp4.insert(fmp4.end(), chunk, chunk + sizeof(chunk));
  }
  CHECK(decloak(fmp4) == fmp4);
}

TEST_CASE("decloak_passes_through_when_sync_sits_past_the_scan_window") {
  // Sync at offset 5000 (> kMaxPrefixScan): returned unchanged, never a
  // wrong-offset strip. Documents the ceiling as a known pass-through.
  std::vector<std::uint8_t> buf(5000 + 3 * kTsPacket, 0);
  for (std::size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<std::uint8_t>(i % 251);
  }
  buf[0] = 0x89;
  buf[5000] = 0x47;
  buf[5000 + kTsPacket] = 0x47;
  buf[5000 + 2 * kTsPacket] = 0x47;
  CHECK(decloak(buf).size() == buf.size());
  CHECK(decloak(buf)[0] == 0x89);
}

TEST_CASE("looks_like_fmp4_distinguishes_iso_bmff_from_a_cloak_that_outgrew_the_window") {
  CHECK(looks_like_fmp4_s(std::string("\x00\x00\x00\x18""ftypmp42", 12)));
  CHECK(looks_like_fmp4_s(std::string("\x00\x00\x00\x18""styp....", 12)));
  CHECK(looks_like_fmp4_s(std::string("\x00\x00\x00\x18""moof....", 12)));
  CHECK(!looks_like_fmp4_s(std::string("\x89PNG\r\n\x1a\n", 8)));
  // Too short to carry a box type; also the head of a real TS packet.
  CHECK(!looks_like_fmp4_s(std::string("\x47\x40\x00", 3)));
}

// ===========================================================================
// url_bytes_clean
// ===========================================================================
TEST_CASE("url_bytes_clean_rejects_crlf_nul_and_nonprintables") {
  CHECK(url_bytes_clean("https://cdn.example/x/master.m3u8?sig=ab-cd_ef.gh"));
  CHECK(!url_bytes_clean("https://cdn.example/x\r\nX-Injected: evil"));
  CHECK(!url_bytes_clean("https://cdn.example/x\ny"));
  CHECK(!url_bytes_clean(std::string("https://cdn.example/x\x00y", 22)));
  CHECK(!url_bytes_clean("https://cdn.example/a b"));
  CHECK(!url_bytes_clean(std::string("https://cdn.example/\x7f", 21)));
  CHECK(!url_bytes_clean(""));
}

// ===========================================================================
// build_loopback_url
// ===========================================================================
TEST_CASE("build_loopback_url_encodes_upstream_into_a_decodable_ref") {
  const std::string up = "https://cdn.example/seg/000.ts?sig=xyz";
  const std::string prefix = path_prefix("0123abcd");
  const std::string lb = build_loopback_url(3210, prefix, up);
  CHECK(lb.rfind("http://127.0.0.1:3210/r.ts?t=0123abcd&u=", 0) == 0);
  // Only the synthetic path extension; no literal dot from the upstream (the
  // hex token cannot carry one).
  std::size_t dot_ts = 0, at = 0;
  while ((at = lb.find(".ts", at)) != std::string::npos) {
    ++dot_ts;
    at += 3;
  }
  CHECK(dot_ts == 1);
  const std::string head = "http://127.0.0.1:3210/r.ts?t=0123abcd&u=";
  const std::string enc = lb.substr(head.size());
  auto dec = percent_decode(enc);
  REQUIRE(dec.has_value());
  CHECK(*dec == bytes(up));
}

// ===========================================================================
// rewrite_playlist
// ===========================================================================
TEST_CASE("rewrite_playlist_repoints_variants_segments_and_uri_tags") {
  const std::string base = "https://cdn.nekostream.site/hls/master.m3u8";
  const std::string master =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,URI=\"audio/en.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=842x480\n"
      "480/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1920x1080\n"
      "https://other.cdn/1080/index.m3u8\n";
  const std::string prefix = path_prefix("feedc0de");
  const std::string out = rewrite_playlist(master, base, 45678, prefix);

  CHECK(out.rfind("#EXTM3U\n", 0) == 0);
  // Every rewritten ref carries the playback token.
  CHECK(out.find("http://127.0.0.1:45678/r.ts?t=feedc0de&u=") != std::string::npos);
  // The absolute variant is encoded (no bare https:// left on a URI line).
  CHECK(out.find("\nhttps://other.cdn/1080") == std::string::npos);
  // The audio rendition URI attribute was rewritten in place.
  CHECK(out.find("#EXT-X-MEDIA:TYPE=AUDIO,URI=\"http://127.0.0.1:45678/r.ts?t="
                 "feedc0de&u=") != std::string::npos);
  // Every rewritten target decodes back to a real upstream url.
  std::size_t pos = 0;
  while (pos < out.size()) {
    const std::size_t nl = out.find('\n', pos);
    const std::string line =
        out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? out.size() : nl + 1;
    const std::size_t at2 = line.find("&u=");
    if (at2 == std::string::npos) continue;
    std::string enc = line.substr(at2 + 3);
    const std::size_t q = enc.find('"');
    if (q != std::string::npos) enc = enc.substr(0, q);
    auto decoded = percent_decode(enc);
    REQUIRE(decoded.has_value());
    const std::string s(decoded->begin(), decoded->end());
    CHECK(s.rfind("https://", 0) == 0);
  }
}

// ===========================================================================
// fetch_upstream — guard + header-injection, no network
// ===========================================================================
TEST_CASE("fetch_upstream_refuses_a_private_ip_before_any_request") {
  for (const char* blocked : {"http://127.0.0.1/x",
                              "http://169.254.169.254/latest/meta-data/",
                              "http://[::1]/x"}) {
    auto r = fetch_upstream(blocked, std::nullopt, std::nullopt);
    REQUIRE(!r.has_value());
    CHECK(r.error() == FetchError::Blocked);
  }
}

TEST_CASE("fetch_upstream_rejects_header_injection_bytes_before_any_request") {
  auto r = fetch_upstream("https://cdn.example/x\r\nEvil: 1", std::nullopt,
                          std::nullopt);
  REQUIRE(!r.has_value());
  CHECK(r.error() == FetchError::BadUrl);
}

// ===========================================================================
// read_request_target — over a ByteSource
// ===========================================================================
// A ByteSource over a fixed buffer (pipelined requests).
static ByteSource buffer_source(std::shared_ptr<std::size_t> pos,
                                std::shared_ptr<std::vector<std::uint8_t>> buf) {
  return [pos, buf]() -> int {
    if (*pos >= buf->size()) return -1;
    return (*buf)[(*pos)++];
  };
}

TEST_CASE("read_request_target_reads_sequential_requests_on_one_connection") {
  // Two pipelined requests off one buffer prove the read loop yields both
  // targets in order, then nullopt at EOF.
  auto buf = std::make_shared<std::vector<std::uint8_t>>(bytes(
      "GET /r.ts?u=abc HTTP/1.1\r\nHost: x\r\n\r\nGET /second HTTP/1.1\r\n\r\n"));
  auto pos = std::make_shared<std::size_t>(0);
  ByteSource src = buffer_source(pos, buf);
  auto a = read_request_target(src);
  REQUIRE(a.has_value());
  CHECK(*a == "/r.ts?u=abc");
  auto b = read_request_target(src);
  REQUIRE(b.has_value());
  CHECK(*b == "/second");
  CHECK(!read_request_target(src).has_value());
}

TEST_CASE("read_request_target_cap_terminates_on_an_infinite_stream") {
  // An endless 'a' stream with no newline: without the head cap the read would
  // grow without bound. The cap makes it terminate with a bounded (spaceless ->
  // empty) target. A finite reader can't demonstrate this; the generator can.
  ByteSource infinite = []() -> int { return 'a'; };
  auto t = read_request_target(infinite);
  REQUIRE(t.has_value());
  CHECK(*t == "");
}

// ===========================================================================
// respond — dispatch playlist -> rewrite, segment -> decloak
// ===========================================================================
TEST_CASE("respond_dispatches_playlist_to_rewrite_and_segment_to_decloak") {
  const std::string prefix = path_prefix("ba5eba11");

  // Playlist: every URI re-pointed to loopback, m3u8 type, keep-alive.
  std::vector<std::uint8_t> out;
  auto pl = bytes("#EXTM3U\n480/index.m3u8\n");
  const KeepAlive ka =
      respond(out, pl.data(), pl.size(), "https://cdn.example/hls/master.m3u8",
              4444, prefix);
  CHECK(ka == KeepAlive::Yes);
  const std::string text(out.begin(), out.end());
  CHECK(text.find("Content-Type: application/vnd.apple.mpegurl") !=
        std::string::npos);
  CHECK(text.find("Connection: keep-alive") != std::string::npos);
  CHECK(text.find("http://127.0.0.1:4444/r.ts?t=ba5eba11&u=") !=
        std::string::npos);
  CHECK(text.find("\n480/index.m3u8") == std::string::npos);  // rewritten

  // Segment: decoy prefix stripped to the TS sync, mp2t type, exact length.
  std::vector<std::uint8_t> seg(70 + 3 * kTsPacket, 0);
  seg[0] = 0x89;
  seg[70] = 0x47;
  seg[70 + kTsPacket] = 0x47;
  seg[70 + 2 * kTsPacket] = 0x47;
  std::vector<std::uint8_t> out2;
  (void)respond(out2, seg.data(), seg.size(), "https://cdn.example/seg/0.ts",
                4444, prefix);
  // Split head/body at the CRLFCRLF.
  const std::string needle = "\r\n\r\n";
  std::size_t head_end = std::string::npos;
  for (std::size_t i = 0; i + 4 <= out2.size(); ++i) {
    if (std::memcmp(out2.data() + i, needle.data(), 4) == 0) {
      head_end = i + 4;
      break;
    }
  }
  REQUIRE(head_end != std::string::npos);
  const std::string head(out2.begin(), out2.begin() + head_end);
  const std::size_t body_len = out2.size() - head_end;
  CHECK(head.find("Content-Type: video/mp2t") != std::string::npos);
  CHECK(head.find("Content-Length: " + std::to_string(3 * kTsPacket)) !=
        std::string::npos);
  CHECK(body_len == 3 * kTsPacket);
  CHECK(out2[head_end] == 0x47);
}

// ===========================================================================
// Live loopback: lifecycle stop-wakes-accept, and the token gate.
// ===========================================================================

// Split a live guard url into (host:port, tokened path prefix up to &u=).
static void split_guard_url(const std::string& url, std::string& host,
                            std::string& prefix) {
  const std::string h = "http://";
  const std::string rest = url.substr(h.size());
  const std::size_t slash = rest.find('/');
  host = rest.substr(0, slash);
  const std::string path = rest.substr(slash);
  const std::size_t u_end = path.find("&u=") + 3;
  prefix = path.substr(0, u_end);
}

// One raw request over a fresh connection to host:port; the error paths under
// test all close it, so read-to-EOF returns the full response.
static std::string roundtrip(const std::string& hostport,
                             const std::string& raw) {
  const std::size_t colon = hostport.rfind(':');
  const std::string host = hostport.substr(0, colon);
  const std::uint16_t port =
      static_cast<std::uint16_t>(std::stoi(hostport.substr(colon + 1)));
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  ::write(fd, raw.data(), raw.size());
  std::string buf;
  char tmp[4096];
  for (;;) {
    const ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n <= 0) break;
    buf.append(tmp, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return buf;
}

static StreamLink decloak_link() {
  StreamLink link;
  link.url = "https://example.invalid/master.m3u8";
  link.decloak_segments = true;
  return link;
}

// engage() is the seam build_argv's play_url rides on (player.cpp attempt_play):
// a flagged link yields a loopback master url (the positional mpv opens), an
// unflagged link is a transparent pass-through (play_url == link.url). This is
// the "loopback positional iff decloak_segments" half of the P12 DoD; the other
// half — that play_url becomes the argv positional verbatim — is player_tests'
// argv golden (any play_url lands as the last argv entry).
TEST_CASE("engage_passes_through_an_unflagged_link_without_a_proxy") {
  StreamLink link;
  link.url = "https://cdn.example/plain/master.m3u8";
  link.decloak_segments = false;
  auto engaged = engage(link);
  REQUIRE(engaged.has_value());
  // No proxy: the url mpv opens is the upstream itself, not a loopback ref.
  CHECK(std::string(engaged->url()) == link.url);
  CHECK(std::string(engaged->url()).find("127.0.0.1") == std::string::npos);
}

TEST_CASE("engage_a_flagged_link_yields_a_tokened_loopback_master") {
  auto engaged = engage(decloak_link());
  REQUIRE(engaged.has_value());
  const std::string url(engaged->url());
  // Loopback master, carrying the playback token, encoding the upstream in `u`.
  CHECK(url.rfind("http://127.0.0.1:", 0) == 0);
  CHECK(url.find("/r.ts?t=") != std::string::npos);
  const std::size_t u = url.find("&u=");
  REQUIRE(u != std::string::npos);
  auto dec = percent_decode(url.substr(u + 3));
  REQUIRE(dec.has_value());
  CHECK(std::string(dec->begin(), dec->end()) ==
        "https://example.invalid/master.m3u8");
}

TEST_CASE("proxy_lifecycle_stop_wakes_accept_and_does_not_hang") {
  auto engaged = engage(decloak_link());
  REQUIRE(engaged.has_value());
  // Move the guard into an optional so we can destroy it early (the C++ analog
  // of Rust's `drop(guard)`; std::expected has no reset()).
  std::optional<Decloak> guard(std::move(*engaged));
  const std::string url(guard->url());
  CHECK(url.rfind("http://127.0.0.1:", 0) == 0);

  // Drive one request to an unknown path: accept + parse/response path with no
  // upstream fetch (no network). A 404 closes the connection. stop() (via the
  // guard's destructor) must self-dial to unblock accept and join; a regression
  // here hangs under the test timeout.
  std::string host, prefix;
  split_guard_url(url, host, prefix);
  CHECK(roundtrip(host, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n").find("404") !=
        std::string::npos);
  guard.reset();  // destroy the Decloak -> Proxy::stop(); must return, not hang
}

TEST_CASE("serve_gates_on_the_playback_token") {
  auto guard = engage(decloak_link());
  REQUIRE(guard.has_value());
  std::string host, prefix;
  split_guard_url(std::string(guard->url()), host, prefix);

  // `u` points at loopback, which the SSRF guard rejects AFTER the token gate
  // and BEFORE any network send. So the real token gets past the gate and dies
  // at the guard (502); flipping one token nibble dies at the gate (404).
  // Together they prove the gate exists AND sits in front of the fetch path,
  // with zero network.
  const std::string blocked = percent_encode("http://127.0.0.1/x");
  const std::string good =
      "GET " + prefix + blocked + " HTTP/1.1\r\nHost: x\r\n\r\n";
  CHECK(roundtrip(host, good).find("502") != std::string::npos);

  const std::size_t tok_at = prefix.find("t=") + 2;
  std::string bad_prefix = prefix;
  const char flipped = (prefix[tok_at] == '0') ? '1' : '0';
  bad_prefix[tok_at] = flipped;
  const std::string bad =
      "GET " + bad_prefix + blocked + " HTTP/1.1\r\nHost: x\r\n\r\n";
  CHECK(roundtrip(host, bad).find("404") != std::string::npos);
}
