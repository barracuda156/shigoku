// semver_tests.cpp — P28 version comparison tests.
//
// Ports sabigoku src/semver.rs #[test] cases 1:1 (§8: golden tests are the
// port contract — same names in snake_case, same fixture values).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/semver.hpp"

using namespace shigoku::semver;

TEST_CASE("parse_accepts_optional_v_prefix_and_plain_core") {
  auto a = parse_version("0.4.1");
  auto b = parse_version("v0.4.1");
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(*a == *b);
  CHECK(a->order(*b) == std::strong_ordering::equal);
}

TEST_CASE("parse_rejects_garbage_and_wrong_arity_cores") {
  for (const char* bad : {"", "v", "0.4", "0.4.1.2", "0.x.1", "latest", "0..1", "1.2.-3"}) {
    INFO("bad = ", bad);
    CHECK_FALSE(parse_version(bad).has_value());
  }
}

TEST_CASE("order_compares_fields_numerically_not_lexically") {
  auto lo = parse_version("0.9.0");
  auto hi = parse_version("0.10.0");
  REQUIRE(lo.has_value());
  REQUIRE(hi.has_value());
  CHECK(lo->order(*hi) == std::strong_ordering::less);
  CHECK(hi->order(*lo) == std::strong_ordering::greater);
}

TEST_CASE("order_ranks_a_prerelease_below_the_same_released_core") {
  auto dev = parse_version("0.4.1-dev");
  auto rel = parse_version("0.4.1");
  REQUIRE(dev.has_value());
  REQUIRE(rel.has_value());
  CHECK(dev->order(*rel) == std::strong_ordering::less);
  CHECK(rel->order(*dev) == std::strong_ordering::greater);
}

TEST_CASE("build_metadata_is_ignored_not_treated_as_prerelease") {
  auto a = parse_version("0.4.1+abc123");
  auto b = parse_version("0.4.1");
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->order(*b) == std::strong_ordering::equal);
}

// Ratified deviation (ROD-465 review): zigoku splits on the first `-` even
// inside build metadata and rejects this input; the semver-correct read is
// kept on purpose.
TEST_CASE("hyphen_inside_build_metadata_stays_build_metadata") {
  auto a = parse_version("0.4.1+build-info");
  auto b = parse_version("0.4.1");
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->order(*b) == std::strong_ordering::equal);
}

TEST_CASE("is_newer_the_update_check_question") {
  CHECK(is_newer("0.5.0", "0.4.1"));
  CHECK(is_newer("0.10.0", "0.9.9"));
  CHECK(is_newer("v0.1.2", "0.1.1"));
  CHECK_FALSE(is_newer("0.4.1", "0.4.1"));
  CHECK_FALSE(is_newer("0.4.0", "0.4.1"));
  // A local dev build ahead of the last release must never nag.
  CHECK_FALSE(is_newer("0.4.1", "0.5.0-dev"));
  CHECK_FALSE(is_newer("garbage", "0.4.1"));
  CHECK_FALSE(is_newer("", "0.4.1"));
}
