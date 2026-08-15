// mal_login.cpp — MAL OAuth login core (P31 §9.1 slice 2). No sabigoku
// reference (see mal_login.hpp).

#include "mal_login.hpp"

#include <nlohmann/json.hpp>

#include "nonce.hpp"

namespace shigoku::mal_login {

namespace {

using json = nlohmann::json;

bool is_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.' || c == '_' || c == '~';
}

}  // namespace

namespace detail {

std::string form_encode(std::string_view s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (is_unreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0x0f]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

std::optional<TokenResponse> parse_token_response(std::string_view json_body) {
  json parsed;
  try {
    parsed = json::parse(json_body);
  } catch (const json::parse_error&) {
    return std::nullopt;
  }
  if (!parsed.is_object()) return std::nullopt;

  TokenResponse t;
  t.access_token = parsed.value("access_token", std::string{});
  t.refresh_token = parsed.value("refresh_token", std::string{});
  t.token_type = parsed.value("token_type", std::string{});
  t.expires_in = parsed.value("expires_in", std::int64_t{0});
  return t;
}

}  // namespace detail

Result<Pkce, std::string> mint_pkce() {
  auto a = nonce::mint();
  if (!a.has_value()) return err(a.error());
  auto b = nonce::mint();
  if (!b.has_value()) return err(b.error());
  Pkce p;
  p.code_verifier = *a + *b;  // 64 hex chars: inside RFC 7636's 43-128 floor.
  p.code_challenge = p.code_verifier;  // "plain": challenge is the identity.
  return p;
}

std::string authorize_url(std::string_view client_id, std::string_view state,
                           std::string_view code_challenge) {
  std::string url = std::string(kAuthorizeEndpoint) +
                     "?response_type=code&client_id=" + detail::form_encode(client_id) +
                     "&redirect_uri=" + detail::form_encode(kRedirectUri) +
                     "&code_challenge=" + detail::form_encode(code_challenge) +
                     "&code_challenge_method=plain" +
                     "&state=" + detail::form_encode(state);
  return url;
}

Exchanger make_http_exchanger(const http::Client& client) {
  return [&client](std::string_view form_body) -> Result<TokenResponse, ProviderError> {
    http::Request req;
    req.method = http::Method::Post;
    req.url = kTokenEndpoint;
    req.content_type = "application/x-www-form-urlencoded";
    req.body.assign(form_body.begin(), form_body.end());
    req.accept = http::Accept::Any2xx;

    auto resp = client.fetch(req);
    if (!resp.has_value()) return err(resp.error());

    const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
    auto parsed = detail::parse_token_response(raw);
    if (!parsed.has_value()) return err(ProviderError::decode("mal token response"));
    return *parsed;
  };
}

namespace {

// Shared tail: verify the exchanged token against the floor, merge it into
// the CURRENT on-disk Auth's .mal block (never touching .anilist), save, and
// report a terminal result carrying the still-empty user_name (slice 3's
// REST client fills that in via a follow-up call — see mal_login.hpp).
MalConnectResult finish_exchange(const Result<TokenResponse, ProviderError>& exchanged,
                                  const std::string& auth_path, std::int64_t now) {
  if (!exchanged.has_value()) return MalConnectResult::network_error();
  const TokenResponse& tok = *exchanged;
  if (tok.access_token.size() < kTokenFloor) return MalConnectResult::rejected();

  Auth auth = Auth::load(auth_path);
  auth.mal.access_token = tok.access_token;
  auth.mal.refresh_token = tok.refresh_token;
  auth.mal.token_type = tok.token_type.empty() ? kDefaultTokenType : tok.token_type;
  auth.mal.expires_at = tok.expires_in > 0 ? now + tok.expires_in : 0;

  auto saved = auth.save(auth_path);
  if (!saved.has_value()) return MalConnectResult::save_failed();
  return MalConnectResult::ok(std::string{});
}

}  // namespace

MalConnectResult complete_mal_login(std::string_view client_id, std::string_view code,
                                     std::string_view code_verifier, const Exchanger& exchanger,
                                     const std::string& auth_path, std::int64_t now) {
  if (client_id.empty()) return MalConnectResult::no_client_id();
  if (code.empty()) return MalConnectResult::no_code();

  std::string body = "client_id=" + detail::form_encode(client_id) +
                      "&grant_type=authorization_code" + "&code=" + detail::form_encode(code) +
                      "&code_verifier=" + detail::form_encode(code_verifier) +
                      "&redirect_uri=" + detail::form_encode(kRedirectUri);

  return finish_exchange(exchanger(body), auth_path, now);
}

Result<MalAuth, MalConnectResult> refresh_mal_token(std::string_view client_id,
                                                     const MalAuth& current,
                                                     const Exchanger& exchanger,
                                                     std::int64_t now) {
  if (client_id.empty()) return err(MalConnectResult::no_client_id());
  if (current.refresh_token.empty()) return err(MalConnectResult::no_code());

  std::string body = "client_id=" + detail::form_encode(client_id) +
                      "&grant_type=refresh_token" +
                      "&refresh_token=" + detail::form_encode(current.refresh_token);

  auto exchanged = exchanger(body);
  if (!exchanged.has_value()) return err(MalConnectResult::network_error());
  const TokenResponse& tok = *exchanged;
  if (tok.access_token.size() < kTokenFloor) return err(MalConnectResult::rejected());

  MalAuth next = current;
  next.access_token = tok.access_token;
  // MAL's refresh response always carries a new refresh_token in practice;
  // guard anyway so a lenient/partial mock response can't null out a live
  // refresh_token (the next rollover would otherwise have nothing to redeem).
  if (!tok.refresh_token.empty()) next.refresh_token = tok.refresh_token;
  next.token_type = tok.token_type.empty() ? kDefaultTokenType : tok.token_type;
  next.expires_at = tok.expires_in > 0 ? now + tok.expires_in : 0;
  return next;
}

}  // namespace shigoku::mal_login
