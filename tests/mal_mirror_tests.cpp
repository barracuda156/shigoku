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
#include <utility>
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

  FakeMal& with_list(Result<std::vector<mal_catalog::UserListEntry>, ProviderError> list) {
    list_ = std::move(list);
    return *this;
  }

  // Per-call list answers, consumed in order; `with_list`'s single answer
  // serves once these run out.
  FakeMal& with_lists(
      std::vector<Result<std::vector<mal_catalog::UserListEntry>, ProviderError>> lists) {
    lists_.assign(lists.begin(), lists.end());
    return *this;
  }

  [[nodiscard]] Result<std::vector<mal_catalog::UserListEntry>, ProviderError> fetch_list(
      std::string_view token) const override {
    ++list_calls;
    list_tokens.emplace_back(token);
    if (!lists_.empty()) {
      auto answer = lists_.front();
      lists_.pop_front();
      return answer;
    }
    return list_;
  }

  mutable std::vector<std::int64_t> guard_calls;
  mutable std::vector<std::string> list_tokens;  // the bearer each fetch_list carried.
  mutable std::vector<
      std::tuple<std::int64_t, ListStatus, std::uint32_t, std::optional<std::uint32_t>>>
      push_calls;
  mutable int list_calls = 0;

 private:
  mutable std::deque<SaveAnswer> saves_;
  mutable std::deque<GuardAnswer> guards_;
  Result<std::vector<mal_catalog::UserListEntry>, ProviderError> list_ =
      std::vector<mal_catalog::UserListEntry>{};
  mutable std::deque<Result<std::vector<mal_catalog::UserListEntry>, ProviderError>> lists_;
};

using ListAnswer = Result<std::vector<mal_catalog::UserListEntry>, ProviderError>;

mal_catalog::UserListEntry list_entry(std::int64_t mal_id, std::int64_t anilist_id,
                                      std::string title, ListStatus status,
                                      std::uint32_t progress, std::uint32_t score) {
  mal_catalog::UserListEntry e;
  e.seed.anilist_id = anilist_id;
  e.seed.mal_id = mal_id;
  e.seed.title_romaji = std::move(title);
  e.seed.total_episodes = 12;
  e.status = status;
  e.progress = progress;
  e.score = score;
  return e;
}

MalAuth connected_mal() {
  MalAuth a;
  a.access_token = "mal-token-value";
  a.user_id = 9;
  return a;
}

// A pair whose access token is past its date, with a refresh_token to redeem.
constexpr std::int64_t kAfterExpiry = 200;
MalAuth expired_mal() {
  MalAuth a = connected_mal();
  a.refresh_token = "mal-refresh-value";
  a.expires_at = kAfterExpiry - 100;
  return a;
}

// What the token endpoint hands back for expired_mal()'s refresh_token.
MalAuth renewed_pair() {
  MalAuth a;
  a.access_token = "mal-token-renewed";
  a.refresh_token = "mal-refresh-renewed";
  a.expires_at = kAfterExpiry + 2'678'400;
  a.user_id = 9;
  return a;
}

// A scripted refresh seam: every call gets `answer`; the spy counts calls and
// keeps what it was handed.
struct RefreshSpy {
  explicit RefreshSpy(Refresh a) : answer(std::move(a)) {}

  Refresh answer;
  int calls = 0;
  std::string redeemed;
  std::int64_t at = -1;

  Refresher fn() {
    return [this](const MalAuth& current, std::int64_t now) {
      ++calls;
      redeemed = current.refresh_token;
      at = now;
      return answer;
    };
  }
};

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

constexpr std::int64_t kNow = 1000;

// MAL's live entry as the guard reads it; `score` on MAL's 0..=10.
GuardAnswer live(ListStatus status, std::uint32_t progress, std::uint32_t score) {
  return std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>(
      std::make_tuple(status, progress, score));
}

GuardAnswer no_entry() {
  return std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>{};
}

using PushCall =
    std::tuple<std::int64_t, ListStatus, std::uint32_t, std::optional<std::uint32_t>>;

// A library row the mirror once settled at (snap_status, snap_progress,
// snap_score on MAL's scale; nullopt = withheld), edited locally since to
// (status, progress, raw_score on the store's 0..=100).
void mirrored_row(Store& store, std::int64_t anilist_id, std::int64_t mal_id,
                  ListStatus snap_status, std::uint32_t snap_progress,
                  std::optional<std::uint32_t> snap_score, ListStatus status,
                  std::uint32_t progress, std::optional<std::uint32_t> raw_score) {
  dirty_lib_mal(store, anilist_id, mal_id);
  REQUIRE(store.mark_mal_synced(anilist_id, snap_status, snap_progress, snap_score).has_value());
  REQUIRE(store.restore_list_status(anilist_id, status, progress, 100).has_value());
  REQUIRE(store.set_user_score(anilist_id, raw_score).has_value());
}

// A never-mirrored library row at (status, progress, raw_score).
void fresh_row(Store& store, std::int64_t anilist_id, std::int64_t mal_id, ListStatus status,
               std::uint32_t progress, std::optional<std::uint32_t> raw_score) {
  dirty_lib_mal(store, anilist_id, mal_id);
  REQUIRE(store.restore_list_status(anilist_id, status, progress, 100).has_value());
  REQUIRE(store.set_user_score(anilist_id, raw_score).has_value());
}

Show show_of(Store& store, std::int64_t anilist_id) {
  auto g = store.get_show(anilist_id);
  REQUIRE(g.has_value());
  REQUIRE(g->has_value());
  return **g;
}

// The row's place in the mirror's work list, nullopt when it owes MAL nothing.
std::optional<MalMirrorRow> owed_row(Store& store, std::int64_t anilist_id) {
  auto rows = store.list_dirty_for_mal_mirror();
  REQUIRE(rows.has_value());
  for (const MalMirrorRow& r : *rows) {
    if (r.anilist_id == anilist_id) return r;
  }
  return std::nullopt;
}

// The mirror snapshot a clean row carries, read back by nudging the local
// progress one up so the row lists with it.
MalMirrorRow snapshot_of(Store& store, std::int64_t anilist_id) {
  REQUIRE_FALSE(owed_row(store, anilist_id).has_value());
  const Show s = show_of(store, anilist_id);
  REQUIRE(store.restore_list_status(anilist_id, s.list_status, s.progress + 1, 100).has_value());
  auto row = owed_row(store, anilist_id);
  REQUIRE(row.has_value());
  REQUIRE(store.restore_list_status(anilist_id, s.list_status, s.progress, 100).has_value());
  return *row;
}

}  // namespace

TEST_CASE("disabled_is_a_noop") {
  FakeMal client({});
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  auto out = push_mirror(client, connected_mal(), *store, false, kNow);
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
  auto out = push_mirror(client, MalAuth{}, *store, true, kNow);
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
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(out->dirty == 1);
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(std::get<0>(client.push_calls[0]) == 503);
}

// ── pull_mirror ─────────────────────────────────────────────────────────────

TEST_CASE("pull_disabled_or_signed_out_is_a_noop") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  FakeMal client({});
  auto off = pull_mirror(client, connected_mal(), *store, false, 0);
  REQUIRE(off.has_value());
  CHECK(off->outcome == MirrorOutcome::Disabled);
  auto out = pull_mirror(client, MalAuth{}, *store, true, 0);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Disabled);
  CHECK(client.list_calls == 0);
}

TEST_CASE("pull_imports_the_whole_list_every_status_with_the_mirror_snapshot_set") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  FakeMal client({});
  client.with_list(std::vector<mal_catalog::UserListEntry>{
      list_entry(610, 10, "A", ListStatus::Watching, 3, 8),
      list_entry(611, -611, "B", ListStatus::Completed, 12, 0),  // synthetic (no AniList id).
      list_entry(612, 12, "C", ListStatus::Planning, 0, 10),
  });
  auto out = pull_mirror(client, connected_mal(), *store, true, 50);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(!out->pull_failed);
  CHECK(out->pulled.imported == 3);
  CHECK(client.list_calls == 1);

  auto history = store->list_history();
  REQUIRE(history.has_value());
  CHECK(history->size() == 3);
  auto a = store->get_show(10);
  REQUIRE(a.has_value());
  REQUIRE(a->has_value());
  CHECK((*a)->list_status == ListStatus::Watching);
  CHECK((*a)->progress == 3);
  CHECK((*a)->user_score == 80);  // MAL's 8 on the store's raw scale.
  auto b = store->get_show(-611);
  REQUIRE(b.has_value());
  REQUIRE(b->has_value());
  CHECK((*b)->list_status == ListStatus::Completed);
  CHECK((*b)->progress == 12);
  auto c = store->get_show(12);
  REQUIRE(c.has_value());
  REQUIRE(c->has_value());
  CHECK((*c)->user_score == 100);
  // The mirror's own snapshot moved with the adopt: nothing to push back.
  CHECK(store->list_dirty_for_mal_mirror()->empty());
  // AniList never heard of these: the two real-id rows owe it a push, the
  // synthetic one never will.
  CHECK(store->list_dirty_for_sync()->size() == 2);
}

TEST_CASE("pull_401_is_unauthorized_429_rate_limited_and_a_transport_miss_is_pull_failed") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  {
    FakeMal client({});
    client.with_list(err(http_401()));
    auto out = pull_mirror(client, connected_mal(), *store, true, 0);
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Unauthorized);
  }
  {
    FakeMal client({});
    client.with_list(err(ProviderError::rate_limited()));
    auto out = pull_mirror(client, connected_mal(), *store, true, 0);
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::RateLimited);
  }
  {
    FakeMal client({});
    client.with_list(err(network_error()));
    auto out = pull_mirror(client, connected_mal(), *store, true, 0);
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Completed);
    CHECK(out->pull_failed);
    CHECK(out->pulled.imported == 0);
  }
}

TEST_CASE("pull_keeps_a_local_edit_made_since_the_last_mirror_push") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 20, 620);
  REQUIRE(store->mark_mal_synced(20, ListStatus::Planning, 0, std::nullopt).has_value());
  REQUIRE(store->restore_list_status(20, ListStatus::Watching, 5, 30).has_value());
  FakeMal client({});
  client.with_list(std::vector<mal_catalog::UserListEntry>{
      list_entry(620, 20, "Show 20", ListStatus::Completed, 12, 0)});
  auto out = pull_mirror(client, connected_mal(), *store, true, 50);
  REQUIRE(out.has_value());
  CHECK(out->pulled.reconciled == 1);
  CHECK(out->pulled.conflicts == 1);
  auto g = store->get_show(20);
  REQUIRE(g.has_value());
  REQUIRE(g->has_value());
  CHECK((*g)->list_status == ListStatus::Watching);  // local status kept...
  CHECK((*g)->progress == 12);  // ...progress merges upward, the AniList law's cell.
  CHECK(store->list_dirty_for_mal_mirror()->size() == 1);  // ...and pushes back up next.
}

TEST_CASE("pull_matches_a_library_row_by_mal_id_when_the_bridge_disagrees_and_backfills") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 30, 630);  // a real AniList id holding mal 630.
  dirty_lib_no_mal(*store, 40);    // a row AniList never linked to MAL.
  FakeMal client({});
  client.with_list(std::vector<mal_catalog::UserListEntry>{
      list_entry(630, -630, "Show 30", ListStatus::Watching, 4, 0),  // bridged synthetic.
      list_entry(640, 40, "No MAL 40", ListStatus::Completed, 12, 7),
  });
  auto out = pull_mirror(client, connected_mal(), *store, true, 50);
  REQUIRE(out.has_value());
  CHECK(out->pulled.imported == 0);
  CHECK(out->pulled.reconciled == 2);
  auto thirty = store->get_show(30);
  REQUIRE(thirty.has_value());
  REQUIRE(thirty->has_value());
  CHECK((*thirty)->list_status == ListStatus::Watching);
  CHECK((*thirty)->progress == 4);
  auto dup = store->get_show(-630);
  REQUIRE(dup.has_value());
  CHECK(!dup->has_value());  // no duplicate row under the synthetic id.
  auto forty = store->get_show(40);
  REQUIRE(forty.has_value());
  REQUIRE(forty->has_value());
  CHECK((*forty)->enrichment.mal_id == 640);
  CHECK((*forty)->user_score == 70);
  CHECK(store->list_dirty_for_mal_mirror()->empty());
}

TEST_CASE("push_advances_the_mal_snapshot_and_retires_the_row") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 10, 610);
  FakeMal client({Unit{}});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
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
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
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
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
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
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
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
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
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
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->push_failed == 1);
  CHECK(out->pushed == 0);
  CHECK(out->unsent() == 1);
  CHECK(out->failures ==
        std::vector<std::string>{"myanimelist.net/anime/616: entry read failed, no answer"});
  CHECK(client.push_calls.empty());
  auto still_dirty = store->list_dirty_for_mal_mirror();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->size() == 1);
}

TEST_CASE("a_failed_row_is_noted_and_the_run_goes_on") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 41, 641);
  dirty_lib_mal(*store, 42, 642);
  dirty_lib_mal(*store, 43, 643);
  // 641's entry read is refused, 642's save fails, 643 goes.
  FakeMal client({err(ProviderError::from_status(500)), Unit{}});
  client.with_guards({err(ProviderError::from_status(403)), no_entry(), no_entry()});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(out->dirty == 3);
  CHECK(out->push_failed == 2);
  CHECK(out->pushed == 1);
  CHECK(out->unsent() == 2);
  CHECK(client.guard_calls == std::vector<std::int64_t>{641, 642, 643});
  REQUIRE(client.push_calls.size() == 2);
  CHECK(std::get<0>(client.push_calls[1]) == 643);
  CHECK(out->failures == std::vector<std::string>{
                             "myanimelist.net/anime/641: entry read failed, http 403",
                             "myanimelist.net/anime/642: save failed, http 500"});
  CHECK(owed_row(*store, 41).has_value());
  CHECK(owed_row(*store, 42).has_value());
  CHECK_FALSE(owed_row(*store, 43).has_value());
}

TEST_CASE("failure_notes_stop_at_the_cap_while_the_count_goes_on") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  const std::size_t rows = kFailureNotes + 3;
  std::vector<SaveAnswer> saves;
  for (std::size_t i = 0; i < rows; ++i) {
    dirty_lib_mal(*store, 100 + static_cast<std::int64_t>(i), 700 + static_cast<std::int64_t>(i));
    saves.push_back(err(ProviderError::from_status(500)));
  }
  FakeMal client(saves);
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->push_failed == rows);
  CHECK(out->failures.size() == kFailureNotes);
  CHECK(out->failures.front() == "myanimelist.net/anime/700: save failed, http 500");
  CHECK(out->unsent() == rows);
}

TEST_CASE("a_stop_mid_run_counts_the_rows_it_left_behind") {
  {
    auto store = Store::open_memory();
    REQUIRE(store.has_value());
    dirty_lib_mal(*store, 44, 644);
    dirty_lib_mal(*store, 45, 645);
    dirty_lib_mal(*store, 46, 646);
    FakeMal client({Unit{}});
    client.with_guards({no_entry(), err(http_401())});
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Unauthorized);
    CHECK(out->pushed == 1);
    CHECK(out->push_failed == 0);
    CHECK(out->unsent() == 2);
    CHECK(out->failures.empty());  // the stop is the outcome, not a failed row.
    CHECK(client.guard_calls == std::vector<std::int64_t>{644, 645});
    CHECK(client.push_calls.size() == 1);
    CHECK(owed_row(*store, 45).has_value());
    CHECK(owed_row(*store, 46).has_value());
  }
  {
    auto store = Store::open_memory();
    REQUIRE(store.has_value());
    dirty_lib_mal(*store, 47, 647);
    dirty_lib_mal(*store, 48, 648);
    FakeMal client({err(ProviderError::rate_limited())});
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::RateLimited);
    CHECK(out->pushed == 0);
    CHECK(out->push_failed == 0);
    CHECK(out->unsent() == 2);
    CHECK(out->failures.empty());
    CHECK(client.guard_calls == std::vector<std::int64_t>{647});
  }
  {
    // A row that failed before the stop keeps its note; the stop row adds none.
    auto store = Store::open_memory();
    REQUIRE(store.has_value());
    dirty_lib_mal(*store, 91, 691);
    dirty_lib_mal(*store, 92, 692);
    dirty_lib_mal(*store, 93, 693);
    FakeMal client({});
    client.with_guards({err(ProviderError::from_status(500)), err(http_401())});
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Unauthorized);
    CHECK(out->push_failed == 1);
    CHECK(out->unsent() == 3);
    CHECK(out->failures ==
          std::vector<std::string>{"myanimelist.net/anime/691: entry read failed, http 500"});
    CHECK(client.push_calls.empty());
  }
}

// MAL's grid: a local 75 went to MAL as 8, so the snapshot's 8 is what the
// local score already says. A score changed on MAL's side is taken, not
// overwritten by the off-grid local value.
TEST_CASE("a_score_changed_on_mal_wins_over_an_off_grid_local_score") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  mirrored_row(*store, 35, 635, ListStatus::Watching, 3, 8, ListStatus::Watching, 4, 75);
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Watching, 3, 6)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 1);
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{635, ListStatus::Watching, 4, 6});
  CHECK(show_of(*store, 35).user_score == std::optional<std::uint32_t>(60));
  CHECK_FALSE(owed_row(*store, 35).has_value());
}

TEST_CASE("an_off_grid_local_score_mal_still_holds_keeps_its_value_and_a_re_score_wins") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  // Re-scored here from 75 to 90 since MAL took the 8.
  mirrored_row(*store, 36, 636, ListStatus::Watching, 3, 8, ListStatus::Watching, 3, 90);
  // Still 75 here, MAL still 8; only the progress moved, on both sides.
  mirrored_row(*store, 37, 637, ListStatus::Watching, 3, 8, ListStatus::Watching, 4, 75);
  FakeMal client({Unit{}, Unit{}});
  client.with_guards({live(ListStatus::Watching, 5, 8), live(ListStatus::Watching, 5, 8)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 2);
  CHECK(out->pushed == 1);
  CHECK(out->settled == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{636, ListStatus::Watching, 5, 9});
  CHECK(show_of(*store, 36).user_score == std::optional<std::uint32_t>(90));
  const Show kept = show_of(*store, 37);
  CHECK(kept.progress == 5);
  CHECK(kept.user_score == std::optional<std::uint32_t>(75));
  CHECK_FALSE(owed_row(*store, 36).has_value());
  CHECK_FALSE(owed_row(*store, 37).has_value());
}

// A row never mirrored before meets MAL's entry as first contact: the merge
// takes what MAL holds for every field the local row never moved.
TEST_CASE("a_never_mirrored_row_merges_with_mals_entry_instead_of_resetting_it") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 17, 617);  // Planning/0, unscored.
  FakeMal client({Unit{}});
  // Pushing first would have reset this entry to plan_to_watch 0 and no score.
  client.with_guards({live(ListStatus::Completed, 12, 9)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(out->dirty == 1);
  CHECK(out->merged == 1);
  CHECK(out->settled == 1);
  CHECK(out->pushed == 0);
  CHECK(out->push_skipped == 0);
  CHECK(out->unsent() == 0);
  CHECK(client.push_calls.empty());

  const Show s = show_of(*store, 17);
  CHECK(s.list_status == ListStatus::Completed);
  CHECK(s.progress == 12);
  CHECK(s.user_score == std::optional<std::uint32_t>(90));
  CHECK_FALSE(owed_row(*store, 17).has_value());
}

TEST_CASE("a_never_mirrored_row_ahead_of_mal_sends_its_own_progress") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  fresh_row(*store, 25, 625, ListStatus::Watching, 5, std::nullopt);
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Watching, 3, 0)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 1);
  CHECK(out->pushed == 1);
  CHECK(out->settled == 0);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{625, ListStatus::Watching, 5, std::nullopt});
  CHECK(show_of(*store, 25).progress == 5);
  CHECK_FALSE(owed_row(*store, 25).has_value());
}

TEST_CASE("a_never_mirrored_row_mal_already_agrees_with_settles_without_a_call") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  fresh_row(*store, 26, 626, ListStatus::Watching, 5, std::nullopt);
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Watching, 5, 0)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 1);
  CHECK(out->settled == 1);
  CHECK(out->pushed == 0);
  CHECK(client.push_calls.empty());
  CHECK_FALSE(owed_row(*store, 26).has_value());
  // The snapshot now holds MAL's entry, its unscored 0 as a belief.
  const MalMirrorRow snap = snapshot_of(*store, 26);
  CHECK(snap.synced == std::make_pair(ListStatus::Watching, std::uint32_t{5}));
  CHECK(snap.synced_score == std::optional<std::uint32_t>(0));
}

// MAL's live entry moved past what the mirror last pushed while the local row
// moved too: the two are merged first, a status both sides changed keeps the
// local one, and the merge is what goes.
TEST_CASE("a_mal_side_edit_since_the_last_push_is_merged_before_the_push") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 18, 618);

  // First run: mirror it once so a snapshot exists.
  {
    FakeMal client({Unit{}});
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
  }

  // The user now edits the status locally...
  REQUIRE(store->set_list_status(18, ListStatus::Watching, 100).has_value());
  // ...and MAL's live entry has moved off the snapshot (Planning/0) too.
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Dropped, 2, 0)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 1);
  CHECK(out->pushed == 1);
  CHECK(out->push_skipped == 0);
  CHECK(out->settled == 0);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{618, ListStatus::Watching, 2, std::nullopt});

  const Show s = show_of(*store, 18);
  CHECK(s.list_status == ListStatus::Watching);  // local status kept...
  CHECK(s.progress == 2);                         // ...MAL's progress taken.
  CHECK_FALSE(owed_row(*store, 18).has_value());
}

TEST_CASE("progress_moved_on_both_sides_sends_the_higher_and_rebaselines") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  mirrored_row(*store, 30, 630, ListStatus::Watching, 6, std::nullopt, ListStatus::Watching, 8,
               std::nullopt);
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Watching, 7, 0)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(out->dirty == 1);
  CHECK(out->merged == 1);
  CHECK(out->pushed == 1);
  CHECK(out->settled == 0);
  CHECK(out->unsent() == 0);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{630, ListStatus::Watching, 8, std::nullopt});
  CHECK(show_of(*store, 30).progress == 8);
  const MalMirrorRow snap = snapshot_of(*store, 30);
  CHECK(snap.synced == std::make_pair(ListStatus::Watching, std::uint32_t{8}));
  CHECK_FALSE(snap.synced_score.has_value());  // the push withheld the score.
}

TEST_CASE("a_merge_that_leaves_nothing_to_send_settles_on_mals_entry") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  // Paused here, and on MAL too, where the progress and a score moved on.
  mirrored_row(*store, 31, 631, ListStatus::Watching, 4, std::nullopt, ListStatus::Paused, 4,
               std::nullopt);
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Paused, 6, 7)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 1);
  CHECK(out->settled == 1);
  CHECK(out->pushed == 0);
  CHECK(out->unsent() == 0);
  CHECK(client.push_calls.empty());

  const Show s = show_of(*store, 31);
  CHECK(s.list_status == ListStatus::Paused);
  CHECK(s.progress == 6);
  CHECK(s.user_score == std::optional<std::uint32_t>(70));
  CHECK_FALSE(owed_row(*store, 31).has_value());
}

// A merged row whose save then fails keeps the merge locally and a snapshot
// of MAL's entry, so the next run sends it straight off.
TEST_CASE("a_merged_row_whose_save_fails_goes_unmerged_next_run") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  mirrored_row(*store, 32, 632, ListStatus::Watching, 6, std::nullopt, ListStatus::Watching, 8,
               std::nullopt);
  {
    FakeMal client({err(ProviderError::from_status(500))});
    client.with_guards({live(ListStatus::Watching, 7, 0)});
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Completed);
    CHECK(out->merged == 1);
    CHECK(out->push_failed == 1);
    CHECK(out->pushed == 0);
    CHECK(out->unsent() == 1);
    CHECK(out->failures ==
          std::vector<std::string>{"myanimelist.net/anime/632: save failed, http 500"});
  }
  const auto owed = owed_row(*store, 32);
  REQUIRE(owed.has_value());
  CHECK(owed->progress == 8);
  CHECK(owed->synced == std::make_pair(ListStatus::Watching, std::uint32_t{7}));

  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Watching, 7, 0)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 0);
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{632, ListStatus::Watching, 8, std::nullopt});
  CHECK_FALSE(owed_row(*store, 32).has_value());
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
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
  }

  REQUIRE(store->set_list_status(19, ListStatus::Dropped, 0).has_value());
  FakeMal client({Unit{}});
  client.with_guards({no_entry()});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);
  CHECK(out->merged == 0);
  CHECK(out->settled == 0);
  CHECK(out->push_skipped == 0);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{619, ListStatus::Dropped, 0, std::nullopt});
  CHECK_FALSE(owed_row(*store, 19).has_value());
}

// user_score is the store's raw 0..=100; the mirror converts to MAL's native
// 0..=10 (round-half-up) before it ever reaches the client.
TEST_CASE("push_converts_raw_score_to_mals_0_to_10_scale") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 21, 621);
  REQUIRE(store->set_user_score(21, 75).has_value());  // round(75/10) = 8.
  FakeMal client({Unit{}});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(std::get<3>(client.push_calls[0]) == 8);
}

// The score's OWN guard: a status/progress-agreeing row whose MAL-live score
// has moved past the mirror's last-accepted score snapshot is merged, even
// though the (status, progress) pair alone would have passed; a score both
// sides changed keeps the local one.
TEST_CASE("a_mal_side_score_edit_since_the_last_push_is_merged_before_the_push") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib_mal(*store, 22, 622);
  REQUIRE(store->set_user_score(22, 50).has_value());  // -> MAL score 5.

  // First run: mirror it once so both snapshots exist.
  {
    FakeMal client({Unit{}});
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
  }

  // The local score changes again...
  REQUIRE(store->set_user_score(22, 80).has_value());  // -> MAL score 8.
  // ...but MAL's live entry disagrees with the mirror's last-accepted score
  // snapshot (5): status/progress still agree (Planning/0), so only the
  // score-moved branch is exercised.
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Planning, 0, 3)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 1);
  CHECK(out->pushed == 1);
  CHECK(out->push_skipped == 0);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{622, ListStatus::Planning, 0, 8});
  CHECK(show_of(*store, 22).user_score == std::optional<std::uint32_t>(80));
  CHECK(snapshot_of(*store, 22).synced_score == std::optional<std::uint32_t>(8));
}

// The score arm on its own: MAL's score moving off the snapshot's merges the
// row although MAL's pair still matches it, and a local score that never
// moved takes MAL's.
TEST_CASE("a_mal_side_score_edit_alone_is_adopted_on_the_next_local_change") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  mirrored_row(*store, 27, 627, ListStatus::Watching, 3, 5, ListStatus::Watching, 4, 50);
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Watching, 3, 9)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 1);
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{627, ListStatus::Watching, 4, 9});
  CHECK(show_of(*store, 27).user_score == std::optional<std::uint32_t>(90));
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

  // No entry on MAL yet: the first push goes as is, with NO score field.
  {
    FakeMal client({Unit{}});
    client.with_guards({no_entry()});
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
    CHECK(out->merged == 0);
    REQUIRE(client.push_calls.size() == 1);
    CHECK(std::get<3>(client.push_calls[0]) == std::nullopt);
  }

  // The user rates it 8 on the site, then edits locally. The pair still
  // matches the snapshot and the snapshot holds no score belief, so the
  // score alone triggers no merge, and the push still withholds it.
  REQUIRE(store->set_list_status(23, ListStatus::Watching, 200).has_value());
  FakeMal client({Unit{}});
  client.with_guards({live(ListStatus::Planning, 0, 8)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 0);
  CHECK(out->pushed == 1);
  CHECK(out->push_skipped == 0);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == PushCall{623, ListStatus::Watching, 0, std::nullopt});
  CHECK_FALSE(show_of(*store, 23).user_score.has_value());
  CHECK_FALSE(owed_row(*store, 23).has_value());
}

// A merge never turns a locally-unscored row into a score=0 send.
TEST_CASE("a_merged_unscored_row_still_withholds_the_score") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  // MAL unscored as well: nothing to adopt, the score stays withheld.
  mirrored_row(*store, 33, 633, ListStatus::Watching, 3, std::nullopt, ListStatus::Watching, 5,
               std::nullopt);
  // The local score was cleared since the mirror sent a 6 that MAL still holds.
  mirrored_row(*store, 34, 634, ListStatus::Watching, 3, 6, ListStatus::Watching, 5,
               std::nullopt);
  FakeMal client({Unit{}, Unit{}});
  client.with_guards({live(ListStatus::Watching, 4, 0), live(ListStatus::Watching, 4, 6)});
  auto out = push_mirror(client, connected_mal(), *store, true, kNow);
  REQUIRE(out.has_value());
  CHECK(out->merged == 2);
  CHECK(out->pushed == 2);
  REQUIRE(client.guard_calls == std::vector<std::int64_t>{633, 634});
  REQUIRE(client.push_calls.size() == 2);
  CHECK(client.push_calls[0] == PushCall{633, ListStatus::Watching, 5, std::nullopt});
  CHECK(client.push_calls[1] == PushCall{634, ListStatus::Watching, 5, std::nullopt});
  CHECK(show_of(*store, 33).user_score.value_or(0) == 0);
  CHECK(show_of(*store, 34).user_score.value_or(0) == 0);
  CHECK_FALSE(owed_row(*store, 33).has_value());
  CHECK_FALSE(owed_row(*store, 34).has_value());
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
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
    REQUIRE(out.has_value());
    CHECK(out->pushed == 1);
    CHECK(std::get<3>(client.push_calls[0]) == 8);
  }

  REQUIRE(store->set_user_score(24, std::nullopt).has_value());
  {
    FakeMal client({Unit{}});
    client.with_guards({std::make_tuple(ListStatus::Planning, std::uint32_t{0}, std::uint32_t{8})});
    auto out = push_mirror(client, connected_mal(), *store, true, kNow);
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

// ── pull_mirror: token renewal ──────────────────────────────────────────────

TEST_CASE("pull_renews_a_token_past_its_date_before_asking_for_the_list") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  FakeMal client({});
  client.with_list(std::vector<mal_catalog::UserListEntry>{
      list_entry(610, 10, "A", ListStatus::Watching, 3, 8)});
  RefreshSpy spy{Refresh::rolled(renewed_pair())};
  auto out = pull_mirror(client, expired_mal(), *store, true, kAfterExpiry, spy.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(!out->pull_failed);
  CHECK(out->pulled.imported == 1);
  CHECK(spy.calls == 1);
  CHECK(spy.redeemed == "mal-refresh-value");
  CHECK(spy.at == kAfterExpiry);
  // The list went out under the renewed bearer, and the pair comes back for
  // the caller to save.
  REQUIRE(client.list_tokens.size() == 1);
  CHECK(client.list_tokens[0] == "mal-token-renewed");
  REQUIRE(out->rolled.has_value());
  CHECK(*out->rolled == renewed_pair());
}

TEST_CASE("pull_leaves_a_live_token_alone_and_without_a_seam_an_expired_one_goes_out_as_is") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  {
    FakeMal client({});
    RefreshSpy spy{Refresh::rolled(renewed_pair())};
    // Undated, and dated but not reached: no roll either way.
    auto undated = pull_mirror(client, connected_mal(), *store, true, kAfterExpiry, spy.fn());
    REQUIRE(undated.has_value());
    MalAuth dated = expired_mal();
    dated.expires_at = kAfterExpiry + 1;
    auto live = pull_mirror(client, dated, *store, true, kAfterExpiry, spy.fn());
    REQUIRE(live.has_value());
    CHECK(spy.calls == 0);
    CHECK(!undated->rolled.has_value());
    CHECK(!live->rolled.has_value());
    REQUIRE(client.list_tokens.size() == 2);
    CHECK(client.list_tokens[1] == "mal-token-value");
  }
  {
    FakeMal client({});
    client.with_list(ListAnswer(err(http_401())));
    auto out = pull_mirror(client, expired_mal(), *store, true, kAfterExpiry);
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Unauthorized);
    CHECK(!out->rolled.has_value());
    CHECK(client.list_calls == 1);
  }
}

TEST_CASE("pull_renews_after_a_401_and_asks_once_more_under_the_new_bearer") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  FakeMal client({});
  client.with_lists({ListAnswer(err(http_401())),
                     ListAnswer(std::vector<mal_catalog::UserListEntry>{
                         list_entry(610, 10, "A", ListStatus::Watching, 3, 8)})});
  RefreshSpy spy{Refresh::rolled(renewed_pair())};
  MalAuth live = connected_mal();  // undated: MAL retired it early.
  live.refresh_token = "mal-refresh-value";
  auto out = pull_mirror(client, live, *store, true, kAfterExpiry, spy.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(out->pulled.imported == 1);
  CHECK(spy.calls == 1);
  REQUIRE(client.list_tokens.size() == 2);
  CHECK(client.list_tokens[0] == "mal-token-value");
  CHECK(client.list_tokens[1] == "mal-token-renewed");
  REQUIRE(out->rolled.has_value());
  CHECK(out->rolled->access_token == "mal-token-renewed");
}

TEST_CASE("pull_refused_refresh_is_unauthorized_and_the_list_is_not_asked_for") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  {
    FakeMal client({});
    RefreshSpy spy{Refresh::refused()};
    auto out = pull_mirror(client, expired_mal(), *store, true, kAfterExpiry, spy.fn());
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Unauthorized);
    CHECK(!out->rolled.has_value());
    CHECK(spy.calls == 1);
    CHECK(client.list_calls == 0);
  }
  {
    // A live-dated token MAL retired early: the one 401, one refusal, done.
    FakeMal client({});
    client.with_list(ListAnswer(err(http_401())));
    RefreshSpy spy{Refresh::refused()};
    MalAuth live = connected_mal();
    live.refresh_token = "mal-refresh-value";
    auto out = pull_mirror(client, live, *store, true, kAfterExpiry, spy.fn());
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Unauthorized);
    CHECK(!out->rolled.has_value());
    CHECK(spy.calls == 1);
    CHECK(client.list_calls == 1);
  }
}

TEST_CASE("pull_unreachable_refresh_is_a_missed_pull_not_a_lost_sign_in") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  FakeMal client({});
  RefreshSpy spy{Refresh::unreachable()};
  auto out = pull_mirror(client, expired_mal(), *store, true, kAfterExpiry, spy.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == MirrorOutcome::Completed);
  CHECK(out->pull_failed);
  CHECK(!out->rolled.has_value());
  CHECK(client.list_calls == 0);
}

TEST_CASE("pull_rolls_at_most_once_and_keeps_the_pair_when_the_new_bearer_is_refused_too") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  {
    // Expired: rolled before the fetch; the renewed bearer's 401 ends it.
    FakeMal client({});
    client.with_list(ListAnswer(err(http_401())));
    RefreshSpy spy{Refresh::rolled(renewed_pair())};
    auto out = pull_mirror(client, expired_mal(), *store, true, kAfterExpiry, spy.fn());
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Unauthorized);
    CHECK(spy.calls == 1);
    CHECK(client.list_calls == 1);
    REQUIRE(out->rolled.has_value());
    CHECK(out->rolled->access_token == "mal-token-renewed");
  }
  {
    // Live: the 401 rolls, the retry's 401 ends it; the pair is still kept.
    FakeMal client({});
    client.with_list(ListAnswer(err(http_401())));
    RefreshSpy spy{Refresh::rolled(renewed_pair())};
    MalAuth live = connected_mal();
    live.refresh_token = "mal-refresh-value";
    auto out = pull_mirror(client, live, *store, true, kAfterExpiry, spy.fn());
    REQUIRE(out.has_value());
    CHECK(out->outcome == MirrorOutcome::Unauthorized);
    CHECK(spy.calls == 1);
    CHECK(client.list_calls == 2);
    REQUIRE(out->rolled.has_value());
  }
}
