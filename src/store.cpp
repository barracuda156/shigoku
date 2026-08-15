// store.cpp — the P9 store (A7). Hand-rolled sqlite3 C-API; no ORM, no async.
//
// The read-compute-write writers (record_finish, recompute_progress) run under
// one BEGIN IMMEDIATE so a second process on the same file cannot regress the
// ratchet from a stale read (store.rs ROD-434). WAL + a short busy_timeout
// serialize writer-vs-writer; the two lock upgrades busy_timeout does not cover
// (the WAL flip and BEGIN IMMEDIATE) get a bounded hand-rolled retry.
//
// §3 discipline: no buffer-to-int casts here; all numbers cross the boundary
// through sqlite3_bind_*/column_* by value, so the storage is endianness-clean.

#include "store.hpp"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "domain.hpp"

namespace shigoku {

using json = nlohmann::json;

namespace {

// SQLITE_BUSY wait (store.rs BUSY_TIMEOUT). Writer-vs-writer only (WAL lets
// readers through). Short: real collisions are sub-20ms and writes ride the UI
// thread, so this caps a stall rather than covering a real backlog.
constexpr int kBusyTimeoutMs = 250;

// Bounded hand-rolled retry for the WAL flip and the BEGIN IMMEDIATE — the two
// lock upgrades busy_timeout does not cover (store.rs LOCK_RETRY_*).
constexpr int kLockRetryLimit = 100;
constexpr int kLockRetryBackoffMs = 5;

// The 02 §3.3 shape. P9 ships the two A7 tables. `show` carries the FULL 02
// column set from day one (identity + enrichment + user state) so P14's
// enrichment/sync growth is additive column *writes* on an existing table, not
// an ALTER-heavy migration. episode_progress is the resume/watched row. Both
// key exactly as 02 §3.4 mandates (anilist_id / (id, translation, episode)) so
// sabigoku↔shigoku data-compat holds by construction.
constexpr const char* kMigrationV1 = R"SQL(
CREATE TABLE show (
    anilist_id                  INTEGER PRIMARY KEY,
    mal_id                      INTEGER,
    title_romaji                TEXT NOT NULL,
    title_english               TEXT,
    title_native                TEXT,
    cover_url                   TEXT,
    total_episodes              INTEGER,
    duration_minutes            INTEGER,
    year                        INTEGER,
    season                      TEXT,
    status                      TEXT,
    description                 TEXT,
    score                       INTEGER,
    kind                        TEXT,
    start_year                  INTEGER,
    start_month                 INTEGER,
    start_day                   INTEGER,
    genres                      TEXT,
    studios                     TEXT,
    source_material             TEXT,
    rank                        INTEGER,
    rank_type                   TEXT,
    rank_year                   INTEGER,
    next_airing_at              INTEGER,
    next_airing_episode         INTEGER,
    country                     TEXT,
    enrichment_fetched_at       INTEGER,
    enrichment_fieldset_version INTEGER,
    list_status                 TEXT NOT NULL DEFAULT 'planning',
    user_rating                 REAL,
    notes                       TEXT,
    play_count                  INTEGER NOT NULL DEFAULT 0,
    progress                    INTEGER NOT NULL DEFAULT 0,
    progress_stamped_at         INTEGER,
    library_added_at            INTEGER,
    last_watched_at             INTEGER,
    synced_status               TEXT,
    synced_progress             INTEGER
);

CREATE INDEX idx_show_mal ON show(mal_id);
CREATE INDEX idx_show_list_status ON show(list_status);
CREATE INDEX idx_show_last_watched ON show(last_watched_at DESC);

CREATE TABLE episode_progress (
    anilist_id     INTEGER NOT NULL REFERENCES show(anilist_id) ON DELETE CASCADE,
    translation    TEXT    NOT NULL,
    episode        TEXT    NOT NULL,
    position_secs  REAL    NOT NULL DEFAULT 0,
    duration_secs  REAL    NOT NULL DEFAULT 0,
    fully_watched  INTEGER NOT NULL DEFAULT 0,
    updated_at     INTEGER NOT NULL,
    last_provider  TEXT,
    PRIMARY KEY (anilist_id, translation, episode)
);
)SQL";

// P14 growth: the seven remaining 02 §3.3 tables. Split into V2 (rather than
// folded into V1) so a P9 v1 DB upgrades forward by ADDING these tables — never
// an ALTER on `show`, which already carries its full column set. The end state
// at v2 is byte-identical to sabigoku's v2 schema (its V1 created all tables +
// V2 added show.progress_stamped_at, which shigoku's V1 already has), so the
// two stores' DBs are interchangeable at v2 (02 §3.6). bind/absence/route/
// episode_cache/pin all FK show(anilist_id) ON DELETE CASCADE, so delete_show
// from the PK sweeps every child (02 §5 FIX-IN-RUST). catalog_cache is
// user-state-free and NOT FK'd (a durable browse cache outlives library rows;
// 02 §3.5). genres/studios/episodes_blob are JSON string arrays the Rust layer
// owns (02 L5); SQL never splits inside them.
constexpr const char* kMigrationV2 = R"SQL(
CREATE TABLE provider_binding (
    anilist_id   INTEGER NOT NULL REFERENCES show(anilist_id) ON DELETE CASCADE,
    provider     TEXT    NOT NULL,
    provider_id  TEXT    NOT NULL,
    bound_at     INTEGER NOT NULL,
    PRIMARY KEY (anilist_id, provider),
    UNIQUE (provider, provider_id)
);

CREATE TABLE episode_cache (
    anilist_id    INTEGER NOT NULL REFERENCES show(anilist_id) ON DELETE CASCADE,
    provider      TEXT    NOT NULL,
    translation   TEXT    NOT NULL,
    episodes_blob TEXT    NOT NULL,
    fetched_at    INTEGER NOT NULL,
    expires_at    INTEGER NOT NULL,
    PRIMARY KEY (anilist_id, provider, translation)
);

CREATE TABLE provider_pin (
    anilist_id INTEGER PRIMARY KEY REFERENCES show(anilist_id) ON DELETE CASCADE,
    provider   TEXT NOT NULL
);

CREATE TABLE provider_absence (
    anilist_id INTEGER NOT NULL REFERENCES show(anilist_id) ON DELETE CASCADE,
    provider   TEXT    NOT NULL,
    checked_at INTEGER NOT NULL,
    PRIMARY KEY (anilist_id, provider)
);

CREATE TABLE provider_route (
    anilist_id    INTEGER PRIMARY KEY REFERENCES show(anilist_id) ON DELETE CASCADE,
    resolved_pref TEXT NOT NULL
);

CREATE TABLE catalog_cache (
    anilist_id                  INTEGER PRIMARY KEY,
    mal_id                      INTEGER,
    title_romaji                TEXT NOT NULL,
    title_english               TEXT,
    title_native                TEXT,
    cover_url                   TEXT,
    total_episodes              INTEGER,
    duration_minutes            INTEGER,
    year                        INTEGER,
    season                      TEXT,
    status                      TEXT,
    description                 TEXT,
    score                       INTEGER,
    kind                        TEXT,
    start_year                  INTEGER,
    start_month                 INTEGER,
    start_day                   INTEGER,
    genres                      TEXT,
    studios                     TEXT,
    source_material             TEXT,
    rank                        INTEGER,
    rank_type                   TEXT,
    rank_year                   INTEGER,
    next_airing_at              INTEGER,
    next_airing_episode         INTEGER,
    country                     TEXT,
    fieldset_version            INTEGER NOT NULL,
    fetched_at                  INTEGER NOT NULL,
    expires_at                  INTEGER
);

CREATE INDEX idx_catalog_fetched ON catalog_cache(fetched_at DESC);

CREATE TABLE app_meta (
    key   TEXT NOT NULL PRIMARY KEY,
    value TEXT NOT NULL
);
)SQL";

// P31 slice 4 growth: the MAL mirror's own snapshot pair, additive columns on
// `show` (same "ADD, never rewrite" rule as V2's tables). Independent of
// synced_status/synced_progress (the AniList sync snapshot, v1) — the two
// mirrors are separate client relationships and must not share a dirty gate
// (PORT_PARITY.md P31). No sabigoku equivalent (MAL mirror is shigoku-only),
// so this column pair has no cross-store compat claim.
constexpr const char* kMigrationV3 = R"SQL(
ALTER TABLE show ADD COLUMN mal_synced_status TEXT;
ALTER TABLE show ADD COLUMN mal_synced_progress INTEGER;
)SQL";

// P34 growth: user_score (raw 0..=100, AniList POINT_100 canonical storage;
// NULL/0 = unset) plus its OWN dirty-snapshot column, same "ADD, never
// rewrite" rule and same independent-snapshot-per-relationship law as V3's
// mal_synced_* pair (PORT_PARITY.md P34) — the AniList sync snapshot must not
// decide this field's dirtiness, and vice versa. Distinct from the dead
// zimport-only `user_rating` REAL column (P29 waived; do not conflate).
constexpr const char* kMigrationV4 = R"SQL(
ALTER TABLE show ADD COLUMN user_score INTEGER;
ALTER TABLE show ADD COLUMN synced_score INTEGER;
)SQL";

// P34 slice 2 growth: the MAL mirror's OWN score snapshot, same independent-
// snapshot-per-relationship law as V3's mal_synced_status/progress pair —
// mal_synced_score gates MAL-mirror score dirtiness exactly as
// mal_synced_status/progress already gate status/progress, never sharing a
// dirty bit with V4's AniList-sync synced_score.
constexpr const char* kMigrationV5 = R"SQL(
ALTER TABLE show ADD COLUMN mal_synced_score INTEGER;
)SQL";

// P37 slice 3 growth: the schedule new-episode notice's own state, additive
// columns on `show` (same "ADD, never rewrite" rule as V3's pair).
// notice_last_episode is the dedup high-water mark (detect_schedule_notices,
// domain.cpp) — never decreases, never cleared by opening a show.
// notice_pending is the independent History NEW-marker bit, defaulting to 0
// so every existing row starts unmarked; it is cleared on open regardless of
// notice_last_episode's value. No sabigoku equivalent (Schedule is
// shigoku-only, §9 P37).
constexpr const char* kMigrationV6 = R"SQL(
ALTER TABLE show ADD COLUMN notice_last_episode INTEGER;
ALTER TABLE show ADD COLUMN notice_pending INTEGER NOT NULL DEFAULT 0;
)SQL";

// Retryable contention: BUSY or LOCKED (rusqlite classifies both, store.rs).
bool is_contended(int rc) {
  return rc == SQLITE_BUSY || rc == SQLITE_LOCKED;
}

std::string driver_msg(sqlite3* db) {
  const char* m = sqlite3_errmsg(db);
  return m != nullptr ? std::string(m) : std::string("(no message)");
}

// Non-owning view of an optional<string> for a bind_opt_text. The referent
// (the Enrichment field, or a local encoding) must outlive the bind; every
// caller here binds via SQLITE_TRANSIENT before the referent drops.
std::optional<std::string_view> opt_view(const std::optional<std::string>& s) {
  return s.has_value() ? std::optional<std::string_view>(*s) : std::nullopt;
}

// Sleep out a lock-upgrade backoff. store.rs uses a fixed 5ms; the connection
// is UI-thread-owned so the whole retry loop is bounded (kLockRetryLimit).
void backoff() {
  const struct timespec ts{0, static_cast<long>(kLockRetryBackoffMs) * 1'000'000L};
  nanosleep(&ts, nullptr);
}

// Minimal RAII prepared-statement wrapper: bind by index (1-based), step, read
// columns, finalize on scope exit. Only the shapes this store needs.
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
  [[nodiscard]] int prepare_rc() const { return rc_; }

  // Every bind's rc is accumulated and a failed bind fails the step() loud
  // (ID-4: zigoku wrote a failed bind as silent NULL; rusqlite propagates —
  // so must we). SQLITE_RANGE (index 0 from a typo'd name, or out-of-range),
  // SQLITE_TOOBIG, SQLITE_NOMEM all land here.
  void bind_int64(int i, std::int64_t v) { note(sqlite3_bind_int64(stmt_, i, v)); }
  void bind_double(int i, double v) { note(sqlite3_bind_double(stmt_, i, v)); }
  void bind_text(int i, std::string_view v) {
    // SQLITE_TRANSIENT: sqlite copies, so v need not outlive the step.
    note(sqlite3_bind_text(stmt_, i, v.data(), static_cast<int>(v.size()),
                           SQLITE_TRANSIENT));
  }
  void bind_null(int i) { note(sqlite3_bind_null(stmt_, i)); }
  void bind_opt_int64(int i, std::optional<std::int64_t> v) {
    if (v.has_value()) bind_int64(i, *v); else bind_null(i);
  }
  void bind_opt_uint(int i, std::optional<std::uint32_t> v) {
    if (v.has_value()) bind_int64(i, static_cast<std::int64_t>(*v)); else bind_null(i);
  }
  void bind_opt_text(int i, std::optional<std::string_view> v) {
    if (v.has_value()) bind_text(i, *v); else bind_null(i);
  }

  // Named-parameter binds (:name). The full-fieldset enrichment SQL is far more
  // legible with names than positional indexes; the C API resolves the index
  // per statement. An absent name resolves to index 0, whose bind returns
  // SQLITE_RANGE — accumulated above, so step() fails loud instead of writing
  // NULL (ID-4). The enrichment writers share these names across statements so
  // a typo cannot silently bind the wrong column.
  [[nodiscard]] int index_of(const char* name) const {
    return sqlite3_bind_parameter_index(stmt_, name);
  }
  void bind_int64(const char* n, std::int64_t v) { bind_int64(index_of(n), v); }
  void bind_bool(const char* n, bool v) { bind_int64(index_of(n), v ? 1 : 0); }
  void bind_text(const char* n, std::string_view v) { bind_text(index_of(n), v); }
  void bind_null(const char* n) { bind_null(index_of(n)); }
  void bind_opt_int64(const char* n, std::optional<std::int64_t> v) {
    bind_opt_int64(index_of(n), v);
  }
  void bind_opt_uint(const char* n, std::optional<std::uint32_t> v) {
    bind_opt_uint(index_of(n), v);
  }
  void bind_opt_text(const char* n, std::optional<std::string_view> v) {
    bind_opt_text(index_of(n), v);
  }

  // SQLITE_ROW / SQLITE_DONE / an error code. A statement with any failed
  // bind never steps: it would execute with a silent NULL in that slot (ID-4).
  [[nodiscard]] int step() {
    if (bind_rc_ != SQLITE_OK) return bind_rc_;
    return sqlite3_step(stmt_);
  }

  [[nodiscard]] std::int64_t col_int64(int i) const { return sqlite3_column_int64(stmt_, i); }
  [[nodiscard]] double col_double(int i) const { return sqlite3_column_double(stmt_, i); }
  [[nodiscard]] bool col_is_null(int i) const {
    return sqlite3_column_type(stmt_, i) == SQLITE_NULL;
  }
  [[nodiscard]] std::string col_text(int i) const {
    const auto* p = sqlite3_column_text(stmt_, i);
    if (p == nullptr) return {};
    const int n = sqlite3_column_bytes(stmt_, i);
    return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
  }

  // Typed nullable column reads for the enrichment/show row mappers. A SQL NULL
  // maps to nullopt regardless of the declared column type.
  [[nodiscard]] std::optional<std::int64_t> col_opt_int64(int i) const {
    return col_is_null(i) ? std::nullopt : std::optional<std::int64_t>(col_int64(i));
  }
  [[nodiscard]] std::optional<std::uint32_t> col_opt_uint(int i) const {
    return col_is_null(i) ? std::nullopt
                          : std::optional<std::uint32_t>(static_cast<std::uint32_t>(col_int64(i)));
  }
  [[nodiscard]] std::optional<double> col_opt_double(int i) const {
    return col_is_null(i) ? std::nullopt : std::optional<double>(col_double(i));
  }
  [[nodiscard]] std::optional<std::string> col_opt_text(int i) const {
    return col_is_null(i) ? std::nullopt : std::optional<std::string>(col_text(i));
  }

 private:
  // First failed bind's rc, sticky; step() refuses to run while set (ID-4).
  void note(int rc) {
    if (bind_rc_ == SQLITE_OK && rc != SQLITE_OK) bind_rc_ = rc;
  }

  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
  int rc_ = SQLITE_ERROR;
  int bind_rc_ = SQLITE_OK;
};

// Run a DDL/DML batch with no result rows (sqlite3_exec). Contention is not
// retried here — callers that can race (migrate) drive their own retry.
Result<Unit, StoreError> exec(sqlite3* db, const char* sql) {
  char* msg = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
  if (rc != SQLITE_OK) {
    std::string why = msg != nullptr ? std::string(msg) : driver_msg(db);
    sqlite3_free(msg);
    return err(StoreError::sqlite(std::move(why)));
  }
  return Unit{};
}

// PRAGMA journal_mode = WAL, retrying contention by hand (busy_timeout does not
// cover this lock upgrade). In-memory DBs report "memory"; only the flip
// failing hard is an error (store.rs enable_wal).
Result<Unit, StoreError> enable_wal(sqlite3* db) {
  for (int attempt = 0;; ++attempt) {
    Stmt s(db, "PRAGMA journal_mode = WAL");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
    const int rc = s.step();
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) return Unit{};
    if (attempt < kLockRetryLimit && is_contended(rc)) {
      backoff();
      continue;
    }
    return err(StoreError::sqlite(driver_msg(db)));
  }
}

// Read + validate user_version: negative (a legal header state) is a clean
// SchemaInvalid, too-new refuses before any write (store.rs user_version).
Result<std::uint32_t, StoreError> user_version(sqlite3* db) {
  Stmt s(db, "PRAGMA user_version");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  if (s.step() != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(db)));
  const std::int64_t v = s.col_int64(0);
  if (v < 0 || v > static_cast<std::int64_t>(UINT32_MAX)) {
    return err(StoreError::schema_invalid(static_cast<std::uint32_t>(v & 0xFFFFFFFF)));
  }
  const auto uv = static_cast<std::uint32_t>(v);
  if (uv > kSchemaVersion) return err(StoreError::schema_too_new(uv, kSchemaVersion));
  return uv;
}

// user_version alone proves nothing: a stamped version over missing DDL (a
// foreign file, a mid-write backup) must fail here, not as a raw "no such
// table" later (store.rs verify_schema_present, ROD-434).
Result<Unit, StoreError> verify_schema_present(sqlite3* db) {
  Stmt s(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='show'");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  if (s.step() != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(db)));
  if (s.col_int64(0) == 0) return err(StoreError::schema_missing(kSchemaVersion));
  return Unit{};
}

// BEGIN IMMEDIATE with the WAL-flip's bounded retry: a peer holding the ladder
// lock past busy_timeout is waited out, not turned into a hard failure.
Result<Unit, StoreError> begin_immediate(sqlite3* db) {
  for (int attempt = 0;; ++attempt) {
    const int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (rc == SQLITE_OK) return Unit{};
    if (attempt < kLockRetryLimit && is_contended(rc)) {
      backoff();
      continue;
    }
    return err(StoreError::sqlite(driver_msg(db)));
  }
}

// Run the ladder + version bump under one BEGIN IMMEDIATE (store.rs migrate):
// DDL and the bump land atomically, no half-applied state. Fast-path the
// already-current version without a write lock (WAL).
Result<Unit, StoreError> migrate(sqlite3* db) {
  {
    auto v = user_version(db);
    if (!v.has_value()) return err(v.error());
    if (*v == kSchemaVersion) return verify_schema_present(db);
  }

  auto begun = begin_immediate(db);
  if (!begun.has_value()) return err(begun.error());

  // Re-read under the lock: a peer may have finished while we waited.
  auto v = user_version(db);
  if (!v.has_value()) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(v.error());
  }
  std::uint32_t at = *v;
  if (at == kSchemaVersion) {
    auto ok = verify_schema_present(db);
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return ok;
  }

  if (at < 1) {
    auto ddl = exec(db, kMigrationV1);
    if (!ddl.has_value()) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(ddl.error());
    }
    at = 1;
  }
  if (at < 2) {
    auto ddl = exec(db, kMigrationV2);
    if (!ddl.has_value()) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(ddl.error());
    }
    at = 2;
  }
  if (at < 3) {
    auto ddl = exec(db, kMigrationV3);
    if (!ddl.has_value()) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(ddl.error());
    }
    at = 3;
  }
  if (at < 4) {
    auto ddl = exec(db, kMigrationV4);
    if (!ddl.has_value()) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(ddl.error());
    }
    at = 4;
  }
  if (at < 5) {
    auto ddl = exec(db, kMigrationV5);
    if (!ddl.has_value()) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(ddl.error());
    }
    at = 5;
  }
  if (at < 6) {
    auto ddl = exec(db, kMigrationV6);
    if (!ddl.has_value()) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(ddl.error());
    }
    at = 6;
  }

  // A strippable assert here would BE the half-applied-schema bug (02 §5); this
  // is a real runtime check in every build.
  if (at != kSchemaVersion) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(StoreError::migration_incomplete(at, kSchemaVersion));
  }

  // PRAGMA user_version does not take a bound parameter; the value is a
  // compile-time constant, so the concatenation is injection-safe.
  const std::string bump = "PRAGMA user_version = " + std::to_string(kSchemaVersion);
  auto stamped = exec(db, bump.c_str());
  if (!stamped.has_value()) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(stamped.error());
  }
  return exec(db, "COMMIT");
}

// SQLite silently downgrades to read-only when the file denies READWRITE; that
// must fail loud at open, not at the first write (store.rs probe_writable).
Result<Unit, StoreError> probe_writable(sqlite3* db, std::string_view path) {
  if (sqlite3_db_readonly(db, "main") == 1) {
    return err(StoreError::read_only(std::string(path)));
  }
  return Unit{};
}

// Shared open tail (store.rs finish_open): busy_timeout BEFORE the first
// statement so migrate's BEGIN IMMEDIATE can wait out a concurrent opener, then
// WAL, FKs, migrate, writability probe. On success the caller adopts the handle
// into a Store; on failure the handle is closed here and the error returned.
Result<Unit, StoreError> finish_open(sqlite3* db, std::string_view path) {
  auto fail = [&](StoreError e) {
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

// The current show's status/progress snapshot for the ratchet, read inside the
// caller's own transaction (store.rs status_row: takes the conn, not &self).
struct StatusRow {
  ListStatus status = ListStatus::Planning;
  std::uint32_t progress = 0;
  std::optional<std::uint32_t> total;
  bool airing = false;
  bool found = false;
};

Result<StatusRow, StoreError> status_row(sqlite3* db, std::int64_t anilist_id) {
  Stmt s(db,
         "SELECT list_status, progress, total_episodes, status "
         "FROM show WHERE anilist_id = ?1");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  s.bind_int64(1, anilist_id);
  const int rc = s.step();
  if (rc == SQLITE_DONE) return StatusRow{};  // found == false.
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(db)));
  StatusRow out;
  out.found = true;
  out.status = parse_list_status(s.col_text(0));
  out.progress = static_cast<std::uint32_t>(s.col_int64(1));
  out.total = s.col_is_null(2) ? std::nullopt
                               : std::optional<std::uint32_t>(
                                     static_cast<std::uint32_t>(s.col_int64(2)));
  const std::optional<std::string> media =
      s.col_is_null(3) ? std::nullopt : std::optional<std::string>(s.col_text(3));
  out.airing = is_still_airing(media.has_value()
                                   ? std::optional<std::string_view>(*media)
                                   : std::nullopt);
  return out;
}

// The shared episode_progress upsert body used by save_progress and
// record_finish. `watched` is computed by the caller from the single-authority
// ratio so both writers agree byte-for-byte.
Result<Unit, StoreError> upsert_progress(sqlite3* db, std::int64_t anilist_id,
                                         Translation translation, std::string_view episode,
                                         double position_secs, double duration_secs,
                                         bool watched,
                                         std::optional<std::string_view> last_provider,
                                         std::int64_t now) {
  Stmt s(db,
         "INSERT INTO episode_progress "
         "(anilist_id, translation, episode, position_secs, duration_secs, "
         " fully_watched, updated_at, last_provider) "
         "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
         "ON CONFLICT(anilist_id, translation, episode) DO UPDATE SET "
         "  position_secs = excluded.position_secs, "
         "  duration_secs = excluded.duration_secs, "
         "  fully_watched = excluded.fully_watched, "
         "  updated_at    = excluded.updated_at, "
         "  last_provider = excluded.last_provider");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  s.bind_int64(1, anilist_id);
  s.bind_text(2, to_string(translation));
  s.bind_text(3, episode);
  s.bind_double(4, position_secs);
  s.bind_double(5, duration_secs);
  s.bind_int64(6, watched ? 1 : 0);
  s.bind_int64(7, now);
  s.bind_opt_text(8, last_provider);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(db)));
  return Unit{};
}

// The show-row engagement/ratchet UPDATE shared by record_finish. Stamps
// progress_stamped_at exactly when the frontier moves (the SET RHS reads the
// OLD row — store.rs ROD-477), bumps play_count/last_watched, and sets
// library_added_at once.
Result<Unit, StoreError> bump_engagement(sqlite3* db, std::int64_t anilist_id,
                                         std::uint32_t new_progress, ListStatus new_status,
                                         std::int64_t now) {
  Stmt s(db,
         "UPDATE show SET "
         "  play_count = play_count + 1, "
         "  last_watched_at = ?1, "
         "  progress = ?2, "
         "  progress_stamped_at = CASE WHEN progress <> ?2 THEN ?1 "
         "                             ELSE progress_stamped_at END, "
         "  list_status = ?3, "
         "  library_added_at = COALESCE(library_added_at, ?1) "
         "WHERE anilist_id = ?4");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  s.bind_int64(1, now);
  s.bind_int64(2, static_cast<std::int64_t>(new_progress));
  s.bind_text(3, to_string(new_status));
  s.bind_int64(4, anilist_id);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(db)));
  return Unit{};
}

// --- Enrichment column plumbing (store.rs ENRICH_COLS / enrich_params) ------
//
// The enrichment column set, in the one order every reader and writer uses.
// `enrichment_from_row` reads these by index (0..25); keep the const and the
// reader in lockstep. `kEnrichVals` is the matching :named VALUES list.
constexpr const char* kEnrichCols =
    "anilist_id, mal_id, title_romaji, title_english, title_native, "
    "cover_url, total_episodes, duration_minutes, year, season, status, description, score, "
    "kind, start_year, start_month, start_day, genres, studios, source_material, rank, "
    "rank_type, rank_year, next_airing_at, next_airing_episode, country";

constexpr const char* kEnrichVals =
    ":anilist_id, :mal_id, :title_romaji, :title_english, :title_native, "
    ":cover_url, :total_episodes, :duration_minutes, :year, :season, :status, :description, "
    ":score, :kind, :start_year, :start_month, :start_day, :genres, :studios, :source_material, "
    ":rank, :rank_type, :rank_year, :next_airing_at, :next_airing_episode, :country";

// The 15 show-only state columns, appended after kEnrichCols in the full read.
constexpr const char* kShowStateCols =
    "enrichment_fetched_at, enrichment_fieldset_version, list_status, "
    "user_rating, notes, play_count, progress, library_added_at, last_watched_at, "
    "synced_status, synced_progress, user_score, synced_score, "
    "notice_last_episode, notice_pending";

// Empty binds NULL so the merge keeps prior values: a source that stopped
// sending lists must not wipe them (ROD-261 rule; JSON encoding, 02 L5). The
// compact dump matches serde_json's `["a","b"]` byte-for-byte (data-compat).
std::optional<std::string> encode_list(const std::vector<std::string>& list) {
  if (list.empty()) return std::nullopt;
  return json(list).dump();
}

// Display-only lists: a corrupt cell degrades to empty, never an error.
std::vector<std::string> decode_list(const std::optional<std::string>& cell) {
  if (!cell.has_value()) return {};
  json parsed = json::parse(*cell, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_array()) return {};
  std::vector<std::string> out;
  for (const auto& v : parsed) {
    if (v.is_string()) out.push_back(v.get<std::string>());
  }
  return out;
}

// Bind every :enrichment param on `s` from `e`. Owned encodings (season string,
// JSON lists) are materialized here and bound via SQLITE_TRANSIENT, so nothing
// must outlive the call (unlike rusqlite's borrow-checked EnrichBind).
void bind_enrichment(Stmt& s, const Enrichment& e) {
  s.bind_int64(":anilist_id", e.anilist_id);
  s.bind_opt_int64(":mal_id", e.mal_id);
  s.bind_text(":title_romaji", e.title_romaji);
  s.bind_opt_text(":title_english", opt_view(e.title_english));
  s.bind_opt_text(":title_native", opt_view(e.title_native));
  s.bind_opt_text(":cover_url", opt_view(e.cover_url));
  s.bind_opt_uint(":total_episodes", e.total_episodes);
  s.bind_opt_uint(":duration_minutes", e.duration_minutes);
  s.bind_opt_uint(":year", e.year);
  s.bind_opt_text(":season", e.season.has_value()
                                 ? std::optional<std::string_view>(season_as_str(*e.season))
                                 : std::nullopt);
  s.bind_opt_text(":status", opt_view(e.status));
  s.bind_opt_text(":description", opt_view(e.description));
  s.bind_opt_uint(":score", e.score);
  s.bind_opt_text(":kind", opt_view(e.kind));
  s.bind_opt_uint(":start_year", e.start_date.has_value()
                                     ? std::optional<std::uint32_t>(e.start_date->year)
                                     : std::nullopt);
  s.bind_opt_uint(":start_month",
                  e.start_date.has_value() ? e.start_date->month : std::nullopt);
  s.bind_opt_uint(":start_day", e.start_date.has_value() ? e.start_date->day : std::nullopt);
  const std::optional<std::string> genres = encode_list(e.genres);
  const std::optional<std::string> studios = encode_list(e.studios);
  s.bind_opt_text(":genres", opt_view(genres));
  s.bind_opt_text(":studios", opt_view(studios));
  s.bind_opt_text(":source_material", opt_view(e.source_material));
  s.bind_opt_uint(":rank", e.rank);
  s.bind_opt_text(":rank_type", opt_view(e.rank_type));
  s.bind_opt_uint(":rank_year", e.rank_year);
  s.bind_opt_int64(":next_airing_at", e.next_airing_at);
  s.bind_opt_uint(":next_airing_episode", e.next_airing_episode);
  s.bind_opt_text(":country", opt_view(e.country));
}

// Read the kEnrichCols order by index (0..25). Keep in lockstep with the const.
Enrichment enrichment_from_row(const Stmt& s) {
  Enrichment e;
  e.anilist_id = s.col_int64(0);
  e.mal_id = s.col_opt_int64(1);
  e.title_romaji = s.col_text(2);
  e.title_english = s.col_opt_text(3);
  e.title_native = s.col_opt_text(4);
  e.cover_url = s.col_opt_text(5);
  e.total_episodes = s.col_opt_uint(6);
  e.duration_minutes = s.col_opt_uint(7);
  e.year = s.col_opt_uint(8);
  if (const auto season = s.col_opt_text(9); season.has_value()) {
    e.season = parse_season(*season);
  }
  e.status = s.col_opt_text(10);
  e.description = s.col_opt_text(11);
  e.score = s.col_opt_uint(12);
  e.kind = s.col_opt_text(13);
  if (const auto start_year = s.col_opt_uint(14); start_year.has_value()) {
    e.start_date = Date{*start_year, s.col_opt_uint(15), s.col_opt_uint(16)};
  }
  e.genres = decode_list(s.col_opt_text(17));
  e.studios = decode_list(s.col_opt_text(18));
  e.source_material = s.col_opt_text(19);
  e.rank = s.col_opt_uint(20);
  e.rank_type = s.col_opt_text(21);
  e.rank_year = s.col_opt_uint(22);
  e.next_airing_at = s.col_opt_int64(23);
  e.next_airing_episode = s.col_opt_uint(24);
  e.country = s.col_opt_text(25);
  return e;
}

// The full library row: kEnrichCols (0..25) then kShowStateCols (26..40).
Show show_from_row(const Stmt& s) {
  Show show;
  show.enrichment = enrichment_from_row(s);
  show.enrichment_fetched_at = s.col_opt_int64(26);
  show.enrichment_fieldset_version = s.col_opt_uint(27);
  show.list_status = parse_list_status(s.col_text(28));
  show.user_rating = s.col_opt_double(29);
  show.notes = s.col_opt_text(30);
  show.play_count = static_cast<std::uint32_t>(s.col_int64(31));
  show.progress = static_cast<std::uint32_t>(s.col_int64(32));
  show.library_added_at = s.col_opt_int64(33);
  show.last_watched_at = s.col_opt_int64(34);
  if (const auto synced = s.col_opt_text(35); synced.has_value()) {
    show.synced_status = parse_list_status(*synced);
  }
  show.synced_progress = s.col_opt_uint(36);
  show.user_score = s.col_opt_uint(37);
  show.synced_score = s.col_opt_uint(38);
  show.notice_last_episode = s.col_opt_uint(39);
  show.notice_pending = s.col_int64(40) != 0;
  return show;
}

// The enrichment merge SET fragment, shared by every writer so the shape cannot
// drift between tables (store.rs enrichment_merge_set). Incoming-first COALESCE:
// a fresh non-null wins, an incoming NULL never wipes (a re-search must not
// erase enrichment). The cover CASE keeps an absolute URL from downgrading to a
// relative one; GLOB is case-sensitive on purpose (ROD-267). total_episodes is
// absent here: each writer owns its own total rule (ROD-419).
std::string enrichment_merge_set(const std::string& new_, const std::string& old) {
  static const char* cols[] = {
      "mal_id",       "title_english", "title_native",    "duration_minutes",
      "year",         "season",        "status",          "description",
      "score",        "kind",          "start_year",      "start_month",
      "start_day",    "genres",        "studios",         "source_material",
      "rank",         "rank_type",     "rank_year",       "next_airing_at",
      "next_airing_episode",           "country"};
  // NULLIF: a blank incoming title is absence, not a value; it must never wipe
  // a real one (ROD-434; kin to zigoku's ROD-312 title guard).
  std::string set =
      "title_romaji = COALESCE(NULLIF(" + new_ + "title_romaji, ''), " + old + "title_romaji)";
  for (const char* col : cols) {
    set += ", " + std::string(col) + " = COALESCE(" + new_ + col + ", " + old + col + ")";
  }
  const std::string old_cover = old + "cover_url";
  set += ", cover_url = CASE"
         " WHEN " + new_ + "cover_url GLOB 'http://*' OR " + new_ +
         "cover_url GLOB 'https://*' THEN " + new_ + "cover_url"
         " WHEN " + old_cover + " GLOB 'http://*' OR " + old_cover +
         " GLOB 'https://*' THEN " + old_cover +
         " ELSE COALESCE(" + new_ + "cover_url, " + old_cover + ") END";
  return set;
}

// Mint an identity row from enrichment when absent, leaving an existing row's
// enrichment untouched (store.rs ensure_show_row). Shared by every mint-on-
// write path (bind, absence, route). ON CONFLICT DO NOTHING.
Result<Unit, StoreError> ensure_show_row(sqlite3* db, const Enrichment& e) {
  const std::string sql = "INSERT INTO show (" + std::string(kEnrichCols) + ") VALUES (" +
                          std::string(kEnrichVals) + ") ON CONFLICT(anilist_id) DO NOTHING";
  Stmt s(db, sql.c_str());
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  bind_enrichment(s, e);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(db)));
  return Unit{};
}

// The watchlist-add INSERT on any connection (store.rs add_to_library_on): the
// enrichment merge plus a set-once library_added_at stamp. User-state columns
// are absent from the SET entirely so a re-add never clobbers them (02 §5).
Result<Unit, StoreError> add_to_library_on(sqlite3* db, const Enrichment& e, std::int64_t now) {
  const std::string sql =
      "INSERT INTO show (" + std::string(kEnrichCols) + ", library_added_at) VALUES (" +
      std::string(kEnrichVals) + ", :now) ON CONFLICT(anilist_id) DO UPDATE SET " +
      enrichment_merge_set("excluded.", "show.") +
      ", total_episodes = COALESCE(excluded.total_episodes, show.total_episodes)"
      ", library_added_at = COALESCE(show.library_added_at, excluded.library_added_at)";
  Stmt s(db, sql.c_str());
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  bind_enrichment(s, e);
  s.bind_int64(":now", now);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(db)));
  return Unit{};
}

}  // namespace

// --- TTL helpers (store.rs enrichment_ttl_secs / episode_cache_ttl_secs) ----

namespace {
bool eq_ascii_ci(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto ca = static_cast<unsigned char>(a[i]);
    const auto cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}
}  // namespace

std::int64_t enrichment_ttl_secs(std::optional<std::string_view> status) {
  if (status.has_value()) {
    if (eq_ascii_ci(*status, "FINISHED")) return kEnrichTtlFinishedSecs;
    if (eq_ascii_ci(*status, "RELEASING")) return kEnrichTtlReleasingSecs;
  }
  return kEnrichTtlDefaultSecs;
}

std::int64_t episode_cache_ttl_secs(std::optional<std::string_view> status) {
  if (status.has_value()) {
    if (eq_ascii_ci(*status, "FINISHED")) return kEpCacheTtlFinishedSecs;
    if (eq_ascii_ci(*status, "RELEASING")) return kEpCacheTtlReleasingSecs;
  }
  return kEpCacheTtlDefaultSecs;
}

// --- Resume::start_secs (store.rs Resume::start_secs) ----------------------

double Resume::start_secs(std::uint32_t resume_offset_secs) const {
  if (fully_watched || natural_end(position_secs, duration_secs) ||
      !std::isfinite(position_secs) || position_secs <= 0.0) {
    return 0.0;
  }
  return std::max(position_secs - static_cast<double>(resume_offset_secs), 0.0);
}

// --- Store lifecycle -------------------------------------------------------

Store::~Store() {
  if (conn_ != nullptr) sqlite3_close(conn_);
}

Store::Store(Store&& o) noexcept : conn_(o.conn_) { o.conn_ = nullptr; }

Store& Store::operator=(Store&& o) noexcept {
  if (this != &o) {
    if (conn_ != nullptr) sqlite3_close(conn_);
    conn_ = o.conn_;
    o.conn_ = nullptr;
  }
  return *this;
}

Result<Store, StoreError> Store::open(std::string_view path) {
  sqlite3* db = nullptr;
  // Need a NUL-terminated path for the C API.
  const std::string p(path);
  const int rc = sqlite3_open_v2(
      p.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    std::string why = db != nullptr ? driver_msg(db) : std::string("open failed");
    sqlite3_close(db);
    return err(StoreError::sqlite(std::move(why)));
  }
  if (auto r = finish_open(db, path); !r.has_value()) return err(r.error());
  return Store(db);
}

Result<Store, StoreError> Store::open_memory() {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open(":memory:", &db);
  if (rc != SQLITE_OK) {
    std::string why = db != nullptr ? driver_msg(db) : std::string("open failed");
    sqlite3_close(db);
    return err(StoreError::sqlite(std::move(why)));
  }
  if (auto r = finish_open(db, ":memory:"); !r.has_value()) return err(r.error());
  return Store(db);
}

// --- Writers ---------------------------------------------------------------

Result<Unit, StoreError> Store::ensure_show(std::int64_t anilist_id,
                                            std::string_view title_romaji,
                                            std::optional<std::int64_t> mal_id,
                                            std::optional<std::uint32_t> total_episodes,
                                            std::optional<std::string_view> media_status) {
  Stmt s(conn_,
         "INSERT INTO show (anilist_id, title_romaji, mal_id, total_episodes, status) "
         "VALUES (?1, ?2, ?3, ?4, ?5) "
         "ON CONFLICT(anilist_id) DO NOTHING");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  s.bind_text(2, title_romaji);
  s.bind_opt_int64(3, mal_id);
  s.bind_opt_uint(4, total_episodes);
  s.bind_opt_text(5, media_status);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

Result<Unit, StoreError> Store::save_progress(std::int64_t anilist_id, Translation translation,
                                              std::string_view episode, double position_secs,
                                              double duration_secs,
                                              std::optional<std::string_view> last_provider,
                                              std::int64_t now) {
  if (!std::isfinite(position_secs) || !std::isfinite(duration_secs)) {
    return err(StoreError::non_finite("save_progress: non-finite position/duration"));
  }
  const bool watched = duration_secs > 0.0 && position_secs / duration_secs >= kWatchedRatio;
  return upsert_progress(conn_, anilist_id, translation, episode, position_secs, duration_secs,
                         watched, last_provider, now);
}

Result<Unit, StoreError> Store::record_finish(std::int64_t anilist_id, Translation translation,
                                              std::string_view episode,
                                              std::uint32_t episode_index, double position_secs,
                                              double duration_secs,
                                              std::optional<std::string_view> last_provider,
                                              std::int64_t now) {
  if (!std::isfinite(position_secs) || !std::isfinite(duration_secs)) {
    return err(StoreError::non_finite("record_finish: non-finite position/duration"));
  }
  if (episode_index == 0) return Unit{};  // membership must not ride an unknown ep.

  if (auto b = begin_immediate(conn_); !b.has_value()) return err(b.error());
  auto rollback = [&](StoreError e) {
    sqlite3_exec(conn_, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(std::move(e));
  };

  auto cur = status_row(conn_, anilist_id);
  if (!cur.has_value()) return rollback(cur.error());
  if (!cur->found) {
    sqlite3_exec(conn_, "ROLLBACK", nullptr, nullptr, nullptr);
    return Unit{};  // unknown show: no-op (not an error).
  }

  const bool watched = duration_secs > 0.0 && position_secs / duration_secs >= kWatchedRatio;
  if (auto u = upsert_progress(conn_, anilist_id, translation, episode, position_secs,
                               duration_secs, watched, last_provider, now);
      !u.has_value()) {
    return rollback(u.error());
  }

  const std::uint32_t new_progress =
      natural_end(position_secs, duration_secs)
          ? std::max(cur->progress, episode_index)
          : cur->progress;
  const ListStatus new_status =
      after_play_status(cur->status, new_progress, cur->total, cur->airing);
  if (auto e = bump_engagement(conn_, anilist_id, new_progress, new_status, now);
      !e.has_value()) {
    return rollback(e.error());
  }

  if (auto c = exec(conn_, "COMMIT"); !c.has_value()) return rollback(c.error());
  return Unit{};
}

// --- Readers ---------------------------------------------------------------

Result<std::optional<Resume>, StoreError> Store::get_resume(std::int64_t anilist_id,
                                                            Translation translation,
                                                            std::string_view episode) const {
  Stmt s(conn_,
         "SELECT position_secs, duration_secs, fully_watched FROM episode_progress "
         "WHERE anilist_id = ?1 AND translation = ?2 AND episode = ?3");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  s.bind_text(2, to_string(translation));
  s.bind_text(3, episode);
  const int rc = s.step();
  if (rc == SQLITE_DONE) return std::optional<Resume>(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  Resume r;
  r.position_secs = s.col_double(0);
  r.duration_secs = s.col_double(1);
  r.fully_watched = s.col_int64(2) != 0;
  return std::optional<Resume>(r);
}

Result<std::optional<std::pair<std::string, Resume>>, StoreError> Store::latest_resume(
    std::int64_t anilist_id, Translation translation) const {
  // Only a partial written AFTER the frontier stamp resumes: one at or before
  // it was ratcheted past (recompute, completion, AniList adoption) and is a
  // dead resume point (ROD-477; strict >, epoch seconds).
  Stmt s(conn_,
         "SELECT ep.episode, ep.position_secs, ep.duration_secs, ep.fully_watched "
         "FROM episode_progress ep "
         "JOIN show s ON s.anilist_id = ep.anilist_id "
         "WHERE ep.anilist_id = ?1 AND ep.translation = ?2 "
         "  AND ep.fully_watched = 0 AND ep.position_secs > 0 "
         "  AND ep.updated_at > COALESCE(s.progress_stamped_at, 0) "
         "ORDER BY ep.updated_at DESC LIMIT 1");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  s.bind_text(2, to_string(translation));
  const int rc = s.step();
  using Out = std::optional<std::pair<std::string, Resume>>;
  if (rc == SQLITE_DONE) return Out(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  Resume r;
  std::string ep = s.col_text(0);
  r.position_secs = s.col_double(1);
  r.duration_secs = s.col_double(2);
  r.fully_watched = s.col_int64(3) != 0;
  return Out(std::make_pair(std::move(ep), r));
}

Result<std::uint32_t, StoreError> Store::recompute_progress(std::int64_t anilist_id,
                                                            Translation translation,
                                                            std::int64_t now) {
  // Present rows for the active translation; sort by label and take the 1-based
  // ordinal of the last fully_watched (positional high-water, 02 §4b).
  std::vector<std::pair<std::string, bool>> rows;
  {
    Stmt s(conn_,
           "SELECT episode, fully_watched FROM episode_progress "
           "WHERE anilist_id = ?1 AND translation = ?2");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, anilist_id);
    s.bind_text(2, to_string(translation));
    for (;;) {
      const int rc = s.step();
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
      rows.emplace_back(s.col_text(0), s.col_int64(1) != 0);
    }
  }
  // Stable sort: specials (all +inf key) keep their incoming order.
  std::stable_sort(rows.begin(), rows.end(),
                   [](const auto& a, const auto& b) {
                     return episode_label_cmp(a.first, b.first) < 0;
                   });
  std::uint32_t high_water = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].second) high_water = static_cast<std::uint32_t>(i) + 1;
  }

  // Overwrite show.progress unconditionally, stamping the frontier on a move
  // (same clause as bump_engagement — keep them in lockstep, ROD-477).
  Stmt u(conn_,
         "UPDATE show SET progress = ?1, "
         "  progress_stamped_at = CASE WHEN progress <> ?1 THEN ?2 "
         "                             ELSE progress_stamped_at END "
         "WHERE anilist_id = ?3");
  if (!u.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  u.bind_int64(1, static_cast<std::int64_t>(high_water));
  u.bind_int64(2, now);
  u.bind_int64(3, anilist_id);
  if (u.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return high_water;
}

Result<std::optional<std::uint32_t>, StoreError> Store::get_progress(
    std::int64_t anilist_id) const {
  Stmt s(conn_, "SELECT progress FROM show WHERE anilist_id = ?1");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  const int rc = s.step();
  if (rc == SQLITE_DONE) return std::optional<std::uint32_t>(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  return std::optional<std::uint32_t>(static_cast<std::uint32_t>(s.col_int64(0)));
}

Result<WatchedMarks, StoreError> Store::watched_marks(
    std::int64_t anilist_id, Translation translation,
    const std::vector<std::string>& episodes) const {
  // Pull the fully_watched labels once, then mark the grid in its own order.
  std::vector<std::string> watched;
  {
    Stmt s(conn_,
           "SELECT episode FROM episode_progress "
           "WHERE anilist_id = ?1 AND translation = ?2 AND fully_watched = 1");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, anilist_id);
    s.bind_text(2, to_string(translation));
    for (;;) {
      const int rc = s.step();
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
      watched.push_back(s.col_text(0));
    }
  }
  WatchedMarks marks(episodes.size(), false);
  for (std::size_t i = 0; i < episodes.size(); ++i) {
    marks[i] = std::find(watched.begin(), watched.end(), episodes[i]) != watched.end();
  }
  return marks;
}

// --- Catalog cache + enrichment (02 §3.5, 04 §10 L2) -----------------------

Result<Unit, StoreError> Store::upsert_catalog_cache(const Enrichment& e, std::int64_t now,
                                                     std::optional<std::int64_t> expires_at) {
  const std::string sql =
      "INSERT INTO catalog_cache (" + std::string(kEnrichCols) +
      ", fieldset_version, fetched_at, expires_at) VALUES (" + std::string(kEnrichVals) +
      ", :fieldset_version, :now, :expires_at) ON CONFLICT(anilist_id) DO UPDATE SET " +
      enrichment_merge_set("excluded.", "catalog_cache.") +
      ", total_episodes = COALESCE(excluded.total_episodes, catalog_cache.total_episodes)"
      ", fieldset_version = excluded.fieldset_version"
      ", fetched_at = excluded.fetched_at"
      ", expires_at = excluded.expires_at";
  Stmt s(conn_, sql.c_str());
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  bind_enrichment(s, e);
  s.bind_int64(":fieldset_version", static_cast<std::int64_t>(kEnrichmentFieldsetVersion));
  s.bind_int64(":now", now);
  s.bind_opt_int64(":expires_at", expires_at);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

Result<std::optional<CatalogHit>, StoreError> Store::get_catalog(std::int64_t anilist_id) const {
  const std::string sql = "SELECT " + std::string(kEnrichCols) +
                          ", fieldset_version, fetched_at, expires_at "
                          "FROM catalog_cache WHERE anilist_id = ?1";
  Stmt s(conn_, sql.c_str());
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  const int rc = s.step();
  using Out = std::optional<CatalogHit>;
  if (rc == SQLITE_DONE) return Out(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  CatalogHit hit;
  hit.enrichment = enrichment_from_row(s);
  hit.fieldset_version = static_cast<std::uint32_t>(s.col_int64(26));
  hit.fetched_at = s.col_int64(27);
  hit.expires_at = s.col_opt_int64(28);
  return Out(std::move(hit));
}

Result<Unit, StoreError> Store::add_to_library(const Enrichment& e, std::int64_t now) {
  return add_to_library_on(conn_, e, now);
}

Result<bool, StoreError> Store::patch_show_enrichment(const Enrichment& e, bool stamp_fresh,
                                                      std::int64_t now) {
  // A stamped answer with no total on an airing show proves the stored total
  // was an availability snapshot, not the finale; COALESCE would pin it forever
  // (ROD-419). Partial feeds (stamp_fresh=false) can neither stamp nor clear.
  const bool clear_total =
      stamp_fresh && !e.total_episodes.has_value() && is_still_airing(opt_view(e.status));
  const std::string sql =
      "UPDATE show SET " + enrichment_merge_set(":", "") +
      ", total_episodes = CASE WHEN :clear_total THEN NULL"
      " ELSE COALESCE(:total_episodes, total_episodes) END"
      ", enrichment_fetched_at = CASE WHEN :stamp THEN :now ELSE enrichment_fetched_at END"
      ", enrichment_fieldset_version = CASE WHEN :stamp THEN :fieldset_version"
      " ELSE enrichment_fieldset_version END"
      " WHERE anilist_id = :anilist_id";
  Stmt s(conn_, sql.c_str());
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  bind_enrichment(s, e);
  s.bind_bool(":stamp", stamp_fresh);
  s.bind_bool(":clear_total", clear_total);
  s.bind_int64(":now", now);
  s.bind_int64(":fieldset_version", static_cast<std::int64_t>(kEnrichmentFieldsetVersion));
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return sqlite3_changes(conn_) > 0;
}

Result<bool, StoreError> Store::enrichment_stale(std::int64_t anilist_id, std::int64_t now) const {
  auto show = get_show(anilist_id);
  if (!show.has_value()) return err(show.error());
  if (show->has_value()) {
    const Show& sh = **show;
    if (!sh.enrichment_fetched_at.has_value()) return true;  // never enriched.
    if (sh.enrichment_fieldset_version != std::optional<std::uint32_t>(kEnrichmentFieldsetVersion)) {
      return true;  // fieldset drift heals without waiting out the TTL.
    }
    const std::int64_t ttl = enrichment_ttl_secs(opt_view(sh.enrichment.status));
    return now >= *sh.enrichment_fetched_at + ttl;
  }
  auto hit = get_catalog(anilist_id);
  if (!hit.has_value()) return err(hit.error());
  if (!hit->has_value()) return true;  // no row anywhere is a miss.
  const CatalogHit& c = **hit;
  if (c.fieldset_version != kEnrichmentFieldsetVersion) return true;
  return !c.expires_at.has_value() || now >= *c.expires_at;
}

Result<bool, StoreError> Store::stamp_enrichment_checked(std::int64_t anilist_id,
                                                         std::int64_t now) {
  bool any = false;
  {
    Stmt s(conn_,
           "UPDATE show SET enrichment_fetched_at = ?1, enrichment_fieldset_version = ?2 "
           "WHERE anilist_id = ?3");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, now);
    s.bind_int64(2, static_cast<std::int64_t>(kEnrichmentFieldsetVersion));
    s.bind_int64(3, anilist_id);
    if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
    any = sqlite3_changes(conn_) > 0;
  }
  // Cache rows judge staleness by expiry, so their stamp is a renewed
  // expires_at keyed off the same status-aware TTL.
  auto hit = get_catalog(anilist_id);
  if (!hit.has_value()) return err(hit.error());
  if (hit->has_value()) {
    const std::int64_t ttl = enrichment_ttl_secs(opt_view((*hit)->enrichment.status));
    Stmt s(conn_,
           "UPDATE catalog_cache SET fetched_at = ?1, fieldset_version = ?2, expires_at = ?3 "
           "WHERE anilist_id = ?4");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, now);
    s.bind_int64(2, static_cast<std::int64_t>(kEnrichmentFieldsetVersion));
    s.bind_int64(3, now + ttl);
    s.bind_int64(4, anilist_id);
    if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
    any = any || sqlite3_changes(conn_) > 0;
  }
  return any;
}

Result<bool, StoreError> Store::promote_catalog_to_show(std::int64_t anilist_id,
                                                        std::int64_t now) {
  auto hit = get_catalog(anilist_id);
  if (!hit.has_value()) return err(hit.error());
  if (!hit->has_value()) return false;
  if (auto r = add_to_library((*hit)->enrichment, now); !r.has_value()) return err(r.error());
  return true;
}

// --- Provider bindings / pins / absences / routes --------------------------

Result<Unit, StoreError> Store::bind_provider(const Enrichment& e, std::string_view provider,
                                              std::string_view provider_id, std::int64_t now) {
  if (auto b = begin_immediate(conn_); !b.has_value()) return err(b.error());
  auto rollback = [&](StoreError er) {
    sqlite3_exec(conn_, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(std::move(er));
  };
  if (auto r = ensure_show_row(conn_, e); !r.has_value()) return rollback(r.error());
  // Steal the (provider, provider_id) edge from any other show (02 O5).
  {
    Stmt s(conn_,
           "DELETE FROM provider_binding "
           "WHERE provider = ?1 AND provider_id = ?2 AND anilist_id <> ?3");
    if (!s.prepared()) return rollback(StoreError::sqlite(driver_msg(conn_)));
    s.bind_text(1, provider);
    s.bind_text(2, provider_id);
    s.bind_int64(3, e.anilist_id);
    if (s.step() != SQLITE_DONE) return rollback(StoreError::sqlite(driver_msg(conn_)));
  }
  {
    Stmt s(conn_,
           "INSERT INTO provider_binding (anilist_id, provider, provider_id, bound_at) "
           "VALUES (?1, ?2, ?3, ?4) "
           "ON CONFLICT(anilist_id, provider) DO UPDATE SET "
           "  provider_id = excluded.provider_id, bound_at = excluded.bound_at");
    if (!s.prepared()) return rollback(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, e.anilist_id);
    s.bind_text(2, provider);
    s.bind_text(3, provider_id);
    s.bind_int64(4, now);
    if (s.step() != SQLITE_DONE) return rollback(StoreError::sqlite(driver_msg(conn_)));
  }
  // Bound and absent never coexist: bind clears the negative (02 §5).
  {
    Stmt s(conn_, "DELETE FROM provider_absence WHERE anilist_id = ?1 AND provider = ?2");
    if (!s.prepared()) return rollback(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, e.anilist_id);
    s.bind_text(2, provider);
    if (s.step() != SQLITE_DONE) return rollback(StoreError::sqlite(driver_msg(conn_)));
  }
  if (auto c = exec(conn_, "COMMIT"); !c.has_value()) return rollback(c.error());
  return Unit{};
}

Result<bool, StoreError> Store::unbind_provider(std::int64_t anilist_id,
                                                std::string_view provider) {
  Stmt s(conn_, "DELETE FROM provider_binding WHERE anilist_id = ?1 AND provider = ?2");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  s.bind_text(2, provider);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return sqlite3_changes(conn_) > 0;
}

Result<std::vector<Binding>, StoreError> Store::bindings_for(std::int64_t anilist_id) const {
  Stmt s(conn_,
         "SELECT provider, provider_id, bound_at FROM provider_binding "
         "WHERE anilist_id = ?1 ORDER BY provider");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  std::vector<Binding> out;
  for (;;) {
    const int rc = s.step();
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
    out.push_back(Binding{s.col_text(0), s.col_text(1), s.col_int64(2)});
  }
  return out;
}

Result<std::optional<std::int64_t>, StoreError> Store::show_id_for_binding(
    std::string_view provider, std::string_view provider_id) const {
  Stmt s(conn_,
         "SELECT anilist_id FROM provider_binding WHERE provider = ?1 AND provider_id = ?2");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, provider);
  s.bind_text(2, provider_id);
  const int rc = s.step();
  using Out = std::optional<std::int64_t>;
  if (rc == SQLITE_DONE) return Out(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  return Out(s.col_int64(0));
}

Result<Unit, StoreError> Store::mark_provider_absent(const Enrichment& e, std::string_view provider,
                                                     std::int64_t now) {
  if (auto b = begin_immediate(conn_); !b.has_value()) return err(b.error());
  auto rollback = [&](StoreError er) {
    sqlite3_exec(conn_, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(std::move(er));
  };
  if (auto r = ensure_show_row(conn_, e); !r.has_value()) return rollback(r.error());
  {
    Stmt s(conn_,
           "INSERT INTO provider_absence (anilist_id, provider, checked_at) VALUES (?1, ?2, ?3) "
           "ON CONFLICT(anilist_id, provider) DO UPDATE SET checked_at = excluded.checked_at");
    if (!s.prepared()) return rollback(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, e.anilist_id);
    s.bind_text(2, provider);
    s.bind_int64(3, now);
    if (s.step() != SQLITE_DONE) return rollback(StoreError::sqlite(driver_msg(conn_)));
  }
  if (auto c = exec(conn_, "COMMIT"); !c.has_value()) return rollback(c.error());
  return Unit{};
}

Result<bool, StoreError> Store::provider_absent_fresh(std::int64_t anilist_id,
                                                      std::string_view provider,
                                                      std::int64_t now) const {
  Stmt s(conn_,
         "SELECT checked_at FROM provider_absence WHERE anilist_id = ?1 AND provider = ?2");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  s.bind_text(2, provider);
  const int rc = s.step();
  if (rc == SQLITE_DONE) return false;
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  const std::int64_t checked_at = s.col_int64(0);
  return now < checked_at + kAbsenceTtlSecs;
}

Result<ProviderAvailability, StoreError> Store::provider_availability(std::int64_t anilist_id,
                                                                      std::string_view provider,
                                                                      std::int64_t now) const {
  {
    Stmt s(conn_, "SELECT 1 FROM provider_binding WHERE anilist_id = ?1 AND provider = ?2");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, anilist_id);
    s.bind_text(2, provider);
    const int rc = s.step();
    if (rc == SQLITE_ROW) return ProviderAvailability::Bound;
    if (rc != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  }
  auto absent = provider_absent_fresh(anilist_id, provider, now);
  if (!absent.has_value()) return err(absent.error());
  if (*absent) return ProviderAvailability::Absent;
  return ProviderAvailability::Unchecked;
}

Result<Unit, StoreError> Store::set_provider_pin(std::int64_t anilist_id,
                                                 std::optional<std::string_view> provider) {
  if (provider.has_value()) {
    Stmt s(conn_,
           "INSERT INTO provider_pin (anilist_id, provider) VALUES (?1, ?2) "
           "ON CONFLICT(anilist_id) DO UPDATE SET provider = excluded.provider");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, anilist_id);
    s.bind_text(2, *provider);
    if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  } else {
    Stmt s(conn_, "DELETE FROM provider_pin WHERE anilist_id = ?1");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, anilist_id);
    if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  }
  return Unit{};
}

Result<std::optional<std::string>, StoreError> Store::get_provider_pin(
    std::int64_t anilist_id) const {
  Stmt s(conn_, "SELECT provider FROM provider_pin WHERE anilist_id = ?1");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  const int rc = s.step();
  using Out = std::optional<std::string>;
  if (rc == SQLITE_DONE) return Out(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  return Out(s.col_text(0));
}

Result<std::optional<std::string>, StoreError> Store::get_route_pref(
    std::int64_t anilist_id) const {
  Stmt s(conn_, "SELECT resolved_pref FROM provider_route WHERE anilist_id = ?1");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  const int rc = s.step();
  using Out = std::optional<std::string>;
  if (rc == SQLITE_DONE) return Out(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  return Out(s.col_text(0));
}

Result<Unit, StoreError> Store::set_route_pref(const Enrichment& e, std::string_view pref) {
  if (auto b = begin_immediate(conn_); !b.has_value()) return err(b.error());
  auto rollback = [&](StoreError er) {
    sqlite3_exec(conn_, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(std::move(er));
  };
  if (auto r = ensure_show_row(conn_, e); !r.has_value()) return rollback(r.error());
  {
    Stmt s(conn_,
           "INSERT INTO provider_route (anilist_id, resolved_pref) VALUES (?1, ?2) "
           "ON CONFLICT(anilist_id) DO UPDATE SET resolved_pref = excluded.resolved_pref");
    if (!s.prepared()) return rollback(StoreError::sqlite(driver_msg(conn_)));
    s.bind_int64(1, e.anilist_id);
    s.bind_text(2, pref);
    if (s.step() != SQLITE_DONE) return rollback(StoreError::sqlite(driver_msg(conn_)));
  }
  if (auto c = exec(conn_, "COMMIT"); !c.has_value()) return rollback(c.error());
  return Unit{};
}

// --- Episode-list cache + app_meta -----------------------------------------

Result<Unit, StoreError> Store::set_episode_cache(std::int64_t anilist_id, std::string_view provider,
                                                  Translation translation,
                                                  const std::vector<std::string>& episodes,
                                                  std::optional<std::string_view> airing_status,
                                                  std::int64_t now) {
  const std::string blob = json(episodes).dump();
  Stmt s(conn_,
         "INSERT INTO episode_cache "
         "(anilist_id, provider, translation, episodes_blob, fetched_at, expires_at) "
         "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
         "ON CONFLICT(anilist_id, provider, translation) DO UPDATE SET "
         "  episodes_blob = excluded.episodes_blob, "
         "  fetched_at    = excluded.fetched_at, "
         "  expires_at    = excluded.expires_at");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  s.bind_text(2, provider);
  s.bind_text(3, to_string(translation));
  s.bind_text(4, blob);
  s.bind_int64(5, now);
  s.bind_int64(6, now + episode_cache_ttl_secs(airing_status));
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

Result<std::optional<std::vector<std::string>>, StoreError> Store::get_cached_episodes(
    std::int64_t anilist_id, std::string_view provider, Translation translation,
    std::int64_t now) const {
  Stmt s(conn_,
         "SELECT episodes_blob, expires_at FROM episode_cache "
         "WHERE anilist_id = ?1 AND provider = ?2 AND translation = ?3");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  s.bind_text(2, provider);
  s.bind_text(3, to_string(translation));
  const int rc = s.step();
  using Out = std::optional<std::vector<std::string>>;
  if (rc == SQLITE_DONE) return Out(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  const std::string blob = s.col_text(0);
  const std::int64_t expires_at = s.col_int64(1);
  if (now >= expires_at) return Out(std::nullopt);  // stale is a miss.
  // A corrupt blob is a miss (refetch), never an error.
  json parsed = json::parse(blob, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_array()) return Out(std::nullopt);
  std::vector<std::string> eps;
  for (const auto& v : parsed) {
    if (!v.is_string()) return Out(std::nullopt);
    eps.push_back(v.get<std::string>());
  }
  return Out(std::move(eps));
}

Result<std::optional<std::string>, StoreError> Store::meta_get(std::string_view key) const {
  Stmt s(conn_, "SELECT value FROM app_meta WHERE key = ?1");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, key);
  const int rc = s.step();
  using Out = std::optional<std::string>;
  if (rc == SQLITE_DONE) return Out(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  return Out(s.col_text(0));
}

Result<Unit, StoreError> Store::meta_set(std::string_view key, std::string_view value) {
  Stmt s(conn_,
         "INSERT INTO app_meta (key, value) VALUES (?1, ?2) "
         "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, key);
  s.bind_text(2, value);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

// --- Show reads / delete ---------------------------------------------------

Result<std::optional<Show>, StoreError> Store::get_show(std::int64_t anilist_id) const {
  const std::string sql = "SELECT " + std::string(kEnrichCols) + ", " +
                          std::string(kShowStateCols) + " FROM show WHERE anilist_id = ?1";
  Stmt s(conn_, sql.c_str());
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  const int rc = s.step();
  using Out = std::optional<Show>;
  if (rc == SQLITE_DONE) return Out(std::nullopt);
  if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
  return Out(show_from_row(s));
}

Result<std::vector<Show>, StoreError> Store::list_history() const {
  const std::string sql = "SELECT " + std::string(kEnrichCols) + ", " +
                          std::string(kShowStateCols) +
                          " FROM show WHERE library_added_at IS NOT NULL "
                          "ORDER BY last_watched_at DESC NULLS LAST, "
                          "library_added_at DESC, anilist_id";
  Stmt s(conn_, sql.c_str());
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  std::vector<Show> out;
  for (;;) {
    const int rc = s.step();
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
    out.push_back(show_from_row(s));
  }
  return out;
}

Result<bool, StoreError> Store::delete_show(std::int64_t anilist_id) {
  Stmt s(conn_, "DELETE FROM show WHERE anilist_id = ?1");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return sqlite3_changes(conn_) > 0;
}

// The show-row write shared by set_list_status/restore_list_status: status +
// progress + the same progress_stamped_at conditional-touch and
// library_added_at set-once semantics record_finish's bump_engagement uses,
// minus play_count/last_watched_at (a manual status change is never an
// engagement).
Result<Unit, StoreError> write_list_status(sqlite3* db, std::int64_t anilist_id,
                                           ListStatus status, std::uint32_t progress,
                                           std::int64_t now) {
  Stmt s(db,
         "UPDATE show SET "
         "  list_status = ?1, "
         "  progress = ?2, "
         "  progress_stamped_at = CASE WHEN progress <> ?2 THEN ?3 "
         "                             ELSE progress_stamped_at END, "
         "  library_added_at = COALESCE(library_added_at, ?3) "
         "WHERE anilist_id = ?4");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  s.bind_text(1, to_string(status));
  s.bind_int64(2, static_cast<std::int64_t>(progress));
  s.bind_int64(3, now);
  s.bind_int64(4, anilist_id);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(db)));
  return Unit{};
}

Result<Unit, StoreError> Store::set_list_status(std::int64_t anilist_id, ListStatus status,
                                                std::int64_t now) {
  if (auto b = begin_immediate(conn_); !b.has_value()) return err(b.error());
  auto rollback = [&](StoreError e) {
    sqlite3_exec(conn_, "ROLLBACK", nullptr, nullptr, nullptr);
    return err(std::move(e));
  };

  auto cur = status_row(conn_, anilist_id);
  if (!cur.has_value()) return rollback(cur.error());
  if (!cur->found) {
    sqlite3_exec(conn_, "ROLLBACK", nullptr, nullptr, nullptr);
    return Unit{};  // unknown show: no-op.
  }

  const std::uint32_t new_progress =
      (status == ListStatus::Completed && cur->total.has_value() && *cur->total > 0)
          ? *cur->total
          : cur->progress;
  if (auto w = write_list_status(conn_, anilist_id, status, new_progress, now);
      !w.has_value()) {
    return rollback(w.error());
  }

  if (auto c = exec(conn_, "COMMIT"); !c.has_value()) return rollback(c.error());
  return Unit{};
}

Result<Unit, StoreError> Store::restore_list_status(std::int64_t anilist_id, ListStatus status,
                                                     std::uint32_t progress, std::int64_t now) {
  return write_list_status(conn_, anilist_id, status, progress, now);
}

// --- User score (P34 slice 1) -----------------------------------------------

Result<Unit, StoreError> Store::set_user_score(std::int64_t anilist_id,
                                                std::optional<std::uint32_t> score) {
  Stmt s(conn_, "UPDATE show SET user_score = ?1 WHERE anilist_id = ?2");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_opt_uint(1, score);
  s.bind_int64(2, anilist_id);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

// --- Schedule notices (P37 slice 3) -----------------------------------------

Result<Unit, StoreError> Store::set_schedule_notice(std::int64_t anilist_id,
                                                     std::uint32_t episode) {
  Stmt s(conn_,
         "UPDATE show SET notice_last_episode = ?1, notice_pending = 1 WHERE anilist_id = ?2");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, static_cast<std::int64_t>(episode));
  s.bind_int64(2, anilist_id);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

Result<Unit, StoreError> Store::seed_schedule_notice(std::int64_t anilist_id,
                                                      std::uint32_t episode) {
  Stmt s(conn_, "UPDATE show SET notice_last_episode = ?1 WHERE anilist_id = ?2");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, static_cast<std::int64_t>(episode));
  s.bind_int64(2, anilist_id);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

Result<Unit, StoreError> Store::clear_notice_pending(std::int64_t anilist_id) {
  Stmt s(conn_, "UPDATE show SET notice_pending = 0 WHERE anilist_id = ?1");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_int64(1, anilist_id);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

// --- AniList sync (P20, 06 §5) -----------------------------------------------

namespace store_detail {

// Status/score's shared three-way matrix (06 §5.4, score folded P34 slice
// 2): adopt remote alone-moved, keep local alone-moved, keep local (flagging
// conflict) when both moved and disagree. Progress runs its own
// merge_progress instead (rise-biased; see that function's own doc).
template <typename T>
std::pair<T, bool> merge_local_wins(T base, T local, T remote) {
  const bool local_moved = local != base;
  const bool remote_moved = remote != base;
  if (!local_moved && remote_moved) return {remote, false};
  if (local_moved && !remote_moved) return {local, false};
  if (local_moved && remote_moved) return {local, local != remote};
  return {base, false};
}

Reconciled reconcile(std::optional<ListEntry> base, ListEntry local, ListEntry remote) {
  const ListEntry eff_base = base.value_or(ListEntry{});

  const auto [status, status_conflict] =
      merge_local_wins(eff_base.status, local.status, remote.status);
  const auto [score, score_conflict] =
      merge_local_wins(eff_base.score, local.score, remote.score);

  const ListEntry snapshot = (base.has_value() && *base == remote) ? *base : remote;

  Reconciled out;
  out.status = status;
  // No status special-case here, `completed` included: every rule tried
  // reduced to overriding the remote-lowered-progress cell, which is the one
  // this ticket exists to preserve (06 §5.4).
  out.progress = merge_progress(eff_base.progress, local.progress, remote.progress);
  out.score = score;
  out.snapshot_status = snapshot.status;
  out.snapshot_progress = snapshot.progress;
  out.snapshot_score = snapshot.score;
  out.conflict = status_conflict || score_conflict;
  return out;
}

std::uint32_t merge_progress(std::uint32_t base, std::uint32_t local, std::uint32_t remote) {
  const bool local_moved = local != base;
  const bool remote_moved = remote != base;
  if (!local_moved && remote_moved) return remote;
  if (local_moved && !remote_moved) return local;
  return std::max(local, remote);
}

}  // namespace store_detail

namespace {

// An import seed is only worth minting if it renders as something: a blank
// canonical title with no english/native would land a nameless library row.
// Checks non-empty content, not just present, since a control-only title
// strips to "" upstream, which would defeat a bare has_value (ROD-467 chaos
// pass).
bool seed_has_title(const Enrichment& e) {
  return !e.title_romaji.empty() ||
         (e.title_english.has_value() && !e.title_english->empty()) ||
         (e.title_native.has_value() && !e.title_native->empty());
}

// Bounds how many rows one pull can mint: a hostile or MITM'd list can hold
// ~14k entries under the 2MB response cap. Overflow is counted as unmatched,
// not imported (ROD-467 chaos pass).
constexpr std::size_t kImportCap = 500;

struct ReconcileCandidate {
  std::int64_t id = 0;
  ListEntry local;
  std::optional<ListEntry> base;
};

}  // namespace

// Read candidates + collapsed remote list into the rows needing a write. The
// concurrent-edit window is between this read and apply_reconcile.
Result<SyncPlan, StoreError> Store::reconcile_plan(const std::vector<RemoteEntry>& remote) const {
  sqlite3* db = conn_;
  // Collapse duplicate ids across groups (06 §5.4) by (updatedAt, progress):
  // recency decides, magnitude only breaks a tie. Defensive; real copies are
  // views of one record and agree. Collapsing by progress first would
  // re-raise the correction the merge exists to land. An entirely unstamped
  // group (AniList nulls updatedAt on rows untouched since the field landed,
  // and null maps to 0) falls back to plain max, order-independent but
  // carrying that same upward bias. Seeds are per-media; the first non-empty
  // one per id wins (O3).
  std::unordered_map<std::int64_t, std::tuple<ListStatus, std::uint32_t, std::uint32_t, std::int64_t>>
      remote_map;
  std::unordered_map<std::int64_t, Enrichment> seeds;
  for (const RemoteEntry& e : remote) {
    auto it = remote_map.find(e.anilist_id);
    if (it == remote_map.end()) {
      remote_map.emplace(e.anilist_id,
                         std::make_tuple(e.status, e.progress, e.score, e.updated_at));
    } else {
      const auto cand = std::make_tuple(e.updated_at, e.progress);
      const auto cur = std::make_tuple(std::get<3>(it->second), std::get<1>(it->second));
      if (cand > cur) it->second = std::make_tuple(e.status, e.progress, e.score, e.updated_at);
    }
    if (e.import_seed.has_value() && !seeds.contains(e.anilist_id)) {
      seeds.emplace(e.anilist_id, *e.import_seed);
    }
  }

  Stmt s(db,
         "SELECT anilist_id, list_status, progress, user_score, "
         "       synced_status, synced_progress, synced_score "
         "FROM show WHERE library_added_at IS NOT NULL");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
  std::vector<ReconcileCandidate> candidates;
  for (;;) {
    const int rc = s.step();
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(db)));
    ReconcileCandidate c;
    c.id = s.col_int64(0);
    c.local = {parse_list_status(s.col_text(1)), static_cast<std::uint32_t>(s.col_int64(2)),
               s.col_opt_uint(3).value_or(0)};
    const auto snap_status = s.col_opt_text(4);
    const auto snap_progress = s.col_opt_uint(5);
    if (snap_status.has_value() && snap_progress.has_value()) {
      c.base = ListEntry{parse_list_status(*snap_status), *snap_progress, s.col_opt_uint(6).value_or(0)};
    }
    candidates.push_back(std::move(c));
  }

  std::unordered_set<std::int64_t> matched;
  matched.reserve(candidates.size());
  for (const auto& c : candidates) matched.insert(c.id);

  SyncPlan out;
  for (const auto& c : candidates) {
    const auto it = remote_map.find(c.id);
    if (it == remote_map.end()) continue;  // library row absent from the remote list.
    const auto [rstatus, rprogress, rscore, rupdated] = it->second;
    (void)rupdated;
    const store_detail::Reconciled r =
        store_detail::reconcile(c.base, c.local, ListEntry{rstatus, rprogress, rscore});
    const ListEntry merged{r.status, r.progress, r.score};
    const ListEntry snapshot{r.snapshot_status, r.snapshot_progress, r.snapshot_score};
    // Skip entirely when neither the local triple nor the snapshot moves.
    // `Some(snapshot) == base` in the Rust source: on first contact (no base)
    // this is always false, so a first-contact row is never skipped even if
    // remote happens to already equal (Planning, 0, 0).
    if (merged == c.local && c.base.has_value() && snapshot == *c.base) continue;
    out.plan.push_back(SyncPlanRow{c.id, c.local, merged, snapshot, r.conflict});
  }

  // Partition the remote-only ids: auto-import the WATCHING slice that
  // carries a usable seed (O3); everything else is counted, not imported.
  for (auto& [id, rv] : remote_map) {
    if (matched.contains(id)) continue;
    const auto [status, progress, score, updated] = rv;
    (void)updated;
    auto seed_it = seeds.find(id);
    if (seed_it != seeds.end() && status == ListStatus::Watching && seed_has_title(seed_it->second)) {
      out.imports.push_back(SyncImportRow{seed_it->second, status, progress, score});
    } else {
      out.unmatched.push_back(id);
    }
  }
  std::sort(out.imports.begin(), out.imports.end(),
            [](const SyncImportRow& a, const SyncImportRow& b) {
              return a.seed.anilist_id < b.seed.anilist_id;
            });
  if (out.imports.size() > kImportCap) {
    for (std::size_t i = kImportCap; i < out.imports.size(); ++i) {
      out.unmatched.push_back(out.imports[i].seed.anilist_id);
    }
    out.imports.resize(kImportCap);
  }
  std::sort(out.unmatched.begin(), out.unmatched.end());
  return out;
}

// Apply each planned write, merged pair and snapshot in one guarded UPDATE.
// Zero rows changed = a concurrent edit moved the pair past the guard: count
// contended, leave the row (06 §5.4).
Result<PullOutcome, StoreError> Store::apply_reconcile(const std::vector<SyncPlanRow>& plan,
                                                        const std::vector<SyncImportRow>& imports,
                                                        std::vector<std::int64_t> unmatched,
                                                        std::int64_t now) {
  sqlite3* db = conn_;
  PullOutcome out;
  out.unmatched = std::move(unmatched);

  // Mint each WATCHING/REPEATING seed (O3), then adopt the remote pair as
  // truth with a matching snapshot. Both statements run under ONE BEGIN
  // IMMEDIATE so no other connection sees the transient Planning/0/unsynced
  // mint (which list_dirty_for_sync would push back as PLANNING, clobbering
  // the server).
  for (const SyncImportRow& r : imports) {
    if (auto b = begin_immediate(db); !b.has_value()) return err(b.error());
    if (auto m = add_to_library_on(db, r.seed, now); !m.has_value()) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(m.error());
    }
    // The CAS pins the pre-mint progress at 0, so rise-only and any-change
    // are equivalent here; kept in the rise-only form the merge apply below
    // requires.
    Stmt s(db,
           "UPDATE show SET "
           "  list_status = :status, "
           "  progress = :progress, "
           "  progress_stamped_at = CASE WHEN progress < :progress THEN :now "
           "                             ELSE progress_stamped_at END, "
           "  user_score = :score, "
           "  synced_status = :status, "
           "  synced_progress = :progress, "
           "  synced_score = :score "
           "WHERE anilist_id = :id AND list_status = :minted AND progress = 0 "
           "  AND synced_status IS NULL");
    if (!s.prepared()) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(StoreError::sqlite(driver_msg(db)));
    }
    s.bind_text(":status", to_string(r.status));
    s.bind_int64(":progress", static_cast<std::int64_t>(r.progress));
    s.bind_int64(":score", static_cast<std::int64_t>(r.score));
    s.bind_int64(":now", now);
    s.bind_int64(":id", r.seed.anilist_id);
    s.bind_text(":minted", to_string(ListStatus::Planning));
    if (s.step() != SQLITE_DONE) {
      const StoreError e = StoreError::sqlite(driver_msg(db));
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return err(e);
    }
    if (sqlite3_changes(db) == 0) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      out.contended.push_back(r.seed.anilist_id);
      continue;  // drop(tx) rolls the mint back.
    }
    if (auto c = exec(db, "COMMIT"); !c.has_value()) return err(c.error());
    out.imported++;
  }

  // Stamps on a RISE only. The stamp means "the frontier moved past this
  // partial, so it is dead" (05 §10.7); a downward adopt moves the frontier
  // behind the partial, which cannot retire it (ROD-497). No wrapping
  // transaction: a single guarded UPDATE is its own atomic unit (matches
  // store.rs apply_reconcile exactly).
  for (const SyncPlanRow& p : plan) {
    Stmt s(db,
           "UPDATE show SET "
           "  list_status = :status, "
           "  progress = :progress, "
           "  progress_stamped_at = CASE WHEN progress < :progress THEN :now "
           "                             ELSE progress_stamped_at END, "
           "  user_score = :score, "
           "  synced_status = :snap_status, "
           "  synced_progress = :snap_progress, "
           "  synced_score = :snap_score "
           "WHERE anilist_id = :id AND list_status = :guard_status "
           "  AND progress = :guard_progress "
           "  AND IFNULL(user_score, 0) = :guard_score");
    if (!s.prepared()) return err(StoreError::sqlite(driver_msg(db)));
    s.bind_text(":status", to_string(p.merged.status));
    s.bind_int64(":progress", static_cast<std::int64_t>(p.merged.progress));
    s.bind_int64(":score", static_cast<std::int64_t>(p.merged.score));
    s.bind_int64(":now", now);
    s.bind_text(":snap_status", to_string(p.snapshot.status));
    s.bind_int64(":snap_progress", static_cast<std::int64_t>(p.snapshot.progress));
    s.bind_int64(":snap_score", static_cast<std::int64_t>(p.snapshot.score));
    s.bind_int64(":id", p.id);
    s.bind_text(":guard_status", to_string(p.guard.status));
    s.bind_int64(":guard_progress", static_cast<std::int64_t>(p.guard.progress));
    s.bind_int64(":guard_score", static_cast<std::int64_t>(p.guard.score));
    if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(db)));
    if (sqlite3_changes(db) == 0) {
      out.contended.push_back(p.id);
      continue;
    }
    out.reconciled++;
    if (p.conflict) out.conflicts++;
  }
  return out;
}

Result<std::vector<SyncRow>, StoreError> Store::list_dirty_for_sync() const {
  Stmt s(conn_,
         "SELECT anilist_id, title_romaji, list_status, progress, user_score, "
         "       synced_status, synced_progress, synced_score FROM show "
         "WHERE library_added_at IS NOT NULL "
         "  AND (synced_status IS NULL "
         "       OR synced_status <> list_status "
         "       OR synced_progress IS NULL "
         "       OR synced_progress <> progress "
         "       OR IFNULL(synced_score, 0) <> IFNULL(user_score, 0)) "
         "ORDER BY last_watched_at DESC NULLS LAST, library_added_at DESC, anilist_id");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  std::vector<SyncRow> out;
  for (;;) {
    const int rc = s.step();
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
    SyncRow row;
    row.anilist_id = s.col_int64(0);
    row.title_romaji = s.col_text(1);
    row.list_status = parse_list_status(s.col_text(2));
    row.progress = static_cast<std::uint32_t>(s.col_int64(3));
    row.score = s.col_opt_uint(4).value_or(0);
    const auto snap_status = s.col_opt_text(5);
    const auto snap_progress = s.col_opt_uint(6);
    if (snap_status.has_value() && snap_progress.has_value()) {
      row.synced =
          ListEntry{parse_list_status(*snap_status), *snap_progress, s.col_opt_uint(7).value_or(0)};
    }
    out.push_back(std::move(row));
  }
  return out;
}

Result<Unit, StoreError> Store::mark_synced(std::int64_t anilist_id, ListStatus status,
                                             std::uint32_t progress, std::uint32_t score,
                                             std::optional<std::uint32_t> adopt_from_raw) {
  // The optional user_score adoption (P34 lossy-scoreFormat convergence, see
  // store.hpp): snap the local score to the server's quantized value ONLY
  // while it still equals the raw value the push read — the same contended-
  // skip shape as apply_reconcile's CAS, so a mid-push edit stays dirty and
  // wins the next run instead of being silently overwritten.
  Stmt s(conn_,
         "UPDATE show SET synced_status = ?1, synced_progress = ?2, synced_score = ?3, "
         "                user_score = CASE WHEN ?5 IS NOT NULL "
         "                                   AND IFNULL(user_score, 0) = ?5 "
         "                                  THEN ?3 ELSE user_score END "
         "WHERE anilist_id = ?4");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, to_string(status));
  s.bind_int64(2, static_cast<std::int64_t>(progress));
  s.bind_int64(3, static_cast<std::int64_t>(score));
  s.bind_int64(4, anilist_id);
  if (adopt_from_raw.has_value()) {
    s.bind_int64(5, static_cast<std::int64_t>(*adopt_from_raw));
  } else {
    s.bind_null(5);
  }
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

Result<PullOutcome, StoreError> Store::reconcile_pull(const std::vector<RemoteEntry>& remote,
                                                       std::int64_t now) {
  auto plan = reconcile_plan(remote);
  if (!plan.has_value()) return err(plan.error());
  return apply_reconcile(plan->plan, plan->imports, std::move(plan->unmatched), now);
}

// --- MAL mirror (P31 §9.1 slice 4; score P34 slice 2) -----------------------

namespace {

// Raw 0..=100 (the store's canonical AniList POINT_100 scale) -> MAL's own
// native 0..=10 int scale. Round-half-up, same rounding idiom as domain.cpp's
// to_anilist_score/parse_user_score; not a ScoreFormat case since MAL has
// exactly one scale, never an account-chosen one.
std::uint32_t to_mal_score(std::uint32_t raw) {
  return std::min<std::uint32_t>(static_cast<std::uint32_t>(std::lround(raw / 10.0)), 10);
}

}  // namespace

Result<std::vector<MalMirrorRow>, StoreError> Store::list_dirty_for_mal_mirror() const {
  // mal_id IS NOT NULL is the NULL-mal_id skip (PORT_PARITY.md P31): rows AniList
  // never linked to a MAL entry have nothing to push and must never surface
  // here. Gated on its OWN snapshot pair/score (mal_synced_status/progress/
  // score), not synced_status/progress/score — the AniList-sync dirty gate
  // must not decide this mirror's work list.
  Stmt s(conn_,
         "SELECT anilist_id, mal_id, list_status, progress, user_score, "
         "       mal_synced_status, mal_synced_progress, mal_synced_score FROM show "
         "WHERE library_added_at IS NOT NULL AND mal_id IS NOT NULL "
         "  AND (mal_synced_status IS NULL "
         "       OR mal_synced_status <> list_status "
         "       OR mal_synced_progress IS NULL "
         "       OR mal_synced_progress <> progress "
         "       OR IFNULL(mal_synced_score, 0) <> "
         "          CAST(ROUND(IFNULL(user_score, 0) / 10.0) AS INTEGER)) "
         "ORDER BY last_watched_at DESC NULLS LAST, library_added_at DESC, anilist_id");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  std::vector<MalMirrorRow> out;
  for (;;) {
    const int rc = s.step();
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return err(StoreError::sqlite(driver_msg(conn_)));
    MalMirrorRow row;
    row.anilist_id = s.col_int64(0);
    row.mal_id = s.col_int64(1);
    row.list_status = parse_list_status(s.col_text(2));
    row.progress = static_cast<std::uint32_t>(s.col_int64(3));
    row.score = to_mal_score(s.col_opt_uint(4).value_or(0));
    const auto snap_status = s.col_opt_text(5);
    const auto snap_progress = s.col_opt_uint(6);
    if (snap_status.has_value() && snap_progress.has_value()) {
      row.synced = std::make_pair(parse_list_status(*snap_status), *snap_progress);
    }
    row.synced_score = s.col_opt_uint(7);
    out.push_back(std::move(row));
  }
  return out;
}

Result<Unit, StoreError> Store::mark_mal_synced(std::int64_t anilist_id, ListStatus status,
                                                 std::uint32_t progress,
                                                 std::optional<std::uint32_t> score) {
  Stmt s(conn_,
         "UPDATE show SET mal_synced_status = ?1, mal_synced_progress = ?2, "
         "                 mal_synced_score = ?3 "
         "WHERE anilist_id = ?4");
  if (!s.prepared()) return err(StoreError::sqlite(driver_msg(conn_)));
  s.bind_text(1, to_string(status));
  s.bind_int64(2, static_cast<std::int64_t>(progress));
  // NULL when the push withheld the field (no belief), never 0: the dirty
  // predicate's IFNULL(mal_synced_score, 0) treats both as "unscored", but
  // the ROD-498 guard reads NULL as "no score arm to check" — exactly right
  // for a score the mirror never sent.
  if (score.has_value()) {
    s.bind_int64(3, static_cast<std::int64_t>(*score));
  } else {
    s.bind_null(3);
  }
  s.bind_int64(4, anilist_id);
  if (s.step() != SQLITE_DONE) return err(StoreError::sqlite(driver_msg(conn_)));
  return Unit{};
}

}  // namespace shigoku
