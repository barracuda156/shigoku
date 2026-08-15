// manga_sync_tests.cpp — the manga app's AniList push (msync.hpp). The two
// pure rules that decide whether anything goes on the wire at all (the floor
// of a display chapter number, and the guard that reads the server's own value
// first), then push_chapter driven against a scripted list client: no socket,
// no store, no UI.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "../src/manga/msync.hpp"

using namespace shigoku;
using namespace shigoku::manga;

namespace {

ListEntry entry(ListStatus status, std::uint32_t progress, std::uint32_t score = 0) {
  ListEntry e;
  e.status = status;
  e.progress = progress;
  e.score = score;
  return e;
}

// A MangaListClient that answers from a script and records what it was asked,
// so a test can assert both the decision and that the write never happened.
class FakeListClient final : public MangaListClient {
 public:
  struct SaveCall {
    std::int64_t media_id = 0;
    ListStatus status = ListStatus::Planning;
    std::uint32_t progress = 0;
    std::uint32_t score = 0;
  };

  // What fetch_entry answers with (defaults to "the account has no entry").
  Result<std::optional<ListEntry>, ProviderError> on_fetch = std::optional<ListEntry>{};
  // What save_entry answers with (defaults to a plausible row id).
  Result<std::int64_t, ProviderError> on_save = std::int64_t{9001};

  mutable std::vector<std::int64_t> fetched;  // media ids read, in order.
  mutable std::vector<SaveCall> saved;
  mutable std::string seen_token;
  mutable std::int64_t seen_user_id = 0;
  mutable ScoreFormat seen_format = ScoreFormat::Point100;

  Result<std::optional<ListEntry>, ProviderError> fetch_entry(std::string_view token,
                                                              std::int64_t user_id,
                                                              std::int64_t media_id,
                                                              ScoreFormat format) const override {
    seen_token = std::string(token);
    seen_user_id = user_id;
    seen_format = format;
    fetched.push_back(media_id);
    return on_fetch;
  }

  Result<std::int64_t, ProviderError> save_entry(std::string_view token, std::int64_t media_id,
                                                 ListStatus status, std::uint32_t progress,
                                                 std::uint32_t score,
                                                 ScoreFormat format) const override {
    seen_token = std::string(token);
    seen_format = format;
    saved.push_back(SaveCall{media_id, status, progress, score});
    return on_save;
  }
};

MgSyncResult push(const FakeListClient& c, std::uint32_t want, bool finishes = false) {
  return push_chapter(c, "tok", 7, 42, want, finishes, ScoreFormat::Point100);
}

}  // namespace

// --- chapter_progress: the floor rule ----------------------------------------

TEST_CASE("a whole chapter number is its own progress") {
  CHECK(chapter_progress("1") == std::optional<std::uint32_t>(1));
  CHECK(chapter_progress("7") == std::optional<std::uint32_t>(7));
  CHECK(chapter_progress("140") == std::optional<std::uint32_t>(140));
  CHECK(chapter_progress("007") == std::optional<std::uint32_t>(7));
}

TEST_CASE("a decimal chapter floors: half a chapter finishes nothing new") {
  CHECK(chapter_progress("10.5") == std::optional<std::uint32_t>(10));
  CHECK(chapter_progress("17.2") == std::optional<std::uint32_t>(17));
  CHECK(chapter_progress("5.5") == std::optional<std::uint32_t>(5));
  // Dynasty's hierarchical numbering and the usual "3v2" volume suffix land in
  // the same place: everything past the leading digit run is dropped.
  CHECK(chapter_progress("3v2") == std::optional<std::uint32_t>(3));
  CHECK(chapter_progress("12 - the title") == std::optional<std::uint32_t>(12));
}

TEST_CASE("chapter zero is a number, not an absence") {
  // "0" parses (a prologue chapter really is numbered zero); it is plan_push,
  // not the parse, that declines to write a progress of 0.
  REQUIRE(chapter_progress("0").has_value());
  CHECK(*chapter_progress("0") == 0);
}

TEST_CASE("an unnumbered chapter has no tracker meaning") {
  CHECK_FALSE(chapter_progress("").has_value());
  CHECK_FALSE(chapter_progress("Extra").has_value());
  CHECK_FALSE(chapter_progress("Omake").has_value());
  CHECK_FALSE(chapter_progress(".5").has_value());  // no leading digit run.
  CHECK_FALSE(chapter_progress("-3").has_value());
}

TEST_CASE("leading whitespace is skipped, a pathological run saturates") {
  CHECK(chapter_progress("  9") == std::optional<std::uint32_t>(9));
  // An id-shaped "number" must not wrap into a small progress that then gets
  // pushed over a real one.
  const auto huge = chapter_progress("999999999999999999999");
  REQUIRE(huge.has_value());
  CHECK(*huge >= 1'000'000);
}

// --- plan_push: the never-decrease guard -------------------------------------

TEST_CASE("progress zero is never written") {
  const MgPushPlan plan = plan_push(std::nullopt, 0, false);
  CHECK_FALSE(plan.push);
}

TEST_CASE("an account with no entry gets a fresh reading row") {
  const MgPushPlan plan = plan_push(std::nullopt, 3, false);
  CHECK(plan.push);
  CHECK(plan.progress == 3);
  CHECK(plan.status == ListStatus::Watching);
  CHECK(plan.score == 0);
}

TEST_CASE("a remote already at or past this chapter is left alone") {
  CHECK_FALSE(plan_push(entry(ListStatus::Watching, 40), 3, false).push);
  CHECK_FALSE(plan_push(entry(ListStatus::Watching, 3), 3, false).push);
  CHECK(plan_push(entry(ListStatus::Watching, 2), 3, false).push);
}

TEST_CASE("the remote score is echoed back untouched") {
  // SaveMediaListEntry takes a full row: sending 0 would wipe a rating the
  // user set on the web.
  const MgPushPlan plan = plan_push(entry(ListStatus::Watching, 2, 85), 3, false);
  REQUIRE(plan.push);
  CHECK(plan.score == 85);
}

TEST_CASE("finishing the series completes the entry") {
  const MgPushPlan plan = plan_push(entry(ListStatus::Watching, 39), 40, true);
  REQUIRE(plan.push);
  CHECK(plan.status == ListStatus::Completed);
}

TEST_CASE("re-reading into a completed series keeps it completed") {
  const MgPushPlan plan = plan_push(entry(ListStatus::Completed, 2), 3, false);
  REQUIRE(plan.push);
  CHECK(plan.status == ListStatus::Completed);
}

TEST_CASE("any other remote status becomes reading again") {
  for (const ListStatus s :
       {ListStatus::Planning, ListStatus::Paused, ListStatus::Dropped}) {
    const MgPushPlan plan = plan_push(entry(s, 1), 5, false);
    REQUIRE(plan.push);
    CHECK(plan.status == ListStatus::Watching);
  }
}

// --- push_chapter: read-then-write -------------------------------------------

TEST_CASE("a push moves the entry and reports what the list now shows") {
  FakeListClient c;
  c.on_fetch = std::optional<ListEntry>(entry(ListStatus::Watching, 2, 70));
  const MgSyncResult r = push(c, 3);
  CHECK(r.kind == MgSyncResult::Kind::Pushed);
  CHECK(r.progress == 3);
  REQUIRE(c.saved.size() == 1);
  CHECK(c.saved[0].media_id == 42);
  CHECK(c.saved[0].progress == 3);
  CHECK(c.saved[0].score == 70);
  CHECK(c.saved[0].status == ListStatus::Watching);
  // The read really did precede the write, against the same ids.
  REQUIRE(c.fetched.size() == 1);
  CHECK(c.fetched[0] == 42);
  CHECK(c.seen_user_id == 7);
  CHECK(c.seen_token == "tok");
}

TEST_CASE("an up-to-date server is not written to") {
  FakeListClient c;
  c.on_fetch = std::optional<ListEntry>(entry(ListStatus::Watching, 40));
  const MgSyncResult r = push(c, 3);
  CHECK(r.kind == MgSyncResult::Kind::UpToDate);
  CHECK(c.saved.empty());
}

TEST_CASE("a failed read writes nothing at all") {
  // The whole point of reading first: without the server's value there is no
  // way to honour never-decrease, so the guard refuses rather than guesses.
  FakeListClient c;
  c.on_fetch = err(ProviderError::network());
  const MgSyncResult r = push(c, 3);
  CHECK(r.kind == MgSyncResult::Kind::ReadFailed);
  CHECK(r.cause.kind == ProviderError::Kind::Network);
  CHECK(c.saved.empty());
}

TEST_CASE("a failed write is reported as a failed write") {
  FakeListClient c;
  c.on_save = err(ProviderError::http(401));
  const MgSyncResult r = push(c, 3);
  CHECK(r.kind == MgSyncResult::Kind::PushFailed);
  CHECK(r.cause.kind == ProviderError::Kind::Http);
  CHECK(r.cause.status == 401);
  CHECK(c.saved.size() == 1);  // it was attempted, it just did not land.
}

TEST_CASE("the score format reaches both calls") {
  FakeListClient c;
  (void)push_chapter(c, "tok", 7, 42, 3, false, ScoreFormat::Point10Decimal);
  CHECK(c.seen_format == ScoreFormat::Point10Decimal);
}

TEST_CASE("the last chapter of a finished run completes the entry end to end") {
  FakeListClient c;
  c.on_fetch = std::optional<ListEntry>(entry(ListStatus::Watching, 39));
  const MgSyncResult r = push(c, 40, /*finishes=*/true);
  CHECK(r.kind == MgSyncResult::Kind::Pushed);
  REQUIRE(c.saved.size() == 1);
  CHECK(c.saved[0].status == ListStatus::Completed);
}

// --- the copy ----------------------------------------------------------------

TEST_CASE("every outcome has its own line of copy") {
  const std::string pushed = msync_result_text(MgSyncResult::pushed(12));
  CHECK(pushed.find("12") != std::string::npos);
  const std::string up = msync_result_text(MgSyncResult::up_to_date());
  const std::string read = msync_result_text(MgSyncResult::read_failed(ProviderError::network()));
  const std::string write = msync_result_text(MgSyncResult::push_failed(ProviderError::http(500)));
  CHECK_FALSE(up.empty());
  CHECK(up != pushed);
  CHECK(read != write);
  // A failed read must not read as "synced" — nothing was written.
  CHECK(read.find("not synced") != std::string::npos);
}
