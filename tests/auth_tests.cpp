// auth_tests.cpp — P19 auth.json load/save tests.
//
// Ports the sabigoku auth.rs #[test] cases 1:1 (§8: golden tests are the
// port contract). Offline, tmp files in the OS tmpdir; no network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <limits>
#include <string>

#include "../src/auth.hpp"

using namespace shigoku;

namespace {

// Fresh path in the OS tmpdir (auth.rs tmp()); each call removes any prior
// leftover so a test starts blank.
std::string tmp(const char* name) {
  std::string path = "/tmp/shigoku-auth-test-";
  path += std::to_string(static_cast<long>(::getpid()));
  path += "-";
  path += name;
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

void write_file(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

Auth signed_in() {
  Auth a;
  a.anilist.access_token = "abcdefghijklmnopqrstuvwxyz";
  a.anilist.token_type = "Bearer";
  a.anilist.expires_at = 1'800'000'000;
  a.anilist.user_id = 4242;
  a.anilist.user_name = "rod";
  return a;
}

Auth signed_in_both() {
  Auth a = signed_in();
  a.mal.access_token = "mal-access-token-value";
  a.mal.refresh_token = "mal-refresh-token-value";
  a.mal.token_type = "Bearer";
  a.mal.expires_at = 1'900'000'000;
  a.mal.user_id = 99;
  return a;
}

}  // namespace

TEST_CASE("round trip preserves a signed in record") {
  const std::string path = tmp("round_trip.json");
  Auth auth = signed_in();
  REQUIRE(auth.save(path).has_value());
  CHECK(Auth::load(path) == auth);
}

TEST_CASE("round trip preserves both anilist and mal blocks") {
  const std::string path = tmp("round_trip_both.json");
  Auth auth = signed_in_both();
  REQUIRE(auth.save(path).has_value());
  Auth loaded = Auth::load(path);
  CHECK(loaded == auth);
  CHECK(loaded.mal.refresh_token == "mal-refresh-token-value");
  CHECK(loaded.mal.user_id == 99);
}

// --- score_format (P34 slice 2) ---------------------------------------------

TEST_CASE("round trip preserves a non-default score_format") {
  const std::string path = tmp("score_format_round_trip.json");
  Auth auth = signed_in();
  auth.anilist.score_format = ScoreFormat::Point5;
  REQUIRE(auth.save(path).has_value());
  CHECK(Auth::load(path) == auth);
  CHECK(Auth::load(path).anilist.score_format == ScoreFormat::Point5);
}

TEST_CASE("missing score_format field defaults to Point100") {
  const std::string path = tmp("score_format_missing.json");
  write_file(path, R"({"anilist": {"access_token": "abcdefghijklmnopqrstuvwxyz"}})");
  CHECK(Auth::load(path).anilist.score_format == ScoreFormat::Point100);
}

TEST_CASE("unknown score_format string falls back to Point100") {
  const std::string path = tmp("score_format_unknown.json");
  write_file(path, R"({"anilist": {"access_token": "x", "score_format": "NONSENSE"}})");
  CHECK(Auth::load(path).anilist.score_format == ScoreFormat::Point100);
}

TEST_CASE("mal block absent from file leaves mal signed out") {
  const std::string path = tmp("mal_absent.json");
  Auth auth = signed_in();  // anilist only, no mal field set
  REQUIRE(auth.save(path).has_value());
  write_file(path, R"({"anilist": {"access_token": "abcdefghijklmnopqrstuvwxyz"}})");
  Auth loaded = Auth::load(path);
  CHECK(loaded.anilist.bearer().has_value());
  CHECK(!loaded.mal.bearer().has_value());
  CHECK(loaded.mal.refresh_token.empty());
}

TEST_CASE("default is signed out bearer none") {
  Auth auth;
  CHECK(auth.anilist.token_type == "Bearer");
  CHECK(!auth.anilist.bearer().has_value());
  CHECK(auth.mal.token_type == "Bearer");
  CHECK(!auth.mal.bearer().has_value());
}

TEST_CASE("missing file is signed out") {
  CHECK(Auth::load("/nonexistent/auth.json") == Auth{});
}

TEST_CASE("corrupt file is signed out") {
  const std::string path = tmp("corrupt.json");
  write_file(path, "{ this is not json");
  CHECK(Auth::load(path) == Auth{});
}

TEST_CASE("oversized file is signed out") {
  const std::string path = tmp("oversized.json");
  std::string text = R"({"anilist": {"access_token": "x", "pad": ")";
  text.append(20 * 1024, 'x');
  text += "\"}}";
  REQUIRE(static_cast<std::uint64_t>(text.size()) > kMaxAuthBytes);
  write_file(path, text);
  CHECK(Auth::load(path) == Auth{});
}

TEST_CASE("missing keys default missing block is signed out") {
  const std::string path = tmp("partial.json");
  write_file(path, R"({"anilist": {"user_name": "rod"}})");
  Auth auth = Auth::load(path);
  CHECK(auth.anilist.user_name == "rod");
  CHECK(auth.anilist.token_type == "Bearer");
  CHECK(!auth.anilist.bearer().has_value());

  // A file with no anilist block at all still loads clean.
  const std::string empty = tmp("empty.json");
  write_file(empty, "{}");
  CHECK(Auth::load(empty) == Auth{});
}

TEST_CASE("mal partial block defaults missing fields") {
  const std::string path = tmp("mal_partial.json");
  write_file(path, R"({"mal": {"access_token": "tok", "user_id": 7}})");
  Auth auth = Auth::load(path);
  CHECK(auth.mal.access_token == "tok");
  CHECK(auth.mal.user_id == 7);
  CHECK(auth.mal.token_type == "Bearer");
  CHECK(auth.mal.refresh_token.empty());
  CHECK(auth.mal.expires_at == 0);
}

TEST_CASE("save creates 0600 and leaves no temp") {
  const std::string path = tmp("perms.json");
  REQUIRE(signed_in().save(path).has_value());
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  struct stat tmp_st{};
  CHECK(::stat((path + ".tmp").c_str(), &tmp_st) != 0);
}

TEST_CASE("save over world readable stale temp still lands 0600") {
  const std::string path = tmp("stale.json");
  const std::string stale = path + ".tmp";
  write_file(stale, "leftover");
  ::chmod(stale.c_str(), 0644);
  REQUIRE(signed_in().save(path).has_value());
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
}

TEST_CASE("save replaces an existing file") {
  const std::string path = tmp("replace.json");
  REQUIRE(Auth{}.save(path).has_value());
  CHECK(!Auth::load(path).anilist.bearer().has_value());
  REQUIRE(signed_in().save(path).has_value());
  CHECK(Auth::load(path).anilist.user_name == "rod");
}

TEST_CASE("save failure surfaces an error") {
  const std::string path = "/nonexistent-dir-shigoku/auth.json";
  auto r = signed_in().save(path);
  CHECK(!r.has_value());
}

TEST_CASE("fifo path is signed out not a hang") {
  const std::string path = tmp("fifo.json");
  REQUIRE(::mkfifo(path.c_str(), 0600) == 0);

  auto fut = std::async(std::launch::async, [&path]() { return Auth::load(path); });
  REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(fut.get() == Auth{});

  std::remove(path.c_str());
}

TEST_CASE("control bytes force signed out") {
  const std::string bad[] = {"with\nnewline", "with\rcr", "with\ttab",
                              std::string("with\0nul", 8), "\x01lead"};
  for (const auto& token : bad) {
    AniListAuth a;
    a.access_token = token;
    CHECK(!a.bearer().has_value());
  }
  AniListAuth clean;
  clean.access_token = "clean-token-value";
  REQUIRE(clean.bearer().has_value());
  CHECK(*clean.bearer() == "clean-token-value");
}

TEST_CASE("mal control bytes force signed out") {
  const std::string bad[] = {"with\nnewline", "with\rcr", "with\ttab",
                              std::string("with\0nul", 8), "\x01lead"};
  for (const auto& token : bad) {
    MalAuth m;
    m.access_token = token;
    CHECK(!m.bearer().has_value());
  }
  MalAuth clean;
  clean.access_token = "clean-mal-token";
  REQUIRE(clean.bearer().has_value());
  CHECK(*clean.bearer() == "clean-mal-token");
}

TEST_CASE("is expired respects zero and boundary") {
  AniListAuth undated;
  undated.expires_at = 0;
  CHECK(!undated.is_expired(std::numeric_limits<std::int64_t>::max()));

  AniListAuth dated;
  dated.expires_at = 1000;
  CHECK(!dated.is_expired(999));
  CHECK(dated.is_expired(1000));
  CHECK(dated.is_expired(1001));
}

TEST_CASE("mal is expired respects zero and boundary") {
  MalAuth undated;
  undated.expires_at = 0;
  CHECK(!undated.is_expired(std::numeric_limits<std::int64_t>::max()));

  MalAuth dated;
  dated.expires_at = 1000;
  CHECK(!dated.is_expired(999));
  CHECK(dated.is_expired(1000));
  CHECK(dated.is_expired(1001));
}
