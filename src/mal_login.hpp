// mal_login.hpp — MAL OAuth login core (P31 §9.1 slice 2). No sabigoku
// reference: MAL account support never existed there ("later MAL/Kitsu" —
// auth.rs's own doc comment). New but deliberately mirrors login.hpp's role
// (extract/verify/build/save) so the two providers read the same way.
//
// MAL is Authorization Code + PKCE, not AniList's Implicit Grant (06 §4.1):
// the authorization server hands back a `code` on a normal server-side
// redirect (no URL fragment involved, so no relay-page trick is needed —
// mal_loopback.hpp serves the callback directly). The code must then be
// EXCHANGED (a POST, with the PKCE code_verifier) for the access/refresh
// token pair — unlike AniList's flow, the token never appears in the
// browser's address bar, and there is no bare-token paste fallback for MAL:
// the code can only be redeemed once, server-side, with the verifier this
// process minted, so nothing meaningful survives a copy-paste out of band.
//
// PKCE quirk: MAL supports method "plain" ONLY (no S256), so
// code_challenge = code_verifier verbatim (RFC 7636 §4.3, the identity
// transform). code_verifier must be 43-128 chars from the unreserved set
// [A-Za-z0-9-._~]; two concatenated nonce::mint() calls (32 hex chars each)
// give 64 hex chars, comfortably inside that range and hex-only so every
// char is already unreserved.
//
// Refresh: MAL access tokens live ~31 days; refresh_token handling exists
// nowhere else in the codebase (AniList's Implicit Grant has no refresh
// token at all). refresh_token() is the one genuinely new mechanism in this
// phase — reviewed hardest per PORT_PARITY.md §9.1.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "auth.hpp"
#include "http.hpp"
#include "result.hpp"

namespace shigoku::mal_login {

// Unlike AniList's kClientId (login.hpp — shigoku's own app registration,
// baked in and committed), MAL's client_id is the USER's own: each install
// registers its own app at https://myanimelist.net/apiconfig (client_id
// alone, app type "other" — a native/public client, no client_secret) and
// pastes it into Settings, so it lives in Config::mal_client_id (P34-style
// user-editable string), not a compile-time constant. Every entry point
// below takes it as a parameter and refuses cleanly (Kind::NoClientId) on
// an empty string rather than sending a request MAL would reject.

// Same loopback port as AniList (login::kLoopbackPort, 8767) — only one
// connect session runs at a time (App::ConnectSession, single slot), so
// there is no port conflict; the callback PATH disambiguates the provider.
inline constexpr std::uint16_t kLoopbackPort = 8767;
inline constexpr const char* kCallbackPath = "/mal/callback";

// THE FINAL REDIRECT URI — register exactly this string as the App Redirect
// URL at https://myanimelist.net/apiconfig. MAL requires an exact match; do
// not add or drop a trailing slash, and do not use "localhost" (AniList's
// registration uses that spelling, but this code binds numeric 127.0.0.1 —
// see loopback.cpp's IPv4-only bind note, 06 §4.4).
inline constexpr const char* kRedirectUri = "http://127.0.0.1:8767/mal/callback";

inline constexpr const char* kAuthorizeEndpoint = "https://myanimelist.net/v1/oauth2/authorize";
inline constexpr const char* kTokenEndpoint = "https://myanimelist.net/v1/oauth2/token";

// Tokens observed shorter than this are not real (defensive floor, same
// spirit as login::kTokenFloor).
inline constexpr std::size_t kTokenFloor = 20;

// A minted PKCE pair. code_challenge == code_verifier under "plain" — kept
// as two fields anyway so call sites never have to know that equivalence.
struct Pkce {
  std::string code_verifier;
  std::string code_challenge;
};

// CSPRNG-backed; Err only on a /dev/urandom failure (nonce::mint's own
// failure mode).
[[nodiscard]] Result<Pkce, std::string> mint_pkce();

// The authorize URL the browser is sent to. `state` is the CSRF nonce (same
// role as login::authorize_url's); `code_challenge` is Pkce::code_challenge;
// `client_id` is Config::mal_client_id (the user's own registration).
[[nodiscard]] std::string authorize_url(std::string_view client_id, std::string_view state,
                                         std::string_view code_challenge);

// Outcome of a MAL connect attempt. Mirrors login::ConnectResult::Kind
// (event.hpp's ConnectOutcome already covers Ok/NoToken/Rejected/
// NetworkError/SaveFailed/BadState/Canceled; NoClientId is MAL-only, so it
// maps to NetworkError at the event boundary — never reaching the exchange
// endpoint is the closest existing bucket, and the toast text disambiguates).
struct MalConnectResult {
  enum class Kind {
    Ok,
    NoClientId,
    NoCode,
    Rejected,
    NetworkError,
    SaveFailed,
    BadState,
    Canceled,
  };
  Kind kind = Kind::NoCode;
  std::string user_name;  // Ok only; MAL's token response carries no display
                           // name, so this is populated from a follow-up
                           // /users/@me call (slice 3's REST client) — empty
                           // here is expected and fine for slice 2's DoD.

  friend bool operator==(const MalConnectResult&, const MalConnectResult&) = default;

  static MalConnectResult ok(std::string name) { return {Kind::Ok, std::move(name)}; }
  static MalConnectResult no_client_id() { return {Kind::NoClientId, {}}; }
  static MalConnectResult no_code() { return {Kind::NoCode, {}}; }
  static MalConnectResult rejected() { return {Kind::Rejected, {}}; }
  static MalConnectResult network_error() { return {Kind::NetworkError, {}}; }
  static MalConnectResult save_failed() { return {Kind::SaveFailed, {}}; }
  static MalConnectResult bad_state() { return {Kind::BadState, {}}; }
  static MalConnectResult canceled() { return {Kind::Canceled, {}}; }
};

// A decoded token response (shared shape between the code exchange and the
// refresh exchange — MAL's token endpoint returns the same fields for both).
struct TokenResponse {
  std::string access_token;
  std::string refresh_token;
  std::string token_type;
  std::int64_t expires_in = 0;  // seconds, relative — caller adds `now`.

  friend bool operator==(const TokenResponse&, const TokenResponse&) = default;
};

// The exchange seam, a callable so tests fake the network (login.hpp's
// Verifier precedent). Takes the POST body (already form-encoded) and the
// endpoint; returns the decoded token pair or a transport/decode error.
using Exchanger =
    std::function<Result<TokenResponse, ProviderError>(std::string_view form_body)>;

// The real exchanger: POSTs `body` to kTokenEndpoint over `client`.
[[nodiscard]] Exchanger make_http_exchanger(const http::Client& client);

// Authorization-code exchange (06 §4.2-equivalent for MAL): builds the POST
// body from client_id/code/code_verifier/redirect_uri, calls `exchanger`,
// verifies the response floor, then builds and saves Auth with .mal
// populated (.anilist untouched — Auth::save writes the whole file, so the
// caller must have loaded the CURRENT Auth first and only mutate .mal into
// it, exactly as login::complete_login never touches .mal today).
[[nodiscard]] MalConnectResult complete_mal_login(std::string_view client_id, std::string_view code,
                                                   std::string_view code_verifier,
                                                   const Exchanger& exchanger,
                                                   const std::string& auth_path, std::int64_t now);

// Refresh rollover: builds the refresh_token POST body, exchanges, and
// re-saves .mal with the new access/refresh pair. Same floor + save-failure
// handling as complete_mal_login. Called with the CURRENT MalAuth (its
// refresh_token is the one being redeemed); on success the returned Auth is
// what the caller should persist over the loaded record's .mal field.
[[nodiscard]] Result<MalAuth, MalConnectResult> refresh_mal_token(std::string_view client_id,
                                                                   const MalAuth& current,
                                                                   const Exchanger& exchanger,
                                                                   std::int64_t now);

// --- Internals exposed for the golden tests --------------------------------
namespace detail {

// Percent-encode `s` for a application/x-www-form-urlencoded body value
// (RFC 3986 unreserved set passes through raw; everything else is
// %XX-escaped). Local helper: no form-encoder exists elsewhere in the tree
// (every other POST body in the codebase is JSON via nlohmann).
[[nodiscard]] std::string form_encode(std::string_view s);

// Decode a MAL token-endpoint JSON body into TokenResponse. Missing fields
// default empty/0 (same tolerant-defaulting spirit as auth.cpp's
// json::value pattern) so one absent field degrades alone rather than
// failing the whole parse; a genuinely unparseable body returns nullopt.
[[nodiscard]] std::optional<TokenResponse> parse_token_response(std::string_view json_body);

}  // namespace detail

}  // namespace shigoku::mal_login
