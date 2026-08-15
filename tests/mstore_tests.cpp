// mstore_tests.cpp — manga.db (MG-4). The ladder (fresh create, idempotent
// re-open, a stamped-but-empty file refused), the library query's ordering and
// counters, the progress laws (mark-read clears the resume page, a later
// resume never un-reads, undo restores the prior row exactly), the chapter
// cache round-trip, and the pure helpers. Offline; in-memory or a tmp file.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sqlite3.h>
#include <unistd.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "../src/manga/mstore.hpp"

using namespace shigoku;
using namespace shigoku::manga;

namespace {

// Fresh file-backed DB path in the OS tmpdir; wipes any prior run's
// main/-wal/-shm so every test starts blank.
std::string tmp_db(const char* name) {
  std::string base = "/tmp/shigoku-mstore-test-";
  base += std::to_string(static_cast<long>(::getpid()));
  base += "-";
  base += name;
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((base + suffix).c_str());
  }
  return base;
}

MdManga manga_row(const char* id, const char* title) {
  MdManga m;
  m.id = id;
  m.title = title;
  return m;
}

MdChapter chap(const char* id, const char* num, std::uint32_t pages = 10) {
  MdChapter c;
  c.id = id;
  c.chapter = num;
  c.pages = pages;
  c.lang = "en";
  return c;
}

MangaStore fresh() {
  auto st = MangaStore::open_memory();
  REQUIRE(st.has_value());
  return std::move(*st);
}

// The identity row every progress/cache write needs (they FK manga(id)).
void seed(MangaStore& st, const char* native, const char* title = "T") {
  REQUIRE(st.upsert_manga("md", manga_row(native, title), 1000).has_value());
}

}  // namespace

// --- Ladder -------------------------------------------------------------------

TEST_CASE("fresh open builds the current schema and re-opening is idempotent") {
  const std::string path = tmp_db("ladder");
  {
    auto st = MangaStore::open(path);
    REQUIRE(st.has_value());
    seed(*st, "aaa");
    REQUIRE(st->set_following("md:aaa", true, 10).has_value());
  }
  // Stamp + tables landed; a second open runs the fast path and sees the data.
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* s = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &s, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(s) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(s, 0) == kMangaSchemaVersion);
    sqlite3_finalize(s);
    sqlite3_close(raw);
  }
  auto again = MangaStore::open(path);
  REQUIRE(again.has_value());
  auto lib = again->library();
  REQUIRE(lib.has_value());
  REQUIRE(lib->size() == 1);
  CHECK((*lib)[0].manga.id == "aaa");
  CHECK((*lib)[0].source_key == "md");
}

TEST_CASE("a stamped but empty file is refused, not silently used") {
  const std::string path = tmp_db("stamped-empty");
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    const std::string bump =
        "PRAGMA user_version = " + std::to_string(kMangaSchemaVersion);
    REQUIRE(sqlite3_exec(raw, bump.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  auto st = MangaStore::open(path);
  REQUIRE(!st.has_value());
  CHECK(st.error().kind == MgStoreError::Kind::SchemaMissing);
  CHECK(!mstore_error_text(st.error()).empty());
}

TEST_CASE("a too-new database refuses before any write") {
  const std::string path = tmp_db("too-new");
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 99", nullptr, nullptr, nullptr) ==
            SQLITE_OK);
    sqlite3_close(raw);
  }
  auto st = MangaStore::open(path);
  REQUIRE(!st.has_value());
  CHECK(st.error().kind == MgStoreError::Kind::SchemaTooNew);
  CHECK(st.error().found == 99);
  CHECK(st.error().expected == kMangaSchemaVersion);
}

// --- Identity + following -----------------------------------------------------

TEST_CASE("upsert_manga: blank incoming fields never wipe stored ones") {
  MangaStore st = fresh();
  MdManga full = manga_row("aaa", "Full Title");
  full.year = 2011;
  full.status = "ongoing";
  full.description = "a description";
  full.cover_filename = "cover.jpg";
  full.al_id = 105778;
  REQUIRE(st.upsert_manga("md", full, 100).has_value());
  REQUIRE(st.set_following("md:aaa", true, 100).has_value());

  // A lean search row (no cover, no description) arrives later.
  MdManga lean = manga_row("aaa", "");
  REQUIRE(st.upsert_manga("md", lean, 200).has_value());

  auto lib = st.library();
  REQUIRE(lib.has_value());
  REQUIRE(lib->size() == 1);
  const LibraryEntry& e = (*lib)[0];
  CHECK(e.manga.title == "Full Title");
  CHECK(e.manga.description == "a description");
  CHECK(e.manga.cover_filename == "cover.jpg");
  REQUIRE(e.manga.year.has_value());
  CHECK(*e.manga.year == 2011);
  REQUIRE(e.manga.al_id.has_value());
  CHECK(*e.manga.al_id == 105778);
  CHECK(e.added_at == 100);  // set once on mint, never moved.
  CHECK(e.scoped_id() == "md:aaa");

  // A fuller row DOES overwrite.
  MdManga renamed = manga_row("aaa", "Renamed");
  REQUIRE(st.upsert_manga("md", renamed, 300).has_value());
  lib = st.library();
  REQUIRE(lib.has_value());
  CHECK((*lib)[0].manga.title == "Renamed");
}

TEST_CASE("following toggles; unfollow keeps progress and the cache") {
  MangaStore st = fresh();
  seed(st, "aaa");
  CHECK(st.is_following("md:aaa").value() == false);
  REQUIRE(st.set_following("md:aaa", true, 10).has_value());
  CHECK(st.is_following("md:aaa").value() == true);
  REQUIRE(st.mark_read("md:aaa", "c1", "1", 20).has_value());
  REQUIRE(st.put_chapters("md:aaa", {chap("c1", "1"), chap("c2", "2")}, 20).has_value());

  // Re-following is idempotent (`since` is set-once).
  REQUIRE(st.set_following("md:aaa", true, 999).has_value());

  REQUIRE(st.set_following("md:aaa", false, 30).has_value());
  CHECK(st.is_following("md:aaa").value() == false);
  CHECK(st.library().value().empty());
  // The marks survive the unfollow — re-following restores them.
  CHECK(st.progress("md:aaa").value().size() == 1);
  CHECK(st.cached_chapters("md:aaa").value().size() == 2);
  REQUIRE(st.set_following("md:aaa", true, 40).has_value());
  CHECK(st.library().value().size() == 1);
}

TEST_CASE("library orders by most-recent read, then newest added") {
  MangaStore st = fresh();
  for (const char* id : {"aaa", "bbb", "ccc"}) {
    REQUIRE(st.upsert_manga("md", manga_row(id, id), 100).has_value());
  }
  // ccc added last, so it leads on the added_at tiebreak…
  REQUIRE(st.upsert_manga("md", manga_row("ccc", "ccc"), 100).has_value());
  REQUIRE(st.set_following("md:aaa", true, 1).has_value());
  REQUIRE(st.set_following("md:bbb", true, 2).has_value());
  REQUIRE(st.set_following("md:ccc", true, 3).has_value());

  // …until reads land: bbb read most recently, then aaa; ccc never opened.
  REQUIRE(st.mark_read("md:aaa", "c1", "1", 500).has_value());
  REQUIRE(st.mark_read("md:bbb", "c1", "1", 900).has_value());
  auto lib = st.library();
  REQUIRE(lib.has_value());
  REQUIRE(lib->size() == 3);
  CHECK((*lib)[0].manga.id == "bbb");
  CHECK((*lib)[1].manga.id == "aaa");
  CHECK((*lib)[2].manga.id == "ccc");
  CHECK((*lib)[2].last_read_at == 0);
}

TEST_CASE("library counters: read count, known chapters, unread floor") {
  MangaStore st = fresh();
  seed(st, "aaa");
  REQUIRE(st.set_following("md:aaa", true, 1).has_value());
  REQUIRE(st.put_chapters("md:aaa",
                          {chap("c1", "1"), chap("c2", "2"), chap("c3", "3")}, 10)
              .has_value());
  REQUIRE(st.mark_read("md:aaa", "c1", "1", 20).has_value());
  // A resume-only row is NOT a read chapter.
  REQUIRE(st.set_last_page("md:aaa", "c2", "2", 4, 21).has_value());
  auto lib = st.library();
  REQUIRE(lib.has_value());
  CHECK((*lib)[0].read_count == 1);
  CHECK((*lib)[0].known_chapters == 3);
  CHECK((*lib)[0].unread() == 2);

  // A shrinking feed must not underflow the unread count.
  REQUIRE(st.mark_read("md:aaa", "c2", "2", 22).has_value());
  REQUIRE(st.mark_read("md:aaa", "c3", "3", 23).has_value());
  REQUIRE(st.put_chapters("md:aaa", {chap("c1", "1")}, 30).has_value());
  lib = st.library();
  REQUIRE(lib.has_value());
  CHECK((*lib)[0].known_chapters == 1);
  CHECK((*lib)[0].read_count == 3);
  CHECK((*lib)[0].unread() == 0);
}

// --- Progress -----------------------------------------------------------------

TEST_CASE("progress round-trip: resume page, mark-read clears it, no un-read") {
  MangaStore st = fresh();
  seed(st, "aaa");

  // Mid-chapter exit.
  REQUIRE(st.set_last_page("md:aaa", "c1", "1", 4, 100).has_value());
  auto row = st.progress_row("md:aaa", "c1");
  REQUIRE(row.has_value());
  REQUIRE(row->has_value());
  CHECK((*row)->read == false);
  REQUIRE((*row)->last_page.has_value());
  CHECK(*(*row)->last_page == 4);
  CHECK((*row)->chapter == "1");
  CHECK((*row)->read_at == 100);

  // Read to the end: marked, resume page dropped.
  REQUIRE(st.mark_read("md:aaa", "c1", "1", 200).has_value());
  row = st.progress_row("md:aaa", "c1");
  REQUIRE((*row).has_value());
  CHECK((*row)->read == true);
  CHECK(!(*row)->last_page.has_value());
  CHECK((*row)->read_at == 200);

  // Re-reading and quitting halfway records a resume page but never un-reads.
  REQUIRE(st.set_last_page("md:aaa", "c1", "1", 2, 300).has_value());
  row = st.progress_row("md:aaa", "c1");
  CHECK((*row)->read == true);
  CHECK(*(*row)->last_page == 2);

  // An untouched chapter has no row at all.
  auto none = st.progress_row("md:aaa", "c9");
  REQUIRE(none.has_value());
  CHECK(!none->has_value());
}

TEST_CASE("undo restores the prior row exactly, or removes a minted one") {
  MangaStore st = fresh();
  seed(st, "aaa");

  // Case 1: no prior row → undo deletes.
  REQUIRE(st.mark_read("md:aaa", "c1", "1", 100).has_value());
  REQUIRE(st.clear_progress("md:aaa", "c1").has_value());
  CHECK(!st.progress_row("md:aaa", "c1").value().has_value());

  // Case 2: a resume row existed → undo puts it back byte-for-byte.
  REQUIRE(st.set_last_page("md:aaa", "c2", "2", 7, 100).has_value());
  const ChapterProgress prior = *st.progress_row("md:aaa", "c2").value();
  REQUIRE(st.mark_read("md:aaa", "c2", "2", 200).has_value());
  REQUIRE(st.restore_progress("md:aaa", prior).has_value());
  const ChapterProgress back = *st.progress_row("md:aaa", "c2").value();
  CHECK(back == prior);
}

TEST_CASE("progress for an unknown manga fails loud (FK), never a silent row") {
  MangaStore st = fresh();
  auto r = st.mark_read("md:ghost", "c1", "1", 100);
  CHECK(!r.has_value());
  CHECK(st.progress("md:ghost").value().empty());
}

// --- Chapter cache ------------------------------------------------------------

TEST_CASE("chapter cache round-trips in reading order and replaces whole") {
  MangaStore st = fresh();
  seed(st, "aaa");
  MdChapter one = chap("c1", "1", 12);
  one.title = "The Start";
  one.publish_at = "2019-01-01T00:00:00+00:00";
  const std::vector<MdChapter> first = {one, chap("c2", "2", 8), chap("c3", "10.5", 3)};
  REQUIRE(st.put_chapters("md:aaa", first, 500).has_value());
  auto got = st.cached_chapters("md:aaa");
  REQUIRE(got.has_value());
  CHECK(*got == first);  // whole MdChapter, in the order it was written.

  // A refetch replaces the set (a dropped chapter really disappears).
  REQUIRE(st.put_chapters("md:aaa", {chap("c9", "9")}, 600).has_value());
  got = st.cached_chapters("md:aaa");
  REQUIRE(got.has_value());
  REQUIRE(got->size() == 1);
  CHECK((*got)[0].id == "c9");

  // Never fetched → empty, not an error.
  seed(st, "bbb");
  CHECK(st.cached_chapters("md:bbb").value().empty());
}

// --- Pure helpers -------------------------------------------------------------

TEST_CASE("split_scoped splits on the first colon; a bare id keeps no key") {
  CHECK(split_scoped("md:abc") == ScopedId{"md", "abc"});
  CHECK(split_scoped("dy:kase_san_ch01") == ScopedId{"dy", "kase_san_ch01"});
  CHECK(split_scoped("bare") == ScopedId{"", "bare"});
  CHECK(split_scoped("wc:01J:76") == ScopedId{"wc", "01J:76"});  // first colon only.
}

TEST_CASE("next_unread_index: gaps are the reader's business") {
  const std::vector<MdChapter> chapters = {chap("c1", "1"), chap("c2", "2"),
                                           chap("c3", "3"), chap("c4", "4")};
  CHECK(next_unread_index(chapters, {}) == 0);              // nothing read → the top.
  CHECK(next_unread_index(chapters, {"c1"}) == 1);
  CHECK(next_unread_index(chapters, {"c1", "c3"}) == 3);    // read 1,3 → next is 4.
  CHECK(next_unread_index(chapters, {"c4"}) == -1);         // highest read is last.
  CHECK(next_unread_index(chapters, {"c1", "c2", "c3", "c4"}) == -1);
  CHECK(next_unread_index({}, {"c1"}) == -1);

  // Decimal + oneshot rows ride the source's own order, not a re-sort.
  const std::vector<MdChapter> mixed = {chap("a", "1"), chap("b", "1.5"),
                                        chap("c", "2"), chap("d", "")};
  CHECK(next_unread_index(mixed, {"a"}) == 1);
  CHECK(next_unread_index(mixed, {"c"}) == 3);
}

TEST_CASE("new_chapter_ids is the sweep's diff, in fresh order") {
  const std::vector<MdChapter> cached = {chap("c1", "1"), chap("c2", "2")};
  const std::vector<MdChapter> fresh_feed = {chap("c1", "1"), chap("c2", "2"),
                                             chap("c3", "3"), chap("c4", "4")};
  CHECK(new_chapter_ids(cached, fresh_feed) == std::vector<std::string>{"c3", "c4"});
  CHECK(new_chapter_ids(fresh_feed, cached).empty());  // a shrunk feed adds nothing.
  CHECK(new_chapter_ids({}, cached) == std::vector<std::string>{"c1", "c2"});
  CHECK(new_chapter_ids(cached, {}).empty());
}

// --- Tracker high-water mark ---------------------------------------------------

TEST_CASE("al_synced starts at zero and only ever moves forward") {
  MangaStore st = fresh();
  seed(st, "aaa");
  // Never pushed — and a manga the store has never heard of answers the same,
  // because "nothing has been pushed" is the truth either way.
  CHECK(st.al_synced("md:aaa").value() == 0);
  CHECK(st.al_synced("md:ghost").value() == 0);

  REQUIRE(st.set_al_synced("md:aaa", 12).has_value());
  CHECK(st.al_synced("md:aaa").value() == 12);

  // Re-reading chapter 3 of a 40-chapter list must not walk the mark back.
  REQUIRE(st.set_al_synced("md:aaa", 3).has_value());
  CHECK(st.al_synced("md:aaa").value() == 12);

  REQUIRE(st.set_al_synced("md:aaa", 13).has_value());
  CHECK(st.al_synced("md:aaa").value() == 13);
}

TEST_CASE("the library row carries what the tracker was last told") {
  MangaStore st = fresh();
  seed(st, "aaa");
  REQUIRE(st.set_following("md:aaa", true, 10).has_value());
  REQUIRE(st.set_al_synced("md:aaa", 7).has_value());
  auto lib = st.library();
  REQUIRE(lib.has_value());
  REQUIRE(lib->size() == 1);
  CHECK((*lib)[0].al_synced_chapter == 7);
}

TEST_CASE("a v1 database upgrades by adding the synced-chapter column") {
  const std::string path = tmp_db("v1-upgrade");
  {
    auto st = MangaStore::open(path);
    REQUIRE(st.has_value());
    seed(*st, "aaa");
    REQUIRE(st->set_following("md:aaa", true, 10).has_value());
    REQUIRE(st->mark_read("md:aaa", "c1", "1", 20).has_value());
  }
  {
    // Roll the file back to what a pre-MG-5 install has on disk: the column
    // gone and the stamp with it.
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "ALTER TABLE manga DROP COLUMN al_synced_chapter",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "PRAGMA user_version = 1", nullptr, nullptr, nullptr) ==
            SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    auto st = MangaStore::open(path);
    REQUIRE(st.has_value());
    // The library and its progress survive the rung untouched...
    auto lib = st->library();
    REQUIRE(lib.has_value());
    REQUIRE(lib->size() == 1);
    CHECK((*lib)[0].read_count == 1);
    // ...and the new column reads as "never synced", not as a fake push.
    CHECK((*lib)[0].al_synced_chapter == 0);
    CHECK(st->al_synced("md:aaa").value() == 0);
    REQUIRE(st->set_al_synced("md:aaa", 4).has_value());
    CHECK(st->al_synced("md:aaa").value() == 4);
  }
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::remove((path + suffix).c_str());
  }
}
