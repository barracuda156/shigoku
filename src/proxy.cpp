// proxy.cpp — localhost HLS de-cloaking reverse proxy (P12). Ported from
// sabigoku src/proxy.rs.
//
// Structure mirrors the Rust module: the pure codec/scan/rewrite helpers, then
// fetch_upstream (curl, redirects by hand, per-hop SSRF guard), then the Proxy
// class (bind loopback:0, accept loop, per-conn handler, serve = token gate +
// fetch + respond). engage() returns the Decloak guard.
//
// Threading model (A5 blocking, request-at-a-time is fine): one accept thread
// spawns one detached handler thread per connection. Each handler holds a
// shared_ptr<Proxy>, so teardown (stop) needs no drain: the socket + duped
// strings live until the last handler returns. std gives per-socket timeouts
// that zigoku's Io lacked, so a stalled client is dropped, not pinned.
//
// §3: the byte-strip and every url/host scan is byte-wise; no buffer is cast to
// a wider int, so it is identical on PPC (big-endian) and x86.

#include "proxy.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

// macOS (and other BSDs) has no MSG_NOSIGNAL; SO_NOSIGPIPE set on the fd in
// handle_conn() below is the equivalent guard against SIGPIPE on a write to
// a peer that has hung up.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include <atomic>
#include <cerrno>
#include <chrono>

#include "debug_log.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "hls.hpp"
#include "http.hpp"    // guard_fetch_url
#include "nonce.hpp"   // playback token (ROD-447)

namespace shigoku::proxy {

namespace {

// Response ceiling per upstream object. Playlists are tiny; TS segments a few
// MiB. Larger than the 4 MiB provider cap: this fetches whole segments.
constexpr std::uint64_t kMaxBody = 32ull << 20;
// Redirect hops per upstream fetch (the ibyteimg 302 is one; leave headroom).
constexpr int kMaxRedirects = 5;
// Wall-clock ceiling on one upstream fetch (redirect chain + body). Seconds.
constexpr long kFetchDeadlineSecs = 30;
// Per-socket idle timeout on the client-facing (mpv) side; a stalled local
// client is dropped instead of pinning its handler thread (and shared_ptr).
constexpr std::chrono::seconds kClientIdleTimeout{30};
// Ceiling on one request head (line + headers). Bounds memory against a hostile
// local client streaming an endless header.
constexpr std::uint64_t kMaxHeadBytes = 16 * 1024;

constexpr const char* kStatusOk = "200 OK";
constexpr const char* kStatusNotFound = "404 Not Found";
constexpr const char* kStatusBadGateway = "502 Bad Gateway";
constexpr const char* kContentTypePlain = "text/plain";
constexpr const char* kContentTypeM3u8 = "application/vnd.apple.mpegurl";
constexpr const char* kContentTypeTs = "video/mp2t";

char hex_digit(std::uint8_t nibble) {
  return nibble < 10 ? static_cast<char>('0' + nibble)
                     : static_cast<char>('A' + (nibble - 10));
}

int unhex(std::uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool is_unreserved(std::uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '~';
}

// Build an empty HTTP response with the given status + content-type into `out`.
// The `keep_alive` flag selects Connection: keep-alive vs close.
void write_response(std::vector<std::uint8_t>& out, const char* status,
                    const char* content_type, const std::uint8_t* body,
                    std::size_t body_len, bool keep_alive) {
  const char* connection = keep_alive ? "keep-alive" : "close";
  char head[256];
  const int n = std::snprintf(
      head, sizeof(head),
      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Cache-Control: no-store\r\nConnection: %s\r\n\r\n",
      status, content_type, body_len, connection);
  out.insert(out.end(), head, head + n);
  out.insert(out.end(), body, body + body_len);
}

detail::KeepAlive close_with(std::vector<std::uint8_t>& out, const char* status) {
  write_response(out, status, kContentTypePlain, nullptr, 0, false);
  return detail::KeepAlive::No;
}

}  // namespace

namespace detail {

std::string path_prefix(std::string_view token) {
  return "/r.ts?t=" + std::string(token) + "&u=";
}

std::string percent_encode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const auto c = static_cast<std::uint8_t>(ch);
    if (is_unreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex_digit(c >> 4));
      out.push_back(hex_digit(c & 0x0f));
    }
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> percent_decode(std::string_view s) {
  std::vector<std::uint8_t> out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '%') {
      if (i + 2 >= s.size()) return std::nullopt;
      const int hi = unhex(static_cast<std::uint8_t>(s[i + 1]));
      const int lo = unhex(static_cast<std::uint8_t>(s[i + 2]));
      if (hi < 0 || lo < 0) return std::nullopt;
      out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
      i += 3;
    } else {
      out.push_back(static_cast<std::uint8_t>(s[i]));
      i += 1;
    }
  }
  return out;
}

std::string build_loopback_url(std::uint16_t port, std::string_view prefix,
                               std::string_view upstream) {
  return "http://127.0.0.1:" + std::to_string(port) + std::string(prefix) +
         percent_encode(upstream);
}

bool looks_like_fmp4(const std::uint8_t* body, std::size_t len) {
  if (len < 8) return false;
  return std::memcmp(body + 4, "ftyp", 4) == 0 ||
         std::memcmp(body + 4, "styp", 4) == 0 ||
         std::memcmp(body + 4, "moof", 4) == 0;
}

std::size_t decloak_offset(const std::uint8_t* body, std::size_t len) {
  const std::size_t stride = kTsPacket;
  const std::size_t limit = len < kMaxPrefixScan ? len : kMaxPrefixScan;
  std::size_t i = 0;
  while (i < limit && i + 2 * stride < len) {
    if (body[i] == 0x47 && body[i + stride] == 0x47 &&
        body[i + 2 * stride] == 0x47) {
      return i;
    }
    i += 1;
  }
  // No sync in the window. Legit for fMP4; otherwise a segment whose decoy
  // prefix outgrew kMaxPrefixScan, which silently un-fixes ROD-443. Log so a
  // prefix-size shift is visible instead of a mute black screen.
  if (len >= 2 * stride && !looks_like_fmp4(body, len)) {
    std::fprintf(stderr,
                 "decloak: no TS sync in first %zuB of a %zuB segment; decoy "
                 "prefix may exceed the scan window\n",
                 limit, len);
  }
  return 0;
}

bool is_playlist(const std::uint8_t* body, std::size_t len) {
  const std::uint8_t* b = body;
  std::size_t n = len;
  if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
    b += 3;
    n -= 3;
  }
  // trim_start_ascii_ws
  while (n > 0 && (b[0] == ' ' || b[0] == '\t' || b[0] == '\r' || b[0] == '\n')) {
    b += 1;
    n -= 1;
  }
  static const char kMagic[] = "#EXTM3U";
  const std::size_t mlen = sizeof(kMagic) - 1;
  if (n < mlen || std::memcmp(b, kMagic, mlen) != 0) return false;
  // No NUL in the first 1024 bytes of the ORIGINAL body.
  const std::size_t head = len < 1024 ? len : 1024;
  for (std::size_t i = 0; i < head; ++i) {
    if (body[i] == 0) return false;
  }
  return true;
}

bool url_bytes_clean(std::string_view s) {
  if (s.empty()) return false;
  for (char ch : s) {
    const auto c = static_cast<std::uint8_t>(ch);
    if (c < 0x21 || c > 0x7e) return false;
  }
  return true;
}

// Re-point a `URI="…"` attribute inside a tag line; lines without one pass
// through unchanged.
static std::string rewrite_tag_uri(std::string_view line,
                                   std::string_view base_url, std::uint16_t port,
                                   std::string_view prefix) {
  static const char kKey[] = "URI=\"";
  const std::size_t klen = sizeof(kKey) - 1;
  const std::size_t at = line.find(kKey);
  if (at == std::string_view::npos) return std::string(line);
  const std::size_t vstart = at + klen;
  const std::size_t vend_rel = line.substr(vstart).find('"');
  if (vend_rel == std::string_view::npos) return std::string(line);
  const std::size_t vend = vstart + vend_rel;
  auto abs = hls::join_url(base_url, line.substr(vstart, vend - vstart));
  if (!abs) return std::string(line);
  const std::string loopback = build_loopback_url(port, prefix, *abs);
  return std::string(line.substr(0, vstart)) + loopback +
         std::string(line.substr(vend));
}

std::string rewrite_playlist(std::string_view text, std::string_view base_url,
                             std::uint16_t port, std::string_view prefix) {
  std::string out;
  bool first = true;
  std::size_t pos = 0;
  while (true) {
    const std::size_t nl = text.find('\n', pos);
    std::string_view raw = (nl == std::string_view::npos)
                               ? text.substr(pos)
                               : text.substr(pos, nl - pos);
    if (!first) out.push_back('\n');
    first = false;

    // trim_matches([' ', '\t', '\r']) — both ends.
    std::size_t b = 0, e = raw.size();
    auto is_trim = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (b < e && is_trim(raw[b])) ++b;
    while (e > b && is_trim(raw[e - 1])) --e;
    const std::string_view line = raw.substr(b, e - b);

    if (line.empty()) {
      // Rust `continue`s: nothing appended for this line, but the '\n' pushes
      // above already happened for the separator (matching Rust's split('\n')
      // join semantics).
    } else if (line.front() == '#') {
      out += rewrite_tag_uri(line, base_url, port, prefix);
    } else if (auto abs = hls::join_url(base_url, line)) {
      out += build_loopback_url(port, prefix, *abs);
    } else {
      out += std::string(line);
    }

    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return out;
}

KeepAlive respond(std::vector<std::uint8_t>& out, const std::uint8_t* body,
                  std::size_t len, std::string_view final_url,
                  std::uint16_t port, std::string_view prefix) {
  if (is_playlist(body, len)) {
    // is_playlist already proved a text #EXTM3U head with no NUL; treat the
    // body as UTF-8 text (a stray non-UTF-8 byte would only mangle the rewrite,
    // never crash — the scan is byte-wise, unlike Rust's str::from_utf8 gate,
    // which would 502; here the head-NUL check is the only hard reject).
    const std::string_view text(reinterpret_cast<const char*>(body), len);
    const std::string rewritten = rewrite_playlist(text, final_url, port, prefix);
    write_response(out, kStatusOk, kContentTypeM3u8,
                   reinterpret_cast<const std::uint8_t*>(rewritten.data()),
                   rewritten.size(), true);
  } else {
    const std::size_t off = decloak_offset(body, len);
    write_response(out, kStatusOk, kContentTypeTs, body + off, len - off, true);
  }
  return KeepAlive::Yes;
}

std::optional<std::string> read_request_target(const ByteSource& next) {
  // Bound the whole head: a hostile local client streaming an endless line or
  // header run hits the cap (a full-cap read with no newline yields a bounded,
  // spaceless target that fails the path match -> 404) instead of growing
  // memory. Read byte-at-a-time up to kMaxHeadBytes across the line + headers.
  std::uint64_t budget = kMaxHeadBytes;
  auto read_line = [&](std::string& line) -> bool {
    // Returns false at a clean EOF with no bytes consumed for this line.
    line.clear();
    bool any = false;
    while (budget > 0) {
      const int c = next();
      if (c < 0) break;  // EOF
      any = true;
      --budget;
      line.push_back(static_cast<char>(c));
      if (c == '\n') break;
    }
    return any;
  };

  std::string line;
  if (!read_line(line)) return std::nullopt;  // clean EOF between requests

  // target = second space-separated token of the request line (or empty).
  std::string target;
  {
    const std::size_t sp1 = line.find(' ');
    if (sp1 != std::string::npos) {
      const std::size_t start = sp1 + 1;
      const std::size_t sp2 = line.find(' ', start);
      target = line.substr(start, (sp2 == std::string::npos)
                                      ? std::string::npos
                                      : sp2 - start);
    }
  }

  // Consume headers to the blank line (or EOF / cap).
  while (true) {
    std::string header;
    if (!read_line(header)) break;  // EOF
    if (header == "\r\n" || header == "\n") break;
    if (budget == 0) break;  // head cap hit mid-run
  }
  return target;
}

// ---------------------------------------------------------------------------
// fetch_upstream — curl, redirects by hand so every hop is SSRF-guarded.
// ---------------------------------------------------------------------------
namespace {

struct BodyState {
  std::vector<std::uint8_t> buf;
  bool overflowed = false;
};

std::size_t body_cb(char* ptr, std::size_t size, std::size_t nmemb, void* ud) {
  auto* st = static_cast<BodyState*>(ud);
  const std::size_t n = size * nmemb;
  if (static_cast<std::uint64_t>(st->buf.size()) + n > kMaxBody + 1) {
    st->overflowed = true;
    return 0;  // abort the transfer
  }
  const auto* p = reinterpret_cast<const std::uint8_t*>(ptr);
  st->buf.insert(st->buf.end(), p, p + n);
  if (static_cast<std::uint64_t>(st->buf.size()) > kMaxBody) st->overflowed = true;
  return n;
}

}  // namespace

Result<Fetched, FetchError> fetch_upstream(
    std::string_view start_url, std::optional<std::string_view> referer,
    std::optional<std::string_view> user_agent) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(kFetchDeadlineSecs);
  std::string url(start_url);
  int hops = 0;

  for (;;) {
    // Reject control bytes BEFORE the request: a decoded upstream or a redirect
    // Location carrying CR/LF/NUL would otherwise reach the outbound request
    // line. Runs on every hop, so redirects get it too.
    if (!url_bytes_clean(url)) return err(FetchError::BadUrl);
    if (!http::guard_fetch_url(url)) return err(FetchError::Blocked);

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return err(FetchError::Timeout);
    const long remaining_ms = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());

    CURL* h = curl_easy_init();
    if (h == nullptr) return err(FetchError::Network);
    struct Cleanup {
      CURL* h;
      curl_slist* headers = nullptr;
      ~Cleanup() {
        if (headers) curl_slist_free_all(headers);
        if (h) curl_easy_cleanup(h);
      }
    } cleanup{h};

    BodyState st;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION,
                     static_cast<long>(CURL_HTTP_VERSION_2TLS));
    // Redirects handled by hand (guard every hop): never let curl follow.
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    // identity: bytes need no decompression before de-cloak.
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, remaining_ms > 0 ? remaining_ms : 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &body_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &st);

    if (referer) {
      const std::string line = "Referer: " + std::string(*referer);
      cleanup.headers = curl_slist_append(cleanup.headers, line.c_str());
    }
    if (user_agent) {
      const std::string ua(*user_agent);
      curl_easy_setopt(h, CURLOPT_USERAGENT, ua.c_str());
    }
    if (cleanup.headers) curl_easy_setopt(h, CURLOPT_HTTPHEADER, cleanup.headers);

    const CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
      if (st.overflowed) return err(FetchError::TooLarge);
      if (rc == CURLE_OPERATION_TIMEDOUT) return err(FetchError::Timeout);
      return err(FetchError::Network);
    }

    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    const auto code = static_cast<int>(status);

    if (code >= 300 && code < 400) {
      // Pull the Location header (curl already parsed it via REDIRECT_URL, which
      // resolves relatives against the current hop — matching hls::join_url).
      char* redir = nullptr;
      curl_easy_getinfo(h, CURLINFO_REDIRECT_URL, &redir);
      if (redir == nullptr) return err(FetchError::RedirectNoLocation);
      if (hops >= kMaxRedirects) return err(FetchError::TooManyRedirects);
      hops += 1;
      url = redir;  // copied before the handle (and thus redir) is freed
      continue;
    }
    if (!(code >= 200 && code < 300)) return err(FetchError::Status);

    if (st.overflowed || static_cast<std::uint64_t>(st.buf.size()) > kMaxBody) {
      return err(FetchError::TooLarge);
    }
    return Fetched{std::move(st.buf), std::move(url)};
  }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Proxy — bind loopback, accept loop, per-conn handler, serve.
// ---------------------------------------------------------------------------
class Proxy {
 public:
  static Result<std::shared_ptr<Proxy>, ProxyStartError> start(
      const StreamLink& link);
  ~Proxy();

  void stop();
  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] const std::string& prefix() const { return path_prefix_; }
  [[nodiscard]] const std::string& upstream() const { return upstream_url_; }

 private:
  Proxy() = default;

  void accept_loop(std::shared_ptr<Proxy> self);
  void handle_conn(int fd);
  detail::KeepAlive serve(std::string_view target, int fd);
  void wake_accept();

  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::string path_prefix_;
  std::string upstream_url_;
  std::optional<std::string> referer_;
  std::optional<std::string> user_agent_;
  std::atomic<bool> shutting_down_{false};
  std::thread accept_thread_;
};

namespace {

// Write all bytes to fd, honoring short writes. false on any error (a client
// that hung up mid-response — a normal disconnect).
bool write_all(int fd, const std::uint8_t* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    const ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

Result<std::shared_ptr<Proxy>, ProxyStartError> Proxy::start(
    const StreamLink& link) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return err(ProxyStartError{ProxyStartError::Kind::Bind,
                               std::string("socket: ") + std::strerror(errno)});
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 ONLY, never 0.0.0.0
  addr.sin_port = 0;                              // ephemeral
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const std::string msg = std::string("bind loopback: ") + std::strerror(errno);
    ::close(fd);
    return err(ProxyStartError{ProxyStartError::Kind::Bind, msg});
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
      ::listen(fd, 16) != 0) {
    const std::string msg = std::string("listen: ") + std::strerror(errno);
    ::close(fd);
    return err(ProxyStartError{ProxyStartError::Kind::Bind, msg});
  }
  const std::uint16_t port = ntohs(addr.sin_port);

  auto token = nonce::mint();
  if (!token) {
    ::close(fd);
    return err(ProxyStartError{ProxyStartError::Kind::Token, token.error()});
  }

  std::shared_ptr<Proxy> proxy(new Proxy());
  proxy->listen_fd_ = fd;
  proxy->port_ = port;
  proxy->path_prefix_ = detail::path_prefix(*token);
  proxy->upstream_url_ = link.url;
  proxy->referer_ = link.referer;
  proxy->user_agent_ = link.user_agent;
  // Spawn the accept loop with its own shared_ptr so the Proxy outlives any
  // in-flight handlers even after the guard drops.
  std::shared_ptr<Proxy> accept_arc = proxy;
  proxy->accept_thread_ =
      std::thread([accept_arc]() mutable { accept_arc->accept_loop(accept_arc); });
  return proxy;
}

Proxy::~Proxy() {
  stop();
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

// Stop accepting and join the accept thread. In-flight handler threads are
// detached; each holds a shared_ptr<Proxy>, so the socket + duped strings stay
// alive until the last one returns, then the shared_ptr drop frees everything.
// No drain, no leak-vs-free choice: refcounting is the whole story.
void Proxy::stop() {
  bool was = shutting_down_.exchange(true, std::memory_order_release);
  if (was) {
    // Already stopping/stopped; still ensure the accept thread is joined once.
    if (accept_thread_.joinable()) accept_thread_.join();
    return;
  }
  wake_accept();
  if (accept_thread_.joinable()) accept_thread_.join();
}

// One best-effort loopback dial to unblock a thread parked in accept().
// ::accept has no cancel, so mirror the self-dial: set shutting_down_ first,
// then dial once; the loop re-checks the flag right after accept returns and
// exits. A refused dial means it already left.
void Proxy::wake_accept() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port_);
  (void)::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ::close(fd);
}

void Proxy::accept_loop(std::shared_ptr<Proxy> self) {
  for (;;) {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    const int cfd = ::accept(listen_fd_, nullptr, nullptr);
    if (cfd < 0) {
      if (shutting_down_.load(std::memory_order_acquire)) return;
      if (errno == EINTR) continue;
      // Transient accept error while live: back off, do not hot-spin the core
      // (fd exhaustion would feed itself).
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    if (shutting_down_.load(std::memory_order_acquire)) {
      // The wake dial (or a late real conn) landed during shutdown; drop it.
      ::close(cfd);
      return;
    }
    // Detached handler: its own shared_ptr keeps Proxy alive past stop() if it
    // outlives the drain-free teardown.
    std::shared_ptr<Proxy> handler = self;
    std::thread([handler, cfd]() mutable { handler->handle_conn(cfd); }).detach();
  }
}

namespace {

// A buffered byte source over a socket fd: fills a small buffer with recv, hands
// out one byte at a time, returns -1 at EOF or on a read error/timeout. The
// per-socket timeout (set on the fd) turns a stalled client into an EOF here.
struct FdByteSource {
  explicit FdByteSource(int f) : fd(f) {}
  int fd;
  std::uint8_t buf[4096] = {};
  std::size_t len = 0;
  std::size_t pos = 0;

  int next() {
    if (pos >= len) {
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        if (n < 0 && errno == EINTR) return next();
        return -1;
      }
      len = static_cast<std::size_t>(n);
      pos = 0;
    }
    return buf[pos++];
  }
};

}  // namespace

void Proxy::handle_conn(int fd) {
  // Drop a stalled/slow client instead of pinning this thread (and its
  // shared_ptr) for the port's lifetime. Recv bounds a client that never sends;
  // send bounds one that stops reading our response body.
  timeval tv{};
  tv.tv_sec = static_cast<long>(
      std::chrono::duration_cast<std::chrono::seconds>(kClientIdleTimeout)
          .count());
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  const int one_nosigpipe = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one_nosigpipe,
               sizeof(one_nosigpipe));
#endif

  FdByteSource src(fd);
  detail::ByteSource next = [&src]() { return src.next(); };
  // Keep-alive loop: mpv reuses one connection across many segment GETs.
  for (;;) {
    auto target = detail::read_request_target(next);
    if (!target) break;  // clean EOF or malformed request line: done
    const detail::KeepAlive ka = serve(*target, fd);
    if (ka == detail::KeepAlive::No) break;
    // A client that hangs up mid-response surfaces as a write error inside
    // serve() (send fails); serve() returns No on any write failure, so a
    // normal disconnect ends the loop.
  }
  ::close(fd);
}

// Serve one loopback request. Off-prefix targets (path or token) 404; any
// upstream failure is a 502. Both close the connection. The prefix compare is
// not constant-time: accepted, loopback jitter drowns a timing probe.
detail::KeepAlive Proxy::serve(std::string_view target, int fd) {
  std::vector<std::uint8_t> out;
  detail::KeepAlive ka;

  if (target.size() < path_prefix_.size() ||
      target.compare(0, path_prefix_.size(), path_prefix_) != 0) {
    ka = close_with(out, kStatusNotFound);
  } else {
    const std::string_view encoded = target.substr(path_prefix_.size());
    auto decoded = detail::percent_decode(encoded);
    if (!decoded) {
      ka = close_with(out, kStatusBadGateway);
    } else {
      const std::string upstream(decoded->begin(), decoded->end());
      auto fetched = detail::fetch_upstream(
          upstream, referer_ ? std::optional<std::string_view>(*referer_)
                             : std::nullopt,
          user_agent_ ? std::optional<std::string_view>(*user_agent_)
                      : std::nullopt);
      if (!fetched) {
        ka = close_with(out, kStatusBadGateway);
      } else {
        ka = detail::respond(out, fetched->body.data(), fetched->body.size(),
                             fetched->final_url, port_, path_prefix_);
      }
    }
  }

  if (!write_all(fd, out.data(), out.size())) return detail::KeepAlive::No;
  return ka;
}

// ---------------------------------------------------------------------------
// engage / Decloak
// ---------------------------------------------------------------------------
Decloak::~Decloak() {
  if (proxy_) {
    debug_log("proxy: stop begin");
    proxy_->stop();
    debug_log("proxy: stop end");
  }
}

Result<Decloak, ProxyStartError> engage(const StreamLink& link) {
  if (!link.decloak_segments) {
    return Decloak(nullptr, link.url);
  }
  auto proxy = Proxy::start(link);
  if (!proxy) return err(std::move(proxy.error()));
  const std::string url =
      detail::build_loopback_url((*proxy)->port(), (*proxy)->prefix(), link.url);
  return Decloak(std::move(*proxy), url);
}

}  // namespace shigoku::proxy
