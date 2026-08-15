// discover_tests.cpp — Discover card-grid STATE tests (P17a). Ports the
// sabigoku src/tui/view/discover.rs #[test] suite 1:1 (§8: golden tests are the
// port contract): feed filing by axis, stale/duplicate discard, MAX_FEED_ROWS
// exhaustion, catalog_cache upsert, wanted_fetch, sticky feed error, nav
// wrap/clamp, cursor/scroll survival across axis switches, selected_entry, the
// loading-arm reset, the genre glyph table, and format labels. Plus the P17a
// deviations from the plan (tick-clock loading, positional staleness).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "../src/store.hpp"
#include "../src/tui/cover_geom.hpp"
#include "../src/tui/discover.hpp"

using namespace shigoku;
using namespace shigoku::tui;

namespace {

CatalogRow entry(std::int64_t id) {
  Enrichment e;
  e.anilist_id = id;
  e.title_romaji = "Show " + std::to_string(id);
  e.cover_url = "http://127.0.0.1:9/" + std::to_string(id) + ".png";
  return CatalogRow{e};
}

std::vector<CatalogRow> entries(std::int64_t from, std::int64_t n) {
  std::vector<CatalogRow> v;
  for (std::int64_t i = from; i < from + n; ++i) v.push_back(entry(i));
  return v;
}

// 100 cols, halfblocks (no reported cell geometry): large tier, 4 cols per row.
GridGeo geo() { return grid_geo(100, 27, CellPx{}); }

Store store() {
  auto s = Store::open_memory();
  REQUIRE(s.has_value());
  return std::move(*s);
}

}  // namespace

TEST_CASE("feed pages append and file by axis") {
  DiscoverState d;
  Store s = store();
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  d.on_feed(DiscoverAxis::Popular, 1, entries(100, 5), false, &s, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 20);
  CHECK(!d.slot().exhausted);
  d.cycle_axis(1);
  CHECK(d.slot().entries.size() == 5);
  CHECK(d.slot().exhausted);  // has_next=false exhausts.
}

TEST_CASE("stale or duplicate pages are discarded") {
  DiscoverState d;
  Store s = store();
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 20);  // duplicate page 1 dropped.
  d.on_feed(DiscoverAxis::Trending, 3, entries(41, 20), true, &s, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 20);  // page 3 before 2 dropped.
  d.on_feed(DiscoverAxis::Trending, 2, entries(21, 20), true, &s, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 40);
}

TEST_CASE("MAX_FEED_ROWS forces exhaustion") {
  DiscoverState d;
  Store s = store();
  for (std::uint32_t page = 1; page <= 15; ++page) {
    const std::int64_t from = (static_cast<std::int64_t>(page) - 1) * 20 + 1;
    d.on_feed(DiscoverAxis::Trending, page, entries(from, 20), true, &s, 1000, d.filter_gen());
  }
  CHECK(d.slot().entries.size() == kMaxFeedRows);
  CHECK(d.slot().exhausted);  // 300 rows force-exhaust (ROD-339).
  CHECK(!d.wanted_fetch(geo()).has_value());
}

TEST_CASE("applied pages upsert catalog_cache") {
  DiscoverState d;
  Store s = store();
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 3), true, &s, 1000, d.filter_gen());
  for (std::int64_t id = 1; id <= 3; ++id) {
    auto hit = s.get_catalog(id);
    REQUIRE(hit.has_value());
    CHECK(hit->has_value());  // row cached.
  }
}

TEST_CASE("on_feed with no store still files the feed") {
  // P17a deviation: on_feed's store is nullable (demo / no-persist); the feed
  // still renders (the "never drop a rendered feed" contract at its limit).
  DiscoverState d;
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 4), false, nullptr, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 4);
  CHECK(d.slot().exhausted);
}

TEST_CASE("wanted_fetch wants page one for an empty slot") {
  DiscoverState d;
  CHECK(d.wanted_fetch(geo()) == std::optional<std::uint32_t>{1});
}

TEST_CASE("wanted_fetch holds while loading, failed, or far") {
  DiscoverState d;
  Store s = store();
  d.arm_loading(0);
  CHECK(!d.wanted_fetch(geo()).has_value());  // loading holds.
  d.disarm_loading();
  d.on_feed_error(DiscoverAxis::Trending, "offline", d.filter_gen());
  CHECK(!d.wanted_fetch(geo()).has_value());  // failed holds until retry.
  d.select_axis(0);
  CHECK(d.wanted_fetch(geo()) == std::optional<std::uint32_t>{1});  // axis key re-arms.
  // 5 rows of 4: cursor at top, last row 4 rows away: no prefetch.
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  CHECK(!d.wanted_fetch(geo()).has_value());
  // Move within 2 card-rows of the end: prefetch page 2.
  d.nav(0, 2, geo());  // cursor → row 2 (index 8).
  CHECK(d.wanted_fetch(geo()) == std::optional<std::uint32_t>{2});
}

TEST_CASE("feed error is sticky until an axis retry") {
  DiscoverState d;
  d.on_feed_error(DiscoverAxis::Trending, "offline", d.filter_gen());
  CHECK(d.slot().failed.has_value());
  CHECK(!d.wanted_fetch(geo()).has_value());
  d.cycle_axis(1);
  d.cycle_axis(-1);
  CHECK(!d.slot().failed.has_value());  // cycling back re-arms the slot.
}

TEST_CASE("nav wraps rows and clamps columns") {
  DiscoverState d;
  Store s = store();
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 10), false, &s, 1000, d.filter_gen());
  const GridGeo g = geo();
  REQUIRE(g.cols == 4);
  d.nav(-1, 0, g);
  CHECK(d.slot().cursor == 3);  // h wraps within the row.
  d.nav(1, 0, g);
  CHECK(d.slot().cursor == 0);  // l wraps back.
  d.nav(0, 2, g);
  CHECK(d.slot().cursor == 8);  // j moves card-rows.
  d.nav(1, 0, g);
  CHECK(d.slot().cursor == 9);
  d.nav(1, 0, g);
  CHECK(d.slot().cursor == 8);  // short last row wraps within itself.
  d.nav(0, 5, g);
  CHECK(d.slot().cursor == 9);  // j clamps to the last card.
}

TEST_CASE("cursor and scroll survive axis switches") {
  DiscoverState d;
  Store s = store();
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 40), true, &s, 1000, d.filter_gen());
  const GridGeo g = geo();
  for (int i = 0; i < 6; ++i) d.nav(0, 1, g);
  const std::size_t cursor = d.slot().cursor;
  const std::size_t scroll = d.slot().scroll_row;
  CHECK(scroll > 0);  // cursor motion scrolled the band.
  d.cycle_axis(1);
  CHECK(d.slot().cursor == 0);
  d.cycle_axis(-1);
  CHECK(d.slot().cursor == cursor);  // slot state preserved (DESIGN §8.6).
  CHECK(d.slot().scroll_row == scroll);
}

TEST_CASE("selected_entry follows the active slot") {
  DiscoverState d;
  Store s = store();
  CHECK(d.selected_entry() == nullptr);
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 4), false, &s, 1000, d.filter_gen());
  REQUIRE(d.selected_entry() != nullptr);
  CHECK(d.selected_entry()->anilist_id == 1);
  d.nav(1, 0, geo());
  REQUIRE(d.selected_entry() != nullptr);
  CHECK(d.selected_entry()->anilist_id == 2);
}

TEST_CASE("loading arm can be set and reset") {
  // Shigoku deviation from the Rust spawn_failure test: there is no drain
  // machinery (A1), so a spawn always launches. The arm-reset contract is
  // proven directly on arm_loading / disarm_loading (the tick pump resets the
  // arm only via disarm, and on_feed/on_feed_error clear it on a result).
  DiscoverState d;
  CHECK(!d.slot().loading);
  d.arm_loading(42);
  CHECK(d.slot().loading);
  CHECK(d.slot().loading_since == 42);
  d.disarm_loading();
  CHECK(!d.slot().loading);
  // A landed page clears loading too.
  d.arm_loading(1);
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 2), true, nullptr, 0, d.filter_gen());
  CHECK(!d.slot().loading);
}

TEST_CASE("axis switch re-arms only the target slot's failed state") {
  DiscoverState d;
  d.on_feed_error(DiscoverAxis::Trending, "offline", d.filter_gen());
  // select the Popular axis directly: it was never failed, and switching does
  // NOT clear Trending's sticky failure (only the target slot is re-armed).
  d.select_axis(1);
  CHECK(!d.slot().failed.has_value());
  CHECK(d.slot_for(DiscoverAxis::Trending).failed.has_value());
}

// ===========================================================================
// P38 (§9 — no Rust precedent): DiscoverState::set_filters. Filters are
// GLOBAL (unlike axis switches, which re-arm only the target slot) — every
// axis slot was fetched under the old filter set, so a filter change must
// invalidate all four, not just the one on screen.
// ===========================================================================

TEST_CASE("set_filters starts empty and composes into the accessor") {
  DiscoverState d;
  CHECK(d.filters().empty());
  DiscoverFilters f;
  f.status = "RELEASING";
  d.set_filters(f);
  CHECK(d.filters().status == "RELEASING");
}

TEST_CASE("set_filters re-arms every slot, not just the active one") {
  DiscoverState d;
  Store s = store();
  // Fill all four slots with real feed state (entries + a cursor position).
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  d.select_axis(1);
  d.on_feed(DiscoverAxis::Popular, 1, entries(100, 5), false, &s, 1000, d.filter_gen());  // exhausted.
  d.select_axis(2);
  d.on_feed_error(DiscoverAxis::TopRated, "offline", d.filter_gen());  // sticky failure.
  d.select_axis(0);  // back to Trending, the active slot at filter-change time.
  d.nav(1, 0, geo());  // move the cursor off 0 so the reset is observable.
  REQUIRE(d.slot().cursor != 0);

  DiscoverFilters f;
  f.genres = {"Action"};
  d.set_filters(f);

  CHECK(d.filters().genres == std::vector<std::string>{"Action"});
  // Active slot (Trending): entries/cursor/page all reset to a fresh slot.
  CHECK(d.slot().entries.empty());
  CHECK(d.slot().cursor == 0);
  CHECK(d.slot().page == 0);
  // Off-screen slots also reset: Popular's exhaustion and TopRated's sticky
  // failure both clear, because both were fetched under the OLD filters and
  // would otherwise render stale rows if switched to.
  CHECK(!d.slot_for(DiscoverAxis::Popular).exhausted);
  CHECK(d.slot_for(DiscoverAxis::Popular).entries.empty());
  CHECK(!d.slot_for(DiscoverAxis::TopRated).failed.has_value());
  // The axis selection itself is untouched by a filter change.
  CHECK(d.axis() == DiscoverAxis::Trending);
}

TEST_CASE("set_filters leaves pagination/exhaustion machinery itself unchanged") {
  // The positional on_feed contract (page == slot.page+1, MAX_FEED_ROWS
  // exhaustion) must still work exactly the same after a filter change —
  // P38 introduces no new staleness/generation mechanism.
  DiscoverState d;
  Store s = store();
  DiscoverFilters f;
  f.min_score = 50;
  d.set_filters(f);
  CHECK(d.wanted_fetch(geo()) == std::uint32_t{1});  // empty slot -> page 1, same as ever.
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 20);
  // Stale/duplicate/out-of-order pages still self-discard post-filter-change.
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 20);
  d.on_feed(DiscoverAxis::Trending, 3, entries(41, 20), true, &s, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 20);  // page 3 before 2 dropped.
}

TEST_CASE("set_filters invalidates in-flight old-filter answers (generation gate)") {
  // P38 review F7: the positional page check only orders pages WITHIN one
  // filter set — an unfiltered page still in flight when the user commits a
  // filter would otherwise file as the new set's perfectly-positioned page 1.
  DiscoverState d;
  Store s = store();
  const std::uint32_t old_gen = d.filter_gen();

  DiscoverFilters f;
  f.genres = {"Action"};
  d.set_filters(f);

  // The old fetch answers late: right axis, right page, stale generation.
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, old_gen);
  CHECK(d.slot().entries.empty());  // discarded whole.
  CHECK(d.slot().page == 0);

  // Its error twin is discarded too — a stale failure must not poison the
  // freshly re-armed slot with a sticky `failed`.
  d.on_feed_error(DiscoverAxis::Trending, "offline", old_gen);
  CHECK(!d.slot().failed.has_value());

  // The new generation's own page files normally.
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  CHECK(d.slot().entries.size() == 20);
  CHECK(d.slot().page == 1);
}

TEST_CASE("set_filters with an identical set is a no-op") {
  // P38 review F9: Enter on an unchanged draft must not wipe every slot's
  // pages/scroll and burn four page-1 refetches.
  DiscoverState d;
  Store s = store();
  DiscoverFilters f;
  f.year = 2020;
  d.set_filters(f);
  d.on_feed(DiscoverAxis::Trending, 1, entries(1, 20), true, &s, 1000, d.filter_gen());
  const std::uint32_t gen = d.filter_gen();

  d.set_filters(f);  // identical: nothing moves.
  CHECK(d.filter_gen() == gen);
  CHECK(d.slot().entries.size() == 20);
  CHECK(d.slot().page == 1);
}

TEST_CASE("DiscoverFilters::empty is true only with every field unset") {
  CHECK(DiscoverFilters{}.empty());
  DiscoverFilters f;
  f.genres.push_back("Action");
  CHECK(!f.empty());
  f = DiscoverFilters{};
  f.year = 2020;
  CHECK(!f.empty());
  f = DiscoverFilters{};
  f.status = "FINISHED";
  CHECK(!f.empty());
  f = DiscoverFilters{};
  f.min_score = 50;
  CHECK(!f.empty());
}

TEST_CASE("genre glyphs map the first two known genres in AniList order") {
  const std::vector<std::string> genres = {"Isekai", "Action", "Drama", "Comedy"};
  const auto g = genre_glyphs(genres);
  REQUIRE(g.size() == 2);
  CHECK(std::string(g[0]) == "\xE2\x9A\x94");  // ⚔ (Action).
  CHECK(std::string(g[1]) == "\xE2\x97\x86");  // ◆ (Drama).
  CHECK(genre_glyphs({"Unknown"}).empty());
}

TEST_CASE("genre glyph table matches DESIGN §3.8a (18 BMP entries)") {
  const auto& table = genre_glyphs_table();
  CHECK(table.size() == 18);
  for (const auto& [name, glyph] : table) {
    CHECK(std::string(name).size() > 0);
    CHECK(std::string(glyph).size() > 0);
  }
}

TEST_CASE("format labels map and reject unknown") {
  auto label = [](std::string_view k) -> std::string {
    const auto r = format_label(k);
    return r.has_value() ? std::string(*r) : std::string("<none>");
  };
  CHECK(label("TV") == "TV");
  CHECK(label("TV_SHORT") == "TV");
  CHECK(label("SPECIAL") == "Spec");
  CHECK(!format_label(std::string_view("VHS")).has_value());
  CHECK(!format_label(std::nullopt).has_value());
}

TEST_CASE("axis labels and index round-trip the freeze order") {
  CHECK(std::string(axis_label(DiscoverAxis::Trending)) == "Trending");
  CHECK(std::string(axis_label(DiscoverAxis::Popular)) == "Popular");
  CHECK(std::string(axis_label(DiscoverAxis::TopRated)) == "Top Rated");
  CHECK(std::string(axis_label(DiscoverAxis::ThisSeason)) == "This Season");
  CHECK(axis_index(DiscoverAxis::Trending) == 0);
  CHECK(axis_index(DiscoverAxis::ThisSeason) == 3);
}

TEST_CASE("is_new_this_cour flags current-cour off This Season only") {
  Enrichment e;
  e.season = Season::Summer;
  e.year = 2026;
  const Cour cour{Season::Summer, 2026};
  CHECK(is_new_this_cour(e, DiscoverAxis::Trending, cour));
  CHECK(!is_new_this_cour(e, DiscoverAxis::ThisSeason, cour));  // suppressed there.
  e.year = 2025;
  CHECK(!is_new_this_cour(e, DiscoverAxis::Trending, cour));  // wrong year.
}
