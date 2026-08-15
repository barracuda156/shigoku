// cover_pool_tests.cpp — P17b CoverPool coordinator + render store tests.
// Part A ports the sabigoku src/tui/covers/discover.rs #[test] suite 1:1
// (§8: golden tests are the port contract), Instant -> TickCount. Part B
// (id recycling, retain-sync) has no Rust counterpart (cover_pool.hpp file
// comment: ratatui-image's ProtocolPool does not port).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "../src/tui/cover_pool.hpp"
#include "../src/tui/covers.hpp"  // kCoverCooldownTicks

using namespace shigoku;
using namespace shigoku::tui;

namespace {

std::vector<std::string> urls(std::size_t n) {
  std::vector<std::string> v;
  for (std::size_t i = 0; i < n; ++i) v.push_back("https://img/" + std::to_string(i) + ".png");
  return v;
}

}  // namespace

TEST_CASE("pump fetches up to cap-minus-busy in window order") {
  CoverPool cp;
  const auto u = urls(6);
  const auto chosen = cp.pump(u, /*now=*/0, /*cap=*/4, /*busy=*/2);
  REQUIRE(chosen.size() == 2);
  CHECK(chosen[0] == u[0]);
  CHECK(chosen[1] == u[1]);
  for (const auto& url : chosen) {
    REQUIRE(cp.get(url) != nullptr);
    CHECK(cp.get(url)->status == CoverSlotStatus::Loading);
  }
  CHECK(cp.get(u[2]) == nullptr);  // past budget: no slot minted.
}

TEST_CASE("pump leaves room for in-flight even past a live cap decrease") {
  CoverPool cp;
  const auto u = urls(3);
  CHECK(cp.pump(u, 0, 4, 4).empty());
  // Live cap decrease: busy exceeds cap; still no new spawns.
  CHECK(cp.pump(u, 0, 2, 3).empty());
}

TEST_CASE("pump skips ready, loading, and cooling slots") {
  CoverPool cp;
  const auto u = urls(4);
  const std::vector<std::string> first_two(u.begin(), u.begin() + 2);
  cp.adopt(u[0]);
  const auto first = cp.pump(first_two, 0, 8, 0);
  REQUIRE(first.size() == 1);
  CHECK(first[0] == u[1]);  // ready slot skipped.
  const auto again = cp.pump(first_two, 0, 8, 0);
  CHECK(again.empty());  // loading slot skipped.
  cp.note_failure(u[1], 0);
  CHECK(cp.pump(first_two, 0, 8, 0).empty());  // cooling.
  const TickCount past = kCoverCooldownTicks;
  const auto reopened = cp.pump(first_two, past, 8, 0);
  REQUIRE(reopened.size() == 1);
  CHECK(reopened[0] == u[1]);  // cooldown boundary re-admits.
}

TEST_CASE("duplicate url in the window is single-flight") {
  CoverPool cp;
  const auto u = urls(1);
  const std::vector<std::string> w = {u[0], u[0]};
  const auto chosen = cp.pump(w, 0, 8, 0);
  REQUIRE(chosen.size() == 1);
  CHECK(chosen[0] == u[0]);
}

TEST_CASE("reset_loading only unwedges a loading slot") {
  CoverPool cp;
  const auto u = urls(1);
  (void)cp.pump(u, 0, 8, 0);
  cp.reset_loading(u[0]);
  REQUIRE(cp.get(u[0]) != nullptr);
  CHECK(cp.get(u[0])->status == CoverSlotStatus::Idle);
  cp.adopt(u[0]);
  cp.reset_loading(u[0]);
  CHECK(cp.get(u[0])->status == CoverSlotStatus::Ready);
}

TEST_CASE("adoption recreates a slot evicted mid-flight") {
  CoverPool cp;
  const auto u = urls(1);
  (void)cp.pump(u, 0, 8, 0);
  cp.evict(u[0]);
  CHECK(cp.get(u[0]) == nullptr);
  cp.adopt(u[0]);
  REQUIRE(cp.get(u[0]) != nullptr);
  CHECK(cp.get(u[0])->status == CoverSlotStatus::Ready);
  CHECK(cp.get(u[0])->has_pixels);
}

TEST_CASE("failure then success clears the cooldown") {
  CoverPool cp;
  const auto u = urls(1);
  cp.note_failure(u[0], 0);
  cp.adopt(u[0]);
  REQUIRE(cp.get(u[0]) != nullptr);
  CHECK(cp.get(u[0])->status == CoverSlotStatus::Ready);
  CHECK_FALSE(cp.get(u[0])->has_failed_at);
}

TEST_CASE("eviction sheds oldest offscreen past the cap") {
  CoverPool cp;
  const auto old = urls(kCoverPoolCap + 2);
  // Two pumps ago: everything minted (cap is generous).
  (void)cp.pump(old, 0, 1000, 0);
  for (const auto& u : old) cp.reset_loading(u);
  // New window: two fresh urls push the pool past cap; the two oldest-seen
  // off-screen slots go, the window itself is untouched.
  const std::vector<std::string> fresh = {"https://img/fresh-a.png", "https://img/fresh-b.png"};
  (void)cp.pump(fresh, 0, 1000, 0);
  CHECK(cp.size() <= kCoverPoolCap + 2);
  CHECK(cp.get(fresh[0]) != nullptr);
  CHECK(cp.get(fresh[1]) != nullptr);
  CHECK((cp.get(old[0]) == nullptr || cp.get(old[1]) == nullptr));
}

TEST_CASE("eviction never touches visible or loading slots") {
  CoverPool cp;
  const auto many = urls(kCoverPoolCap + 4);
  // Mint everything as loading: the whole pool is in flight.
  (void)cp.pump(many, 0, 1000, 0);
  const std::vector<std::string> first_two(many.begin(), many.begin() + 2);
  (void)cp.pump(first_two, 0, 1000, 1000);
  CHECK(cp.size() == kCoverPoolCap + 4);  // loading slots are the eviction floor.
}

TEST_CASE("eviction is a noop at or under the cap") {
  CoverPool cp;
  const auto u = urls(kCoverPoolCap);
  (void)cp.pump(u, 0, 1000, 0);
  for (const auto& url : u) cp.reset_loading(url);
  (void)cp.pump({}, 1, 8, 0);
  CHECK(cp.size() == kCoverPoolCap);
}

TEST_CASE("pump recency protects recently seen slots") {
  CoverPool cp;
  const auto all = urls(kCoverPoolCap + 2);
  (void)cp.pump(all, 0, 1000, 0);
  for (const auto& u : all) cp.reset_loading(u);
  // Re-see everything but the first two, then push two fresh urls in.
  const std::vector<std::string> rest(all.begin() + 2, all.end());
  (void)cp.pump(rest, 0, 1000, 1000);
  const std::vector<std::string> fresh = {"https://img/f1.png", "https://img/f2.png"};
  (void)cp.pump(fresh, 0, 1000, 1000);
  CHECK(cp.get(all[0]) == nullptr);  // unseen slot evicted first.
  CHECK(cp.get(all[1]) == nullptr);  // unseen slot evicted first.
  CHECK(cp.get(all[2]) != nullptr);  // recently seen slot survives.
}

// --- Part B: render store (shigoku-only) ------------------------------------

TEST_CASE("render store installs and recycles ids on evict") {
  CoverRenderStore store;
  CoverPixels px;
  px.w = 2;
  px.h = 2;
  px.rgba = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

  const auto id_a = store.install("u/a", px);
  REQUIRE(id_a.has_value());
  CHECK(*id_a == kGridImageIdBase);
  const auto id_b = store.install("u/b", px);
  REQUIRE(id_b.has_value());
  CHECK(*id_b == kGridImageIdBase + 1);

  store.erase("u/a");
  CHECK(store.get("u/a") == nullptr);
  // The freed id is reused before minting a new one.
  const auto id_c = store.install("u/c", px);
  REQUIRE(id_c.has_value());
  CHECK(*id_c == kGridImageIdBase);
}

TEST_CASE("render store install on an existing url updates pixels, keeps the id") {
  CoverRenderStore store;
  CoverPixels px1;
  px1.w = 1;
  px1.h = 1;
  px1.rgba = {1, 2, 3, 4};
  const auto id1 = store.install("u/a", px1);
  REQUIRE(id1.has_value());

  CoverPixels px2;
  px2.w = 3;
  px2.h = 3;
  px2.rgba = std::vector<unsigned char>(36, 9);
  const auto id2 = store.install("u/a", px2);
  REQUIRE(id2.has_value());
  CHECK(*id2 == *id1);
  REQUIRE(store.get("u/a") != nullptr);
  CHECK(store.get("u/a")->pixels.w == 3);
}

TEST_CASE("render store retain drops entries whose pool slot is gone") {
  CoverPool cp;
  CoverRenderStore store;
  CoverPixels px;
  px.w = 1;
  px.h = 1;
  px.rgba = {1, 2, 3, 4};

  cp.adopt("u/a");
  cp.adopt("u/b");
  store.install("u/a", px);
  store.install("u/b", px);
  CHECK(store.size() == 2);

  cp.evict("u/a");
  store.retain(cp);
  CHECK(store.get("u/a") == nullptr);
  CHECK(store.get("u/b") != nullptr);
  CHECK(store.size() == 1);
}
