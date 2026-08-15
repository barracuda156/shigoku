// mstore.cpp — manga.db. Hand-rolled sqlite3 C API, no ORM, no async.
//
// The prepared-statement wrapper below is deliberately a local copy of the
// anime store's rather than a shared extraction: the two stores are separate
// libraries with separate schemas, and hoisting a helper out of store.cpp
// would edit the anime app's link graph for nobody's benefit. What IS shared
// is the discipline — a failed bind is sticky and refuses to step, so a typo'd
// parameter fails loud instead of writing NULL.

#include "mstore.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_set>
#include <utility>

namespace shigoku::manga {

namespace {

// SQLITE_BUSY wait. Two shigoku-manga processes on one DB is the only
// contention here (there is no background writer), so this caps a stall
// rather than covering a backlog.
constexpr int kBusyTimeoutMs = 250;

// Bounded hand-rolled retry for the WAL flip and BEGIN IMMEDIATE — the two
// lock upgrades busy_timeout does not cover.
constexpr int kLockRetryLimit = 100;
constexpr int kLockRetryBackoffMs = 5;

// v1: the four tables. `manga` carries the full MdManga column set so a
// library row re-renders (title, cover, description, tracker ids) with no
// network at all; `chapters_cache` likewise carries the whole MdChapter so an
// offline library can list chapters and mark them read.
constexpr const char* kMigrationV1 = R"SQL(
CREATE TABLE manga (
    id             TEXT PRIMARY KEY,
    title          TEXT NOT NULL,
    year           INTEGER,
    status         TEXT,
    description    TEXT,
    cover_filename TEXT,
    al_id          INTEGER,
    mal_id         INTEGER,
    added_at       INTEGER NOT NULL
);

CREATE TABLE following (
    manga_id TEXT PRIMARY KEY REFERENCES manga(id) ON DELETE CASCADE,
    since    INTEGER NOT NULL
);

CREATE TABLE progress (
    manga_id   TEXT    NOT NULL REFERENCES manga(id) ON DELETE CASCADE,
    chapter_id TEXT    NOT NULL,
    chapter    TEXT    NOT NULL,
    is_read    INTEGER NOT NULL DEFAULT 0,
    read_at    INTEGER NOT NULL,
    last_page  INTEGER,
    PRIMARY KEY (manga_id, chapter_id)
);

CREATE INDEX idx_progress_read_at ON progress(manga_id, read_at DESC);

CREATE TABLE chapters_cache (
    manga_id   TEXT    NOT NULL REFERENCES manga(id) ON DELETE CASCADE,
    chapter_id TEXT    NOT NULL,
    chapter    TEXT    NOT NULL,
    title      TEXT    NOT NULL,
    pages      INTEGER NOT NULL,
    lang       TEXT    NOT NULL,
    publish_at TEXT    NOT NULL,
    ord        INTEGER NOT NULL,
    fetched_at INTEGER NOT NULL,
    PRIMARY KEY (manga_id, chapter_id)
);
)SQL";

// v2: the AniList high-water mark. One column on `manga` rather than its own
// table — it is a per-manga scalar with the same lifetime as the identity row,
// and CASCADE already drops it with the manga.
constexpr const char* kMigrationV2 = R"SQL(
ALTER TABLE manga ADD COLUMN al_synced_chapter INTEGER NOT NULL DEFAULT 0;
)SQL";

bool is_contended(int rc) { return rc == SQLITE_BUSY || rc == SQLITE_LOCKED; }

std::string driver_msg(sqlite3* db) {
  const char* m = sqlite3_errmsg(db);
  return m != nullptr ? std::string(m) : std::string("(no message)");
}

void backoff() {
  const struct timespec ts{0, static_cast<long>(kLockRetryBackoffMs) * 1'000'000L};
  nanosleep(&ts, nullptr);
}

// Minimal RAII prepared statement: bind by 1-based index, step, read columns.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) : db_(db) {
    rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
  }
  ~Stmt() {
    if (stmt_ != nullptr) sqlite3_finalize(stmt_);
  }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  [[nodiscard]] bool prepared() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }

  void bind_int64(int i, std::int64_t v) { note(sqlite3_bind_int64(stmt_, i, v)); }
  void bind_text(int i, std::string_view v) {
    // SQLITE_TRANSIENT: sqlite copies, so v need not outlive the step.
    note(sqlite3_bind_text(stmt_, i, v.data(), static_cast<int>(v.size()),
                           SQLITE_TRANSIENT));
  }
  void bind_null(int i) { note(sqlite3_bind_null(stmt_, i)); }
  void bind_bool(int i, bool v) { bind_int64(i, v ? 1 : 0); }
  void bind_opt_int64(int i, std::optional<std::int64_t> v) {
    if (v.has_value()) bind_int64(i, *v); else bind_null(i);
  }
  void bind_opt_int(int i, std::optional<int> v) {
    if (v.has_value()) bind_int64(i, *v); else bind_null(i);
  }

  // A statement with any failed bind never steps: it would execute with a
  // silent NULL in that slot.
  [[nodiscard]] int step() {
    if (bind_rc_ != SQLITE_OK) return bind_rc_;
    return sqlite3_step(stmt_);
  }

  [[nodiscard]] std::int64_t col_int64(int i) const { return sqlite3_column_int64(stmt_, i); }
  [[nodiscard]] bool col_is_null(int i) const {
    return sqlite3_column_type(stmt_, i) == SQLITE_NULL;
  }
  [[nodiscard]] std::string col_text(int i) const {
    const auto* p = sqlite3_column_text(stmt_, i);
    if (p == nullptr) return {};
    const int n = sqlite3_column_bytes(stmt_, i);
    return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
  }
  [[nodiscard]] std::optional<std::int64_t> col_opt_int64(int i) const {
    return col_is_null(i) ? std::nullopt : std::optional<std::int64_t>(col_int64(i));
  }
  [[nodiscard]] std::optional<int> col_opt_int(int i) const {
    return col_is_null(i) ? std::nullopt
                          : std::optional<int>(static_cast<int>(col_int64(i)));
  }

 private:
  void note(int rc) {
    if (bind_rc_ == SQLITE_OK && rc != SQLITE_OK) bind_rc_ = rc;
  }

  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
  int rc_ = SQLITE_ERROR;
  int bind_rc_ = SQLITE_OK;
};

Result<Unit, MgStoreError> exec(sqlite3* db, const char* sql) {
  char* msg = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
  if (rc != SQLITE_OK) {
    std::string why = msg != nullptr ? std::string(msg) : driver_msg(db);
    sqlite3_free(msg);
    return err(MgStoreError::sqlite(std::move(why)));
  }
  return Unit{};
}

// PRAGMA journal_mode = WAL, retrying contention by hand (busy_timeout does
// not cover this lock upgrade). In-memory DBs report "memory".
Result<Unit, MgStoreError> enable_wal(sqlite3* db) {
  for (int attempt = 0;; ++attempt) {
    Stmt s(db, "PRAGMA journal_mode = WAL");
    if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(db)));
    const int rc = s.step();
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) return Unit{};
    if (attempt < kLockRetryLimit && is_contended(rc)) {
      backoff();
      continue;
    }
    return err(MgStoreError::sqlite(driver_msg(db)));
  }
}

Result<std::uint32_t, MgStoreError> user_version(sqlite3* db) {
  Stmt s(db, "PRAGMA user_version");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(db)));
  if (s.step() != SQLITE_ROW) return err(MgStoreError::sqlite(driver_msg(db)));
  const std::int64_t v = s.col_int64(0);
  if (v < 0 || v > static_cast<std::int64_t>(UINT32_MAX)) {
    return err(MgStoreError::schema_invalid(static_cast<std::uint32_t>(v & 0xFFFFFFFF)));
  }
  const auto uv = static_cast<std::uint32_t>(v);
  if (uv > kMangaSchemaVersion) {
    return err(MgStoreError::schema_too_new(uv, kMangaSchemaVersion));
  }
  return uv;
}

// user_version alone proves nothing: a stamped version over missing DDL (a
// foreign file, a truncated copy) must fail here, not as a raw "no such
// table" at the first write.
Result<Unit, MgStoreError> verify_schema_present(sqlite3* db) {
  Stmt s(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='manga'");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(db)));
  if (s.step() != SQLITE_ROW) return err(MgStoreError::sqlite(driver_msg(db)));
  if (s.col_int64(0) == 0) return err(MgStoreError::schema_missing(kMangaSchemaVersion));
  return Unit{};
}

Result<Unit, MgStoreError> begin_immediate(sqlite3* db) {
  for (int attempt = 0;; ++attempt) {
    const int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (rc == SQLITE_OK) return Unit{};
    if (attempt < kLockRetryLimit && is_contended(rc)) {
      backoff();
      continue;
    }
    return err(MgStoreError::sqlite(driver_msg(db)));
  }
}

// The ladder + the version bump under one BEGIN IMMEDIATE: DDL and stamp land
// atomically, never half-applied. The already-current version fast-paths
// without taking a write lock.
Result<Unit, MgStoreError> migrate(sqlite3* db) {
  {
    auto v = user_version(db);
    if (!v.has_value()) return err(v.error());
    if (*v == kMangaSchemaVersion) return verify_schema_present(db);
  }

  auto begun = begin_immediate(db);
  if (!begun.has_value()) return err(begun.error());

  auto rollback = [&](MgStoreError e) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(std::move(e));
  };

  // Re-read under the lock: a peer may have finished while we waited.
  auto v = user_version(db);
  if (!v.has_value()) return rollback(v.error());
  std::uint32_t at = *v;
  if (at == kMangaSchemaVersion) {
    auto ok = verify_schema_present(db);
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return ok;
  }

  if (at < 1) {
    auto ddl = exec(db, kMigrationV1);
    if (!ddl.has_value()) return rollback(ddl.error());
    at = 1;
  }
  if (at < 2) {
    auto ddl = exec(db, kMigrationV2);
    if (!ddl.has_value()) return rollback(ddl.error());
    at = 2;
  }

  // A strippable assert here would BE the half-applied-schema bug; this is a
  // real runtime check in every build.
  if (at != kMangaSchemaVersion) {
    return rollback(MgStoreError::migration_incomplete(at, kMangaSchemaVersion));
  }

  // PRAGMA user_version takes no bound parameter; the value is a compile-time
  // constant, so the concatenation is injection-safe.
  const std::string bump = "PRAGMA user_version = " + std::to_string(kMangaSchemaVersion);
  auto stamped = exec(db, bump.c_str());
  if (!stamped.has_value()) return rollback(stamped.error());
  return exec(db, "COMMIT");
}

// SQLite silently downgrades to read-only when the file denies READWRITE; that
// must fail loud at open, not at the first write.
Result<Unit, MgStoreError> probe_writable(sqlite3* db, std::string_view path) {
  if (sqlite3_db_readonly(db, "main") == 1) {
    return err(MgStoreError::read_only(std::string(path)));
  }
  return Unit{};
}

Result<Unit, MgStoreError> finish_open(sqlite3* db, std::string_view path) {
  auto fail = [&](MgStoreError e) {
    sqlite3_close(db);
    return err(std::move(e));
  };
  sqlite3_busy_timeout(db, kBusyTimeoutMs);
  if (auto r = enable_wal(db); !r.has_value()) return fail(r.error());
  if (auto r = exec(db, "PRAGMA foreign_keys = ON"); !r.has_value()) return fail(r.error());
  if (auto r = migrate(db); !r.has_value()) return fail(r.error());
  if (auto r = probe_writable(db, path); !r.has_value()) return fail(r.error());
  return Unit{};
}

// Run a single-statement write whose binds the caller already made.
Result<Unit, MgStoreError> run_done(sqlite3* db, Stmt& s) {
  if (s.step() != SQLITE_DONE) return err(MgStoreError::sqlite(driver_msg(db)));
  return Unit{};
}

}  // namespace

// --- Pure helpers -------------------------------------------------------------

std::string mstore_error_text(const MgStoreError& e) {
  switch (e.kind) {
    case MgStoreError::Kind::Sqlite:
      return "library db: " + e.detail;
    case MgStoreError::Kind::SchemaTooNew:
      return "library db is newer than this build (v" + std::to_string(e.found) + ")";
    case MgStoreError::Kind::SchemaMissing:
      return "library db is stamped but empty";
    case MgStoreError::Kind::SchemaInvalid:
      return "library db has an invalid schema stamp";
    case MgStoreError::Kind::MigrationIncomplete:
      return "library db migration stopped at v" + std::to_string(e.found);
    case MgStoreError::Kind::ReadOnlyDb:
      return "library db is read-only: " + e.detail;
  }
  return "library db error";  // unreachable; the switch is exhaustive.
}

ScopedId split_scoped(std::string_view id) {
  const std::size_t colon = id.find(':');
  if (colon == std::string_view::npos) return ScopedId{{}, std::string(id)};
  return ScopedId{std::string(id.substr(0, colon)), std::string(id.substr(colon + 1))};
}

int next_unread_index(const std::vector<MdChapter>& chapters,
                      const std::set<std::string>& read_ids) {
  if (chapters.empty()) return -1;
  int highest_read = -1;
  for (int i = 0; i < static_cast<int>(chapters.size()); ++i) {
    if (read_ids.count(chapters[static_cast<std::size_t>(i)].id) != 0) highest_read = i;
  }
  const int next = highest_read + 1;
  return next < static_cast<int>(chapters.size()) ? next : -1;
}

std::vector<std::string> new_chapter_ids(const std::vector<MdChapter>& cached,
                                         const std::vector<MdChapter>& fresh) {
  std::unordered_set<std::string> known;
  known.reserve(cached.size() * 2);
  for (const MdChapter& c : cached) known.insert(c.id);
  std::vector<std::string> out;
  for (const MdChapter& c : fresh) {
    if (known.count(c.id) == 0) out.push_back(c.id);
  }
  return out;
}

// --- Open / lifetime ----------------------------------------------------------

Result<MangaStore, MgStoreError> MangaStore::open(std::string_view path) {
  sqlite3* db = nullptr;
  const std::string p(path);  // the C API needs NUL termination.
  const int rc = sqlite3_open_v2(p.c_str(), &db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    std::string why = db != nullptr ? driver_msg(db) : std::string("open failed");
    sqlite3_close(db);
    return err(MgStoreError::sqlite(std::move(why)));
  }
  if (auto r = finish_open(db, path); !r.has_value()) return err(r.error());
  return MangaStore(db);
}

Result<MangaStore, MgStoreError> MangaStore::open_memory() {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open(":memory:", &db);
  if (rc != SQLITE_OK) {
    std::string why = db != nullptr ? driver_msg(db) : std::string("open failed");
    sqlite3_close(db);
    return err(MgStoreError::sqlite(std::move(why)));
  }
  if (auto r = finish_open(db, ":memory:"); !r.has_value()) return err(r.error());
  return MangaStore(db);
}

MangaStore::~MangaStore() {
  if (conn_ != nullptr) sqlite3_close(conn_);
}

MangaStore::MangaStore(MangaStore&& other) noexcept
    : conn_(std::exchange(other.conn_, nullptr)) {}

MangaStore& MangaStore::operator=(MangaStore&& other) noexcept {
  if (this != &other) {
    if (conn_ != nullptr) sqlite3_close(conn_);
    conn_ = std::exchange(other.conn_, nullptr);
  }
  return *this;
}

// --- Identity + following -----------------------------------------------------

Result<Unit, MgStoreError> MangaStore::upsert_manga(std::string_view source_key,
                                                    const MdManga& m, std::int64_t now) {
  // Blank/absent incoming fields never wipe a stored value: a lean search row
  // must not erase the fuller row a detail fetch wrote earlier (some sources
  // carry no cover or description in search results at all).
  Stmt s(conn_,
         "INSERT INTO manga "
         "(id, title, year, status, description, cover_filename, al_id, mal_id, added_at) "
         "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) "
         "ON CONFLICT(id) DO UPDATE SET "
         "  title = CASE WHEN excluded.title <> '' THEN excluded.title ELSE manga.title END, "
         "  year = COALESCE(excluded.year, manga.year), "
         "  status = CASE WHEN excluded.status <> '' THEN excluded.status ELSE manga.status END, "
         "  description = CASE WHEN excluded.description <> '' "
         "                THEN excluded.description ELSE manga.description END, "
         "  cover_filename = CASE WHEN excluded.cover_filename <> '' "
         "                   THEN excluded.cover_filename ELSE manga.cover_filename END, "
         "  al_id = COALESCE(excluded.al_id, manga.al_id), "
         "  mal_id = COALESCE(excluded.mal_id, manga.mal_id)");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  const std::string id = std::string(source_key) + ":" + m.id;
  s.bind_text(1, id);
  s.bind_text(2, m.title);
  s.bind_opt_int64(3, m.year.has_value() ? std::optional<std::int64_t>(*m.year)
                                         : std::nullopt);
  s.bind_text(4, m.status);
  s.bind_text(5, m.description);
  s.bind_text(6, m.cover_filename);
  s.bind_opt_int64(7, m.al_id);
  s.bind_opt_int64(8, m.mal_id);
  s.bind_int64(9, now);
  return run_done(conn_, s);
}

Result<Unit, MgStoreError> MangaStore::set_following(std::string_view scoped, bool on,
                                                     std::int64_t now) {
  if (on) {
    Stmt s(conn_,
           "INSERT INTO following (manga_id, since) VALUES (?1, ?2) "
           "ON CONFLICT(manga_id) DO NOTHING");  // since is set-once.
    if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
    s.bind_text(1, scoped);
    s.bind_int64(2, now);
    return run_done(conn_, s);
  }
  Stmt s(conn_, "DELETE FROM following WHERE manga_id = ?1");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  return run_done(conn_, s);
}

Result<bool, MgStoreError> MangaStore::is_following(std::string_view scoped) const {
  Stmt s(conn_, "SELECT 1 FROM following WHERE manga_id = ?1");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  const int rc = s.step();
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  return err(MgStoreError::sqlite(driver_msg(conn_)));
}

Result<std::vector<LibraryEntry>, MgStoreError> MangaStore::library() const {
  Stmt s(conn_,
         "SELECT m.id, m.title, m.year, m.status, m.description, m.cover_filename, "
         "       m.al_id, m.mal_id, m.added_at, "
         "       (SELECT MAX(read_at) FROM progress p WHERE p.manga_id = m.id) AS last_read, "
         "       (SELECT COUNT(*) FROM progress p "
         "         WHERE p.manga_id = m.id AND p.is_read = 1) AS read_count, "
         "       (SELECT COUNT(*) FROM chapters_cache c WHERE c.manga_id = m.id) AS known, "
         "       m.al_synced_chapter "
         "FROM manga m JOIN following f ON f.manga_id = m.id "
         "ORDER BY COALESCE(last_read, 0) DESC, m.added_at DESC, m.title ASC");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  std::vector<LibraryEntry> out;
  for (;;) {
    const int rc = s.step();
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return err(MgStoreError::sqlite(driver_msg(conn_)));
    const ScopedId id = split_scoped(s.col_text(0));
    LibraryEntry e;
    e.source_key = id.key;
    e.manga.id = id.native;
    e.manga.title = s.col_text(1);
    if (const auto y = s.col_opt_int(2); y.has_value()) e.manga.year = *y;
    e.manga.status = s.col_text(3);
    e.manga.description = s.col_text(4);
    e.manga.cover_filename = s.col_text(5);
    e.manga.al_id = s.col_opt_int64(6);
    e.manga.mal_id = s.col_opt_int64(7);
    e.added_at = s.col_int64(8);
    e.last_read_at = s.col_is_null(9) ? 0 : s.col_int64(9);
    e.read_count = static_cast<std::uint32_t>(s.col_int64(10));
    e.known_chapters = static_cast<std::uint32_t>(s.col_int64(11));
    e.al_synced_chapter = static_cast<std::uint32_t>(s.col_int64(12));
    out.push_back(std::move(e));
  }
  return out;
}

// --- Progress -----------------------------------------------------------------

namespace {

ChapterProgress read_progress_row(const Stmt& s) {
  ChapterProgress p;
  p.chapter_id = s.col_text(0);
  p.chapter = s.col_text(1);
  p.read = s.col_int64(2) != 0;
  p.read_at = s.col_int64(3);
  p.last_page = s.col_opt_int(4);
  return p;
}

}  // namespace

Result<std::vector<ChapterProgress>, MgStoreError> MangaStore::progress(
    std::string_view scoped) const {
  Stmt s(conn_, "SELECT chapter_id, chapter, is_read, read_at, last_page "
                "FROM progress WHERE manga_id = ?1");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  std::vector<ChapterProgress> out;
  for (;;) {
    const int rc = s.step();
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return err(MgStoreError::sqlite(driver_msg(conn_)));
    out.push_back(read_progress_row(s));
  }
  return out;
}

Result<std::optional<ChapterProgress>, MgStoreError> MangaStore::progress_row(
    std::string_view scoped, std::string_view chapter_id) const {
  Stmt s(conn_, "SELECT chapter_id, chapter, is_read, read_at, last_page "
                "FROM progress WHERE manga_id = ?1 AND chapter_id = ?2");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  s.bind_text(2, chapter_id);
  const int rc = s.step();
  if (rc == SQLITE_DONE) return std::optional<ChapterProgress>{};
  if (rc != SQLITE_ROW) return err(MgStoreError::sqlite(driver_msg(conn_)));
  return std::optional<ChapterProgress>(read_progress_row(s));
}

Result<Unit, MgStoreError> MangaStore::mark_read(std::string_view scoped,
                                                 std::string_view chapter_id,
                                                 std::string_view chapter,
                                                 std::int64_t now) {
  Stmt s(conn_,
         "INSERT INTO progress (manga_id, chapter_id, chapter, is_read, read_at, last_page) "
         "VALUES (?1, ?2, ?3, 1, ?4, NULL) "
         "ON CONFLICT(manga_id, chapter_id) DO UPDATE SET "
         "  chapter = excluded.chapter, is_read = 1, read_at = excluded.read_at, "
         "  last_page = NULL");  // finished: there is nothing left to resume.
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  s.bind_text(2, chapter_id);
  s.bind_text(3, chapter);
  s.bind_int64(4, now);
  return run_done(conn_, s);
}

Result<Unit, MgStoreError> MangaStore::set_last_page(std::string_view scoped,
                                                     std::string_view chapter_id,
                                                     std::string_view chapter,
                                                     int last_page, std::int64_t now) {
  Stmt s(conn_,
         "INSERT INTO progress (manga_id, chapter_id, chapter, is_read, read_at, last_page) "
         "VALUES (?1, ?2, ?3, 0, ?4, ?5) "
         "ON CONFLICT(manga_id, chapter_id) DO UPDATE SET "
         "  chapter = excluded.chapter, read_at = excluded.read_at, "
         "  last_page = excluded.last_page");  // is_read untouched: re-reading
                                               // a finished chapter keeps it
                                               // finished.
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  s.bind_text(2, chapter_id);
  s.bind_text(3, chapter);
  s.bind_int64(4, now);
  s.bind_int64(5, last_page);
  return run_done(conn_, s);
}

Result<Unit, MgStoreError> MangaStore::clear_progress(std::string_view scoped,
                                                      std::string_view chapter_id) {
  Stmt s(conn_, "DELETE FROM progress WHERE manga_id = ?1 AND chapter_id = ?2");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  s.bind_text(2, chapter_id);
  return run_done(conn_, s);
}

Result<Unit, MgStoreError> MangaStore::restore_progress(std::string_view scoped,
                                                        const ChapterProgress& row) {
  Stmt s(conn_,
         "INSERT INTO progress (manga_id, chapter_id, chapter, is_read, read_at, last_page) "
         "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
         "ON CONFLICT(manga_id, chapter_id) DO UPDATE SET "
         "  chapter = excluded.chapter, is_read = excluded.is_read, "
         "  read_at = excluded.read_at, last_page = excluded.last_page");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  s.bind_text(2, row.chapter_id);
  s.bind_text(3, row.chapter);
  s.bind_bool(4, row.read);
  s.bind_int64(5, row.read_at);
  s.bind_opt_int(6, row.last_page);
  return run_done(conn_, s);
}

// --- Chapter cache ------------------------------------------------------------

Result<Unit, MgStoreError> MangaStore::put_chapters(std::string_view scoped,
                                                    const std::vector<MdChapter>& chapters,
                                                    std::int64_t now) {
  if (auto b = begin_immediate(conn_); !b.has_value()) return err(b.error());
  auto rollback = [&](MgStoreError e) {
    sqlite3_exec(conn_, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(std::move(e));
  };
  {
    Stmt del(conn_, "DELETE FROM chapters_cache WHERE manga_id = ?1");
    if (!del.prepared()) return rollback(MgStoreError::sqlite(driver_msg(conn_)));
    del.bind_text(1, scoped);
    if (del.step() != SQLITE_DONE) return rollback(MgStoreError::sqlite(driver_msg(conn_)));
  }
  for (std::size_t i = 0; i < chapters.size(); ++i) {
    const MdChapter& c = chapters[i];
    Stmt s(conn_,
           "INSERT INTO chapters_cache "
           "(manga_id, chapter_id, chapter, title, pages, lang, publish_at, ord, fetched_at) "
           "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
    if (!s.prepared()) return rollback(MgStoreError::sqlite(driver_msg(conn_)));
    s.bind_text(1, scoped);
    s.bind_text(2, c.id);
    s.bind_text(3, c.chapter);
    s.bind_text(4, c.title);
    s.bind_int64(5, static_cast<std::int64_t>(c.pages));
    s.bind_text(6, c.lang);
    s.bind_text(7, c.publish_at);
    s.bind_int64(8, static_cast<std::int64_t>(i));
    s.bind_int64(9, now);
    if (s.step() != SQLITE_DONE) return rollback(MgStoreError::sqlite(driver_msg(conn_)));
  }
  return exec(conn_, "COMMIT");
}

Result<std::vector<MdChapter>, MgStoreError> MangaStore::cached_chapters(
    std::string_view scoped) const {
  Stmt s(conn_,
         "SELECT chapter_id, chapter, title, pages, lang, publish_at "
         "FROM chapters_cache WHERE manga_id = ?1 ORDER BY ord ASC");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  std::vector<MdChapter> out;
  for (;;) {
    const int rc = s.step();
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return err(MgStoreError::sqlite(driver_msg(conn_)));
    MdChapter c;
    c.id = s.col_text(0);
    c.chapter = s.col_text(1);
    c.title = s.col_text(2);
    c.pages = static_cast<std::uint32_t>(s.col_int64(3));
    c.lang = s.col_text(4);
    c.publish_at = s.col_text(5);
    out.push_back(std::move(c));
  }
  return out;
}

// --- Tracker sync ---------------------------------------------------------------

Result<std::uint32_t, MgStoreError> MangaStore::al_synced(std::string_view scoped) const {
  Stmt s(conn_, "SELECT al_synced_chapter FROM manga WHERE id = ?1");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  const int rc = s.step();
  if (rc == SQLITE_DONE) return std::uint32_t{0};  // no row: nothing pushed.
  if (rc != SQLITE_ROW) return err(MgStoreError::sqlite(driver_msg(conn_)));
  const std::int64_t v = s.col_int64(0);
  return v > 0 ? static_cast<std::uint32_t>(v) : std::uint32_t{0};
}

Result<Unit, MgStoreError> MangaStore::set_al_synced(std::string_view scoped,
                                                     std::uint32_t progress) {
  Stmt s(conn_,
         "UPDATE manga SET al_synced_chapter = MAX(al_synced_chapter, ?2) WHERE id = ?1");
  if (!s.prepared()) return err(MgStoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, scoped);
  s.bind_int64(2, static_cast<std::int64_t>(progress));
  return run_done(conn_, s);
}

}  // namespace shigoku::manga
