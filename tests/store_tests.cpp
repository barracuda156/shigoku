// store_tests.cpp — P9 + P14 store tests.
//
// Ports the sabigoku store.rs #[test] cases 1:1 (§8: golden tests are the port
// contract — same names in snake_case, same fixture numbers). P9 subset:
// resume_start_rule, resume_round_trip_and_watched_ratio, recompute positional
// high-water, migration idempotence + resume DoD. P14 (schema growth): the
// catalog_cache / enrichment merge laws (cover downgrade, blank-title, ROD-419
// total clear), bindings (mint-no-membership, rebind steal), absence TTL +
// availability precedence, pin/route round-trip, episode_cache expiry + TTL,
// app_meta, delete cascade, every-02-table, and the shigoku-specific v1→v2
// (tables-added) migration. Offline, in-memory or a tmp file; no network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sqlite3.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "../src/domain.hpp"
#include "../src/store.hpp"

using namespace shigoku;

namespace {

// Fresh file-backed DB path in the OS tmpdir; wipes any prior run's
// main/-wal/-shm so every test starts blank (store.rs tmp_db).
std::string tmp_db(const char* name) {
  std::string base = "/tmp/shigoku-store-test-";
  base += std::to_string(static_cast<long>(::getpid()));
  base += "-";
  base += name;
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((base + suffix).c_str());
  }
  return base;
}

// store.rs identity_row: mint the bare identity row a progress writer needs.
Store open_mem() {
  auto s = Store::open_memory();
  REQUIRE(s.has_value());
  return std::move(*s);
}

void identity_row(Store& s, std::int64_t id) {
  auto r = s.ensure_show(id, "Test Show", std::nullopt, std::nullopt, std::nullopt);
  REQUIRE(r.has_value());
}

// A show with a known total + FINISHED status, so after_play can complete it.
void finished_row(Store& s, std::int64_t id, std::uint32_t total) {
  auto r = s.ensure_show(id, "Test Show", std::nullopt, total, std::string_view("FINISHED"));
  REQUIRE(r.has_value());
}

// store.rs sample(): a fully-populated Enrichment across the whole 02 fieldset,
// so catalog/enrichment round-trips exercise every column (P14).
Enrichment sample(std::int64_t id) {
  Enrichment e;
  e.anilist_id = id;
  e.mal_id = 100 + id;
  e.title_romaji = "Show " + std::to_string(id);
  e.title_english = "Show " + std::to_string(id) + " EN";
  e.title_native = "ショウ";
  e.cover_url = "https://img/cover.png";
  e.total_episodes = 12;
  e.duration_minutes = 24;
  e.year = 2024;
  e.season = Season::Fall;
  e.status = "FINISHED";
  e.description = "desc";
  e.score = 83;
  e.kind = "TV";
  e.start_date = Date{2024, 10, 5};
  e.genres = {"Action", "Drama"};
  e.studios = {"MAPPA"};
  e.source_material = "MANGA";
  e.rank = 42;
  e.rank_type = "POPULAR";
  e.rank_year = 2024;
  e.country = "JP";
  return e;
}

}  // namespace

// --- resume_start_rule (store.rs:3409) 1:1 --------------------------------
TEST_CASE("resume_start_rule") {
  auto resume = [](double pos, double dur, bool watched) {
    return Resume{pos, dur, watched};
  };
  CHECK(resume(100.0, 1000.0, true).start_secs(5) == 0.0);
  CHECK(resume(800.0, 1000.0, false).start_secs(5) == 0.0);
  CHECK(resume(-3.0, 1000.0, false).start_secs(5) == 0.0);
  CHECK(resume(0.0, 1000.0, false).start_secs(5) == 0.0);
  CHECK(resume(std::nan(""), 1000.0, false).start_secs(5) == 0.0);
  CHECK(resume(100.0, 1000.0, false).start_secs(5) == 95.0);
  CHECK(resume(3.0, 1000.0, false).start_secs(5) == 0.0);
  // Unknown duration resumes; only a real ratio restarts.
  CHECK(resume(500.0, 0.0, false).start_secs(5) == 495.0);
}

// --- resume_round_trip_and_watched_ratio (store.rs:3084) 1:1 --------------
TEST_CASE("resume_round_trip_and_watched_ratio") {
  Store store = open_mem();
  identity_row(store, 60);
  {
    auto r = store.get_resume(60, Translation::Sub, "1");
    REQUIRE(r.has_value());
    CHECK_FALSE(r->has_value());
  }

  REQUIRE(store.save_progress(60, Translation::Sub, "1", 94.9, 100.0,
                              std::string_view("senshi"), 500)
              .has_value());
  {
    auto r = store.get_resume(60, Translation::Sub, "1");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->position_secs == doctest::Approx(94.9));
    CHECK_FALSE((*r)->fully_watched);  // 0.949 is under WATCHED_RATIO.
  }

  REQUIRE(store.save_progress(60, Translation::Sub, "1", 95.0, 100.0,
                              std::string_view("senshi"), 600)
              .has_value());
  {
    auto r = store.get_resume(60, Translation::Sub, "1");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->fully_watched);
  }

  // Zero duration can never mark watched (division guard).
  REQUIRE(
      store.save_progress(60, Translation::Sub, "2", 10.0, 0.0, std::nullopt, 700).has_value());
  {
    auto r = store.get_resume(60, Translation::Sub, "2");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK_FALSE((*r)->fully_watched);
  }

  // Tracks never mix.
  {
    auto r = store.get_resume(60, Translation::Dub, "1");
    REQUIRE(r.has_value());
    CHECK_FALSE(r->has_value());
  }
}

// --- save_progress rejects non-finite (store.rs ROD-434 red-team) ----------
TEST_CASE("save_progress_rejects_non_finite") {
  Store store = open_mem();
  identity_row(store, 61);
  auto bad = store.save_progress(61, Translation::Sub, "1", std::nan(""), 100.0,
                                 std::nullopt, 500);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().kind == StoreError::Kind::NonFinitePosition);
  // +Inf would sail through the ratio into a permanent fully_watched.
  auto inf = store.save_progress(61, Translation::Sub, "1",
                                 std::numeric_limits<double>::infinity(), 100.0,
                                 std::nullopt, 500);
  REQUIRE_FALSE(inf.has_value());
  CHECK(inf.error().kind == StoreError::Kind::NonFinitePosition);
}

// --- recompute is positional high-water (store.rs:3427) --------------------
TEST_CASE("recompute_is_positional_high_water") {
  Store store = open_mem();
  identity_row(store, 61);
  auto watch = [&](const char* ep, bool done) {
    const double pos = done ? 100.0 : 10.0;
    REQUIRE(store.save_progress(61, Translation::Sub, ep, pos, 100.0, std::nullopt, 500)
                .has_value());
  };
  // Present rows sorted: 1, 2, 5, SP1. Last fully-watched is "5" at position 3:
  // positional, not the label value, not a count.
  watch("5", true);
  watch("1", true);
  watch("2", false);
  watch("SP1", false);
  {
    auto p = store.recompute_progress(61, Translation::Sub, 600);
    REQUIRE(p.has_value());
    CHECK(*p == 3);
  }
  {
    auto g = store.get_progress(61);
    REQUIRE(g.has_value());
    REQUIRE(g->has_value());
    CHECK(**g == 3);
  }

  // Gap-watch under-counts on purpose: only "5" watched → 1.
  Store store2 = open_mem();
  identity_row(store2, 61);
  REQUIRE(store2.save_progress(61, Translation::Sub, "5", 100.0, 100.0, std::nullopt, 500)
              .has_value());
  {
    auto p = store2.recompute_progress(61, Translation::Sub, 600);
    REQUIRE(p.has_value());
    CHECK(*p == 1);
  }
  // Dub rows never count toward a sub recompute.
  REQUIRE(store2.save_progress(61, Translation::Dub, "1", 100.0, 100.0, std::nullopt, 500)
              .has_value());
  {
    auto p = store2.recompute_progress(61, Translation::Sub, 600);
    REQUIRE(p.has_value());
    CHECK(*p == 1);
  }
}

// --- record_finish: ratchet + engagement + fully_watched (02 §4b) ----------
TEST_CASE("record_finish_ratchets_and_marks_watched") {
  Store store = open_mem();
  finished_row(store, 70, /*total=*/12);

  // A mid-episode finish under 0.80: no progress ratchet, resume row saved.
  REQUIRE(store.record_finish(70, Translation::Sub, "1", 1, 40.0, 100.0,
                              std::string_view("senshi"), 500)
              .has_value());
  {
    auto g = store.get_progress(70);
    REQUIRE(g.has_value());
    CHECK(**g == 0);  // under natural end → no ratchet.
    auto r = store.get_resume(70, Translation::Sub, "1");
    REQUIRE(r->has_value());
    CHECK_FALSE((*r)->fully_watched);  // under 0.95.
  }

  // Finish ep 1 past 0.95: fully_watched set, progress ratchets to 1.
  REQUIRE(store.record_finish(70, Translation::Sub, "1", 1, 99.0, 100.0,
                              std::string_view("senshi"), 600)
              .has_value());
  {
    auto g = store.get_progress(70);
    CHECK(**g == 1);
    auto r = store.get_resume(70, Translation::Sub, "1");
    CHECK((*r)->fully_watched);
  }

  // Natural end (0.80) of ep 2 ratchets progress but does NOT mark watched.
  REQUIRE(store.record_finish(70, Translation::Sub, "2", 2, 85.0, 100.0,
                              std::string_view("senshi"), 700)
              .has_value());
  {
    auto g = store.get_progress(70);
    CHECK(**g == 2);
    auto r = store.get_resume(70, Translation::Sub, "2");
    CHECK_FALSE((*r)->fully_watched);  // 0.85 < 0.95.
  }

  // Ratchet never lowers: replaying ep 1 at a natural end keeps progress at 2.
  REQUIRE(store.record_finish(70, Translation::Sub, "1", 1, 90.0, 100.0,
                              std::string_view("senshi"), 800)
              .has_value());
  {
    auto g = store.get_progress(70);
    CHECK(**g == 2);
  }

  // Unknown show / zero index are no-ops, not errors.
  CHECK(store.record_finish(999, Translation::Sub, "1", 1, 90.0, 100.0, std::nullopt, 900)
            .has_value());
  CHECK(store.record_finish(70, Translation::Sub, "1", 0, 90.0, 100.0, std::nullopt, 900)
            .has_value());
}

// --- set_list_status / restore_list_status (store.rs:2808/2852, P16 ROD-139/193)

TEST_CASE("set_list_status_snap_rules") {
  Store store = open_mem();

  // Snap to total: a show with a known total completes at that total,
  // regardless of whatever progress a prior partial watch left it at.
  REQUIRE(store.add_to_library(sample(9), 50).has_value());
  REQUIRE(store.record_finish(9, Translation::Sub, "3", 3, 60.0, 60.0, std::nullopt, 60)
              .has_value());
  REQUIRE(store.set_list_status(9, ListStatus::Completed, 100).has_value());
  {
    auto g = store.get_show(9);
    REQUIRE(g->has_value());
    CHECK((*g)->progress == 12);  // snapped to sample()'s total_episodes.
  }

  // No total: completed keeps progress verbatim (no snap target to reach for).
  identity_row(store, 10);
  REQUIRE(store.restore_list_status(10, ListStatus::Watching, 3, 50).has_value());
  REQUIRE(store.set_list_status(10, ListStatus::Completed, 100).has_value());
  {
    auto g = store.get_show(10);
    REQUIRE(g->has_value());
    CHECK((*g)->progress == 3);
    CHECK((*g)->library_added_at == 50);  // status writers stamp membership once.
  }

  // Total 0 must not zero progress (the snap guard is `total > 0`).
  {
    auto e = store.ensure_show(11, "t", std::nullopt, 0u, std::nullopt);
    REQUIRE(e.has_value());
    REQUIRE(store.save_progress(11, Translation::Sub, "4", 100.0, 100.0, std::nullopt, 50)
                .has_value());
    REQUIRE(store.record_finish(11, Translation::Sub, "4", 4, 100.0, 100.0, std::nullopt, 50)
                .has_value());
    REQUIRE(store.set_list_status(11, ListStatus::Completed, 100).has_value());
    auto g = store.get_show(11);
    REQUIRE(g->has_value());
    CHECK((*g)->progress == 4);
  }

  // Unknown show: silent no-op, not an error.
  CHECK(store.set_list_status(999, ListStatus::Watching, 100).has_value());
}

TEST_CASE("restore_is_the_exact_pair") {
  Store store = open_mem();
  REQUIRE(store.add_to_library(sample(12), 50).has_value());
  REQUIRE(store.set_list_status(12, ListStatus::Completed, 60).has_value());
  {
    auto g = store.get_show(12);
    REQUIRE(g->has_value());
    CHECK((*g)->progress == 12);  // snapped to total by set_list_status.
  }
  // Undo restores the exact captured pair — no re-derivation, no snap, even
  // though the show still has a positive total.
  REQUIRE(store.restore_list_status(12, ListStatus::Watching, 7, 70).has_value());
  {
    auto g = store.get_show(12);
    REQUIRE(g->has_value());
    CHECK((*g)->list_status == ListStatus::Watching);
    CHECK((*g)->progress == 7);
  }

  // Unknown show: silent no-op.
  CHECK(store.restore_list_status(999, ListStatus::Watching, 1, 100).has_value());
}

// --- set_user_score (P34 slice 1) -------------------------------------------

TEST_CASE("set_user_score_round_trip_and_clear") {
  Store store = open_mem();
  REQUIRE(store.add_to_library(sample(13), 50).has_value());

  // Unset by default.
  {
    auto g = store.get_show(13);
    REQUIRE(g->has_value());
    CHECK_FALSE((*g)->user_score.has_value());
  }

  // A write round-trips as the raw 0..=100 value (×10 of the 0-10 UI scale
  // happens in the parser, not here — the store takes the already-scaled int).
  REQUIRE(store.set_user_score(13, 75).has_value());
  {
    auto g = store.get_show(13);
    REQUIRE(g->has_value());
    REQUIRE((*g)->user_score.has_value());
    CHECK(*(*g)->user_score == 75);
  }

  // nullopt clears it back to unset, not 0-as-a-score.
  REQUIRE(store.set_user_score(13, std::nullopt).has_value());
  {
    auto g = store.get_show(13);
    REQUIRE(g->has_value());
    CHECK_FALSE((*g)->user_score.has_value());
  }

  // synced_score is untouched by set_user_score (dirtiness is the caller's,
  // slice 2's, concern — this writer only owns the live value).
  REQUIRE(store.set_user_score(13, 40).has_value());
  {
    auto g = store.get_show(13);
    REQUIRE(g->has_value());
    CHECK_FALSE((*g)->synced_score.has_value());
  }

  // Unknown show: silent no-op, not an error.
  CHECK(store.set_user_score(999, 50).has_value());
}

// --- schedule notices (P37 slice 3) -----------------------------------------

TEST_CASE("set_schedule_notice_round_trip_and_clear_notice_pending") {
  Store store = open_mem();
  REQUIRE(store.add_to_library(sample(14), 50).has_value());

  // Unset by default: no mark, no pending marker.
  {
    auto g = store.get_show(14);
    REQUIRE(g->has_value());
    CHECK_FALSE((*g)->notice_last_episode.has_value());
    CHECK_FALSE((*g)->notice_pending);
  }

  // A notice write raises the high-water mark AND the pending marker together.
  REQUIRE(store.set_schedule_notice(14, 5).has_value());
  {
    auto g = store.get_show(14);
    REQUIRE(g->has_value());
    REQUIRE((*g)->notice_last_episode.has_value());
    CHECK(*(*g)->notice_last_episode == 5);
    CHECK((*g)->notice_pending);
  }

  // clear_notice_pending (open_detail) clears ONLY the marker; the dedup
  // high-water mark survives — a re-check must not re-raise episode 5.
  REQUIRE(store.clear_notice_pending(14).has_value());
  {
    auto g = store.get_show(14);
    REQUIRE(g->has_value());
    REQUIRE((*g)->notice_last_episode.has_value());
    CHECK(*(*g)->notice_last_episode == 5);
    CHECK_FALSE((*g)->notice_pending);
  }

  // A later, higher episode raises the mark again and re-sets pending.
  REQUIRE(store.set_schedule_notice(14, 6).has_value());
  {
    auto g = store.get_show(14);
    REQUIRE(g->has_value());
    CHECK(*(*g)->notice_last_episode == 6);
    CHECK((*g)->notice_pending);
  }

  // Unknown show: silent no-op, not an error.
  CHECK(store.set_schedule_notice(999, 1).has_value());
  CHECK(store.clear_notice_pending(999).has_value());
}

// --- watched marks index-parallel to the grid (05 §10.5) -------------------
TEST_CASE("watched_marks_track_the_grid_order") {
  Store store = open_mem();
  identity_row(store, 80);
  REQUIRE(store.save_progress(80, Translation::Sub, "1", 100.0, 100.0, std::nullopt, 500)
              .has_value());
  REQUIRE(store.save_progress(80, Translation::Sub, "3", 100.0, 100.0, std::nullopt, 500)
              .has_value());
  REQUIRE(store.save_progress(80, Translation::Sub, "2", 40.0, 100.0, std::nullopt, 500)
              .has_value());  // partial, not watched.

  std::vector<std::string> grid{"1", "2", "3", "4"};
  auto marks = store.watched_marks(80, Translation::Sub, grid);
  REQUIRE(marks.has_value());
  REQUIRE(marks->size() == 4);
  CHECK((*marks)[0]);        // "1" fully watched.
  CHECK_FALSE((*marks)[1]);  // "2" only partial.
  CHECK((*marks)[2]);        // "3" fully watched.
  CHECK_FALSE((*marks)[3]);  // "4" no row.

  // Dub track sees none of the sub marks.
  auto dub = store.watched_marks(80, Translation::Dub, grid);
  REQUIRE(dub.has_value());
  for (bool m : *dub) CHECK_FALSE(m);
}

// --- latest_resume picks the freshest usable partial (05 §10.7) ------------
TEST_CASE("latest_resume_picks_freshest_partial") {
  Store store = open_mem();
  identity_row(store, 90);
  REQUIRE(store.save_progress(90, Translation::Sub, "1", 50.0, 100.0, std::nullopt, 500)
              .has_value());
  REQUIRE(store.save_progress(90, Translation::Sub, "2", 30.0, 100.0, std::nullopt, 600)
              .has_value());
  // A fully-watched row is never a resume point.
  REQUIRE(store.save_progress(90, Translation::Sub, "3", 100.0, 100.0, std::nullopt, 700)
              .has_value());

  auto r = store.latest_resume(90, Translation::Sub);
  REQUIRE(r.has_value());
  REQUIRE(r->has_value());
  CHECK((*r)->first == "2");  // freshest partial by updated_at.
  CHECK((*r)->second.position_secs == doctest::Approx(30.0));

  // No partial rows → nullopt.
  auto empty = store.latest_resume(90, Translation::Dub);
  REQUIRE(empty.has_value());
  CHECK_FALSE(empty->has_value());
}

// --- ROD-477: a partial behind the frontier stamp is a dead resume point ----
// (store.rs latest_resume_dies_when_a_local_ratchet_passes_it, ported 1:1)
TEST_CASE("latest_resume_dies_when_a_local_ratchet_passes_it") {
  Store store = open_mem();
  identity_row(store, 91);
  // Ep 9 abandoned short of the watched ratio, then 10 and 11 finished.
  REQUIRE(store.save_progress(91, Translation::Sub, "9", 40.0, 100.0, std::nullopt, 500)
              .has_value());
  {
    auto r = store.latest_resume(91, Translation::Sub);
    REQUIRE(r.has_value());
    CHECK(r->has_value());
  }
  REQUIRE(store.record_finish(91, Translation::Sub, "10", 10, 96.0, 100.0,
                              std::nullopt, 600)
              .has_value());
  REQUIRE(store.record_finish(91, Translation::Sub, "11", 11, 96.0, 100.0,
                              std::nullopt, 700)
              .has_value());
  {
    auto r = store.latest_resume(91, Translation::Sub);
    REQUIRE(r.has_value());
    CHECK_FALSE(r->has_value());  // partial behind the ratchet is dead.
  }

  // A rewatch checkpoint written after the ratchet is live again.
  REQUIRE(store.save_progress(91, Translation::Sub, "3", 30.0, 100.0, std::nullopt, 800)
              .has_value());
  {
    auto r = store.latest_resume(91, Translation::Sub);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->first == "3");  // post-frontier activity is a deliberate rewatch.
  }

  // Replaying an episode without moving the frontier stamps nothing: the
  // rewatch checkpoint survives an unrelated full replay.
  REQUIRE(store.record_finish(91, Translation::Sub, "5", 5, 96.0, 100.0,
                              std::nullopt, 900)
              .has_value());
  {
    auto r = store.latest_resume(91, Translation::Sub);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->first == "3");  // a no-move replay must not kill the checkpoint.
  }
}

// --- ROD-477: recompute stamps in either direction and retires the marker ---
TEST_CASE("latest_resume_dies_after_recompute_to_zero") {
  Store store = open_mem();
  identity_row(store, 92);
  REQUIRE(store.record_finish(92, Translation::Sub, "1", 1, 96.0, 100.0,
                              std::nullopt, 500)
              .has_value());
  REQUIRE(store.save_progress(92, Translation::Sub, "2", 40.0, 100.0, std::nullopt, 600)
              .has_value());
  {
    auto r = store.latest_resume(92, Translation::Sub);
    REQUIRE(r.has_value());
    CHECK(r->has_value());
  }
  // Overwrite ep 1 as a partial (fully_watched is overwritten, not sticky) so
  // the recompute below is a genuine lowering: high-water 1 → 0.
  REQUIRE(store.save_progress(92, Translation::Sub, "1", 40.0, 100.0, std::nullopt, 650)
              .has_value());
  auto hw = store.recompute_progress(92, Translation::Sub, 700);
  REQUIRE(hw.has_value());
  CHECK(*hw == 0);  // no fully-watched rows left.
  {
    auto r = store.latest_resume(92, Translation::Sub);
    REQUIRE(r.has_value());
    CHECK_FALSE(r->has_value());  // recompute-to-0 retires the resume marker.
  }
}

// --- migration is idempotent across reopen (store.rs reopen_is_idempotent) --
TEST_CASE("open_migrates_and_reopen_is_idempotent") {
  const std::string path = tmp_db("reopen.db");
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    // Write a row so the reopen carries real data forward.
    REQUIRE(s->ensure_show(100, "Persisted", 42, 12, std::string_view("FINISHED"))
                .has_value());
    REQUIRE(s->save_progress(100, Translation::Sub, "1", 300.0, 1000.0,
                             std::string_view("senshi"), 500)
                .has_value());
  }
  // Reopen the same file: migration is a no-op, data survives.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    auto r = s->get_resume(100, Translation::Sub, "1");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->position_secs == doctest::Approx(300.0));
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}

// --- the P9 DoD: kill mid-episode, relaunch, resume ~5s before the kill -----
TEST_CASE("resume_dod_kill_and_relaunch") {
  const std::string path = tmp_db("resume-dod.db");
  // Session 1: watch ~half of a 1000s episode; the last checkpoint lands at 480.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    REQUIRE(s->ensure_show(200, "Long Show", std::nullopt, 24, std::string_view("FINISHED"))
                .has_value());
    // 30s checkpoints as PositionUpdate ticks would post them.
    for (double pos : {30.0, 60.0, 300.0, 480.0}) {
      REQUIRE(s->save_progress(200, Translation::Sub, "5", pos, 1000.0,
                               std::string_view("senshi"), 1000)
                  .has_value());
    }
    // "kill" = drop the Store (destructor closes the connection).
  }
  // Session 2: relaunch, read the resume start — 480 - 5 = 475.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    auto r = s->get_resume(200, Translation::Sub, "5");
    REQUIRE(r->has_value());
    CHECK_FALSE((*r)->fully_watched);
    CHECK((*r)->start_secs() == doctest::Approx(475.0));
  }
  // Finishing ≥ 0.80 → next play starts at 0.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    REQUIRE(s->record_finish(200, Translation::Sub, "5", 5, 990.0, 1000.0,
                             std::string_view("senshi"), 2000)
                .has_value());
    auto r = s->get_resume(200, Translation::Sub, "5");
    REQUIRE(r->has_value());
    CHECK((*r)->fully_watched);        // 0.99 ≥ 0.95.
    CHECK((*r)->start_secs() == 0.0);  // fully watched → restart at 0.
    // Progress ratcheted to the 1-based ordinal 5.
    auto g = s->get_progress(200);
    CHECK(**g == 5);
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}

// ===========================================================================
// P14 — store schema growth: catalog_cache, bindings, pins, absences, routes,
// episode_cache, app_meta. Tests ported 1:1 from store.rs (§8 golden-as-port).
// ===========================================================================

// --- catalog_upsert_round_trips (store.rs:2455) ----------------------------
TEST_CASE("catalog_upsert_round_trips") {
  Store store = open_mem();
  REQUIRE(store.upsert_catalog_cache(sample(1), 1000, 2000).has_value());
  auto hit = store.get_catalog(1);
  REQUIRE(hit.has_value());
  REQUIRE(hit->has_value());
  CHECK((*hit)->enrichment == sample(1));
  CHECK((*hit)->fieldset_version == kEnrichmentFieldsetVersion);
  CHECK((*hit)->fetched_at == 1000);
  CHECK((*hit)->expires_at == std::optional<std::int64_t>(2000));
  auto miss = store.get_catalog(2);
  REQUIRE(miss.has_value());
  CHECK_FALSE(miss->has_value());
}

// --- catalog_merge_null_never_wipes (store.rs:2469) ------------------------
TEST_CASE("catalog_merge_null_never_wipes") {
  Store store = open_mem();
  REQUIRE(store.upsert_catalog_cache(sample(1), 1000, std::nullopt).has_value());
  Enrichment partial;
  partial.anilist_id = 1;
  partial.title_romaji = "Show 1 v2";
  REQUIRE(store.upsert_catalog_cache(partial, 3000, std::nullopt).has_value());
  auto hit = store.get_catalog(1);
  REQUIRE(hit->has_value());
  const Enrichment& e = (*hit)->enrichment;
  CHECK(e.title_romaji == "Show 1 v2");
  CHECK(e.description == std::optional<std::string>("desc"));
  CHECK(e.genres.size() == 2);
  CHECK(e.total_episodes == std::optional<std::uint32_t>(12));
  CHECK((*hit)->fetched_at == 3000);
}

// --- cover_url_never_downgrades_from_absolute (store.rs:2487, ROD-267) ------
TEST_CASE("cover_url_never_downgrades_from_absolute") {
  Store store = open_mem();
  auto cover = [&]() {
    auto hit = store.get_catalog(1);
    REQUIRE(hit->has_value());
    return (*hit)->enrichment.cover_url;
  };
  Enrichment relative;
  relative.anilist_id = 1;
  relative.title_romaji = "t";
  relative.cover_url = "images/rel.jpg";
  // A relative cover sticks while nothing better exists.
  REQUIRE(store.upsert_catalog_cache(relative, 100, std::nullopt).has_value());
  CHECK(cover() == std::optional<std::string>("images/rel.jpg"));
  // Absolute replaces relative.
  REQUIRE(store.upsert_catalog_cache(sample(1), 200, std::nullopt).has_value());
  CHECK(cover() == std::optional<std::string>("https://img/cover.png"));
  // Relative never downgrades a stored absolute.
  REQUIRE(store.upsert_catalog_cache(relative, 300, std::nullopt).has_value());
  CHECK(cover() == std::optional<std::string>("https://img/cover.png"));
  // Case-shifted scheme garbage neither sticks nor clobbers (GLOB is
  // case-sensitive; ROD-267).
  Enrichment shouty = relative;
  shouty.cover_url = "HTTPS://img/shout.png";
  REQUIRE(store.upsert_catalog_cache(shouty, 400, std::nullopt).has_value());
  CHECK(cover() == std::optional<std::string>("https://img/cover.png"));
}

// --- empty_lists_keep_prior_values (store.rs:2516, ROD-261) -----------------
TEST_CASE("empty_lists_keep_prior_values") {
  Store store = open_mem();
  REQUIRE(store.upsert_catalog_cache(sample(1), 100, std::nullopt).has_value());
  Enrichment no_lists;
  no_lists.anilist_id = 1;
  no_lists.title_romaji = "t";
  // genres/studios explicitly empty.
  REQUIRE(store.upsert_catalog_cache(no_lists, 200, std::nullopt).has_value());
  auto hit = store.get_catalog(1);
  REQUIRE(hit->has_value());
  const Enrichment& e = (*hit)->enrichment;
  CHECK(e.genres == std::vector<std::string>{"Action", "Drama"});
  CHECK(e.studios == std::vector<std::string>{"MAPPA"});
}

// --- add_to_library_stamps_membership_once (store.rs:2533, L4) --------------
TEST_CASE("add_to_library_stamps_membership_once") {
  Store store = open_mem();
  REQUIRE(store.add_to_library(sample(2), 100).has_value());
  {
    auto show = store.get_show(2);
    REQUIRE(show->has_value());
    CHECK((*show)->library_added_at == std::optional<std::int64_t>(100));
    CHECK((*show)->list_status == ListStatus::Planning);
    CHECK((*show)->progress == 0);
    CHECK((*show)->enrichment == sample(2));
  }
  // Re-add later: enrichment merges, the stamp never moves (L4).
  Enrichment renamed = sample(2);
  renamed.title_romaji = "Renamed";
  REQUIRE(store.add_to_library(renamed, 999).has_value());
  {
    auto show = store.get_show(2);
    REQUIRE(show->has_value());
    CHECK((*show)->library_added_at == std::optional<std::int64_t>(100));
    CHECK((*show)->enrichment.title_romaji == "Renamed");
  }
}

// --- enrichment_patch_never_touches_user_state (store.rs:2553) --------------
TEST_CASE("enrichment_patch_never_touches_user_state") {
  Store store = open_mem();
  REQUIRE(store.add_to_library(sample(3), 100).has_value());
  // Seed user state via the P9 play writer (list_status/progress/play_count/
  // last_watched_at/library_added_at). rating/notes have no writer until P16;
  // they stay nullopt on both sides, which is still a faithful equality check.
  REQUIRE(store.record_finish(3, Translation::Sub, "5", 5, 99.0, 100.0,
                              std::string_view("senshi"), 200)
              .has_value());
  auto before = store.get_show(3);
  REQUIRE(before->has_value());
  const Show b = **before;

  Enrichment fresh = sample(3);
  fresh.description = "new synopsis";
  fresh.score = 90;
  auto patched = store.patch_show_enrichment(fresh, true, 500);
  REQUIRE(patched.has_value());
  CHECK(*patched);

  auto after = store.get_show(3);
  REQUIRE(after->has_value());
  const Show a = **after;
  // The user-state fields are simply absent from the SET clause.
  CHECK(a.list_status == b.list_status);
  CHECK(a.user_rating == b.user_rating);
  CHECK(a.notes == b.notes);
  CHECK(a.play_count == b.play_count);
  CHECK(a.progress == b.progress);
  CHECK(a.library_added_at == b.library_added_at);
  CHECK(a.last_watched_at == b.last_watched_at);
  // The enrichment side did move.
  CHECK(a.enrichment.description == std::optional<std::string>("new synopsis"));
  CHECK(a.enrichment_fetched_at == std::optional<std::int64_t>(500));
  CHECK(a.enrichment_fieldset_version == std::optional<std::uint32_t>(kEnrichmentFieldsetVersion));
}

// --- enrichment_patch_never_mints_a_row (store.rs:2596) --------------------
TEST_CASE("enrichment_patch_never_mints_a_row") {
  Store store = open_mem();
  auto patched = store.patch_show_enrichment(sample(99), true, 100);
  REQUIRE(patched.has_value());
  CHECK_FALSE(*patched);
  auto show = store.get_show(99);
  REQUIRE(show.has_value());
  CHECK_FALSE(show->has_value());
}

// --- stale_total_clears_only_on_stamped_airing_answer (store.rs:2603) -------
TEST_CASE("stale_total_clears_only_on_stamped_airing_answer") {
  Store store = open_mem();
  REQUIRE(store.add_to_library(sample(4), 100).has_value());
  auto total = [&]() {
    auto show = store.get_show(4);
    REQUIRE(show->has_value());
    return (*show)->enrichment.total_episodes;
  };
  CHECK(total() == std::optional<std::uint32_t>(12));

  // Unstamped partial with no total: COALESCE keeps 12.
  Enrichment airing_partial = sample(4);
  airing_partial.status = "RELEASING";
  airing_partial.total_episodes = std::nullopt;
  REQUIRE(store.patch_show_enrichment(airing_partial, false, 200).has_value());
  CHECK(total() == std::optional<std::uint32_t>(12));

  // Stamped full answer, airing, no total: the stored total was an
  // availability snapshot; clear it (ROD-419).
  REQUIRE(store.patch_show_enrichment(airing_partial, true, 300).has_value());
  CHECK_FALSE(total().has_value());

  // Finale lands later.
  Enrichment finished = sample(4);
  finished.total_episodes = 13;
  REQUIRE(store.patch_show_enrichment(finished, true, 400).has_value());
  CHECK(total() == std::optional<std::uint32_t>(13));

  // Stamped FINISHED with a null total keeps the known finale.
  Enrichment finished_no_total = sample(4);
  finished_no_total.total_episodes = std::nullopt;
  REQUIRE(store.patch_show_enrichment(finished_no_total, true, 500).has_value());
  CHECK(total() == std::optional<std::uint32_t>(13));
}

// --- promote_copies_enrichment_and_stamps (store.rs:2922) ------------------
TEST_CASE("promote_copies_enrichment_and_stamps") {
  Store store = open_mem();
  REQUIRE(store.upsert_catalog_cache(sample(40), 100, std::nullopt).has_value());
  {
    auto p = store.promote_catalog_to_show(40, 400);
    REQUIRE(p.has_value());
    CHECK(*p);
  }
  auto show = store.get_show(40);
  REQUIRE(show->has_value());
  CHECK((*show)->enrichment == sample(40));
  CHECK((*show)->library_added_at == std::optional<std::int64_t>(400));
  auto cache = store.get_catalog(40);
  REQUIRE(cache.has_value());
  CHECK(cache->has_value());  // cache row remains.
  {
    auto p = store.promote_catalog_to_show(41, 400);
    REQUIRE(p.has_value());
    CHECK_FALSE(*p);
  }
}

// --- bind_mints_identity_row_without_membership (store.rs:2937) -------------
TEST_CASE("bind_mints_identity_row_without_membership") {
  Store store = open_mem();
  REQUIRE(store.bind_provider(sample(50), "senshi", "abc", 100).has_value());
  {
    auto show = store.get_show(50);
    REQUIRE(show->has_value());
    CHECK_FALSE((*show)->library_added_at.has_value());  // binding never joins History.
    auto hist = store.list_history();
    REQUIRE(hist.has_value());
    CHECK(hist->empty());
    auto b = store.bindings_for(50);
    REQUIRE(b.has_value());
    REQUIRE(b->size() == 1);
    CHECK((*b)[0] == Binding{"senshi", "abc", 100});
    auto id = store.show_id_for_binding("senshi", "abc");
    REQUIRE(id.has_value());
    CHECK(*id == std::optional<std::int64_t>(50));
  }
  // Mint is not a patch: an existing row's enrichment is untouched.
  Enrichment renamed = sample(50);
  renamed.title_romaji = "Other";
  REQUIRE(store.bind_provider(renamed, "megaplay", "m1", 200).has_value());
  {
    auto show = store.get_show(50);
    REQUIRE(show->has_value());
    CHECK((*show)->enrichment.title_romaji == "Show 50");
    auto b = store.bindings_for(50);
    CHECK(b->size() == 2);
  }
}

// --- rebind_steals_the_provider_id_edge (store.rs:2973, 02 O5) --------------
TEST_CASE("rebind_steals_the_provider_id_edge") {
  Store store = open_mem();
  REQUIRE(store.bind_provider(sample(51), "senshi", "dup", 100).has_value());
  // Same (provider, provider_id) resolved to another show: the wrong bind dies,
  // the edge moves (delete + insert).
  REQUIRE(store.bind_provider(sample(52), "senshi", "dup", 200).has_value());
  {
    auto b51 = store.bindings_for(51);
    REQUIRE(b51.has_value());
    CHECK(b51->empty());
    auto id = store.show_id_for_binding("senshi", "dup");
    CHECK(*id == std::optional<std::int64_t>(52));
  }
  // Same show re-binding a new provider id just updates the edge.
  REQUIRE(store.bind_provider(sample(52), "senshi", "dup2", 300).has_value());
  {
    auto edges = store.bindings_for(52);
    REQUIRE(edges->size() == 1);
    CHECK((*edges)[0].provider_id == "dup2");
  }
  {
    auto u1 = store.unbind_provider(52, "senshi");
    REQUIRE(u1.has_value());
    CHECK(*u1);
    auto u2 = store.unbind_provider(52, "senshi");
    REQUIRE(u2.has_value());
    CHECK_FALSE(*u2);
  }
}

// --- absence_ttl_and_availability_precedence (store.rs:3000, ROD-347/348) ---
TEST_CASE("absence_ttl_and_availability_precedence") {
  Store store = open_mem();
  REQUIRE(store.mark_provider_absent(sample(53), "senshi", 1000).has_value());
  {
    auto show = store.get_show(53);
    REQUIRE(show->has_value());
    CHECK_FALSE((*show)->library_added_at.has_value());  // absence never joins History.
  }
  auto fresh = [&](std::int64_t now) {
    auto r = store.provider_absent_fresh(53, "senshi", now);
    REQUIRE(r.has_value());
    return *r;
  };
  CHECK(fresh(1000));
  CHECK(fresh(1000 + kAbsenceTtlSecs - 1));
  CHECK_FALSE(fresh(1000 + kAbsenceTtlSecs));  // stale reads as unchecked.
  auto avail = [&](const char* provider, std::int64_t now) {
    auto r = store.provider_availability(53, provider, now);
    REQUIRE(r.has_value());
    return *r;
  };
  CHECK(avail("senshi", 1000) == ProviderAvailability::Absent);
  CHECK(avail("megaplay", 1000) == ProviderAvailability::Unchecked);
  // Bind clears the negative: bound and absent never coexist.
  REQUIRE(store.bind_provider(sample(53), "senshi", "s53", 2000).has_value());
  CHECK_FALSE(fresh(2000));
  CHECK(avail("senshi", 2000) == ProviderAvailability::Bound);
}

// --- pin_and_route_round_trip (store.rs:3042, ROD-345/398) ------------------
TEST_CASE("pin_and_route_round_trip") {
  Store store = open_mem();
  REQUIRE(store.bind_provider(sample(54), "senshi", "s54", 100).has_value());
  auto pin = [&]() {
    auto r = store.get_provider_pin(54);
    REQUIRE(r.has_value());
    return *r;
  };
  CHECK_FALSE(pin().has_value());
  REQUIRE(store.set_provider_pin(54, std::string_view("senshi")).has_value());
  CHECK(pin() == std::optional<std::string>("senshi"));
  REQUIRE(store.set_provider_pin(54, std::string_view("megaplay")).has_value());
  CHECK(pin() == std::optional<std::string>("megaplay"));
  REQUIRE(store.set_provider_pin(54, std::nullopt).has_value());
  CHECK_FALSE(pin().has_value());

  auto route = [&](std::int64_t id) {
    auto r = store.get_route_pref(id);
    REQUIRE(r.has_value());
    return *r;
  };
  CHECK_FALSE(route(54).has_value());
  REQUIRE(store.set_route_pref(sample(54), "senshi").has_value());
  CHECK(route(54) == std::optional<std::string>("senshi"));
  REQUIRE(store.set_route_pref(sample(54), "megaplay").has_value());
  CHECK(route(54) == std::optional<std::string>("megaplay"));

  // The stamp mints the identity row for a never-resolved show
  // (stamp-before-fetch, 03 §5.3); no membership.
  REQUIRE(store.set_route_pref(sample(77), "senshi").has_value());
  CHECK(route(77) == std::optional<std::string>("senshi"));
  auto hist = store.list_history();
  REQUIRE(hist.has_value());
  for (const Show& sh : *hist) CHECK(sh.enrichment.anilist_id != 77);
}

// --- episode_cache_expiry_is_a_miss (store.rs:3517, ROD-68) -----------------
TEST_CASE("episode_cache_expiry_is_a_miss") {
  Store store = open_mem();
  identity_row(store, 63);
  const std::vector<std::string> eps{"1", "1.5", "SP1"};
  REQUIRE(store.set_episode_cache(63, "senshi", Translation::Sub, eps,
                                  std::string_view("FINISHED"), 1000)
              .has_value());
  {
    auto got = store.get_cached_episodes(63, "senshi", Translation::Sub, 1000);
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    CHECK(**got == eps);
  }
  {
    auto got = store.get_cached_episodes(63, "senshi", Translation::Sub,
                                         1000 + kEpCacheTtlFinishedSecs);
    REQUIRE(got.has_value());
    CHECK_FALSE(got->has_value());  // stale is a miss.
  }
  // An empty listing round-trips as empty, distinct from a miss.
  REQUIRE(store.set_episode_cache(63, "senshi", Translation::Dub, {},
                                  std::string_view("FINISHED"), 1000)
              .has_value());
  {
    auto got = store.get_cached_episodes(63, "senshi", Translation::Dub, 1000);
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    CHECK((*got)->empty());
  }
}

// --- episode_cache_ttl_is_status_aware (store.rs:3555) ----------------------
TEST_CASE("episode_cache_ttl_is_status_aware") {
  CHECK(episode_cache_ttl_secs(std::string_view("FINISHED")) == kEpCacheTtlFinishedSecs);
  CHECK(episode_cache_ttl_secs(std::string_view("releasing")) == kEpCacheTtlReleasingSecs);
  CHECK(episode_cache_ttl_secs(std::string_view("HIATUS")) == kEpCacheTtlDefaultSecs);
  CHECK(episode_cache_ttl_secs(std::nullopt) == kEpCacheTtlDefaultSecs);
}

// --- app_meta round-trips (store.rs meta_get/meta_set) ---------------------
TEST_CASE("app_meta_round_trips") {
  Store store = open_mem();
  {
    auto m = store.meta_get("k");
    REQUIRE(m.has_value());
    CHECK_FALSE(m->has_value());
  }
  REQUIRE(store.meta_set("k", "v1").has_value());
  {
    auto m = store.meta_get("k");
    REQUIRE(m->has_value());
    CHECK(**m == "v1");
  }
  // Upsert overwrites.
  REQUIRE(store.meta_set("k", "v2").has_value());
  {
    auto m = store.meta_get("k");
    CHECK(**m == "v2");
  }
}

// --- schema_has_every_02_table (store.rs:2301) -----------------------------
TEST_CASE("schema_has_every_02_table") {
  const std::string path = tmp_db("all-tables.db");
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
  }
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(
              raw, "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name", -1, &st,
              nullptr) == SQLITE_OK);
  std::vector<std::string> names;
  while (sqlite3_step(st) == SQLITE_ROW) {
    names.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  }
  sqlite3_finalize(st);
  sqlite3_close(raw);
  for (const char* table : {"app_meta", "catalog_cache", "episode_cache", "episode_progress",
                            "provider_absence", "provider_binding", "provider_pin",
                            "provider_route", "show"}) {
    CHECK_MESSAGE(std::find(names.begin(), names.end(), table) != names.end(),
                  "missing table ", table);
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}

// --- delete_show_cascades_children (store.rs:2888, 02 §5 FIX-IN-RUST) -------
TEST_CASE("delete_show_cascades_children") {
  const std::string path = tmp_db("cascade.db");
  {
    auto sr = Store::open(path);
    REQUIRE(sr.has_value());
    Store& store = *sr;
    REQUIRE(store.add_to_library(sample(30), 100).has_value());
    REQUIRE(store.bind_provider(sample(30), "senshi", "x", 0).has_value());
    REQUIRE(store.save_progress(30, Translation::Sub, "1", 40.0, 100.0, std::nullopt, 0)
                .has_value());
    auto d = store.delete_show(30);
    REQUIRE(d.has_value());
    CHECK(*d);
  }
  // Verify no orphans remain in the FK-child tables.
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(
              raw,
              "SELECT (SELECT count(*) FROM provider_binding WHERE anilist_id = 30) "
              "+ (SELECT count(*) FROM episode_progress WHERE anilist_id = 30)",
              -1, &st, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(st) == SQLITE_ROW);
  CHECK(sqlite3_column_int64(st, 0) == 0);
  sqlite3_finalize(st);
  sqlite3_close(raw);
  {
    auto sr = Store::open(path);
    REQUIRE(sr.has_value());
    auto d = sr->delete_show(30);
    REQUIRE(d.has_value());
    CHECK_FALSE(*d);  // gone already.
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}

// --- v1 DB upgrades to v2 by ADDING the seven tables (shigoku ladder) -------
// Shigoku-specific: our v1 already carries the full `show` columns + progress_
// stamped_at, so unlike sabigoku's v1→v2 (a stamp backfill) the v2 step ADDS
// the remaining 02 tables. Seed a raw v1 DB (show + episode_progress, user_
// version=1), open through the Store, and prove the upgrade lands the tables,
// preserves data, and re-opens idempotently.
// --- AniList sync (P20, 06 §5) -----------------------------------------------

namespace {

RemoteEntry remote_seed(std::int64_t id, ListStatus status, std::uint32_t progress,
                         std::string_view title, std::int64_t updated_at = 0,
                         std::uint32_t score = 0) {
  RemoteEntry e;
  e.anilist_id = id;
  e.status = status;
  e.progress = progress;
  e.score = score;
  e.updated_at = updated_at;
  Enrichment seed;
  seed.anilist_id = id;
  seed.title_romaji = std::string(title);
  e.import_seed = seed;
  return e;
}

// A library row with a known status/progress (no snapshot yet: never synced).
void lib_row(Store& s, std::int64_t id, ListStatus status, std::uint32_t progress) {
  REQUIRE(s.add_to_library(sample(id), 10).has_value());
  REQUIRE(s.restore_list_status(id, status, progress, 10).has_value());
}

std::pair<ListStatus, std::uint32_t> sync_state(Store& s, std::int64_t id) {
  auto g = s.get_show(id);
  REQUIRE(g.has_value());
  REQUIRE(g->has_value());
  return {(*g)->list_status, (*g)->progress};
}

// Fresh file-backed Store + a cleanup callback, for tests that need two
// connections open on the same file (the reconcile CAS guard is built for
// exactly this cross-connection concurrency; open_memory() connections don't
// share state).
struct FileStore {
  std::string path;
  Store store;

  ~FileStore() {
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
  }
};

FileStore open_file(const char* name) {
  std::string path = tmp_db(name);
  auto s = Store::open(path);
  REQUIRE(s.has_value());
  return FileStore{std::move(path), std::move(*s)};
}

}  // namespace

TEST_CASE("reconcile_matrix_covers_every_cell") {
  using store_detail::reconcile;
  // no/no: unchanged, no conflict.
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 0u}, {ListStatus::Watching, 3u, 0u},
                        {ListStatus::Watching, 3u, 0u});
    CHECK(r.status == ListStatus::Watching);
    CHECK_FALSE(r.conflict);
  }
  // no/yes: adopt remote.
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 0u}, {ListStatus::Watching, 3u, 0u},
                        {ListStatus::Completed, 12u, 0u});
    CHECK(r.status == ListStatus::Completed);
    CHECK_FALSE(r.conflict);
  }
  // yes/no: keep local.
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 0u}, {ListStatus::Paused, 3u, 0u},
                        {ListStatus::Watching, 3u, 0u});
    CHECK(r.status == ListStatus::Paused);
    CHECK_FALSE(r.conflict);
  }
  // yes/yes, same target: keep local, converged, no conflict.
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 0u}, {ListStatus::Dropped, 3u, 0u},
                        {ListStatus::Dropped, 3u, 0u});
    CHECK(r.status == ListStatus::Dropped);
    CHECK_FALSE(r.conflict);
  }
  // yes/yes, different: keep local, conflict.
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 0u}, {ListStatus::Dropped, 3u, 0u},
                        {ListStatus::Completed, 12u, 0u});
    CHECK(r.status == ListStatus::Dropped);
    CHECK(r.conflict);
  }

  using store_detail::merge_progress;
  CHECK(merge_progress(3, 3, 3) == 3);  // no/no: unchanged.
  CHECK(merge_progress(3, 3, 5) == 5);  // no/yes: adopt remote, downward included.
  CHECK(merge_progress(3, 7, 3) == 7);  // yes/no: keep local.
  CHECK(merge_progress(3, 7, 5) == 7);  // yes/yes: max(local, remote).
  CHECK(merge_progress(3, 5, 7) == 7);
}

// --- score's own three-way cell (P34 slice 2) -------------------------------

TEST_CASE("reconcile_score_matrix_mirrors_status_not_progress") {
  using store_detail::reconcile;
  // no/no: unchanged.
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 50u}, {ListStatus::Watching, 3u, 50u},
                        {ListStatus::Watching, 3u, 50u});
    CHECK(r.score == 50);
    CHECK_FALSE(r.conflict);
  }
  // no/yes: adopt remote, even downward (unlike progress's rise bias — a
  // lowered score is as intentional an edit as a raised one).
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 80u}, {ListStatus::Watching, 3u, 80u},
                        {ListStatus::Watching, 3u, 20u});
    CHECK(r.score == 20);
    CHECK_FALSE(r.conflict);
  }
  // yes/no: keep local.
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 50u}, {ListStatus::Watching, 3u, 90u},
                        {ListStatus::Watching, 3u, 50u});
    CHECK(r.score == 90);
    CHECK_FALSE(r.conflict);
  }
  // yes/yes, different: keep local, conflict (score conflict alone still
  // flags the row's overall conflict bit — same shared bit as status).
  {
    auto r = reconcile(ListEntry{ListStatus::Watching, 3u, 50u}, {ListStatus::Watching, 3u, 90u},
                        {ListStatus::Watching, 3u, 10u});
    CHECK(r.score == 90);
    CHECK(r.conflict);
  }
}

TEST_CASE("reconcile_first_contact_treats_base_as_planning") {
  using store_detail::reconcile;
  // No snapshot (nullopt): eff_base is Planning/0/0, exactly like an explicit
  // (Planning, 0, 0) base.
  auto r = reconcile(std::nullopt, {ListStatus::Watching, 3u, 0u}, {ListStatus::Planning, 0u, 0u});
  CHECK(r.status == ListStatus::Watching);  // local moved off Planning, remote did not.
  CHECK(r.progress == 3);
  CHECK_FALSE(r.conflict);
  // Snapshot re-baselines to the raw remote triple on first contact even
  // though it differs from local: server truth in the snapshot is what keeps
  // a kept-local row dirty for the next push.
  CHECK(r.snapshot_status == ListStatus::Planning);
  CHECK(r.snapshot_progress == 0);
  CHECK(r.snapshot_score == 0);
}

TEST_CASE("sync_dirty_set_tracks_the_live_pair") {
  Store store = open_mem();
  // Identity-only row (no library_added_at): never dirty.
  identity_row(store, 1);
  CHECK(store.list_dirty_for_sync()->empty());

  // Never-synced library row: dirty, synced == nullopt.
  lib_row(store, 2, ListStatus::Watching, 3);
  {
    auto dirty = store.list_dirty_for_sync();
    REQUIRE(dirty.has_value());
    REQUIRE(dirty->size() == 1);
    CHECK((*dirty)[0].anilist_id == 2);
    CHECK_FALSE((*dirty)[0].synced.has_value());
  }

  // mark_synced clears it.
  REQUIRE(store.mark_synced(2, ListStatus::Watching, 3, 0).has_value());
  CHECK(store.list_dirty_for_sync()->empty());

  // A later local edit re-dirties it; the OLD snapshot is preserved (not the
  // live pair) — this is exactly what the push guard reads.
  REQUIRE(store.restore_list_status(2, ListStatus::Watching, 5, 20).has_value());
  {
    auto dirty = store.list_dirty_for_sync();
    REQUIRE(dirty.has_value());
    REQUIRE(dirty->size() == 1);
    REQUIRE((*dirty)[0].synced.has_value());
    CHECK((*dirty)[0].synced->status == ListStatus::Watching);
    CHECK((*dirty)[0].synced->progress == 3);
  }
}

TEST_CASE("sync_dirty_set_tracks_the_live_score") {
  Store store = open_mem();
  lib_row(store, 3, ListStatus::Watching, 3);
  REQUIRE(store.mark_synced(3, ListStatus::Watching, 3, 0).has_value());
  CHECK(store.list_dirty_for_sync()->empty());

  // A score-only edit dirties the row even though status/progress agree with
  // the snapshot (IFNULL(synced_score,0) <> IFNULL(user_score,0)).
  REQUIRE(store.set_user_score(3, 75).has_value());
  {
    auto dirty = store.list_dirty_for_sync();
    REQUIRE(dirty.has_value());
    REQUIRE(dirty->size() == 1);
    CHECK((*dirty)[0].score == 75);
    REQUIRE((*dirty)[0].synced.has_value());
    CHECK((*dirty)[0].synced->score == 0);
  }

  REQUIRE(store.mark_synced(3, ListStatus::Watching, 3, 75).has_value());
  CHECK(store.list_dirty_for_sync()->empty());
}

// --- MAL mirror (P31 §9.1 slice 4) -------------------------------------------

TEST_CASE("mal_mirror_dirty_set_skips_null_mal_id_and_tracks_the_live_pair") {
  Store store = open_mem();
  // Identity-only row: never dirty (not in the library).
  identity_row(store, 800);
  CHECK(store.list_dirty_for_mal_mirror()->empty());

  // A library row with no mal_id: never surfaces, dirty or not.
  Enrichment no_mal;
  no_mal.anilist_id = 801;
  no_mal.title_romaji = "No MAL Link";
  REQUIRE(store.add_to_library(no_mal, 10).has_value());
  REQUIRE(store.restore_list_status(801, ListStatus::Watching, 3, 10).has_value());
  CHECK(store.list_dirty_for_mal_mirror()->empty());

  // A library row WITH a mal_id (sample() sets mal_id = 100 + id): dirty,
  // synced == nullopt on first contact.
  lib_row(store, 802, ListStatus::Watching, 3);
  {
    auto dirty = store.list_dirty_for_mal_mirror();
    REQUIRE(dirty.has_value());
    REQUIRE(dirty->size() == 1);
    CHECK((*dirty)[0].anilist_id == 802);
    CHECK((*dirty)[0].mal_id == 902);
    CHECK_FALSE((*dirty)[0].synced.has_value());
  }

  // mark_mal_synced clears it, and does NOT touch the AniList-sync snapshot:
  // row 802 is still AniList-dirty (row 801 also is, having no mal_id at all
  // — it never enters the mal-mirror set but is a perfectly ordinary AniList
  // sync row).
  REQUIRE(store.mark_mal_synced(802, ListStatus::Watching, 3, 0).has_value());
  CHECK(store.list_dirty_for_mal_mirror()->empty());
  {
    auto ani_dirty = store.list_dirty_for_sync();
    REQUIRE(ani_dirty.has_value());
    bool row_802_still_ani_dirty = false;
    for (const auto& r : *ani_dirty) {
      if (r.anilist_id == 802) row_802_still_ani_dirty = true;
    }
    CHECK(row_802_still_ani_dirty);
  }

  // A later local edit re-dirties the mirror; the OLD mirror snapshot is
  // preserved (not the live pair) — this is exactly what push_mirror's guard
  // reads.
  REQUIRE(store.restore_list_status(802, ListStatus::Watching, 5, 20).has_value());
  {
    auto dirty = store.list_dirty_for_mal_mirror();
    REQUIRE(dirty.has_value());
    REQUIRE(dirty->size() == 1);
    REQUIRE((*dirty)[0].synced.has_value());
    CHECK((*dirty)[0].synced->first == ListStatus::Watching);
    CHECK((*dirty)[0].synced->second == 3);
  }
}

TEST_CASE("mal_mirror_snapshot_is_independent_of_the_anilist_sync_snapshot") {
  Store store = open_mem();
  lib_row(store, 810, ListStatus::Watching, 3);

  // Advance only the AniList-sync snapshot.
  REQUIRE(store.mark_synced(810, ListStatus::Watching, 3, 0).has_value());
  CHECK(store.list_dirty_for_sync()->empty());
  // The MAL mirror's own snapshot was never touched: still dirty there.
  CHECK(store.list_dirty_for_mal_mirror()->size() == 1);

  // Advance only the MAL-mirror snapshot on a fresh row.
  lib_row(store, 811, ListStatus::Completed, 12);
  REQUIRE(store.mark_mal_synced(811, ListStatus::Completed, 12, 0).has_value());
  CHECK(store.list_dirty_for_mal_mirror()->size() == 1);  // 810 only.
  CHECK(store.list_dirty_for_sync()->size() == 1);        // 811 only.
}

TEST_CASE("pull_conflict_keeps_local_and_stays_dirty") {
  Store store = open_mem();
  lib_row(store, 5, ListStatus::Watching, 3);
  REQUIRE(store.mark_synced(5, ListStatus::Watching, 3, 0).has_value());
  REQUIRE(store.restore_list_status(5, ListStatus::Dropped, 3, 20).has_value());

  auto out = store.reconcile_pull({remote_seed(5, ListStatus::Completed, 12, "Show 5")}, 50);
  REQUIRE(out.has_value());
  CHECK(out->reconciled == 1);
  CHECK(out->conflicts == 1);
  CHECK(sync_state(store, 5).first == ListStatus::Dropped);  // local kept.
  // Snapshot re-baselined to the raw remote pair, so the row stays dirty.
  CHECK(store.list_dirty_for_sync()->size() == 1);
}

TEST_CASE("pull_adopts_a_remote_score_for_a_never_scored_row") {
  Store store = open_mem();
  lib_row(store, 6, ListStatus::Watching, 3);  // user_score NULL: never scored.
  auto out = store.reconcile_pull(
      {remote_seed(6, ListStatus::Watching, 3, "Show 6", /*updated_at=*/0, /*score=*/80)}, 50);
  REQUIRE(out.has_value());
  CHECK(out->reconciled == 1);
  CHECK(out->conflicts == 0);
  auto g = store.get_show(6);
  REQUIRE(g->has_value());
  REQUIRE((*g)->user_score.has_value());
  CHECK(*(*g)->user_score == 80);
}

TEST_CASE("pull_keeps_a_locally_set_score_the_remote_never_touched") {
  Store store = open_mem();
  lib_row(store, 7, ListStatus::Watching, 3);
  REQUIRE(store.mark_synced(7, ListStatus::Watching, 3, 0).has_value());
  REQUIRE(store.set_user_score(7, 90).has_value());  // local edit, still base score 0.

  // Remote status/progress moved (so the row isn't a no-op skip) but its
  // score stayed at the base (0): the local score alone-moved cell keeps it.
  auto out = store.reconcile_pull(
      {remote_seed(7, ListStatus::Completed, 12, "Show 7", /*updated_at=*/0, /*score=*/0)}, 50);
  REQUIRE(out.has_value());
  CHECK(out->reconciled == 1);
  CHECK(out->conflicts == 0);
  auto g = store.get_show(7);
  REQUIRE(g->has_value());
  REQUIRE((*g)->user_score.has_value());
  CHECK(*(*g)->user_score == 90);
}

TEST_CASE("pull_score_conflict_keeps_local_and_flags_conflict") {
  Store store = open_mem();
  lib_row(store, 8, ListStatus::Watching, 3);
  REQUIRE(store.mark_synced(8, ListStatus::Watching, 3, 50).has_value());
  REQUIRE(store.set_user_score(8, 90).has_value());  // local moved off the base 50.

  // Remote also moved off the base 50, to a different value: both moved and
  // disagree, so local wins and the row's conflict bit is set.
  auto out = store.reconcile_pull(
      {remote_seed(8, ListStatus::Watching, 3, "Show 8", /*updated_at=*/0, /*score=*/20)}, 50);
  REQUIRE(out.has_value());
  CHECK(out->reconciled == 1);
  CHECK(out->conflicts == 1);
  auto g = store.get_show(8);
  REQUIRE(g->has_value());
  REQUIRE((*g)->user_score.has_value());
  CHECK(*(*g)->user_score == 90);
}

TEST_CASE("pull_reports_unmatched_remote_ids_without_importing") {
  Store store = open_mem();
  auto out = store.reconcile_pull({remote_seed(700, ListStatus::Completed, 12, "Show 700")}, 50);
  REQUIRE(out.has_value());
  CHECK(out->imported == 0);
  CHECK(out->unmatched == std::vector<std::int64_t>{700});
  CHECK_FALSE(store.get_show(700)->has_value());
}

TEST_CASE("pull_does_not_import_non_watching_seeds") {
  Store store = open_mem();
  auto out = store.reconcile_pull({remote_seed(701, ListStatus::Planning, 0, "Show 701")}, 50);
  REQUIRE(out.has_value());
  CHECK(out->imported == 0);
  CHECK(out->unmatched == std::vector<std::int64_t>{701});
}

TEST_CASE("pull_rejects_a_control_only_title_seed") {
  Store store = open_mem();
  RemoteEntry e;
  e.anilist_id = 706;
  e.status = ListStatus::Watching;
  e.progress = 1;
  Enrichment seed;
  seed.anilist_id = 706;
  seed.title_romaji = "";
  seed.title_english = "";
  e.import_seed = seed;

  auto out = store.reconcile_pull({e}, 50);
  REQUIRE(out.has_value());
  CHECK(out->imported == 0);
  CHECK(out->unmatched == std::vector<std::int64_t>{706});
  CHECK_FALSE(store.get_show(706)->has_value());
}

TEST_CASE("pull_imports_a_watching_seed_into_the_library") {
  Store store = open_mem();
  auto out = store.reconcile_pull({remote_seed(710, ListStatus::Watching, 4, "New Show")}, 50);
  REQUIRE(out.has_value());
  CHECK(out->imported == 1);
  CHECK(out->unmatched.empty());
  auto g = store.get_show(710);
  REQUIRE(g->has_value());
  CHECK((*g)->list_status == ListStatus::Watching);
  CHECK((*g)->progress == 4);
}

TEST_CASE("imported_row_is_born_clean_and_never_pushed_back") {
  Store store = open_mem();
  auto out = store.reconcile_pull({remote_seed(711, ListStatus::Watching, 4, "New Show")}, 50);
  REQUIRE(out.has_value());
  CHECK(out->imported == 1);
  // The stamped snapshot matches the just-adopted pair: not dirty.
  CHECK(store.list_dirty_for_sync()->empty());
}

TEST_CASE("pull_import_promotes_an_identity_row_without_wiping_enrichment") {
  Store store = open_mem();
  // A richer identity row exists (bound provider, never library-added).
  Enrichment rich;
  rich.anilist_id = 704;
  rich.title_romaji = "Full Title";
  rich.cover_url = "https://img/cover.jpg";
  REQUIRE(store.bind_provider(rich, "senshi", "abc", 10).has_value());
  {
    auto g = store.get_show(704);
    REQUIRE(g->has_value());
    CHECK_FALSE((*g)->library_added_at.has_value());
  }

  // A sparse WATCHING seed for the same id promotes it into the library.
  auto out = store.reconcile_pull({remote_seed(704, ListStatus::Watching, 2, "Seed Title")}, 50);
  REQUIRE(out.has_value());
  CHECK(out->imported == 1);
  auto g = store.get_show(704);
  REQUIRE(g->has_value());
  CHECK((*g)->library_added_at == 50);
  // Sparse seed did not clobber the richer existing cover (a field the seed
  // never sets).
  CHECK((*g)->enrichment.cover_url == "https://img/cover.jpg");
}

TEST_CASE("import_leaves_a_concurrently_added_row_for_next_run") {
  // The import path's CAS guard, mirror of the matched-path race: an
  // add-and-edit landing between the plan read and apply must survive, not
  // be clobbered by the import stamp.
  FileStore fs = open_file("import-cas.db");
  auto plan = fs.store.reconcile_plan({remote_seed(800, ListStatus::Watching, 5, "Frieren")});
  REQUIRE(plan.has_value());
  REQUIRE(plan->imports.size() == 1);

  // A second connection adds the show and sets a real status before apply.
  {
    auto other = Store::open(fs.path);
    REQUIRE(other.has_value());
    REQUIRE(other->add_to_library(sample(800), 10).has_value());
    REQUIRE(other->restore_list_status(800, ListStatus::Completed, 20, 10).has_value());
  }

  auto out = fs.store.apply_reconcile(plan->plan, plan->imports, plan->unmatched, 0);
  REQUIRE(out.has_value());
  CHECK(out->contended == std::vector<std::int64_t>{800});
  CHECK(out->imported == 0);
  // The concurrent edit survives; the mint rolled back.
  CHECK(sync_state(fs.store, 800).first == ListStatus::Completed);
  CHECK(sync_state(fs.store, 800).second == 20);
}

// The score arm of the same CAS (P34 review finding): a score edit landing
// between the plan read and apply must gate the write on its own — deleting
// the guard's user_score clause would pass every other test in the suite.
TEST_CASE("plan_row_cas_score_contention_leaves_the_row_untouched") {
  FileStore fs = open_file("plan-cas-score.db");
  REQUIRE(fs.store.add_to_library(sample(802), 10).has_value());
  REQUIRE(fs.store.restore_list_status(802, ListStatus::Watching, 3, 10).has_value());
  REQUIRE(fs.store.mark_synced(802, ListStatus::Watching, 3, 0).has_value());

  auto plan = fs.store.reconcile_plan({remote_seed(802, ListStatus::Completed, 12, "Show 802")});
  REQUIRE(plan.has_value());
  REQUIRE(plan->plan.size() == 1);

  // A second connection moves ONLY the local score before apply lands;
  // status/progress still match the plan's guard.
  {
    auto other = Store::open(fs.path);
    REQUIRE(other.has_value());
    REQUIRE(other->set_user_score(802, 90).has_value());
  }

  auto out = fs.store.apply_reconcile(plan->plan, plan->imports, plan->unmatched, 30);
  REQUIRE(out.has_value());
  CHECK(out->contended == std::vector<std::int64_t>{802});
  CHECK(out->reconciled == 0);
  // The concurrent score edit survives untouched.
  auto g = fs.store.get_show(802);
  REQUIRE(g->has_value());
  REQUIRE((*g)->user_score.has_value());
  CHECK(*(*g)->user_score == 90);
}

// mark_synced's adopt arm (P34 lossy-format convergence): the local score
// snaps to the server's quantized value only while it still equals the raw
// value the push read — a mid-push edit is left alone and stays dirty.
TEST_CASE("mark_synced_adopts_the_quantized_score_only_uncontended") {
  Store store = open_mem();
  REQUIRE(store.add_to_library(sample(803), 10).has_value());

  // Uncontended: raw 75 pushed, server stored 80 -> local adopts 80.
  REQUIRE(store.set_user_score(803, 75).has_value());
  REQUIRE(store.mark_synced(803, ListStatus::Planning, 0, 80, 75).has_value());
  {
    auto g = store.get_show(803);
    REQUIRE(g->has_value());
    CHECK(*(*g)->user_score == 80);
  }
  CHECK(store.list_dirty_for_sync()->empty());

  // Contended: the local score moved (95) after the push read 80 -> the
  // fresher edit survives and the row stays dirty for the next run.
  REQUIRE(store.set_user_score(803, 95).has_value());
  REQUIRE(store.mark_synced(803, ListStatus::Planning, 0, 80, 90).has_value());
  {
    auto g = store.get_show(803);
    REQUIRE(g->has_value());
    CHECK(*(*g)->user_score == 95);
  }
  CHECK(store.list_dirty_for_sync()->size() == 1);

  // The no-adopt overload (defaulted param) never touches user_score.
  REQUIRE(store.mark_synced(803, ListStatus::Planning, 0, 40).has_value());
  {
    auto g = store.get_show(803);
    REQUIRE(g->has_value());
    CHECK(*(*g)->user_score == 95);
  }
}

TEST_CASE("plan_row_cas_contention_leaves_the_row_untouched") {
  // Mirror of the import-path race, for the matched (non-import) branch: an
  // edit landing between the plan read and apply must gate the write, not
  // clobber the concurrent value.
  FileStore fs = open_file("plan-cas.db");
  REQUIRE(fs.store.add_to_library(sample(801), 10).has_value());
  REQUIRE(fs.store.restore_list_status(801, ListStatus::Watching, 3, 10).has_value());
  REQUIRE(fs.store.mark_synced(801, ListStatus::Watching, 3, 0).has_value());

  auto plan = fs.store.reconcile_plan({remote_seed(801, ListStatus::Completed, 12, "Show 801")});
  REQUIRE(plan.has_value());
  REQUIRE(plan->plan.size() == 1);

  // A second connection moves the local pair before this run's apply lands.
  {
    auto other = Store::open(fs.path);
    REQUIRE(other.has_value());
    REQUIRE(other->restore_list_status(801, ListStatus::Dropped, 3, 20).has_value());
  }

  auto out = fs.store.apply_reconcile(plan->plan, plan->imports, plan->unmatched, 30);
  REQUIRE(out.has_value());
  CHECK(out->contended == std::vector<std::int64_t>{801});
  CHECK(out->reconciled == 0);
  // The concurrent edit survives untouched.
  CHECK(sync_state(fs.store, 801).first == ListStatus::Dropped);
  CHECK(sync_state(fs.store, 801).second == 3);
}

TEST_CASE("pull_caps_import_count_and_counts_overflow_as_unmatched") {
  Store store = open_mem();
  std::vector<RemoteEntry> entries;
  for (std::int64_t i = 0; i < 510; ++i) {
    entries.push_back(remote_seed(1000 + i, ListStatus::Watching, 1, "Show"));
  }
  auto out = store.reconcile_pull(entries, 50);
  REQUIRE(out.has_value());
  CHECK(out->imported == 500);
  CHECK(out->unmatched.size() == 10);
  for (auto id : out->unmatched) CHECK(id >= 1000 + 500);
}

TEST_CASE("pull_collapses_duplicate_remote_ids_keeping_the_freshest") {
  Store store = open_mem();
  lib_row(store, 900, ListStatus::Watching, 1);
  REQUIRE(store.mark_synced(900, ListStatus::Watching, 1, 0).has_value());

  // Two copies of the same id in different custom lists; the fresher
  // (updatedAt, progress) wins regardless of wire order.
  auto older = remote_seed(900, ListStatus::Watching, 3, "Show 900", /*updated_at=*/10);
  auto newer = remote_seed(900, ListStatus::Completed, 12, "Show 900", /*updated_at=*/20);
  {
    auto out = store.reconcile_pull({older, newer}, 50);
    REQUIRE(out.has_value());
    CHECK(sync_state(store, 900).first == ListStatus::Completed);
    CHECK(sync_state(store, 900).second == 12);
  }

  // Order-independence: swapping wire order does not change the winner.
  REQUIRE(store.mark_synced(900, ListStatus::Completed, 12, 0).has_value());
  {
    auto out = store.reconcile_pull({newer, older}, 60);
    REQUIRE(out.has_value());
    CHECK(sync_state(store, 900).first == ListStatus::Completed);
    CHECK(sync_state(store, 900).second == 12);
  }
}

TEST_CASE("v1_database_upgrades_by_adding_p14_tables") {
  const std::string path = tmp_db("v1-upgrade.db");
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    // The exact v1 DDL: show (full 02 columns) + episode_progress. Kept in sync
    // with kMigrationV1; a drift here is caught by the data-survival asserts.
    const char* v1_ddl =
        "CREATE TABLE show ("
        "  anilist_id INTEGER PRIMARY KEY, mal_id INTEGER, title_romaji TEXT NOT NULL,"
        "  title_english TEXT, title_native TEXT, cover_url TEXT, total_episodes INTEGER,"
        "  duration_minutes INTEGER, year INTEGER, season TEXT, status TEXT, description TEXT,"
        "  score INTEGER, kind TEXT, start_year INTEGER, start_month INTEGER, start_day INTEGER,"
        "  genres TEXT, studios TEXT, source_material TEXT, rank INTEGER, rank_type TEXT,"
        "  rank_year INTEGER, next_airing_at INTEGER, next_airing_episode INTEGER, country TEXT,"
        "  enrichment_fetched_at INTEGER, enrichment_fieldset_version INTEGER,"
        "  list_status TEXT NOT NULL DEFAULT 'planning', user_rating REAL, notes TEXT,"
        "  play_count INTEGER NOT NULL DEFAULT 0, progress INTEGER NOT NULL DEFAULT 0,"
        "  progress_stamped_at INTEGER, library_added_at INTEGER, last_watched_at INTEGER,"
        "  synced_status TEXT, synced_progress INTEGER);"
        "CREATE TABLE episode_progress ("
        "  anilist_id INTEGER NOT NULL REFERENCES show(anilist_id) ON DELETE CASCADE,"
        "  translation TEXT NOT NULL, episode TEXT NOT NULL, position_secs REAL NOT NULL DEFAULT 0,"
        "  duration_secs REAL NOT NULL DEFAULT 0, fully_watched INTEGER NOT NULL DEFAULT 0,"
        "  updated_at INTEGER NOT NULL, last_provider TEXT,"
        "  PRIMARY KEY (anilist_id, translation, episode));";
    REQUIRE(sqlite3_exec(raw, v1_ddl, nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw,
                         "INSERT INTO show (anilist_id, title_romaji, progress, library_added_at) "
                         "VALUES (9, 'Held', 7, 500);"
                         "INSERT INTO episode_progress "
                         "(anilist_id, translation, episode, position_secs, updated_at) "
                         "VALUES (9, 'sub', '3', 40.0, 500);",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 1", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  // Open through the Store: v1 → v2 upgrade adds the P14 tables.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    // Pre-existing data survives.
    auto show = s->get_show(9);
    REQUIRE(show->has_value());
    CHECK((*show)->enrichment.title_romaji == "Held");
    CHECK((*show)->progress == 7);
    CHECK((*show)->library_added_at == std::optional<std::int64_t>(500));
    auto r = s->get_resume(9, Translation::Sub, "3");
    REQUIRE(r->has_value());
    CHECK((*r)->position_secs == doctest::Approx(40.0));
    // The new tables exist and are usable (a bind would fail on a missing table).
    REQUIRE(s->bind_provider(sample(9), "senshi", "x9", 600).has_value());
    REQUIRE(s->meta_set("upgraded", "yes").has_value());
  }
  // Verify user_version is now 6 (v1 upgrades all the way through v6 in one
  // open), the seven P14 tables landed, and the v3/v4/v5/v6 columns exist.
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(st, 0) == 6);
    sqlite3_finalize(st);
    for (const char* table : {"provider_binding", "episode_cache", "provider_pin",
                              "provider_absence", "provider_route", "catalog_cache", "app_meta"}) {
      const std::string q =
          "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='" + std::string(table) +
          "'";
      REQUIRE(sqlite3_prepare_v2(raw, q.c_str(), -1, &st, nullptr) == SQLITE_OK);
      REQUIRE(sqlite3_step(st) == SQLITE_ROW);
      CHECK_MESSAGE(sqlite3_column_int64(st, 0) == 1, "missing after upgrade: ", table);
      sqlite3_finalize(st);
    }
    REQUIRE(sqlite3_prepare_v2(raw,
                               "SELECT mal_synced_status, mal_synced_progress, mal_synced_score "
                               "FROM show",
                               -1, &st, nullptr) == SQLITE_OK);
    sqlite3_finalize(st);
    REQUIRE(sqlite3_prepare_v2(raw, "SELECT user_score, synced_score FROM show",
                               -1, &st, nullptr) == SQLITE_OK);
    sqlite3_finalize(st);
    REQUIRE(sqlite3_prepare_v2(raw, "SELECT notice_last_episode, notice_pending FROM show",
                               -1, &st, nullptr) == SQLITE_OK);
    sqlite3_finalize(st);
    sqlite3_close(raw);
  }
  // Re-open is idempotent: migration is a no-op, data (incl. the new bind)
  // survives.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    auto b = s->bindings_for(9);
    REQUIRE(b.has_value());
    REQUIRE(b->size() == 1);
    CHECK((*b)[0].provider_id == "x9");
    auto m = s->meta_get("upgraded");
    REQUIRE(m->has_value());
    CHECK(**m == "yes");
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}

TEST_CASE("v3_database_upgrades_by_adding_user_score_column") {
  const std::string path = tmp_db("v3-upgrade.db");
  {
    // A v3-shaped DB (mal-mirror columns present, no user_score/synced_score
    // yet) — exactly what a pre-P34 install has on disk.
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    REQUIRE(s->add_to_library(sample(9), 500).has_value());
    REQUIRE(s->restore_list_status(9, ListStatus::Watching, 3, 500).has_value());
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "ALTER TABLE show DROP COLUMN user_score;"
                              "ALTER TABLE show DROP COLUMN synced_score;"
                              "ALTER TABLE show DROP COLUMN mal_synced_score;"
                              "ALTER TABLE show DROP COLUMN notice_last_episode;"
                              "ALTER TABLE show DROP COLUMN notice_pending;",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 3", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  // Open through the Store: v3 -> v4 upgrade adds user_score/synced_score,
  // v4 -> v5 adds mal_synced_score, v5 -> v6 adds the P37 notice columns, all
  // in the same open.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    // Pre-existing data survives untouched.
    auto show = s->get_show(9);
    REQUIRE(show->has_value());
    CHECK((*show)->list_status == ListStatus::Watching);
    CHECK((*show)->progress == 3);
    // The new column defaults to unset (NULL), not 0-as-a-real-score.
    CHECK_FALSE((*show)->user_score.has_value());
    CHECK_FALSE((*show)->synced_score.has_value());
    CHECK_FALSE((*show)->notice_last_episode.has_value());
    CHECK_FALSE((*show)->notice_pending);
    // A write through the new writer round-trips.
    REQUIRE(s->set_user_score(9, 75).has_value());
  }
  // Verify user_version is now 6, and the write survives reopen.
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(st, 0) == 6);
    sqlite3_finalize(st);
    sqlite3_close(raw);
  }
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    auto show = s->get_show(9);
    REQUIRE(show->has_value());
    REQUIRE((*show)->user_score.has_value());
    CHECK(*(*show)->user_score == 75);
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}

TEST_CASE("v4_database_upgrades_by_adding_mal_synced_score_column") {
  const std::string path = tmp_db("v4-upgrade.db");
  {
    // A v4-shaped DB (user_score/synced_score present, no mal_synced_score
    // yet) — exactly what a pre-P34-slice-2 install has on disk.
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    REQUIRE(s->add_to_library(sample(9), 500).has_value());  // sample() sets mal_id = 109.
    REQUIRE(s->restore_list_status(9, ListStatus::Watching, 3, 500).has_value());
    REQUIRE(s->set_user_score(9, 60).has_value());
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "ALTER TABLE show DROP COLUMN mal_synced_score;"
                              "ALTER TABLE show DROP COLUMN notice_last_episode;"
                              "ALTER TABLE show DROP COLUMN notice_pending;",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 4", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  // Open through the Store: v4 -> v5 upgrade adds mal_synced_score, v5 -> v6
  // adds the P37 notice columns, both in this same open.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    auto show = s->get_show(9);
    REQUIRE(show->has_value());
    REQUIRE((*show)->user_score.has_value());
    CHECK(*(*show)->user_score == 60);
    // The mirror is dirty (mal_synced_score starts NULL) and carries the
    // already-converted MAL-scale score.
    auto dirty = s->list_dirty_for_mal_mirror();
    REQUIRE(dirty.has_value());
    REQUIRE(dirty->size() == 1);
    CHECK((*dirty)[0].score == 6);  // round(60/10).
    CHECK_FALSE((*dirty)[0].synced_score.has_value());
    REQUIRE(s->mark_mal_synced(9, ListStatus::Watching, 3, 6).has_value());
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(st, 0) == 6);
    sqlite3_finalize(st);
    sqlite3_close(raw);
  }
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    CHECK(s->list_dirty_for_mal_mirror()->empty());
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}

TEST_CASE("v2_database_upgrades_by_adding_mal_mirror_columns") {
  const std::string path = tmp_db("v2-upgrade.db");
  {
    // A v1 DB, migrated straight to v2 in-process, stamped v2 — no v3
    // columns exist yet, exactly the shape a pre-P31-slice-4 install has on
    // disk.
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    REQUIRE(s->add_to_library(sample(9), 500).has_value());
    REQUIRE(s->restore_list_status(9, ListStatus::Watching, 3, 500).has_value());
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    // A fresh Store::open mints the CURRENT (v6) shape, so rolling back to a
    // "pre-P31-slice-4" v2 fixture must also drop the v3-v6 columns (P31/P34/
    // P37) or the rungs this same reopen replays would ADD COLUMN onto an
    // already-present column and fail.
    REQUIRE(sqlite3_exec(raw, "ALTER TABLE show DROP COLUMN mal_synced_status;"
                              "ALTER TABLE show DROP COLUMN mal_synced_progress;"
                              "ALTER TABLE show DROP COLUMN user_score;"
                              "ALTER TABLE show DROP COLUMN synced_score;"
                              "ALTER TABLE show DROP COLUMN mal_synced_score;"
                              "ALTER TABLE show DROP COLUMN notice_last_episode;"
                              "ALTER TABLE show DROP COLUMN notice_pending;",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 2", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  // Open through the Store: v2 -> v3 upgrade adds the two mal-mirror columns,
  // v3 -> v4 adds user_score/synced_score, v4 -> v5 adds mal_synced_score,
  // v5 -> v6 adds the P37 notice columns.
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    // Pre-existing data survives untouched.
    auto show = s->get_show(9);
    REQUIRE(show->has_value());
    CHECK((*show)->list_status == ListStatus::Watching);
    CHECK((*show)->progress == 3);
    // The new column pair exists and starts NULL: dirty on first read.
    auto dirty = s->list_dirty_for_mal_mirror();
    REQUIRE(dirty.has_value());
    REQUIRE(dirty->size() == 1);
    CHECK_FALSE((*dirty)[0].synced.has_value());
    CHECK_FALSE((*dirty)[0].synced_score.has_value());
    REQUIRE(s->mark_mal_synced(9, ListStatus::Watching, 3, 0).has_value());
  }
  // Verify user_version is now 6, data (incl. the mark_mal_synced write)
  // survives reopen.
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(st, 0) == 6);
    sqlite3_finalize(st);
    sqlite3_close(raw);
  }
  {
    auto s = Store::open(path);
    REQUIRE(s.has_value());
    CHECK(s->list_dirty_for_mal_mirror()->empty());
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}
