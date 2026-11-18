// sqlite_util.hpp — sqlite3 C-API scaffolding shared by the anime and manga
// library stores. Internal and header-only: include it from those two .cpp
// files only, never from another header. Everything that returns an error is a
// template over the store's error type E, which must offer sqlite(std::string),
// schema_invalid(uint32_t), schema_too_new(uint32_t, uint32_t),
// schema_missing(uint32_t) and read_only(std::string).
#pragma once

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "result.hpp"

namespace shigoku::sqlite_util {

// Bounded hand-rolled retry for the WAL flip and the BEGIN IMMEDIATE — the two
// lock upgrades busy_timeout does not cover.
inline constexpr int kLockRetryLimit = 100;
inline constexpr int kLockRetryBackoffMs = 5;

// Retryable contention: BUSY or LOCKED (rusqlite classifies both, store.rs).
inline bool is_contended(int rc) {
  return rc == SQLITE_BUSY || rc == SQLITE_LOCKED;
}

inline std::string driver_msg(sqlite3* db) {
  const char* m = sqlite3_errmsg(db);
  return m != nullptr ? std::string(m) : std::string("(no message)");
}

// Sleep out a lock-upgrade backoff. store.rs uses a fixed 5ms; the connection
// is UI-thread-owned so the whole retry loop is bounded (kLockRetryLimit).
inline void backoff() {
  const struct timespec ts{0, static_cast<long>(kLockRetryBackoffMs) * 1'000'000L};
  nanosleep(&ts, nullptr);
}

// Minimal RAII prepared-statement wrapper: bind by index (1-based), step, read
// columns, finalize on scope exit. Only the shapes these stores need.
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
  void bind_bool(int i, bool v) { bind_int64(i, v ? 1 : 0); }
  void bind_opt_int64(int i, std::optional<std::int64_t> v) {
    if (v.has_value()) bind_int64(i, *v); else bind_null(i);
  }
  void bind_opt_int(int i, std::optional<int> v) {
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
  [[nodiscard]] std::optional<int> col_opt_int(int i) const {
    return col_is_null(i) ? std::nullopt
                          : std::optional<int>(static_cast<int>(col_int64(i)));
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
template <class E>
Result<Unit, E> exec(sqlite3* db, const char* sql) {
  char* msg = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
  if (rc != SQLITE_OK) {
    std::string why = msg != nullptr ? std::string(msg) : driver_msg(db);
    sqlite3_free(msg);
    return err(E::sqlite(std::move(why)));
  }
  return Unit{};
}

// PRAGMA journal_mode = WAL, retrying contention by hand (busy_timeout does not
// cover this lock upgrade). In-memory DBs report "memory"; only the flip
// failing hard is an error (store.rs enable_wal).
template <class E>
Result<Unit, E> enable_wal(sqlite3* db) {
  for (int attempt = 0;; ++attempt) {
    Stmt s(db, "PRAGMA journal_mode = WAL");
    if (!s.prepared()) return err(E::sqlite(driver_msg(db)));
    const int rc = s.step();
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) return Unit{};
    if (attempt < kLockRetryLimit && is_contended(rc)) {
      backoff();
      continue;
    }
    return err(E::sqlite(driver_msg(db)));
  }
}

// Read + validate user_version: negative (a legal header state) is a clean
// SchemaInvalid, too-new (above max_version) refuses before any write
// (store.rs user_version).
template <class E>
Result<std::uint32_t, E> user_version(sqlite3* db, std::uint32_t max_version) {
  Stmt s(db, "PRAGMA user_version");
  if (!s.prepared()) return err(E::sqlite(driver_msg(db)));
  if (s.step() != SQLITE_ROW) return err(E::sqlite(driver_msg(db)));
  const std::int64_t v = s.col_int64(0);
  if (v < 0 || v > static_cast<std::int64_t>(UINT32_MAX)) {
    return err(E::schema_invalid(static_cast<std::uint32_t>(v & 0xFFFFFFFF)));
  }
  const auto uv = static_cast<std::uint32_t>(v);
  if (uv > max_version) return err(E::schema_too_new(uv, max_version));
  return uv;
}

// user_version alone proves nothing: a stamped version over missing DDL (a
// foreign file, a mid-write backup) must fail here, not as a raw "no such
// table" later (store.rs verify_schema_present, ROD-434).
template <class E>
Result<Unit, E> verify_schema_present(sqlite3* db, std::string_view table,
                                      std::uint32_t version) {
  Stmt s(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?1");
  if (!s.prepared()) return err(E::sqlite(driver_msg(db)));
  s.bind_text(1, table);
  if (s.step() != SQLITE_ROW) return err(E::sqlite(driver_msg(db)));
  if (s.col_int64(0) == 0) return err(E::schema_missing(version));
  return Unit{};
}

// BEGIN IMMEDIATE with the WAL-flip's bounded retry: a peer holding the ladder
// lock past busy_timeout is waited out, not turned into a hard failure.
template <class E>
Result<Unit, E> begin_immediate(sqlite3* db) {
  for (int attempt = 0;; ++attempt) {
    const int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (rc == SQLITE_OK) return Unit{};
    if (attempt < kLockRetryLimit && is_contended(rc)) {
      backoff();
      continue;
    }
    return err(E::sqlite(driver_msg(db)));
  }
}

// SQLite silently downgrades to read-only when the file denies READWRITE; that
// must fail loud at open, not at the first write (store.rs probe_writable).
template <class E>
Result<Unit, E> probe_writable(sqlite3* db, std::string_view path) {
  if (sqlite3_db_readonly(db, "main") == 1) {
    return err(E::read_only(std::string(path)));
  }
  return Unit{};
}

}  // namespace shigoku::sqlite_util
