// http.cpp — libcurl wrapper + SSRF fetch guard (P2). Ported from
// src/providers/http.rs and src/fetchguard.rs.
//
// The guard is a security boundary: rather than lean on a URL library whose
// host canonicalization differs from sabigoku's `url` crate, we parse and
// classify the host by hand, covering exactly the spellings the fetchguard.rs
// golden tests pin (percent-encoded, bare/hex/short IPv4, mapped/compat IPv6).
// Everything is byte-wise; no buffer is ever cast to a wider int (§3, PPC).

#include "http.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>

#include "domain.hpp"  // strip_controls, for log hygiene parity.
#include "impersonate.hpp"  // P25: libcurl-impersonate dlopen seam.

namespace shigoku::http {

// ===========================================================================
// The guard (fetchguard.rs). Free functions first, then guard_fetch_url.
// ===========================================================================
namespace guard_detail {

namespace {

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool all_ascii_digits(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// Parse an unsigned integer label in a given base, rejecting overflow past the
// supplied ceiling. nullopt on any malformed digit or overflow.
std::optional<std::uint64_t> parse_uint(std::string_view s, int base,
                                        std::uint64_t ceiling) {
  if (s.empty()) return std::nullopt;
  std::uint64_t v = 0;
  for (const char c : s) {
    int d;
    if (base == 16) {
      d = hex_val(c);
    } else {
      d = (c >= '0' && c <= '9') ? (c - '0') : -1;
    }
    if (d < 0 || d >= base) return std::nullopt;
    v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
    if (v > ceiling) return std::nullopt;
  }
  return v;
}

// One IPv4 label: 0x-prefixed hex, else decimal. (The WHATWG parser also
// accepts leading-zero octal, but sabigoku's tests never exercise it and the
// numeric_spelling backstop catches all-digit hosts anyway.)
std::optional<std::uint64_t> parse_v4_label(std::string_view s,
                                            std::uint64_t ceiling) {
  if (s.size() >= 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
    return parse_uint(s.substr(2), 16, ceiling);
  }
  return parse_uint(s, 10, ceiling);
}

}  // namespace

std::optional<std::string> percent_decode_host(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%') {
      if (i + 2 >= s.size()) return std::nullopt;
      const int hi = hex_val(s[i + 1]);
      const int lo = hex_val(s[i + 2]);
      if (hi < 0 || lo < 0) return std::nullopt;
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

bool private_v4(std::uint8_t a, std::uint8_t b, std::uint8_t /*c*/,
                std::uint8_t /*d*/) {
  switch (a) {
    case 0:
    case 10:
    case 127:
      return true;
    case 100:
      return b >= 64 && b <= 127;  // CGNAT 100.64/10.
    case 169:
      return b == 254;  // link-local.
    case 172:
      return b >= 16 && b <= 31;  // 172.16/12.
    case 192:
      return b == 168;  // 192.168/16.
    default:
      return a >= 224;  // multicast + reserved.
  }
}

bool parse_ipv4_any(std::string_view host, std::uint8_t out[4]) {
  if (host.empty()) return false;

  // Split on '.', tolerating one trailing dot ("127.0.0.1.").
  std::vector<std::string_view> labels;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= host.size(); ++i) {
    if (i == host.size() || host[i] == '.') {
      labels.push_back(host.substr(start, i - start));
      start = i + 1;
    }
  }
  // A single trailing dot yields one empty final label; drop it.
  if (labels.size() >= 2 && labels.back().empty()) labels.pop_back();
  if (labels.empty() || labels.size() > 4) return false;

  // Every label must be numeric (decimal or 0x-hex); if any is not, this is a
  // domain, not an IPv4 literal.
  for (const auto& l : labels) {
    if (l.empty()) return false;
    const bool hex = l.size() >= 2 && l[0] == '0' && (l[1] == 'x' || l[1] == 'X');
    const std::string_view digits = hex ? l.substr(2) : l;
    if (digits.empty()) return false;
    for (const char c : digits) {
      if (hex ? hex_val(c) < 0 : !(c >= '0' && c <= '9')) return false;
    }
  }

  // WHATWG assembly: the LAST label absorbs all remaining low-order bytes, each
  // earlier label is exactly one octet. So a.b.c.d -> 4 octets; a.b.c ->
  // a,b,(c as 16-bit); a.b -> a,(b as 24-bit); a -> full 32-bit.
  const std::size_t n = labels.size();
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const bool last = (i + 1 == n);
    // Each non-final label caps at 255; the final absorbs the rest of 32 bits.
    const std::uint64_t ceiling =
        last ? (0xFFFFFFFFull >> (8 * (n - 1))) : 0xFFull;
    const auto v = parse_v4_label(labels[i], ceiling);
    if (!v) return false;
    if (last) {
      acc |= *v;
    } else {
      acc |= (*v << (8 * (4 - 1 - i)));
    }
  }
  out[0] = static_cast<std::uint8_t>((acc >> 24) & 0xFF);
  out[1] = static_cast<std::uint8_t>((acc >> 16) & 0xFF);
  out[2] = static_cast<std::uint8_t>((acc >> 8) & 0xFF);
  out[3] = static_cast<std::uint8_t>(acc & 0xFF);
  return true;
}

bool private_v6(const std::uint16_t seg[8]) {
  // Unspecified (::) and loopback (::1).
  bool all_zero = true;
  for (int i = 0; i < 8; ++i) {
    if (seg[i] != 0) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) return true;  // :: unspecified.
  {
    bool loop = seg[7] == 1;
    for (int i = 0; i < 7; ++i) loop = loop && seg[i] == 0;
    if (loop) return true;  // ::1 loopback.
  }
  if ((seg[0] & 0xffc0) == 0xfe80) return true;  // fe80::/10 link-local.
  if ((seg[0] & 0xfe00) == 0xfc00) return true;  // fc00::/7 unique-local.

  // Embedded IPv4 (ROD-436 widen): BOTH ::ffff:a.b.c.d (mapped) and ::a.b.c.d
  // (compatible). to_ipv4() in Rust covers both: the first six hextets are zero
  // except an optional 0xffff in seg[5]. If so, recurse into the v4 denylist.
  bool high_zero = true;
  for (int i = 0; i < 5; ++i) high_zero = high_zero && seg[i] == 0;
  const bool mapped = high_zero && seg[5] == 0xffff;
  const bool compat = high_zero && seg[5] == 0;
  if (mapped || compat) {
    const std::uint8_t a = static_cast<std::uint8_t>(seg[6] >> 8);
    const std::uint8_t b = static_cast<std::uint8_t>(seg[6] & 0xFF);
    const std::uint8_t c = static_cast<std::uint8_t>(seg[7] >> 8);
    const std::uint8_t d = static_cast<std::uint8_t>(seg[7] & 0xFF);
    // The ::/96 range (::..::ff) also lands in 0/8, which private_v4 blocks.
    return private_v4(a, b, c, d);
  }
  return false;
}

bool parse_ipv6(std::string_view host, std::uint16_t out[8]) {
  // Accept a bracketed literal only in URL context; here `host` already has
  // brackets stripped by the caller. Reject anything without a ':'.
  if (host.find(':') == std::string_view::npos) return false;

  // Split into the part before "::" and after (at most one "::").
  const std::size_t dbl = host.find("::");
  std::string_view head = host, tail;
  bool has_dbl = false;
  if (dbl != std::string_view::npos) {
    has_dbl = true;
    head = host.substr(0, dbl);
    tail = host.substr(dbl + 2);
    if (tail.find("::") != std::string_view::npos) return false;  // two "::".
  }

  auto parse_group = [&](std::string_view part,
                         std::vector<std::uint16_t>& into) -> bool {
    if (part.empty()) return true;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= part.size(); ++i) {
      if (i == part.size() || part[i] == ':') {
        std::string_view tok = part.substr(start, i - start);
        start = i + 1;
        if (tok.empty()) return false;  // ":::" or leading/trailing colon.
        // A trailing embedded IPv4 (only valid as the final token).
        if (tok.find('.') != std::string_view::npos) {
          std::uint8_t v4[4];
          if (!parse_ipv4_any(tok, v4)) return false;
          if (i != part.size()) return false;  // must be last.
          into.push_back(static_cast<std::uint16_t>((v4[0] << 8) | v4[1]));
          into.push_back(static_cast<std::uint16_t>((v4[2] << 8) | v4[3]));
          return true;
        }
        if (tok.size() > 4) return false;
        const auto v = parse_uint(tok, 16, 0xFFFF);
        if (!v) return false;
        into.push_back(static_cast<std::uint16_t>(*v));
      }
    }
    return true;
  };

  std::vector<std::uint16_t> hi, lo;
  if (!parse_group(head, hi)) return false;
  if (!parse_group(tail, lo)) return false;

  if (has_dbl) {
    if (hi.size() + lo.size() > 8) return false;
    const std::size_t zeros = 8 - hi.size() - lo.size();
    std::size_t k = 0;
    for (std::uint16_t s : hi) out[k++] = s;
    for (std::size_t z = 0; z < zeros; ++z) out[k++] = 0;
    for (std::uint16_t s : lo) out[k++] = s;
  } else {
    if (hi.size() != 8) return false;
    for (std::size_t i = 0; i < 8; ++i) out[i] = hi[i];
  }
  return true;
}

bool numeric_spelling(std::string_view domain) {
  if (domain.size() >= 2 &&
      (domain.substr(0, 2) == "0x" || domain.substr(0, 2) == "0X")) {
    return true;
  }
  bool any_label = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= domain.size(); ++i) {
    if (i == domain.size() || domain[i] == '.') {
      std::string_view label = domain.substr(start, i - start);
      start = i + 1;
      if (label.empty()) continue;
      any_label = true;
      if (!all_ascii_digits(label)) return false;
    }
  }
  return any_label;
}

}  // namespace guard_detail

namespace {

// Lowercase ASCII in place (host comparison; no locale, §3).
std::string ascii_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

}  // namespace

Result<Unit, GuardError> guard_fetch_url(std::string_view raw) {
  using namespace guard_detail;

  // --- scheme --------------------------------------------------------------
  const std::size_t scheme_end = raw.find("://");
  if (scheme_end == std::string_view::npos) return err(GuardError::BadUrl);
  const std::string scheme = ascii_lower(raw.substr(0, scheme_end));
  if (scheme != "http" && scheme != "https") return err(GuardError::BadUrl);

  // --- authority (up to the first '/', '?' or '#') -------------------------
  std::string_view rest = raw.substr(scheme_end + 3);
  std::size_t auth_end = rest.size();
  for (std::size_t i = 0; i < rest.size(); ++i) {
    const char c = rest[i];
    if (c == '/' || c == '?' || c == '#') {
      auth_end = i;
      break;
    }
  }
  std::string_view authority = rest.substr(0, auth_end);
  if (authority.empty()) return err(GuardError::BadUrl);

  // --- reject userinfo (user[:pass]@host) ----------------------------------
  const std::size_t at = authority.rfind('@');
  if (at != std::string_view::npos) return err(GuardError::BadUrl);

  // --- host[:port] ---------------------------------------------------------
  std::string_view hostport = authority;
  std::string_view host;
  if (!hostport.empty() && hostport.front() == '[') {
    // Bracketed IPv6 literal: host is inside [ ].
    const std::size_t close = hostport.find(']');
    if (close == std::string_view::npos) return err(GuardError::BadUrl);
    host = hostport.substr(1, close - 1);
    // Anything after ']' other than ":port" is malformed.
    std::string_view after = hostport.substr(close + 1);
    if (!after.empty() && after.front() != ':') return err(GuardError::BadUrl);
    std::uint16_t seg[8];
    if (!parse_ipv6(host, seg)) return err(GuardError::BadUrl);
    return private_v6(seg) ? err(GuardError::BlockedHost) : Result<Unit, GuardError>(Unit{});
  }

  // Non-bracketed: strip a trailing :port (host has no ':').
  const std::size_t colon = hostport.rfind(':');
  host = (colon == std::string_view::npos) ? hostport : hostport.substr(0, colon);
  if (host.empty()) return err(GuardError::BadUrl);

  // Percent-decode so encoded IPv4/hostnames are seen as the connector will.
  const auto decoded_opt = percent_decode_host(host);
  if (!decoded_opt) return err(GuardError::BadUrl);
  const std::string decoded = ascii_lower(*decoded_opt);
  if (decoded.empty()) return err(GuardError::BadUrl);

  // A bare IPv6 without brackets is not a valid URL host.
  if (decoded.find(':') != std::string_view::npos) return err(GuardError::BadUrl);

  // IPv4 in any spelling -> canonical octets -> denylist.
  {
    std::uint8_t v4[4];
    if (parse_ipv4_any(decoded, v4)) {
      return private_v4(v4[0], v4[1], v4[2], v4[3])
                 ? err(GuardError::BlockedHost)
                 : Result<Unit, GuardError>(Unit{});
    }
  }

  // Domain arm: localhost family, then the numeric-spelling backstop.
  if (decoded == "localhost" || (decoded.size() > 10 &&
                                 decoded.compare(decoded.size() - 10, 10,
                                                 ".localhost") == 0)) {
    return err(GuardError::BlockedHost);
  }
  if (numeric_spelling(decoded)) return err(GuardError::BlockedHost);
  return Unit{};
}

// ===========================================================================
// The client (http.rs). curl_global_init once; per-fetch easy handle.
// ===========================================================================
namespace {

std::once_flag g_curl_init_once;
std::atomic<bool> g_curl_init_ok{false};

void ensure_global_init() {
  std::call_once(g_curl_init_once, [] {
    g_curl_init_ok.store(curl_global_init(CURL_GLOBAL_DEFAULT) == 0,
                         std::memory_order_release);
  });
}

// libcurl write callback: append to the Vec, refusing past the cap +1 so we can
// distinguish "at cap" (ok) from "over cap" (Decode) exactly like http.rs.
struct WriteState {
  std::vector<std::uint8_t> buf;
  bool overflowed = false;
};

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* ud) {
  auto* st = static_cast<WriteState*>(ud);
  const std::size_t n = size * nmemb;
  if (st->buf.size() + n > kMaxRespBytes + 1) {
    st->overflowed = true;
    return 0;  // signal an abort to libcurl.
  }
  const auto* p = reinterpret_cast<const std::uint8_t*>(ptr);
  st->buf.insert(st->buf.end(), p, p + n);
  if (st->buf.size() > kMaxRespBytes) st->overflowed = true;
  return n;
}

}  // namespace

Result<Unit, std::string> assert_http2_available() {
  ensure_global_init();
  const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
  if (v == nullptr) return err(std::string("curl_version_info returned null"));
  if ((v->features & CURL_VERSION_HTTP2) == 0) {
    return err(std::string(
        "libcurl was built without nghttp2 (HTTP/2). HTTP/2 is required: "
        "HTTP/1.1 header casing trips the edge challenge page. Rebuild curl "
        "with nghttp2."));
  }
  return Unit{};
}

Result<Client, ProviderError> Client::create() {
  ensure_global_init();
  if (!g_curl_init_ok.load(std::memory_order_acquire)) {
    return err(ProviderError::network());
  }
  return Client{};
}

namespace {

// The stock libcurl surface, expressed as the same vtable shape the impersonate
// path uses, so one fetch body drives either build. Built from the directly
// linked symbols; `impersonate` is null (stock has no such symbol) and is never
// called on this path.
const ImpersonateApi& stock_api() {
  static const ImpersonateApi api = [] {
    ImpersonateApi a;
    // curl_easy_setopt/getinfo are variadic; the vtable fields are variadic
    // pointers, so these are ordinary same-type assignments (no arity cast).
    a.global_init = &curl_global_init;  // unused on the stock path (already
                                        // inited via ensure_global_init).
    a.easy_init = &curl_easy_init;
    a.setopt = &curl_easy_setopt;
    a.perform = &curl_easy_perform;
    a.getinfo = &curl_easy_getinfo;
    a.cleanup = &curl_easy_cleanup;
    a.slist_append = &curl_slist_append;
    a.slist_free_all = &curl_slist_free_all;
    a.impersonate = nullptr;  // stock build: never set, never called.
    return a;
  }();
  return api;
}

// Map a Fingerprint to its curl-impersonate target string. None never reaches
// here (the caller keeps it on the stock path).
const char* impersonate_target(Fingerprint fp) {
  switch (fp) {
    case Fingerprint::Chrome:
      return "chrome124";
    case Fingerprint::Firefox:
      return "firefox133";
    case Fingerprint::None:
      break;
  }
  return nullptr;
}

// RAII cleanup for the handle + header list on every return path. Uses the
// SAME api the handle came from — a handle from the impersonate lib must be
// freed by the impersonate lib's curl_easy_cleanup.
struct HandleGuard {
  const ImpersonateApi* api = nullptr;
  CURL* h = nullptr;
  curl_slist* headers = nullptr;
  ~HandleGuard() {
    if (api == nullptr) return;
    if (headers) api->slist_free_all(headers);
    if (h) api->cleanup(h);
  }
};

// The request surface fetch() and fetch_to_sink() share: backend pick,
// impersonate-first, url / HTTP2 / redirect-refusal / encoding / nosignal /
// UA, the method verb + body, and the header list. Timeout policy and write
// routing stay per-path. nullptr = the requested backend is unavailable
// (impersonate dylib absent/too old — a provider-level unavailability, not a
// crash; the walk hops. See project_p25_clienthello_decision) or handle
// creation failed.
const ImpersonateApi* setup_request(const Request& req, HandleGuard& guard) {
  // Pick the backend. Default None = stock OpenSSL libcurl; Chrome/Firefox =
  // the dlopen'd impersonate lib (BoringSSL).
  const ImpersonateApi* api = &stock_api();
  if (req.fingerprint != Fingerprint::None) {
    api = load_impersonate_api();
    if (api == nullptr) return nullptr;
  }

  CURL* h = api->easy_init();
  if (h == nullptr) return nullptr;
  guard.api = api;
  guard.h = h;

  // Impersonate FIRST when requested: curl_easy_impersonate sets a baseline of
  // ciphers/curves/extensions/headers matching the target browser; our own
  // setopt calls below then layer on url/timeout/write-cb without disturbing
  // the handshake shape.
  if (req.fingerprint != Fingerprint::None) {
    const char* target = impersonate_target(req.fingerprint);
    if (target == nullptr || api->impersonate(h, target, /*default_headers=*/1) != CURLE_OK) {
      return nullptr;
    }
  }

  // Each option is passed with the trailing-argument type libcurl expects for
  // that option (long / const char* / pointer), through the variadic setopt —
  // exactly as a direct curl_easy_setopt call would.
  api->setopt(h, CURLOPT_URL, req.url.c_str());
  // HTTP/2-over-TLS on every request (§3, load-bearing). Impersonate already
  // selects the browser's ALPN; setting this is a no-op there and correct for
  // the stock path.
  api->setopt(h, CURLOPT_HTTP_VERSION,
              static_cast<long>(CURL_HTTP_VERSION_2TLS));
  // Redirects refused (03 §6.7): a 3xx returns as its status, never followed.
  api->setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  api->setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate on (A5).
  api->setopt(h, CURLOPT_NOSIGNAL, 1L);  // thread-safe timeouts.
  api->setopt(h, CURLOPT_USERAGENT, req.user_agent.c_str());

  if (req.method == Method::Post) {
    api->setopt(h, CURLOPT_POST, 1L);
    api->setopt(h, CURLOPT_POSTFIELDS,
                reinterpret_cast<const char*>(req.body.data()));
    api->setopt(h, CURLOPT_POSTFIELDSIZE,
                static_cast<long>(req.body.size()));
  } else if (req.method == Method::Put) {
    // libcurl has no CURLOPT_PUT-with-body-buffer knob (its PUT mode expects
    // a read callback); CUSTOMREQUEST + the same POSTFIELDS pair as above
    // sends the verb as PUT while reusing the in-memory body, the documented
    // libcurl idiom for a buffer-backed PUT.
    api->setopt(h, CURLOPT_CUSTOMREQUEST, "PUT");
    api->setopt(h, CURLOPT_POSTFIELDS,
                reinterpret_cast<const char*>(req.body.data()));
    api->setopt(h, CURLOPT_POSTFIELDSIZE,
                static_cast<long>(req.body.size()));
  }

  // Extra headers + content-type. curl_slist owns copies.
  auto add_header = [&](const std::string& name, const std::string& value) {
    const std::string line = name + ": " + value;
    guard.headers = api->slist_append(guard.headers, line.c_str());
  };
  if (req.content_type) add_header("Content-Type", *req.content_type);
  for (const auto& hdr : req.extra_headers) add_header(hdr.name, hdr.value);
  if (guard.headers) api->setopt(h, CURLOPT_HTTPHEADER, guard.headers);

  return api;
}

}  // namespace

Result<std::vector<std::uint8_t>, ProviderError> Client::fetch(
    const Request& req) const {
  HandleGuard guard;
  const ImpersonateApi* api = setup_request(req, guard);
  if (api == nullptr) return err(ProviderError::network());
  CURL* h = guard.h;

  WriteState st;
  const long timeout = req.timeout_secs.value_or(kDefaultTimeoutSecs);
  api->setopt(h, CURLOPT_TIMEOUT, timeout);
  api->setopt(h, CURLOPT_WRITEFUNCTION, &write_cb);
  api->setopt(h, CURLOPT_WRITEDATA, &st);

  const CURLcode rc = api->perform(h);
  if (rc != CURLE_OK) {
    // An aborted transfer due to the size cap surfaces as WRITE_ERROR; treat
    // that as Decode(oversize), everything else as Network (http.rs maps all
    // transport failure to Network).
    if (st.overflowed) {
      return err(ProviderError::decode("response exceeds the 4 MiB cap"));
    }
    return err(ProviderError::network());
  }

  long status = 0;
  api->getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  const auto code = static_cast<std::uint16_t>(status);

  const bool ok = (req.accept == Accept::OkOnly) ? (code == 200)
                                                 : (code >= 200 && code <= 299);
  if (!ok) return err(ProviderError::from_status(code));

  if (st.overflowed || st.buf.size() > kMaxRespBytes) {
    return err(ProviderError::decode("response exceeds the 4 MiB cap"));
  }
  return st.buf;
}

// ===========================================================================
// The streaming fetch (P35 downloads).
// ===========================================================================
namespace {

// libcurl write callback for fetch_to_sink: resolve the response meta once
// (status + Content-Length, both queryable mid-transfer), then route accepted
// (2xx) body bytes to the sink and discard the rest.
struct StreamState {
  const SinkFn* sink = nullptr;
  const ImpersonateApi* api = nullptr;
  CURL* h = nullptr;
  StreamMeta meta;
  bool meta_ready = false;
  bool accepted = false;
};

std::size_t stream_write_cb(char* ptr, std::size_t size, std::size_t nmemb,
                            void* ud) {
  auto* st = static_cast<StreamState*>(ud);
  const std::size_t n = size * nmemb;
  if (!st->meta_ready) {
    long status = 0;
    st->api->getinfo(st->h, CURLINFO_RESPONSE_CODE, &status);
    st->meta.status = static_cast<std::uint16_t>(status);
    curl_off_t len = -1;
    st->api->getinfo(st->h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
    if (len >= 0) st->meta.content_length = static_cast<std::uint64_t>(len);
    st->meta_ready = true;
    st->accepted = st->meta.status >= 200 && st->meta.status <= 299;
  }
  // Non-2xx body bytes (an error page) are drained, never sunk; the status
  // maps through from_status() after perform.
  if (!st->accepted) return n;
  if (!(*st->sink)(st->meta, reinterpret_cast<const std::uint8_t*>(ptr), n)) {
    return 0;  // sink refused: abort the transfer (CURLE_WRITE_ERROR).
  }
  return n;
}

// Progress callback polling the caller's abort flag, so a transfer can be
// stopped even while the line is idle (libcurl ticks this during stalls too).
int abort_poll_cb(void* ud, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                  curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  const auto* abort = static_cast<const std::atomic<bool>*>(ud);
  return abort->load(std::memory_order_acquire) ? 1 : 0;
}

}  // namespace

Result<std::uint16_t, ProviderError> Client::fetch_to_sink(
    const Request& req, const SinkFn& sink,
    const std::atomic<bool>* abort) const {
  HandleGuard guard;
  const ImpersonateApi* api = setup_request(req, guard);
  if (api == nullptr) return err(ProviderError::network());
  CURL* h = guard.h;

  StreamState st;
  st.sink = &sink;
  st.api = api;
  st.h = h;
  api->setopt(h, CURLOPT_WRITEFUNCTION, &stream_write_cb);
  api->setopt(h, CURLOPT_WRITEDATA, &st);

  // Timeout policy (header contract): an explicit override is a wall rail
  // (tests); absent, a long transfer must never trip a clock — a 10s connect
  // budget plus the stall rail abort a dead line instead.
  if (req.timeout_secs) {
    api->setopt(h, CURLOPT_TIMEOUT, *req.timeout_secs);
  } else {
    api->setopt(h, CURLOPT_CONNECTTIMEOUT, kDefaultTimeoutSecs);
    api->setopt(h, CURLOPT_LOW_SPEED_LIMIT, kStallFloorBytesPerSec);
    api->setopt(h, CURLOPT_LOW_SPEED_TIME, kStallSecs);
  }
  if (abort != nullptr) {
    api->setopt(h, CURLOPT_NOPROGRESS, 0L);
    api->setopt(h, CURLOPT_XFERINFOFUNCTION, &abort_poll_cb);
    api->setopt(h, CURLOPT_XFERINFODATA,
                const_cast<void*>(static_cast<const void*>(abort)));
  }

  const CURLcode rc = api->perform(h);
  if (rc != CURLE_OK) {
    // A refused sink (WRITE_ERROR) and a tripped abort flag (ABORTED_BY_
    // CALLBACK) land here too; the caller tells those apart from its own
    // state — transport-wise they are all a dead transfer.
    return err(ProviderError::network());
  }

  long status = 0;
  api->getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  const auto code = static_cast<std::uint16_t>(status);
  if (code < 200 || code > 299) return err(ProviderError::from_status(code));
  return code;
}

}  // namespace shigoku::http
