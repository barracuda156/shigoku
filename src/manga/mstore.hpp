// mstore.hpp — `manga.db`: the library, persistent read progress, and the
// chapter cache the offline-first read flow leans on.
//
// Own database, own ladder, own error type — the anime store (store.hpp) is a
// different schema for a different app and the two must never share a file.
// The discipline is the same though: a numbered migration ladder run under one
// BEGIN IMMEDIATE (no half-applied DDL), a version stamp that refuses a
// too-new or DDL-less file at open, and binds whose failure fails the step
// loud rather than writing a silent NULL.
//
// Ownership (A7): the UI thread owns the store. Workers never hold it — they
// post events and tick() writes. Every row here is local state; nothing in
// this file talks to a network.
//
// Ids: every id crossing into store space is source-scoped ("md:{uuid}",
// "dy:{slug}", …) per source.hpp's scoped_id, so a new source is a new prefix
// rather than a schema change. split_scoped() takes them apart again — the
// library view needs the source key to know who owns a row.
//
// Chapter keying: rows key on the source's CHAPTER ID, not the displayed
// chapter number. A number is not unique — Dynasty hands out several
// unnumbered extras per series (all ""), and MG-6d already had to fix the
// same collision in the on-disk layout; keying by number would collapse them
// into one row and mark them all read together. The number rides along as a
// column because the continue-reading pin orders by it.

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../result.hpp"
#include "model.hpp"

struct sqlite3;  // opaque; the C handle lives only in the .cpp.

namespace shigoku::manga {

// The schema version this build writes. Bumping it is a migration event, never
// a silent DDL add.
inline constexpr std::uint32_t kMangaSchemaVersion = 2;

// Failure modes, the closed-enum idiom (error.hpp / StoreError in miniature —
// no float columns here, so there is no non-finite arm).
struct MgStoreError {
  enum class Kind {
    Sqlite,              // any sqlite3_* failure (detail = driver message)
    SchemaTooNew,        // DB newer than we support (found/expected)
    SchemaMissing,       // version stamp over absent DDL (foreign/partial file)
    SchemaInvalid,       // negative user_version (found)
    MigrationIncomplete, // ladder stopped mid-way (found/expected)
    ReadOnlyDb,          // file denies READWRITE (detail = path)
  };

  Kind kind = Kind::Sqlite;
  std::string detail;
  std::uint32_t found = 0;
  std::uint32_t expected = 0;

  static MgStoreError sqlite(std::string why) { return {Kind::Sqlite, std::move(why)}; }
  static MgStoreError schema_too_new(std::uint32_t f, std::uint32_t e) {
    return {Kind::SchemaTooNew, {}, f, e};
  }
  static MgStoreError schema_missing(std::uint32_t v) { return {Kind::SchemaMissing, {}, v, v}; }
  static MgStoreError schema_invalid(std::uint32_t f) { return {Kind::SchemaInvalid, {}, f}; }
  static MgStoreError migration_incomplete(std::uint32_t at, std::uint32_t exp) {
    return {Kind::MigrationIncomplete, {}, at, exp};
  }
  static MgStoreError read_only(std::string path) { return {Kind::ReadOnlyDb, std::move(path)}; }
};

// One line of copy per kind (the UI toasts these).
[[nodiscard]] std::string mstore_error_text(const MgStoreError& e);

// One touched chapter. `read` and `last_page` are independent: a mid-chapter
// exit records a resume page on an unread row, reading to the end marks it
// read and drops the resume page.
struct ChapterProgress {
  std::string chapter_id;        // the source's native chapter id.
  std::string chapter;           // display number ("10.5"); "" = oneshot.
  bool read = false;
  std::int64_t read_at = 0;      // unix secs of the row's last change.
  std::optional<int> last_page;  // 1-based resume page; nullopt = none.
  friend bool operator==(const ChapterProgress&, const ChapterProgress&) = default;
};

// A followed manga plus the counters the library list renders.
struct LibraryEntry {
  std::string source_key;         // "md"/"wc"/"dy"/"nh" — the id's prefix.
  MdManga manga;                  // manga.id is the NATIVE id (unscoped).
  std::int64_t added_at = 0;
  std::int64_t last_read_at = 0;  // newest progress stamp; 0 = never opened.
  std::uint32_t read_count = 0;   // chapters marked read.
  std::uint32_t known_chapters = 0;  // chapters_cache rows (0 = never fetched).
  // Highest chapter number already pushed to AniList (0 = never synced). The
  // tracker's own value is the authority; this is the local memory that keeps
  // a re-read from spending a round trip to be told nothing changed.
  std::uint32_t al_synced_chapter = 0;
  friend bool operator==(const LibraryEntry&, const LibraryEntry&) = default;

  // The library list's headline number. Never negative: a shrinking feed
  // (a source dropping chapters) must not underflow.
  [[nodiscard]] std::uint32_t unread() const {
    return known_chapters > read_count ? known_chapters - read_count : 0;
  }
  // "{key}:{id}" — the store key for this row.
  [[nodiscard]] std::string scoped_id() const { return source_key + ":" + manga.id; }
};

// --- Pure helpers (no DB; tested offline) ------------------------------------

// "md:abc" -> {"md", "abc"}. A missing colon means a bare/legacy id: key is
// empty and the whole string is the native id.
struct ScopedId {
  std::string key;
  std::string native;
  friend bool operator==(const ScopedId&, const ScopedId&) = default;
};
[[nodiscard]] ScopedId split_scoped(std::string_view id);

// The continue-reading pin: `chapters` is in reading order (every source
// returns it sorted), `read_ids` the chapter ids marked read. The answer is
// the first chapter strictly AFTER the highest read one — gaps are the
// reader's business, so read {1,3} of {1,2,3,4} continues at 4, not 2.
// Nothing read -> 0 (start at the top); the highest read one is the last
// chapter -> -1 (nothing left); empty list -> -1.
[[nodiscard]] int next_unread_index(const std::vector<MdChapter>& chapters,
                                    const std::set<std::string>& read_ids);

// Chapter ids present in `fresh` but not in `cached` — the update sweep's
// diff, in `fresh` order.
[[nodiscard]] std::vector<std::string> new_chapter_ids(
    const std::vector<MdChapter>& cached, const std::vector<MdChapter>& fresh);

// --- The store ---------------------------------------------------------------

class MangaStore {
 public:
  // Open (creating if absent), flip WAL, enable FKs, migrate, prove
  // writability. `path` is the manga app's own manga.db.
  [[nodiscard]] static Result<MangaStore, MgStoreError> open(std::string_view path);

  // Isolated blank in-memory DB for tests.
  [[nodiscard]] static Result<MangaStore, MgStoreError> open_memory();

  ~MangaStore();
  MangaStore(MangaStore&&) noexcept;
  MangaStore& operator=(MangaStore&&) noexcept;
  MangaStore(const MangaStore&) = delete;
  MangaStore& operator=(const MangaStore&) = delete;

  // Mint or refresh the identity row. Metadata columns always restamp from
  // the freshest source answer (titles get retranslated, covers rotate);
  // added_at is set once on mint and never moved.
  [[nodiscard]] Result<Unit, MgStoreError> upsert_manga(std::string_view source_key,
                                                        const MdManga& m,
                                                        std::int64_t now);

  // Follow / unfollow. Unfollowing drops ONLY the following row — progress and
  // the chapter cache survive, so re-following restores the marks.
  [[nodiscard]] Result<Unit, MgStoreError> set_following(std::string_view scoped, bool on,
                                                         std::int64_t now);
  [[nodiscard]] Result<bool, MgStoreError> is_following(std::string_view scoped) const;

  // The library list: followed manga, most recently read first, then newest
  // added, then title.
  [[nodiscard]] Result<std::vector<LibraryEntry>, MgStoreError> library() const;

  // --- Progress ------------------------------------------------------------

  [[nodiscard]] Result<std::vector<ChapterProgress>, MgStoreError> progress(
      std::string_view scoped) const;
  [[nodiscard]] Result<std::optional<ChapterProgress>, MgStoreError> progress_row(
      std::string_view scoped, std::string_view chapter_id) const;

  // Read to the end: mark read and drop the resume page.
  [[nodiscard]] Result<Unit, MgStoreError> mark_read(std::string_view scoped,
                                                     std::string_view chapter_id,
                                                     std::string_view chapter,
                                                     std::int64_t now);
  // Mid-chapter exit: record where to reopen. Never clears an existing read
  // mark (re-reading a finished chapter must not un-finish it).
  [[nodiscard]] Result<Unit, MgStoreError> set_last_page(std::string_view scoped,
                                                         std::string_view chapter_id,
                                                         std::string_view chapter,
                                                         int last_page, std::int64_t now);

  // Undo's two halves: the row did not exist before, or it did and must come
  // back exactly as it was.
  [[nodiscard]] Result<Unit, MgStoreError> clear_progress(std::string_view scoped,
                                                          std::string_view chapter_id);
  [[nodiscard]] Result<Unit, MgStoreError> restore_progress(std::string_view scoped,
                                                            const ChapterProgress& row);

  // --- Chapter cache -------------------------------------------------------

  // Replace the cached feed for one manga (one transaction). The cache is
  // what the library renders offline and what the update sweep diffs against.
  [[nodiscard]] Result<Unit, MgStoreError> put_chapters(
      std::string_view scoped, const std::vector<MdChapter>& chapters, std::int64_t now);

  // The cached feed in reading order (the order it was written in — sources
  // hand back sorted lists). Empty = never fetched.
  [[nodiscard]] Result<std::vector<MdChapter>, MgStoreError> cached_chapters(
      std::string_view scoped) const;

  // --- Tracker sync ---------------------------------------------------------

  // The highest chapter number pushed to AniList for this manga, 0 = never.
  // A manga with no row at all also reads 0 — "nothing has been pushed" is the
  // same answer either way.
  [[nodiscard]] Result<std::uint32_t, MgStoreError> al_synced(std::string_view scoped) const;

  // Record a completed push. Monotonic by construction (MAX with what is
  // stored): a push for an older chapter — re-reading chapter 3 after 40 — must
  // never walk the local high-water mark backwards.
  [[nodiscard]] Result<Unit, MgStoreError> set_al_synced(std::string_view scoped,
                                                         std::uint32_t progress);

 private:
  explicit MangaStore(sqlite3* conn) : conn_(conn) {}
  sqlite3* conn_ = nullptr;
};

}  // namespace shigoku::manga
