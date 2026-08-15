// scaffold_tests.cpp — P0 test target.
//
// Proves the test harness itself is wired: doctest compiles under our locked
// flag set, ctest runs it, and the vendored single-headers are reachable and
// at the versions we pinned. Real module tests replace/augment this per phase.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>
#include <tl/expected.hpp>

TEST_CASE("doctest harness runs") {
  CHECK(1 + 1 == 2);
}

TEST_CASE("vendored nlohmann/json is at the pinned version") {
  CHECK(NLOHMANN_JSON_VERSION_MAJOR == 3);
  CHECK(NLOHMANN_JSON_VERSION_MINOR == 11);
  CHECK(NLOHMANN_JSON_VERSION_PATCH == 3);
}

TEST_CASE("nlohmann/json round-trips a small object") {
  const auto j = nlohmann::json::parse(R"({"mal_id": 52991, "title": "Frieren"})");
  CHECK(j.at("mal_id").get<int>() == 52991);
  CHECK(j.at("title").get<std::string>() == "Frieren");
}

TEST_CASE("tl::expected is available as the documented std::expected fallback") {
  tl::expected<int, std::string> ok = 42;
  tl::expected<int, std::string> err = tl::make_unexpected("boom");
  CHECK(ok.has_value());
  CHECK(ok.value() == 42);
  CHECK_FALSE(err.has_value());
  CHECK(err.error() == "boom");
}
