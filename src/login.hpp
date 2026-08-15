// login.hpp — OAuth login core (06 §4, P19). Ported from sabigoku
// src/login.rs. I/O-free relative to the UI: complete_login verifies the
// token against AniList before writing the file, so a bad token never
// persists.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "anilist.hpp"
#include "auth.hpp"
#include "http.hpp"

namespace shigoku::login {

// shigoku's own AniList app registration, retiring zigoku/sabigoku's ids.
// Registered with redirect http://localhost:8767, so the loopback binds
// 8767 (sabigoku uses 8766) — the redirect must match the registration.
inline constexpr const char* kClientId = "46529";
inline constexpr std::uint16_t kLoopbackPort = 8767;

// Implicit-grant tokens are ~1y JWTs; anything shorter is not one (06 §4.2).
inline constexpr std::size_t kTokenFloor = 20;

// The authorize URL without `state`: the paste flow has no CSRF check to
// anchor one (06 §4.1/§4.3), so it carries none, like the loopback's own
// bare-form predecessor.
[[nodiscard]] std::string authorize_url_bare();

// The Implicit-grant authorize URL with the CSRF nonce as `state` (06 §4.1).
[[nodiscard]] std::string authorize_url(std::string_view state);

// A pasted bare JWT (zigoku extractToken parity) becomes an extractable
// param; anything else passes through for the normal redirect-URL extract.
[[nodiscard]] std::string normalize_paste(std::string_view line);

// Open a URL in the user's browser, best-effort: a failure just means the
// user opens the printed URL themselves. Detached, never blocks the caller.
void open_browser(const std::string& url);

// Outcome of a connect attempt, posted to the TUI. Never Ok unless the token
// verified and saved (06 §4.2 states, plus loopback's CSRF/cancel).
struct ConnectResult {
  enum class Kind {
    Ok,
    NoToken,
    Rejected,
    NetworkError,
    SaveFailed,
    BadState,
    Canceled,
  };
  Kind kind = Kind::NoToken;
  std::string user_name;  // Ok only.

  friend bool operator==(const ConnectResult&, const ConnectResult&) = default;

  static ConnectResult ok(std::string name) { return {Kind::Ok, std::move(name)}; }
  static ConnectResult no_token() { return {Kind::NoToken, {}}; }
  static ConnectResult rejected() { return {Kind::Rejected, {}}; }
  static ConnectResult network_error() { return {Kind::NetworkError, {}}; }
  static ConnectResult save_failed() { return {Kind::SaveFailed, {}}; }
  static ConnectResult bad_state() { return {Kind::BadState, {}}; }
  static ConnectResult canceled() { return {Kind::Canceled, {}}; }
};

// The token verify seam (AniList Viewer), a callable so tests skip the net.
// (login.rs's Verifier trait, ported as a std::function since C++ has no
// zero-cost trait-object equivalent worth the ceremony for one call site.)
using Verifier = std::function<Result<std::optional<anilist::Viewer>, ProviderError>(
    std::string_view token)>;

// The real verifier: wraps anilist::viewer over a live http::Client.
[[nodiscard]] Verifier make_anilist_verifier(const http::Client& client);

// Shared login core (06 §4.2): extract the token from a raw redirect/query/
// paste, floor-check it, verify against AniList, then build and save. The
// file is never written before the verify succeeds (the save lives only in
// the verified branch).
[[nodiscard]] ConnectResult complete_login(std::string_view raw, const Verifier& verifier,
                                            const std::string& auth_path, std::int64_t now);

// --- Internals exposed for the golden tests (login.rs tests mod) ----------
namespace detail {

// Value of `key=` in a redirect/query/fragment, up to the next delimiter.
// The match is anchored to a param boundary (start or after &/?/#) so a key
// that is a suffix of another (xstate= vs state=) can never be read as it: a
// query parser in a security callback must not be a substring search.
[[nodiscard]] std::optional<std::string> param(std::string_view raw, std::string_view key);

}  // namespace detail

}  // namespace shigoku::login
