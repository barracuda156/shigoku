// mal_mirror_tests.cpp — P31 §9.1 slice 4 MAL mirror push tests.
//
// No sabigoku golden reference (MAL mirror is shigoku-only, PORT_PARITY.md
// §9 P31). Offline: MalMirrorClient is a scripted fake (FakeMal) — no
// socket. Mirrors sync_tests.cpp's FakeAni/RaceAni shape, scaled down to
// push_mirror's simpler no-pull, no-spacing loop.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <deque>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../src/auth.hpp"
#include "../src/domain.hpp"
#include "../src/mal_mirror.hpp"
#include "../src/store.hpp"

using namespace shigoku;
using namespace shigoku::mal_mirror;

namespace {

using GuardAnswer = Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>,
                           ProviderError>;
using SaveAnswer = Result<Unit, ProviderError>;

// Scripted get/put answers, queued per call (sync_tests.cpp's FakeAni
// precedent). Recording members are `mutable`: MalMirrorClient's methods are
// const.
class FakeMal final : public MalMirrorClient {
 public:
  explicit FakeMal(std::vector<SaveAnswer> saves) : saves_(saves.begin(), saves.end()) {}

  FakeMal& with_guards(std::vector<GuardAnswer> guards) {
    guards_.assign(guards.begin(), guards.end());
    return *this;
  }

  [[nodiscard]] Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>,
                      ProviderError>
  fetch_entry(std::string_view, std::int64_t mal_id) const override {
    guard_calls.push_back(mal_id);
    if (guards_.empty()) return std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>{};
    auto answer = guards_.front();
    guards_.pop_front();
    return answer;
  }

  [[nodiscard]] Result<Unit, ProviderError> save_entry(
      std::string_view, std::int64_t mal_id, ListStatus status, std::uint32_t progress,
      std::optional<std::uint32_t> score) const override {
    push_calls.emplace_back(mal_id, status, progress, score);
    if (saves_.empty()) return Unit{};
    auto answer = saves_.front();
    saves_.pop_front();
    return answer;
  }

  mutable std::vector<std::int64_t> guard_calls;
  mutable std::vector<
      std::tuple<std::int64_t, ListStatus, std::uint32_t, std::optional<std::uint32_t>>>
      push_calls;

 private:
  mutable std::deque<SaveAnswer> saves_;
  mutable std::deque<GuardAnswer> guards_;
};

MalAuth connected_mal() {
  MalAuth a;
  a.access_token = "mal-token-value";
  a.user_id = 9;
  return a;
}

// Library row with a linked mal_id, dirty against the mirror's own snapshot
// (never mirrored yet).
void dirty_lib_mal(Store& store, std::int64_t anilist_id, std::int64_t mal_id) {
  Enrichment e;
  e.anilist_id = anilist_id;
  e.mal_id = mal_id;
  e.title_romaji = "Show " + std::to_string(anilist_id);
  REQUIRE(store.add_to_library(e, 100).has_value());
}

// A library row with NO mal_id: must never surface in the mirror's dirty set.
void dirty_lib_no_mal(Store& store, std::int64_t anilist_id) {
  Enrichment e;
  e.anilist_id = anilist_id;
  e.title_romaji = "No MAL " + std::to_string(anilist_id);
  REQUIRE(store.add_to_library(e, 100).has_value());
}

ProviderError http_401() { return ProviderError::http(401); }
ProviderError network_error() { return ProviderError::network(); }

}  // namespace

TEST_CASE("disabled_is_a_noop") {
  FakeMal client({});
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  auto out = push_mirror(client, connected_mal(), *store, false);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Disabled);
  CHECK(client.guard_calls.empty());
  CHECK(client.push_calls.empty());
}

TEST_CASE("no_bearer_short_circuits_before_any_call") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 1, 501);
  FakeMal client({});
  auto out = push_mirror(client, MalAuth{}, *store, true);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Disabled);
  CHECK(client.guard_calls.empty());
}

TEST_CASE("null_mal_id_rows_never_enter_the_dirty_set") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_no_mal(*store, 2);
  dirty_lib_mal(*store, 3, 503);
  FakeMal client({Unit{}});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(out->dirty == 1);
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(std::get<0>(client.push_calls[0]) == 503);
}

TEST_CASE("push_advances_the_mal_snapshot_and_retires_the_row") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 10, 610);
  FakeMal client({Unit{}});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  // Unscored row: the score field is WITHHELD (nullopt), never sent as 0 —
  // MAL reads score=0 as "remove the user's score" (mal.hpp).
  CHECK(client.push_calls[0] ==
        std::make_tuple(std::int64_t{610}, ListStatus::Planning, std::uint32_t{0},
                        std::optional<std::uint32_t>{}));

  auto still_dirty = store->list_dirty_for_mal_mirror();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->empty());
}

TEST_CASE("push_401_stops_the_run_and_leaves_rows_dirty") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 11, 611);
  dirty_lib_mal(*store, 12, 612);
  FakeMal client({err(http_401()), Unit{}});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Unauthorized);
  CHECK(out->pushed == 0);
  CHECK(out->dirty == 2);
  CHECK(client.push_calls.size() == 1);
  auto still_dirty = store->list_dirty_for_mal_mirror();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->size() == 2);
}

TEST_CASE("push_429_stops_the_run_and_leaves_the_row_dirty") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 13, 613);
  FakeMal client({err(ProviderError::rate_limited())});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::RateLimited);
  CHECK(out->pushed == 0);
  auto still_dirty = store->list_dirty_for_mal_mirror();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->size() == 1);
}

TEST_CASE("guard_401_stops_the_run_before_any_save") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 14, 614);
  FakeMal client({});
  client.with_guards({err(http_401())});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Unauthorized);
  CHECK(client.push_calls.empty());
}

TEST_CASE("guard_429_stops_the_run_before_any_save") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 15, 615);
  FakeMal client({});
  client.with_guards({err(ProviderError::rate_limited())});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::RateLimited);
  CHECK(client.push_calls.empty());
}

TEST_CASE("unverifiable_guard_is_not_pushed_blind") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 16, 616);
  FakeMal client({Unit{}});
  client.with_guards({err(network_error())});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->push_failed == 1);
  CHECK(out->pushed == 0);
  CHECK(client.push_calls.empty());
  auto still_dirty = store->list_dirty_for_mal_mirror();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->size() == 1);
}

// A row never mirrored before (synced == nullopt) is never held back even if
// MAL happens to already carry a live entry: there is no prior belief to have
// been invalidated.
TEST_CASE("first_ever_push_is_never_guard_skipped") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 17, 617);
  FakeMal client({Unit{}});
  client.with_guards({std::make_tuple(ListStatus::Completed, std::uint32_t{24}, std::uint32_t{0})});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);
  CHECK(out->push_skipped == 0);
}

// The mirror's own ROD-498-equivalent guard: MAL's live value has moved past
// what the mirror last believed it pushed, so the stale local write is held
// back rather than overwriting whatever changed it on MAL's side.
TEST_CASE("a_mal_side_edit_since_the_last_push_is_not_overwritten") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 18, 618);

  // First run: mirror it once so a snapshot exists.
  {
    FakeMal client({Unit{}});
    auto out = push_mirror(client, connected_mal(), *store, true);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
  }

  // The user now edits progress locally...
  REQUIRE(store->set_list_status(18, ListStatus::Watching, 100).has_value());
  // ...but MAL's live entry disagrees with the mirror's last-accepted
  // snapshot (Planning/0) — someone changed it out from under the mirror.
  FakeMal client({Unit{}});
  client.with_guards({std::make_tuple(ListStatus::Dropped, std::uint32_t{2}, std::uint32_t{0})});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->pushed == 0);
  CHECK(out->push_skipped == 1);
  CHECK(client.push_calls.empty());

  auto still_dirty = store->list_dirty_for_mal_mirror();
  REQUIRE(still_dirty.has_value());
  REQUIRE(still_dirty->size() == 1);
  CHECK((*still_dirty)[0].anilist_id == 18);
}

// A MAL-deleted entry (my_list_status simply absent) never reappears to clear
// the mirror's snapshot, so treating absence as a mismatch would strand the
// row dirty forever — nothing of theirs is on MAL to destroy, so the write
// goes through.
TEST_CASE("an_absent_mal_entry_does_not_block_the_push") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 19, 619);
  {
    FakeMal client({Unit{}});
    auto out = push_mirror(client, connected_mal(), *store, true);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
  }

  REQUIRE(store->set_list_status(19, ListStatus::Dropped, 0).has_value());
  FakeMal client({Unit{}});
  client.with_guards({std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>{}});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);
  CHECK(out->push_skipped == 0);
}

// user_score is the store's raw 0..=100; the mirror converts to MAL's native
// 0..=10 (round-half-up) before it ever reaches the client.
TEST_CASE("push_converts_raw_score_to_mals_0_to_10_scale") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 21, 621);
  REQUIRE(store->set_user_score(21, 75).has_value());  // round(75/10) = 8.
  FakeMal client({Unit{}});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(std::get<3>(client.push_calls[0]) == 8);
}

// The score's OWN guard: a status/progress-agreeing row whose MAL-live score
// has moved past the mirror's last-accepted score snapshot is still held
// back, even though the (status, progress) pair alone would have passed.
TEST_CASE("a_mal_side_score_edit_since_the_last_push_is_not_overwritten") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 22, 622);
  REQUIRE(store->set_user_score(22, 50).has_value());  // -> MAL score 5.

  // First run: mirror it once so both snapshots exist.
  {
    FakeMal client({Unit{}});
    auto out = push_mirror(client, connected_mal(), *store, true);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
  }

  // The local score changes again...
  REQUIRE(store->set_user_score(22, 80).has_value());  // -> MAL score 8.
  // ...but MAL's live entry disagrees with the mirror's last-accepted score
  // snapshot (5): status/progress still agree (Planning/0), so only the
  // score-moved branch is exercised.
  FakeMal client({Unit{}});
  client.with_guards({std::make_tuple(ListStatus::Planning, std::uint32_t{0}, std::uint32_t{3})});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->pushed == 0);
  CHECK(out->push_skipped == 1);
  CHECK(client.push_calls.empty());
}

// The wipe regression (P34 review): a locally-unscored row must never send
// score=0 — that is MAL's "remove the score" sentinel, and the first-ever
// push of any pre-P34 row would silently erase a rating the user set on the
// MAL site. The snapshot stays NULL (no belief), so MAL's own score never
// reads as "moved" on later pushes either.
TEST_CASE("an_unscored_push_withholds_the_score_and_never_wedges_on_mals_own") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 23, 623);

  // MAL-side already holds score 8 (set on the site); first push, no
  // snapshot -> guard waived, and the PUT must carry NO score field.
  {
    FakeMal client({Unit{}});
    client.with_guards({std::make_tuple(ListStatus::Planning, std::uint32_t{0}, std::uint32_t{8})});
    auto out = push_mirror(client, connected_mal(), *store, true);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
    REQUIRE(client.push_calls.size() == 1);
    CHECK(std::get<3>(client.push_calls[0]) == std::nullopt);
  }

  // A later local progress edit pushes again: the guard's score arm must
  // keep waiving MAL's 8 (snapshot NULL = no belief), not read it as moved.
  REQUIRE(store->set_list_status(23, ListStatus::Watching, 200).has_value());
  FakeMal client({Unit{}});
  client.with_guards({std::make_tuple(ListStatus::Planning, std::uint32_t{0}, std::uint32_t{8})});
  auto out = push_mirror(client, connected_mal(), *store, true);
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);
  CHECK(out->push_skipped == 0);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(std::get<3>(client.push_calls[0]) == std::nullopt);
}

// Clearing a local score settles the row without erasing MAL's copy: the
// push withholds the field, the snapshot drops to NULL, and the dirty
// predicate reads NULL-vs-unset as agreement.
TEST_CASE("clearing_a_local_score_settles_without_erasing_mals") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 24, 624);
  REQUIRE(store->set_user_score(24, 80).has_value());
  {
    FakeMal client({Unit{}});
    auto out = push_mirror(client, connected_mal(), *store, true);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
    CHECK(std::get<3>(client.push_calls[0]) == 8);
  }

  REQUIRE(store->set_user_score(24, std::nullopt).has_value());
  {
    FakeMal client({Unit{}});
    client.with_guards({std::make_tuple(ListStatus::Planning, std::uint32_t{0}, std::uint32_t{8})});
    auto out = push_mirror(client, connected_mal(), *store, true);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
    CHECK(std::get<3>(client.push_calls[0]) == std::nullopt);
  }

  auto still_dirty = store->list_dirty_for_mal_mirror();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->empty());
}

TEST_CASE("mal_mirror_snapshot_is_independent_of_the_anilist_sync_snapshot") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 20, 620);
  // Advance the AniList-sync snapshot only.
  REQUIRE(store->mark_synced(20, ListStatus::Planning, 0, 0).has_value());

  // The AniList snapshot now agrees with the live pair, but the MAL mirror's
  // OWN snapshot has never been set — the row must still be MAL-dirty.
  auto mal_dirty = store->list_dirty_for_mal_mirror();
  REQUIRE(mal_dirty.has_value());
  CHECK(mal_dirty->size() == 1);
  auto ani_dirty = store->list_dirty_for_sync();
  REQUIRE(ani_dirty.has_value());
  CHECK(ani_dirty->empty());
}
