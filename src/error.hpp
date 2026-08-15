// error.hpp — the 03 §7 / A2 error taxonomies as closed enums + detail.
//
// Ported from sabigoku src/providers.rs (ProviderError), src/player.rs
// (PlayError), src/fetchguard.rs (GuardError). Port the NAMES and MEANINGS
// exactly (A2): the TUI's toast copy and retry/hop policy switch on the kind,
// not the message. Every switch over a *Kind must be exhaustive
// (-Werror=switch-enum) so a new class can't silently fall through to the
// wrong toast.
//
// Rust enums here carry data (Forbidden { status }, Decode(String)). C++ has
// no closed sum type with the switch-exhaustiveness we want AND payloads, so
// each error is a small struct: a `Kind` enum class (the closed set the fence
// guards) plus the payload fields that kind uses. Constructors name the
// variants so call sites read like the Rust (`ProviderError::forbidden(403)`).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace shigoku {

// --- ProviderError (03 §7, providers.rs:46) ---------------------------------
//
// `episodes()` returning Ok([]) is NOT an error — it is authoritative
// not-stocked and marks absence; any Err must NOT (03 §4.3). That distinction
// lives in the caller, not here; here we only classify failures.
struct ProviderError {
  enum class Kind {
    Network,      // timeout / refused / dns  -> "network unreachable", hop
    Forbidden,    // 403 / 451                -> "{source} blocked us", hop   (status)
    Server,       // 5xx                      -> "{source} is down", hop      (status)
    Http,         // other non-2xx            -> "{source} returned an error" (status)
    Decode,       // malformed response       -> internal; detail carries why (detail)
    Unsupported,  // provider lacks the op    -> MUST NOT poison absence (03 §8.1)
    // 429, split from Http (P20, debt #3 / providers.rs CatalogError::RateLimited):
    // sync's backoff law (06 §5.3) needs to tell throttling from failure. The
    // client never sleeps or retries on its own — that's the caller's call.
    RateLimited,
  };

  Kind kind;
  std::uint16_t status = 0;  // meaningful only for Forbidden/Server/Http.
  std::string detail;        // meaningful only for Decode (and free context).

  static ProviderError network() { return {Kind::Network, 0, {}}; }
  static ProviderError forbidden(std::uint16_t s) { return {Kind::Forbidden, s, {}}; }
  static ProviderError server(std::uint16_t s) { return {Kind::Server, s, {}}; }
  static ProviderError http(std::uint16_t s) { return {Kind::Http, s, {}}; }
  static ProviderError decode(std::string why) { return {Kind::Decode, 0, std::move(why)}; }
  static ProviderError unsupported() { return {Kind::Unsupported, 0, {}}; }
  static ProviderError rate_limited() { return {Kind::RateLimited, 429, {}}; }

  // ProviderError::from_status (providers.rs:82) — status -> class.
  static ProviderError from_status(std::uint16_t status) {
    if (status == 429) return rate_limited();
    if (status == 403 || status == 451) return forbidden(status);
    if (status >= 500 && status <= 599) return server(status);
    return http(status);
  }

  friend bool operator==(const ProviderError&, const ProviderError&) = default;
};

// --- GuardError (fetchguard.rs, 03 §6.7) ------------------------------------
//
// SSRF guard verdict for provider-supplied URLs. Callers refuse the fetch;
// they never "sanitize and proceed".
enum class GuardError {
  BadUrl,       // unparseable, non-http(s), userinfo, or hostless
  BlockedHost,  // localhost / private-range / alternate-spelling literal
};

// --- PlayError (player.rs:78, mapped to 03 §7 hop policy) -------------------
//
// The play pipeline's failure classes. `OpenFailed` (mpv exit code 2, no
// meaningful playback yet) is the only retry-eligible one (A6): re-resolve a
// fresh URL, backoff, up to MAX_PLAY_ATTEMPTS. The rest do not retry here.
struct PlayError {
  enum class Kind {
    Resolve,      // upstream resolve failed        (detail = provider message)
    UnsafeUrl,    // stream url failed the SSRF guard (guard carries which)
    UnsafeArg,    // an argv field had banned bytes  (detail = field name)
    Spawn,        // posix_spawnp failed, not ENOENT (detail = mpv + errno text)
    MpvNotFound,  // posix_spawnp ENOENT: mpv isn't on PATH (debt #5, P22)
    Wait,         // waitpid on mpv failed           (detail = errno text)
    OpenFailed,  // mpv exit 2, no playback         (attempts) -> retry then hop
    Exit,        // mpv exited nonzero before play  (detail = status) -> no hop
  };

  Kind kind;
  std::string detail;
  GuardError guard = GuardError::BadUrl;  // meaningful only for UnsafeUrl.
  std::uint32_t attempts = 0;             // meaningful only for OpenFailed.

  static PlayError resolve(std::string why) { return {Kind::Resolve, std::move(why)}; }
  static PlayError unsafe_url(GuardError g) { return {Kind::UnsafeUrl, {}, g}; }
  static PlayError unsafe_arg(std::string field) { return {Kind::UnsafeArg, std::move(field)}; }
  static PlayError spawn(std::string why) { return {Kind::Spawn, std::move(why)}; }
  static PlayError mpv_not_found(std::string why) {
    return {Kind::MpvNotFound, std::move(why)};
  }
  static PlayError wait(std::string why) { return {Kind::Wait, std::move(why)}; }
  static PlayError open_failed(std::uint32_t n) { return {Kind::OpenFailed, {}, GuardError::BadUrl, n}; }
  static PlayError exit(std::string status) { return {Kind::Exit, std::move(status)}; }
};

// Human-facing copy is a render concern (DESIGN §4.10, wired in P7). These
// helpers give the *intent* strings from 03 §7 so tests and toasts agree; the
// switch is exhaustive so a new kind forces a copy decision.
inline std::string_view provider_error_copy(ProviderError::Kind k) {
  switch (k) {
    case ProviderError::Kind::Network:     return "network unreachable";
    case ProviderError::Kind::Forbidden:   return "blocked us";
    case ProviderError::Kind::Server:      return "is down";
    case ProviderError::Kind::Http:        return "returned an error";
    case ProviderError::Kind::Decode:      return "malformed response";
    case ProviderError::Kind::Unsupported: return "unsupported operation";
    case ProviderError::Kind::RateLimited: return "is rate-limiting us";
  }
  return "";  // unreachable: the switch is exhaustive over the closed enum.
}

}  // namespace shigoku
