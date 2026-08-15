// http.hpp — the one thin libcurl wrapper + the SSRF fetch guard (P2, A5).
//
// Ported from sabigoku src/providers/http.rs (the client + status mapping) and
// src/fetchguard.rs (the guard). One fetch path, one status taxonomy
// (ProviderError::from_status), redirects refused on EVERY request — a followed
// 3xx would re-enter unguarded (03 §6.7) and fixed API endpoints have no
// business redirecting.
//
// §3: HTTP/2-over-TLS is load-bearing (anti-challenge-page); the client sets
// CURL_HTTP_VERSION_2TLS on every request and a startup nghttp2 tripwire lives
// in `assert_http2_available()`. All host classification is byte-wise (no
// multi-byte int packing) so it is identical big- and little-endian.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "error.hpp"
#include "result.hpp"

namespace shigoku::http {

// Response body ceiling (ROD-341): oversize fails the fetch, never grows
// unbounded. 4 MiB, matching http.rs MAX_RESP_BYTES.
inline constexpr std::size_t kMaxRespBytes = 4u * 1024u * 1024u;
// Default wall-clock rail (http.rs DEADLINE, ROD-262): a silent host must not
// hang a detached worker. Seconds.
inline constexpr long kDefaultTimeoutSecs = 10;
// Stall rail for the streaming path (P35 downloads): a multi-hundred-MB
// transfer must not ride the 10s wall rail, but a dead line must still abort.
// Below kStallFloorBytesPerSec for kStallSecs aborts the transfer.
inline constexpr long kStallFloorBytesPerSec = 1;
inline constexpr long kStallSecs = 30;

// The centralized browser UA (A5). Copied verbatim from senshi.rs:28 — the CDN
// bot-scoring wants a real Chrome shape; without it (and a referer) senshi
// 403s. One string so every provider shares it.
inline constexpr const char* kBrowserUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";

enum class Method {
  Get,
  Post,
  Put,
};

// Success policy (http.rs Accept): allanime wants 200 only; REST providers
// accept any 2xx.
enum class Accept {
  OkOnly,
  Any2xx,
};

// TLS ClientHello shaping (P25). Default None = the app's stock OpenSSL
// handshake (every request except anidbapp's). Chrome/Firefox route the fetch
// through libcurl-impersonate (BoringSSL) so an edge that fingerprints the
// handshake (anidb.app's Cloudflare challenge) sees a real browser. Keeping
// None the default means a BoringSSL/impersonate fault is contained to the one
// provider that opts in — see project_p25_clienthello_decision.
enum class Fingerprint {
  None,     // stock OpenSSL libcurl (app default).
  Chrome,   // impersonate target "chrome124".
  Firefox,  // impersonate target "firefox133".
};

struct Header {
  std::string name;
  std::string value;
};

// A single request. Owns nothing borrowed across the fetch — callers build it
// and hand it in. Payload is (content_type, body) for POST/PUT.
struct Request {
  Method method = Method::Get;
  std::string url;
  std::optional<std::string> content_type;  // set with body for POST/PUT.
  std::vector<std::uint8_t> body;
  std::string user_agent = kBrowserUserAgent;
  std::vector<Header> extra_headers;
  Accept accept = Accept::Any2xx;
  std::optional<long> timeout_secs;  // per-request override of the 10s rail.
  // P25: None = stock OpenSSL (default); Chrome/Firefox = libcurl-impersonate.
  // Only providers whose edge fingerprints the TLS handshake set this.
  Fingerprint fingerprint = Fingerprint::None;
};

// --- Streaming fetch (P35 downloads) ---------------------------------------

// Response metadata handed to the sink alongside every chunk (identical across
// one transfer). For a 206 the content_length is the REMAINING byte count of
// the ranged response, not the full file.
struct StreamMeta {
  std::uint16_t status = 0;
  std::optional<std::uint64_t> content_length;
};

// Body sink for fetch_to_sink: called once per libcurl write with the response
// meta + a chunk. Return false to abort the transfer (surfaces as Network; a
// caller that itself requested the abort classifies it from its own state).
// Only bytes of an accepted (2xx) response ever reach the sink.
using SinkFn =
    std::function<bool(const StreamMeta&, const std::uint8_t*, std::size_t)>;

// Startup tripwire (§3, risk register): a curl without nghttp2 silently drops
// to HTTP/1.1 and trips the edge challenge page. This is a misconfiguration
// detector, not a degrade path — Err(detail) means "rebuild curl with nghttp2".
[[nodiscard]] Result<Unit, std::string> assert_http2_available();

// The client. Holds no per-request state; each fetch() uses a fresh easy handle
// (fine for v0 — one blocking request per worker, A5). curl_global_init is
// done once by the first Client construction (guarded).
class Client {
 public:
  [[nodiscard]] static Result<Client, ProviderError> create();

  Client(Client&&) noexcept = default;
  Client& operator=(Client&&) noexcept = default;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client() = default;

  // Blocking fetch. Maps transport failure -> Network, non-accepted status ->
  // from_status(), oversize -> Decode. Redirects are NOT followed: a 3xx status
  // is returned as Http{3xx} (http.rs redirect_is_refused_not_followed).
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> fetch(
      const Request& req) const;

  // Streaming fetch (P35): like fetch() but hands body chunks to `sink`
  // instead of buffering, with NO 4 MiB cap — the sink owns growth (downloads
  // write to disk). Non-2xx body bytes are discarded (the sink never sees
  // them) and the status maps through from_status() as usual; `req.accept` is
  // ignored (any 2xx is accepted — the sink tells 200 from 206 via the meta).
  // Timeouts differ from fetch(): absent req.timeout_secs means NO wall rail
  // (a long transfer is the point) — a 10s connect budget plus the stall rail
  // (kStallFloorBytesPerSec / kStallSecs) abort a dead line instead. `abort`,
  // when non-null, is polled from libcurl's progress callback so a caller can
  // stop a transfer even while the line is idle. Ok carries the HTTP status
  // (200 vs 206 is the Range-resume distinction).
  [[nodiscard]] Result<std::uint16_t, ProviderError> fetch_to_sink(
      const Request& req, const SinkFn& sink,
      const std::atomic<bool>* abort = nullptr) const;

 private:
  Client() = default;
};

// --- The SSRF fetch guard (fetchguard.rs, 03 §6.7) -------------------------
//
// Verdict for a provider-supplied URL (stream, embed, playlist, sub, cover)
// BEFORE any fetch. Callers refuse on Err; they never "sanitize and proceed".
// Every guarded fetch must ALSO refuse redirects (the Client already does).
//
// Residual (accepted, 03 §6.7): DNS rebinding — a public name resolving to a
// private IP at connect time passes; there is no resolve-then-validate hook.
[[nodiscard]] Result<Unit, GuardError> guard_fetch_url(std::string_view raw);

// --- Guard internals, exposed for the golden tests (fetchguard.rs tests) ----
namespace guard_detail {

// Percent-decode a host component (so 127%2e0%2e0%2e1 / %6c%6fcalhost are seen
// decoded, as the WHATWG parser would). Returns nullopt on a malformed escape.
[[nodiscard]] std::optional<std::string> percent_decode_host(std::string_view s);

// True if the four octets name a private / loopback / link-local / CGNAT /
// multicast v4 address (private_v4). Octets in memory order (§3).
[[nodiscard]] bool private_v4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                              std::uint8_t d);

// Parse every IPv4 spelling the guard must canonicalize: dotted-decimal,
// bare decimal (2130706433), hex (0x7f000001), dotted-hex (0x7f.0.0.1),
// short forms (127.1). nullopt = not an IPv4 literal at all. On success the
// four canonical octets are written to out[0..4].
[[nodiscard]] bool parse_ipv4_any(std::string_view host, std::uint8_t out[4]);

// True if the 8 hextets name a private / loopback / link-local v6 address, or
// an embedded v4 (::ffff:a.b.c.d mapped OR ::a.b.c.d compatible) that is
// private (private_v6, widened per ROD-436). Segments in host order.
[[nodiscard]] bool private_v6(const std::uint16_t seg[8]);

// Parse a bracketed-or-bare IPv6 literal into 8 hextets, resolving `::` and a
// trailing embedded IPv4. nullopt = not an IPv6 literal. Written to out[0..8].
[[nodiscard]] bool parse_ipv6(std::string_view host, std::uint16_t out[8]);

// The numeric-spelling backstop (numeric_spelling): a host that reached the
// domain arm but is all-numeric labels or 0x-prefixed — defense in depth
// against parser drift letting an IPv4 spelling masquerade as a domain.
[[nodiscard]] bool numeric_spelling(std::string_view domain);

}  // namespace guard_detail

}  // namespace shigoku::http
