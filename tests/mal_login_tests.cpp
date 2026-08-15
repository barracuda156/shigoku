// mal_login_tests.cpp — P31 §9.1 slice 2 MAL OAuth login core tests. No
// sabigoku golden reference (MAL never existed there); tests are new,
// authored against mal_login.hpp's own contract. Offline: the Exchanger is
// faked, no network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <string>

#include "../src/auth.hpp"
#include "../src/mal_login.hpp"

using namespace shigoku;
using namespace shigoku::mal_login;

namespace {

std::string tmp(const char* name) {
  std::string path = "/tmp/shigoku-mal-login-test-";
  path += std::to_string(static_cast<long>(::getpid()));
  path += "-";
  path += name;
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

constexpr const char* kGoodClientId = "test-client-id";
constexpr const char* kGoodAccess = "mal-access-token-value-ok";
constexpr const char* kGoodRefresh = "mal-refresh-token-value-ok";

TokenResponse good_token(std::string access = kGoodAccess, std::string refresh = kGoodRefresh,
                          std::int64_t expires_in = 3600) {
  TokenResponse t;
  t.access_token = std::move(access);
  t.refresh_token = std::move(refresh);
  t.token_type = "Bearer";
  t.expires_in = expires_in;
  return t;
}

Exchanger ok_exchanger(TokenResponse t = good_token()) {
  return [t](std::string_view) -> Result<TokenResponse, ProviderError> { return t; };
}

Exchanger err_exchanger() {
  return [](std::string_view) -> Result<TokenResponse, ProviderError> {
    return err(ProviderError::network());
  };
}

Exchanger short_token_exchanger() {
  return [](std::string_view) -> Result<TokenResponse, ProviderError> {
    return good_token("short");
  };
}

}  // namespace

TEST_CASE("mint pkce produces a code_verifier inside rfc7636 bounds") {
  auto p = mint_pkce();
  REQUIRE(p.has_value());
  CHECK(p->code_verifier.size() >= 43);
  CHECK(p->code_verifier.size() <= 128);
  CHECK(p->code_challenge == p->code_verifier);  // "plain" method: identity.
  for (unsigned char c : p->code_verifier) {
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
    CHECK(unreserved);
  }
}

TEST_CASE("mint pkce is not deterministic across calls") {
  auto a = mint_pkce();
  auto b = mint_pkce();
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->code_verifier != b->code_verifier);
}

TEST_CASE("authorize url carries every required param") {
  const std::string u = authorize_url("cid123", "state456", "challenge789");
  CHECK(u.find("myanimelist.net/v1/oauth2/authorize") != std::string::npos);
  CHECK(u.find("response_type=code") != std::string::npos);
  CHECK(u.find("client_id=cid123") != std::string::npos);
  CHECK(u.find("state=state456") != std::string::npos);
  CHECK(u.find("code_challenge=challenge789") != std::string::npos);
  CHECK(u.find("code_challenge_method=plain") != std::string::npos);
  CHECK(u.find("code_challenge_method=S256") == std::string::npos);
  CHECK(u.find(detail::form_encode(kRedirectUri)) != std::string::npos);
}

TEST_CASE("redirect uri is pinned to the registered form") {
  // Byte-exact: this is what the user registers at myanimelist.net/apiconfig
  // (P31 exec-seed decision). A change here silently breaks every existing
  // registration.
  CHECK(std::string(kRedirectUri) == "http://127.0.0.1:8767/mal/callback");
}

TEST_CASE("form encode passes unreserved chars and escapes the rest") {
  CHECK(detail::form_encode("abcXYZ019-._~") == "abcXYZ019-._~");
  CHECK(detail::form_encode("a b") == "a%20b");
  CHECK(detail::form_encode("a&b=c") == "a%26b%3Dc");
  CHECK(detail::form_encode("") == "");
}

TEST_CASE("parse token response tolerates missing fields") {
  auto full = detail::parse_token_response(
      R"({"access_token":"a","refresh_token":"r","token_type":"Bearer","expires_in":100})");
  REQUIRE(full.has_value());
  CHECK(full->access_token == "a");
  CHECK(full->refresh_token == "r");
  CHECK(full->token_type == "Bearer");
  CHECK(full->expires_in == 100);

  auto partial = detail::parse_token_response(R"({"access_token":"a"})");
  REQUIRE(partial.has_value());
  CHECK(partial->access_token == "a");
  CHECK(partial->refresh_token.empty());
  CHECK(partial->expires_in == 0);

  CHECK(!detail::parse_token_response("not json").has_value());
  CHECK(!detail::parse_token_response("[]").has_value());
}

TEST_CASE("complete mal login without a client id refuses before any exchange") {
  const std::string path = tmp("no_client_id.json");
  bool called = false;
  Exchanger spy = [&called](std::string_view) -> Result<TokenResponse, ProviderError> {
    called = true;
    return good_token();
  };
  auto out = complete_mal_login("", "code", "verifier", spy, path, 1000);
  CHECK(out == MalConnectResult::no_client_id());
  CHECK(!called);
  CHECK(Auth::load(path) == Auth{});
}

TEST_CASE("complete mal login with empty code is no code") {
  const std::string path = tmp("no_code.json");
  auto out = complete_mal_login(kGoodClientId, "", "verifier", ok_exchanger(), path, 1000);
  CHECK(out == MalConnectResult::no_code());
  CHECK(Auth::load(path) == Auth{});
}

TEST_CASE("complete mal login exchanges and saves the mal block only") {
  const std::string path = tmp("ok.json");
  // Pre-seed an anilist block to prove it survives untouched (Auth::save
  // writes the whole file; complete_mal_login must load-merge, not clobber).
  Auth seed;
  seed.anilist.access_token = "anilist-token-untouched";
  seed.anilist.user_name = "rod";
  REQUIRE(seed.save(path).has_value());

  auto out = complete_mal_login(kGoodClientId, "auth-code", "the-verifier", ok_exchanger(), path,
                                 1000);
  CHECK(out.kind == MalConnectResult::Kind::Ok);

  const Auth loaded = Auth::load(path);
  REQUIRE(loaded.mal.bearer().has_value());
  CHECK(*loaded.mal.bearer() == kGoodAccess);
  CHECK(loaded.mal.refresh_token == kGoodRefresh);
  CHECK(loaded.mal.expires_at == 1000 + 3600);
  CHECK(loaded.mal.token_type == "Bearer");
  // .anilist untouched.
  CHECK(loaded.anilist.access_token == "anilist-token-untouched");
  CHECK(loaded.anilist.user_name == "rod");
}

TEST_CASE("complete mal login network error never persists") {
  const std::string path = tmp("network.json");
  auto out = complete_mal_login(kGoodClientId, "code", "verifier", err_exchanger(), path, 0);
  CHECK(out == MalConnectResult::network_error());
  CHECK(Auth::load(path) == Auth{});
}

TEST_CASE("complete mal login short token is rejected and never persists") {
  const std::string path = tmp("short.json");
  auto out =
      complete_mal_login(kGoodClientId, "code", "verifier", short_token_exchanger(), path, 0);
  CHECK(out == MalConnectResult::rejected());
  CHECK(Auth::load(path) == Auth{});
}

TEST_CASE("complete mal login save failure surfaces") {
  const std::string path = "/nonexistent-dir-shigoku/auth.json";
  auto out = complete_mal_login(kGoodClientId, "code", "verifier", ok_exchanger(), path, 0);
  CHECK(out == MalConnectResult::save_failed());
}

TEST_CASE("refresh without a client id refuses") {
  MalAuth current;
  current.refresh_token = kGoodRefresh;
  auto out = refresh_mal_token("", current, ok_exchanger(), 1000);
  REQUIRE(!out.has_value());
  CHECK(out.error() == MalConnectResult::no_client_id());
}

TEST_CASE("refresh with no stored refresh token is no code") {
  MalAuth current;  // refresh_token empty.
  auto out = refresh_mal_token(kGoodClientId, current, ok_exchanger(), 1000);
  REQUIRE(!out.has_value());
  CHECK(out.error() == MalConnectResult::no_code());
}

TEST_CASE("refresh rolls access and refresh token forward") {
  MalAuth current;
  current.access_token = "stale-access";
  current.refresh_token = "stale-refresh";
  current.token_type = "Bearer";
  current.expires_at = 500;

  auto rolled = good_token("new-access-token-value", "new-refresh-token-value", 2'678'400);
  auto out = refresh_mal_token(kGoodClientId, current, ok_exchanger(rolled), 1'000'000);
  REQUIRE(out.has_value());
  CHECK(out->access_token == "new-access-token-value");
  CHECK(out->refresh_token == "new-refresh-token-value");
  CHECK(out->expires_at == 1'000'000 + 2'678'400);
}

TEST_CASE("refresh response without a new refresh token keeps the old one") {
  // MAL's docs say a new refresh_token always comes back, but a lenient mock
  // (or an edge case) might omit it; the old one must survive rather than be
  // nulled out, or the next rollover has nothing left to redeem.
  MalAuth current;
  current.refresh_token = "keep-me";

  TokenResponse partial;
  partial.access_token = "new-access-token-value";
  partial.expires_in = 3600;
  // refresh_token left empty.

  auto out = refresh_mal_token(kGoodClientId, current, ok_exchanger(partial), 0);
  REQUIRE(out.has_value());
  CHECK(out->refresh_token == "keep-me");
  CHECK(out->access_token == "new-access-token-value");
}

TEST_CASE("refresh network error and short token surface distinct kinds") {
  MalAuth current;
  current.refresh_token = kGoodRefresh;

  auto net = refresh_mal_token(kGoodClientId, current, err_exchanger(), 0);
  REQUIRE(!net.has_value());
  CHECK(net.error() == MalConnectResult::network_error());

  auto short_tok = refresh_mal_token(kGoodClientId, current, short_token_exchanger(), 0);
  REQUIRE(!short_tok.has_value());
  CHECK(short_tok.error() == MalConnectResult::rejected());
}
