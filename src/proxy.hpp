// proxy.hpp — localhost HLS de-cloaking reverse proxy (P12). Ported from
// sabigoku src/proxy.rs (ROD-443, itself ported from zigoku v0.4.8).
//
// Some CDNs (nekostream via ibyteimg) prepend a decoy image header to each
// MPEG-TS segment: N junk bytes, then the real TS sync at that offset. ffmpeg
// content-probes byte 0, sees the fake magic, classifies the stream as an
// image, and dies. No mpv/ffmpeg flag reaches the inner segment demuxer, so the
// prefix must be stripped before mpv sees it.
//
// Shape: mpv talks plaintext HTTP to 127.0.0.1:<ephemeral>/r.ts?t=<token>&u=
// <pct upstream>. `t` is a random per-playback token (ROD-447): without it any
// local process scanning ephemeral ports gets a free SSRF-guarded relay. Each
// request fetches the upstream (referer + UA, redirects by hand so every hop is
// guarded), then either rewrites a playlist to tokened loopback refs or strips
// a segment's decoy prefix to the first TS-sync triple.
//
// Lifecycle is one playback: engage() starts the proxy when the link is flagged
// and hands back a Decloak guard; the player points mpv at guard.url() and
// destroys the guard on exit, tearing the proxy down. The guard owns an
// Arc<Proxy> (here shared_ptr<Proxy>): in-flight handler threads keep it alive
// past teardown, so there is no drain/leak choice — refcounting is the whole
// story (08 §10).
//
// §3 endianness: the byte-strip and every host/url scan is byte-wise; no buffer
// is ever cast to a wider int, so it is identical big- and little-endian (PPC).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "result.hpp"

namespace shigoku::proxy {

// Reason engage() could not start a proxy. Bind/token failures carry a message.
struct ProxyStartError {
  enum class Kind {
    Bind,   // bind 127.0.0.1:0 failed
    Token,  // /dev/urandom read failed while minting the playback token
  };
  Kind kind;
  std::string detail;
};

class Proxy;  // opaque; defined in proxy.cpp

// Playback-scoped de-cloaking guard. engage() returns one per play call: either
// a pass-through (link not flagged) or a live proxy. Destroying it stops the
// proxy, so the player holds it for exactly the mpv process lifetime.
class Decloak {
 public:
  // The url mpv must open: the loopback master when the link is cloaked, the
  // original stream url otherwise.
  [[nodiscard]] std::string_view url() const { return url_; }

  Decloak(Decloak&&) noexcept = default;
  Decloak& operator=(Decloak&&) noexcept = default;
  Decloak(const Decloak&) = delete;
  Decloak& operator=(const Decloak&) = delete;
  ~Decloak();  // stops the proxy if one is held

 private:
  friend Result<Decloak, ProxyStartError> engage(const StreamLink&);
  Decloak(std::shared_ptr<Proxy> proxy, std::string url)
      : proxy_(std::move(proxy)), url_(std::move(url)) {}

  std::shared_ptr<Proxy> proxy_;  // null for a pass-through
  std::string url_;
};

// Start a de-cloaking proxy for `link` when it is flagged (decloak_segments),
// else a transparent pass-through. The returned guard owns the proxy lifetime;
// the caller points mpv at guard.url() and destroys the guard once mpv exits.
[[nodiscard]] Result<Decloak, ProxyStartError> engage(const StreamLink& link);

// --- Internals, exposed for the golden tests (proxy.rs mod tests) -----------
namespace detail {

// TS packet size; three consecutive sync bytes at this stride mark a real
// stream.
inline constexpr std::size_t kTsPacket = 188;
// Prefixes are tens-to-hundreds of bytes; bound the sync search so a mid-payload
// 0x47 coincidence can never be mistaken for the stream start.
inline constexpr std::size_t kMaxPrefixScan = 4096;

// Whether one connection should be reused after a response (keep-alive).
enum class KeepAlive { Yes, No };

// Loopback request path + query head: `/r.ts?t=<token>&u=`. The `.ts` suffix
// sits in ffmpeg's default extension allowlist; the hex token carries no dot,
// so `.ts` stays the only extension.
[[nodiscard]] std::string path_prefix(std::string_view token);

// `http://127.0.0.1:<port><prefix><pct upstream>`.
[[nodiscard]] std::string build_loopback_url(std::uint16_t port,
                                             std::string_view prefix,
                                             std::string_view upstream);

// Percent-encode all but RFC 3986 unreserved bytes AND the dot (encoding `.`
// keeps the loopback url's only extension the synthetic `.ts`).
[[nodiscard]] std::string percent_encode(std::string_view s);

// Decode a percent-encoded string; nullopt on a truncated (`%`, `%A`) or
// non-hex (`%zz`) escape.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> percent_decode(
    std::string_view s);

// A leading `#EXTM3U` (past an optional BOM/whitespace) with no NUL in the head
// marks a playlist vs a binary segment.
[[nodiscard]] bool is_playlist(const std::uint8_t* body, std::size_t len);

// Strip any decoy prefix to the first TS-sync triple (0x47 at i, i+188, i+376),
// returning a view [offset, len) into `body`. Clean/unrecognized streams pass
// through unchanged (offset 0). Writes the start offset to *out_offset.
[[nodiscard]] std::size_t decloak_offset(const std::uint8_t* body,
                                         std::size_t len);

// ISO-BMFF (fMP4) box type at offset 4 is ftyp/styp/moof.
[[nodiscard]] bool looks_like_fmp4(const std::uint8_t* body, std::size_t len);

// Printable ASCII only (0x21-0x7e): rejects control bytes, spaces, high bytes
// that could split the outbound request line.
[[nodiscard]] bool url_bytes_clean(std::string_view s);

// Rewrite every URI in a playlist to a loopback `/r.ts?u=…` ref (variants,
// segments, and `URI="…"` tag attributes joined against `base_url`).
[[nodiscard]] std::string rewrite_playlist(std::string_view text,
                                           std::string_view base_url,
                                           std::uint16_t port,
                                           std::string_view prefix);

// Dispatch a fetched upstream body to `out`: a playlist is rewritten, a segment
// de-cloaked. Split from serve so it is testable without a live upstream.
// Appends the full HTTP response bytes to `out`.
[[nodiscard]] KeepAlive respond(std::vector<std::uint8_t>& out,
                                const std::uint8_t* body, std::size_t len,
                                std::string_view final_url, std::uint16_t port,
                                std::string_view prefix);

// One fetched upstream object.
struct Fetched {
  std::vector<std::uint8_t> body;
  std::string final_url;  // last hop; playlist relatives resolve against it
};

// Reason a fetch failed (mapped to a 502 at the wire). Kept for the guard /
// injection golden tests.
enum class FetchError {
  BadUrl,
  Blocked,
  RedirectNoLocation,
  TooManyRedirects,
  Status,
  Network,
  TooLarge,
  Timeout,
};

// Fetch `start_url` with referer/UA, following redirects by hand so every hop
// is SSRF-guarded. Body capped at 32 MiB (segments), identity encoding, whole
// chain bounded by a wall-clock deadline.
[[nodiscard]] Result<Fetched, FetchError> fetch_upstream(
    std::string_view start_url, std::optional<std::string_view> referer,
    std::optional<std::string_view> user_agent);

// A byte source for read_request_target: returns the next byte (0-255), or -1
// at EOF. Mirrors the Rust `BufRead` seam so the keep-alive read loop and the
// two golden tests (pipelined buffer / infinite stream) share one code path.
using ByteSource = std::function<int()>;

// Read one HTTP/1.1 request target off `next`. nullopt on a clean EOF between
// requests (no bytes read). Otherwise the request-line's second token (possibly
// empty if the head hit the MAX_HEAD_BYTES cap or was malformed). Headers are
// consumed to the blank line but ignored (local mpv sends bodiless GETs only).
[[nodiscard]] std::optional<std::string> read_request_target(
    const ByteSource& next);

}  // namespace detail

}  // namespace shigoku::proxy
