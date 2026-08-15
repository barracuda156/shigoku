// sync_tests.cpp — P20 AniList sync orchestration tests.
//
// Ports the sabigoku sync.rs #[test] cases 1:1 (§8: golden tests are the port
// contract — same names in snake_case, same fixture numbers). Offline: the
// AniListSync is a scripted fake (FakeAni/RaceAni), the Sleeper a recorder —
// no socket, no real waits.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../src/auth.hpp"
#include "../src/domain.hpp"
#include "../src/store.hpp"
#include "../src/sync.hpp"

using namespace shigoku;
using namespace shigoku::sync;

namespace {

// One scripted answer from the push guard's entry read.
using GuardAnswer = Result<std::optional<ListEntry>, ProviderError>;

// FakeAni (sync.rs): scripted pull + push answers, with an optional queue of
// push-guard (fetch_entry) answers. Recording members are `mutable` since the
// AniListSync methods are const (Rust's RefCell interior mutability has no
// compile-time-enforced C++ equivalent — `mutable` is enough here).
class FakeAni final : public AniListSync {
 public:
  FakeAni(Result<std::vector<RemoteEntry>, ProviderError> remote,
          std::vector<Result<std::int64_t, ProviderError>> pushes)
      : remote_(std::move(remote)), pushes_(pushes.begin(), pushes.end()) {}

  FakeAni& with_guards(std::vector<GuardAnswer> guards) {
    guards_.assign(guards.begin(), guards.end());
    return *this;
  }

  [[nodiscard]] Result<std::vector<RemoteEntry>, ProviderError> fetch_list(
      std::string_view, std::int64_t, ScoreFormat) const override {
    ++pull_calls;
    if (remote_taken_) return std::vector<RemoteEntry>{};
    remote_taken_ = true;
    return remote_;
  }

  [[nodiscard]] Result<std::optional<ListEntry>, ProviderError> fetch_entry(
      std::string_view, std::int64_t, std::int64_t, ScoreFormat) const override {
    if (guards_.empty()) return std::optional<ListEntry>{};
    auto answer = guards_.front();
    guards_.pop_front();
    return answer;
  }

  [[nodiscard]] Result<std::int64_t, ProviderError> save_entry(
      std::string_view, std::int64_t media_id, ListStatus status, std::uint32_t progress,
      std::uint32_t score, ScoreFormat) const override {
    push_calls.emplace_back(media_id, status, progress, score);
    if (pushes_.empty()) return std::int64_t{1};
    auto answer = pushes_.front();
    pushes_.pop_front();
    return answer;
  }

  mutable int pull_calls = 0;
  mutable std::vector<std::tuple<std::int64_t, ListStatus, std::uint32_t, std::uint32_t>> push_calls;

 private:
  mutable Result<std::vector<RemoteEntry>, ProviderError> remote_;
  mutable bool remote_taken_ = false;
  mutable std::deque<Result<std::int64_t, ProviderError>> pushes_;
  mutable std::deque<GuardAnswer> guards_;
};

// A recording sleeper: no real waits, just an append-only log (sync.rs's
// RecordingSleeper). Sleeper is a std::function, so this is just a lambda
// capturing the log by reference.
struct RecordingSleeper {
  std::vector<std::chrono::milliseconds> slept;
  Sleeper fn() {
    return [this](std::chrono::milliseconds d) { slept.push_back(d); };
  }
};

Auth connected(std::int64_t now_offset) {
  Auth auth;
  auth.anilist.access_token = "token-value";
  auth.anilist.user_id = 7;
  auth.anilist.expires_at = now_offset == 0 ? 0 : now_offset;
  return auth;
}

void dirty_lib(Store& store, std::int64_t id) {
  Enrichment e;
  e.anilist_id = id;
  e.title_romaji = "Show " + std::to_string(id);
  REQUIRE(store.add_to_library(e, 100).has_value());
}

RemoteEntry remote_entry(std::int64_t id, ListStatus status, std::uint32_t progress,
                         std::int64_t updated_at = 0, std::uint32_t score = 0) {
  RemoteEntry r;
  r.anilist_id = id;
  r.status = status;
  r.progress = progress;
  r.score = score;
  r.updated_at = updated_at;
  return r;
}

ProviderError http_401() { return ProviderError::http(401); }
ProviderError network_error() { return ProviderError::network(); }

}  // namespace

TEST_CASE("disabled_is_a_noop") {
  FakeAni client(std::vector<RemoteEntry>{}, {});
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, false, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::Disabled);
  CHECK(client.pull_calls == 0);
  CHECK(client.push_calls.empty());
}

TEST_CASE("no_token_and_expired_short_circuit_before_any_call") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  RecordingSleeper sleeper;
  {
    FakeAni client(std::vector<RemoteEntry>{}, {});
    auto out = run_sync(client, Auth{}, *store, 0, true, false, sleeper.fn());
    REQUIRE(out.has_value());
    CHECK(out->outcome == SyncOutcome::NoToken);
    CHECK(client.pull_calls == 0);
  }
  {
    FakeAni client(std::vector<RemoteEntry>{}, {});
    // Token dated at 1000, now is 2000: expired.
    auto out = run_sync(client, connected(1000), *store, 2000, true, false, sleeper.fn());
    REQUIRE(out.has_value());
    CHECK(out->outcome == SyncOutcome::Expired);
    CHECK(client.pull_calls == 0);
  }
}

TEST_CASE("pull_only_reconciles_and_never_pushes") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 10);  // a dirty row exists, but pull_only must not push it.
  FakeAni client(std::vector<RemoteEntry>{remote_entry(10, ListStatus::Watching, 3)}, {});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, true, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::Completed);
  CHECK(out->pulled.reconciled == 1);
  CHECK(client.pull_calls == 1);
  CHECK(client.push_calls.empty());
}

TEST_CASE("token_without_user_id_skips_the_whole_run") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 9);
  FakeAni client(std::vector<RemoteEntry>{}, {std::int64_t{1}});
  Auth auth = connected(0);
  auth.anilist.user_id = 0;
  RecordingSleeper sleeper;
  auto out = run_sync(client, auth, *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::NoUserId);
  CHECK(client.pull_calls == 0);
  CHECK(client.push_calls.empty());
}

// Both walls have a dirty row staged, so a broken gate would visibly push.
TEST_CASE("pull_401_and_429_skip_the_push") {
  struct Case {
    ProviderError err;
    SyncOutcome want;
  };
  std::vector<Case> cases{{http_401(), SyncOutcome::PullUnauthorized},
                          {ProviderError::rate_limited(), SyncOutcome::PullRateLimited}};
  for (const auto& c : cases) {
    auto store = Store::open_memory();
    REQUIRE(store.has_value());
    dirty_lib(*store, 11);
    FakeAni client(err(c.err), {std::int64_t{1}});
    RecordingSleeper sleeper;
    auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
    REQUIRE(out.has_value());
    CHECK(out->outcome == c.want);
    CHECK(client.push_calls.empty());
  }
}

TEST_CASE("pull_transport_failure_still_pushes") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 30);
  FakeAni client(err(network_error()), {std::int64_t{1}});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::Completed);
  CHECK(out->pull_failed);
  CHECK(out->pushed == 1);
  CHECK(out->dirty == 1);
}

TEST_CASE("push_after_pull_advances_the_snapshot") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 12);
  FakeAni client(std::vector<RemoteEntry>{}, {std::int64_t{555}});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::Completed);
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(client.push_calls[0] == std::make_tuple(std::int64_t{12}, ListStatus::Planning,
                                                std::uint32_t{0}, std::uint32_t{0}));
  auto still_dirty = store->list_dirty_for_sync();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->empty());
}

// A score-only local edit dirties the row (IFNULL score comparison in
// list_dirty_for_sync) and pushes the raw store value straight through under
// Point100 (no conversion at that identity format).
TEST_CASE("a_local_score_edit_pushes_and_advances_the_synced_score") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 50);
  REQUIRE(store->mark_synced(50, ListStatus::Planning, 0, 0).has_value());
  CHECK(store->list_dirty_for_sync()->empty());
  REQUIRE(store->set_user_score(50, 85).has_value());

  FakeAni client(std::vector<RemoteEntry>{}, {std::int64_t{1}});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(std::get<3>(client.push_calls[0]) == 85);  // score arg, Point100 identity.
  CHECK(store->list_dirty_for_sync()->empty());
}

// Under a non-Point100 scoreFormat, push_entry must convert the store's raw
// 0..=100 to the wire scale before the AniListSync sees it — proven at the
// anilist.cpp golden layer already; here the sync orchestration threads the
// account's format through to save_entry at all (regression guard for the
// per-call ScoreFormat plumbing, not the conversion math itself).
TEST_CASE("push_threads_the_accounts_score_format_to_save_entry") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 51);
  REQUIRE(store->set_user_score(51, 75).has_value());  // raw 75 -> Point10 wire 7.5, rounds via to_anilist_score.

  class FormatCapturingAni final : public AniListSync {
   public:
    [[nodiscard]] Result<std::vector<RemoteEntry>, ProviderError> fetch_list(
        std::string_view, std::int64_t, ScoreFormat) const override {
      return std::vector<RemoteEntry>{};
    }
    [[nodiscard]] Result<std::optional<ListEntry>, ProviderError> fetch_entry(
        std::string_view, std::int64_t, std::int64_t, ScoreFormat) const override {
      return std::optional<ListEntry>{};
    }
    [[nodiscard]] Result<std::int64_t, ProviderError> save_entry(
        std::string_view, std::int64_t, ListStatus, std::uint32_t, std::uint32_t,
        ScoreFormat format) const override {
      seen_format = format;
      return std::int64_t{1};
    }
    mutable ScoreFormat seen_format = ScoreFormat::Point100;
  };

  FormatCapturingAni client;
  Auth auth = connected(0);
  auth.anilist.score_format = ScoreFormat::Point10;
  RecordingSleeper sleeper;
  auto out = run_sync(client, auth, *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);
  CHECK(client.seen_format == ScoreFormat::Point10);
}

// Lossy-format convergence (P34 review): AniList stores the QUANTIZED score
// (raw 75 on a Point5 account -> wire 4 -> raw 80), so the snapshot — and,
// CAS-guarded, the local score — adopt the round-tripped value. A raw
// snapshot would strand the row dirty (every quitFlush push_skipped against
// the server's own value) and spawn phantom conflicts on the next pull.
TEST_CASE("a_lossy_format_push_adopts_the_quantized_score_locally") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 52);
  REQUIRE(store->mark_synced(52, ListStatus::Planning, 0, 0).has_value());
  REQUIRE(store->set_user_score(52, 75).has_value());  // Point5: wire 4 -> raw 80.

  FakeAni client(std::vector<RemoteEntry>{}, {std::int64_t{1}});
  Auth auth = connected(0);
  auth.anilist.score_format = ScoreFormat::Point5;
  RecordingSleeper sleeper;
  auto out = run_sync(client, auth, *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->pushed == 1);

  // Local and snapshot both hold what the server actually stored: settled.
  auto g = store->get_show(52);
  REQUIRE(g.has_value());
  REQUIRE(g->has_value());
  REQUIRE((*g)->user_score.has_value());
  CHECK(*(*g)->user_score == 80);
  CHECK(store->list_dirty_for_sync()->empty());
}

TEST_CASE("push_401_stops_the_run_and_leaves_rows_dirty") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 13);
  dirty_lib(*store, 14);
  FakeAni client(std::vector<RemoteEntry>{}, {err(http_401()), std::int64_t{1}});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::Unauthorized);
  CHECK(out->pushed == 0);
  CHECK(out->dirty == 2);
  CHECK(client.push_calls.size() == 1);
  auto still_dirty = store->list_dirty_for_sync();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->size() == 2);
}

TEST_CASE("push_429_backs_off_once_then_a_second_stops_the_run") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 15);
  dirty_lib(*store, 16);
  // Row15: 429 -> backoff -> Ok. Row16: 429 -> second, stop.
  FakeAni client(std::vector<RemoteEntry>{},
                 {err(ProviderError::rate_limited()), std::int64_t{1},
                  err(ProviderError::rate_limited())});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::RateLimited);
  CHECK(out->pushed == 1);
  // 60s backoff on row15, then 2s spacing before row16.
  CHECK(sleeper.slept == std::vector<std::chrono::milliseconds>{kRateLimitBackoff, kPushSpacing});
  auto still_dirty = store->list_dirty_for_sync();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->size() == 1);
}

TEST_CASE("push_spacing_is_between_rows_only") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 17);
  dirty_lib(*store, 18);
  dirty_lib(*store, 19);
  FakeAni client(std::vector<RemoteEntry>{},
                 {std::int64_t{1}, std::int64_t{1}, std::int64_t{1}});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->pushed == 3);
  // Three rows -> two gaps, no leading sleep.
  CHECK(sleeper.slept == std::vector<std::chrono::milliseconds>{kPushSpacing, kPushSpacing});
}

// ROD-500: the pull failed to land this row's merge, so its live pair is
// pre-merge and its snapshot never advanced. Pushing it in the same run would
// overwrite the remote change the pull was carrying.
TEST_CASE("a_contended_row_is_held_back_from_the_push") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 23);
  dirty_lib(*store, 24);
  FakeAni client(std::vector<RemoteEntry>{}, {std::int64_t{1}, std::int64_t{1}});
  RecordingSleeper sleeper;
  SyncSummary summary = SyncSummary::terminal(SyncOutcome::Completed);
  auto r = detail::push_dirty(client, "tok", 7, *store, sleeper.fn(), {23}, summary, ScoreFormat::Point100);
  REQUIRE(r.has_value());

  CHECK(summary.push_skipped == 1);
  CHECK(summary.pushed == 1);
  REQUIRE(client.push_calls.size() == 1);
  CHECK(std::get<0>(client.push_calls[0]) == 24);
  CHECK(sleeper.slept.empty());
  auto still_dirty = store->list_dirty_for_sync();
  REQUIRE(still_dirty.has_value());
  REQUIRE(still_dirty->size() == 1);
  CHECK((*still_dirty)[0].anilist_id == 23);
}

TEST_CASE("per_row_error_counts_and_continues") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 20);
  dirty_lib(*store, 21);
  FakeAni client(std::vector<RemoteEntry>{}, {err(network_error()), std::int64_t{1}});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::Completed);
  CHECK(out->push_failed == 1);
  CHECK(out->pushed == 1);
  CHECK(client.push_calls.size() == 2);
}

namespace {

// A fake AniList that holds server state across the pull and the push, with
// hooks that land a concurrent edit at a chosen point in the sequence
// (sync.rs's RaceAni). The three points are the whole ROD-498 story:
// before_guard is the window the guard closes, after_guard is the round trip
// it cannot, and local_edit is the store moving under us while we wait on the
// socket.
class RaceAni final : public AniListSync {
 public:
  RaceAni(Store& store, std::vector<std::tuple<std::int64_t, ListStatus, std::uint32_t>> seed)
      : store_(store) {
    for (const auto& [id, s, p] : seed) server_[id] = {s, p};
  }

  void set_before_guard(std::int64_t id, ListStatus s, std::uint32_t p) {
    before_guard_ = std::make_tuple(id, s, p);
  }
  void set_after_guard(std::int64_t id, ListStatus s, std::uint32_t p) {
    after_guard_ = std::make_tuple(id, s, p);
  }
  void set_local_edit(std::int64_t id, ListStatus s) { local_edit_ = std::make_pair(id, s); }

  [[nodiscard]] std::optional<std::pair<ListStatus, std::uint32_t>> pair(std::int64_t id) const {
    auto it = server_.find(id);
    if (it == server_.end()) return std::nullopt;
    return it->second;
  }

  void erase(std::int64_t id) { server_.erase(id); }

  [[nodiscard]] Result<std::vector<RemoteEntry>, ProviderError> fetch_list(
      std::string_view, std::int64_t, ScoreFormat) const override {
    std::vector<RemoteEntry> out;
    for (const auto& [id, sp] : server_) {
      out.push_back(remote_entry(id, sp.first, sp.second));
    }
    return out;
  }

  // Score is untested by this fake (every seed/edit is status+progress
  // only), so the wire triple always carries score 0.
  [[nodiscard]] Result<std::optional<ListEntry>, ProviderError> fetch_entry(
      std::string_view, std::int64_t, std::int64_t media_id, ScoreFormat) const override {
    if (before_guard_.has_value()) {
      const auto [id, s, p] = *before_guard_;
      before_guard_ = std::nullopt;
      server_[id] = {s, p};
    }
    const auto p = pair(media_id);
    if (!p.has_value()) return std::optional<ListEntry>{};
    return std::optional<ListEntry>(ListEntry{p->first, p->second, 0});
  }

  [[nodiscard]] Result<std::int64_t, ProviderError> save_entry(
      std::string_view, std::int64_t media_id, ListStatus status, std::uint32_t progress,
      std::uint32_t, ScoreFormat) const override {
    if (after_guard_.has_value()) {
      const auto [id, s, p] = *after_guard_;
      after_guard_ = std::nullopt;
      server_[id] = {s, p};
    }
    if (local_edit_.has_value()) {
      const auto [id, s] = *local_edit_;
      local_edit_ = std::nullopt;
      REQUIRE(store_.set_list_status(id, s, 0).has_value());
    }
    server_[media_id] = {status, progress};
    return std::int64_t{1};
  }

 private:
  Store& store_;
  mutable std::unordered_map<std::int64_t, std::pair<ListStatus, std::uint32_t>> server_;
  mutable std::optional<std::tuple<std::int64_t, ListStatus, std::uint32_t>> before_guard_;
  mutable std::optional<std::tuple<std::int64_t, ListStatus, std::uint32_t>> after_guard_;
  mutable std::optional<std::pair<std::int64_t, ListStatus>> local_edit_;
};

}  // namespace

// ROD-498: the row this run wants to push is `dropped` locally against a
// server that said `watching / 0` at pull time. The user then watches 12
// episodes on the site. Without the guard our stale write lands on top of
// their 12 and marks the pair agreed, so nothing is ever dirty again and no
// later run can find the loss.
TEST_CASE("a_remote_edit_after_the_pull_is_not_overwritten") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 40);
  REQUIRE(store->set_list_status(40, ListStatus::Dropped, 0).has_value());
  RaceAni client(*store, {{40, ListStatus::Watching, 0}});
  client.set_before_guard(40, ListStatus::Watching, 12);

  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());

  CHECK(out->pushed == 0);
  CHECK(out->push_skipped == 1);
  CHECK(client.pair(40) == std::make_pair(ListStatus::Watching, std::uint32_t{12}));
  auto still_dirty = store->list_dirty_for_sync();
  REQUIRE(still_dirty.has_value());
  REQUIRE(still_dirty->size() == 1);
  CHECK((*still_dirty)[0].anilist_id == 40);
  CHECK((*still_dirty)[0].list_status == ListStatus::Dropped);
}

// The residual window, stated so nobody mistakes the guard for a CAS.
// SaveMediaListEntry has no compare-and-set, so an edit landing between our
// read and our write is still lost. The guard shrinks that window to one
// round trip; it cannot remove it.
TEST_CASE("a_remote_edit_inside_the_round_trip_is_still_lost") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 42);
  REQUIRE(store->set_list_status(42, ListStatus::Dropped, 0).has_value());
  RaceAni client(*store, {{42, ListStatus::Watching, 0}});
  client.set_after_guard(42, ListStatus::Watching, 12);

  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());

  CHECK(out->pushed == 1);
  CHECK(client.pair(42) == std::make_pair(ListStatus::Dropped, std::uint32_t{0}));
}

// The local half of the same race, and the reason mark_synced stays
// unconditional: the snapshot records what the server now holds, which the
// newer local pair no longer matches, so the row re-dirties itself. Guarding
// that write instead would leave the snapshot claiming a value we never sent.
TEST_CASE("a_local_edit_inside_the_round_trip_leaves_the_row_dirty") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 41);
  REQUIRE(store->set_list_status(41, ListStatus::Watching, 0).has_value());
  RaceAni client(*store, {});
  client.set_local_edit(41, ListStatus::Dropped);

  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, false, sleeper.fn());
  REQUIRE(out.has_value());

  CHECK(out->pushed == 1);
  auto still_dirty = store->list_dirty_for_sync();
  REQUIRE(still_dirty.has_value());
  REQUIRE(still_dirty->size() == 1);
  CHECK((*still_dirty)[0].anilist_id == 41);
  CHECK((*still_dirty)[0].list_status == ListStatus::Dropped);
}

// A deleted remote entry never reappears in the pull to clear our snapshot,
// so treating absence as a mismatch would strand the row dirty forever.
// Nothing of theirs is on the server to destroy, so the write goes.
TEST_CASE("an_absent_remote_entry_does_not_block_the_push") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 43);
  REQUIRE(store->set_list_status(43, ListStatus::Watching, 0).has_value());
  RaceAni client(*store, {{43, ListStatus::Watching, 5}});
  RecordingSleeper sleeper;
  auto out = run_sync(client, connected(0), *store, 0, true, true, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->pulled.reconciled == 1);

  // The user deletes the entry on AniList, then we push.
  client.erase(43);
  REQUIRE(store->set_list_status(43, ListStatus::Dropped, 0).has_value());
  auto out2 = flush_push(client, connected(0), *store, 0, true, sleeper.fn());
  REQUIRE(out2.has_value());

  CHECK(out2->pushed == 1);
  CHECK(out2->push_skipped == 0);
  // Progress 5 rides along: set_list_status keeps the adopted count.
  CHECK(client.pair(43) == std::make_pair(ListStatus::Dropped, std::uint32_t{5}));
}

TEST_CASE("the_guard_stops_the_run_on_401_and_rides_the_429_ladder") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 44);
  {
    FakeAni client(std::vector<RemoteEntry>{}, {});
    client.with_guards({err(http_401())});
    RecordingSleeper sleeper;
    SyncSummary summary = SyncSummary::terminal(SyncOutcome::Completed);
    auto r = detail::push_dirty(client, "tok", 7, *store, sleeper.fn(), {}, summary, ScoreFormat::Point100);
    REQUIRE(r.has_value());
    CHECK(summary.outcome == SyncOutcome::Unauthorized);
    CHECK(client.push_calls.empty());
  }

  // First 429 backs off and retries the guard, which then passes.
  {
    FakeAni client(std::vector<RemoteEntry>{}, {std::int64_t{1}});
    client.with_guards({err(ProviderError::rate_limited()), std::optional<ListEntry>{}});
    RecordingSleeper sleeper;
    SyncSummary summary = SyncSummary::terminal(SyncOutcome::Completed);
    auto r = detail::push_dirty(client, "tok", 7, *store, sleeper.fn(), {}, summary, ScoreFormat::Point100);
    REQUIRE(r.has_value());
    CHECK(summary.pushed == 1);
    CHECK(sleeper.slept == std::vector<std::chrono::milliseconds>{kRateLimitBackoff});
  }
}

TEST_CASE("an_unverifiable_row_is_not_pushed_blind") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 45);
  FakeAni client(std::vector<RemoteEntry>{}, {std::int64_t{1}});
  client.with_guards({err(network_error())});
  RecordingSleeper sleeper;
  SyncSummary summary = SyncSummary::terminal(SyncOutcome::Completed);
  auto r = detail::push_dirty(client, "tok", 7, *store, sleeper.fn(), {}, summary, ScoreFormat::Point100);
  REQUIRE(r.has_value());
  CHECK(summary.push_failed == 1);
  CHECK(summary.pushed == 0);
  CHECK(client.push_calls.empty());
  auto still_dirty = store->list_dirty_for_sync();
  REQUIRE(still_dirty.has_value());
  CHECK(still_dirty->size() == 1);
}

TEST_CASE("flush_push_is_push_only") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  dirty_lib(*store, 22);
  FakeAni client(std::vector<RemoteEntry>{remote_entry(99, ListStatus::Watching, 1)},
                 {std::int64_t{1}});
  RecordingSleeper sleeper;
  auto out = flush_push(client, connected(0), *store, 0, true, sleeper.fn());
  REQUIRE(out.has_value());
  CHECK(out->outcome == SyncOutcome::Completed);
  CHECK(out->pushed == 1);
  CHECK(client.pull_calls == 0);
}
