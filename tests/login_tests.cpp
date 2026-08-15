// login_tests.cpp — P19 OAuth login core tests.
//
// Ports the sabigoku login.rs #[test] cases 1:1 (§8: golden tests are the
// port contract). Offline: the Verifier is faked, no network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <string>

#include "../src/auth.hpp"
#include "../src/login.hpp"

using namespace shigoku;
using namespace shigoku::login;

namespace {

std::string tmp(const char* name) {
  std::string path = "/tmp/shigoku-login-test-";
  path += std::to_string(static_cast<long>(::getpid()));
  path += "-";
  path += name;
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

anilist::Viewer viewer() { return anilist::Viewer{7, "rod"}; }

Verifier ok_verifier(anilist::Viewer v = viewer()) {
  return [v](std::string_view) -> Result<std::optional<anilist::Viewer>, ProviderError> {
    return std::optional<anilist::Viewer>(v);
  };
}

Verifier no_viewer_verifier() {
  return [](std::string_view) -> Result<std::optional<anilist::Viewer>, ProviderError> {
    return std::optional<anilist::Viewer>(std::nullopt);
  };
}

Verifier err_verifier() {
  return [](std::string_view) -> Result<std::optional<anilist::Viewer>, ProviderError> {
    return err(ProviderError::network());
  };
}

constexpr const char* kGoodToken = "abcdefghijklmnopqrstuvwxyz012345";

}  // namespace

TEST_CASE("authorize url carries client response type and state") {
  const std::string u = authorize_url("nonce123");
  CHECK(u.find("client_id=") != std::string::npos);
  CHECK(u.find("response_type=token") != std::string::npos);
  CHECK(u.find("state=nonce123") != std::string::npos);
  // The paste flow has no CSRF anchor, so its URL carries no state at all.
  CHECK(authorize_url_bare().find("state") == std::string::npos);
}

TEST_CASE("normalize paste wraps a bare jwt and passes urls through") {
  CHECK(normalize_paste("eyJabc.def.ghi") == "access_token=eyJabc.def.ghi");
  const std::string url = "http://localhost:8767/#access_token=tok&state=n";
  CHECK(normalize_paste(url) == url);
  CHECK(normalize_paste("").empty());
}

TEST_CASE("complete login verifies then saves") {
  const std::string path = tmp("ok.json");
  const std::string raw =
      std::string("http://localhost:8767/#access_token=") + kGoodToken +
      "&expires_in=3600&state=n";
  const ConnectResult out = complete_login(raw, ok_verifier(), path, 1000);
  CHECK(out == ConnectResult::ok("rod"));

  const Auth auth = Auth::load(path);
  REQUIRE(auth.anilist.bearer().has_value());
  CHECK(*auth.anilist.bearer() == kGoodToken);
  CHECK(auth.anilist.user_id == 7);
  // expires_at = now + expires_in.
  CHECK(auth.anilist.expires_at == 1000 + 3600);
}

TEST_CASE("complete login persists the viewer's score_format") {
  const std::string path = tmp("score_format.json");
  const std::string raw =
      std::string("http://localhost:8767/#access_token=") + kGoodToken + "&state=n";
  const anilist::Viewer v{7, "rod", ScoreFormat::Point10};
  CHECK(complete_login(raw, ok_verifier(v), path, 1000).kind == ConnectResult::Kind::Ok);
  CHECK(Auth::load(path).anilist.score_format == ScoreFormat::Point10);
}

TEST_CASE("absent expires in leaves undated token") {
  const std::string path = tmp("undated.json");
  const std::string raw = std::string("?access_token=") + kGoodToken + "&token_type=Bearer&state=n";
  CHECK(complete_login(raw, ok_verifier(), path, 1000).kind == ConnectResult::Kind::Ok);
  CHECK(Auth::load(path).anilist.expires_at == 0);
}

TEST_CASE("short or missing token is no token and writes nothing") {
  const std::string path = tmp("short.json");
  CHECK(complete_login("#access_token=tooshort&state=n", ok_verifier(), path, 0) ==
        ConnectResult::no_token());
  CHECK(complete_login("#state=n&token_type=Bearer", ok_verifier(), path, 0) ==
        ConnectResult::no_token());
  CHECK(Auth::load(path) == Auth{});  // no verify attempted, nothing to persist.
}

TEST_CASE("rejected and network never persist") {
  const std::string path = tmp("rejected.json");
  const std::string raw = std::string("#access_token=") + kGoodToken + "&state=n";
  CHECK(complete_login(raw, no_viewer_verifier(), path, 0) == ConnectResult::rejected());
  CHECK(Auth::load(path) == Auth{});  // a rejected token must not be written.

  CHECK(complete_login(raw, err_verifier(), path, 0) == ConnectResult::network_error());
  CHECK(Auth::load(path) == Auth{});  // an unverified token must not be written.
}

TEST_CASE("param is anchored to a boundary") {
  // A suffix key must not match the longer one before it.
  auto v1 = detail::param("xstate=evil&state=real", "state");
  REQUIRE(v1.has_value());
  CHECK(*v1 == "real");

  // First real occurrence wins; boundaries are start / & / ? / #.
  auto v2 = detail::param("state=first&state=second", "state");
  REQUIRE(v2.has_value());
  CHECK(*v2 == "first");

  auto v3 = detail::param("a=1?state=q", "state");
  REQUIRE(v3.has_value());
  CHECK(*v3 == "q");

  auto v4 = detail::param("#access_token=tok&x=y", "access_token");
  REQUIRE(v4.has_value());
  CHECK(*v4 == "tok");

  // No real (boundary-anchored) occurrence.
  CHECK(!detail::param("notstate=nope", "state").has_value());
}

TEST_CASE("verified but unwritable is save failed") {
  const std::string path = "/nonexistent-dir-shigoku/auth.json";
  const std::string raw = std::string("#access_token=") + kGoodToken + "&state=n";
  CHECK(complete_login(raw, ok_verifier(), path, 0) == ConnectResult::save_failed());
}
