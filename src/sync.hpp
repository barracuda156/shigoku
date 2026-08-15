// sync.hpp — AniList sync orchestration (P20, 06 §5). Ported from sabigoku
// src/sync.rs. Every TUI entry funnels through run_sync, whose first act is
// the pull, so push-first can never wipe never-synced rows (06 §5.2). The one
// push-only path is flush_push (quit flush), gated by the caller on no pull
// inflight.
//
// Store-keyed on anilist_id, so every library row is linked; the freeze's
// engaged-but-unlinked summary (06 §5.3/§5.6) has no rows to report here.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "anilist.hpp"
#include "auth.hpp"
#include "domain.hpp"
#include "http.hpp"
#include "store.hpp"

namespace shigoku::sync {

// Spacing between push calls (06 §5.3, AniList rate).
inline constexpr std::chrono::milliseconds kPushSpacing{2000};
// One-time backoff on a 429 before retrying the row (06 §5.3).
inline constexpr std::chrono::milliseconds kRateLimitBackoff{60000};

// The AniList calls sync needs, behind an ABC so tests script pull/push
// without a socket (sync.rs's AniListSync trait). Multi-method + used
// together throughout push_dirty's loop, so this is a virtual ABC (provider
// .hpp's StreamProvider shape), not a single-method std::function (login.hpp's
// Verifier shape).
class AniListSync {
 public:
  virtual ~AniListSync() = default;

  // `format` is the account's scoreFormat (Auth.anilist.score_format),
  // needed by fetch_list/fetch_entry/save_entry to convert the wire's score
  // to/from the store's raw 0..=100 — threaded per-call rather than bound at
  // construction so a HttpAniListSync built once at app start still sees a
  // score_format refreshed by a later reconnect (06 §4.2).
  [[nodiscard]] virtual Result<std::vector<RemoteEntry>, ProviderError> fetch_list(
      std::string_view token, std::int64_t user_id, ScoreFormat format) const = 0;

  [[nodiscard]] virtual Result<std::optional<ListEntry>, ProviderError> fetch_entry(
      std::string_view token, std::int64_t user_id, std::int64_t media_id,
      ScoreFormat format) const = 0;

  [[nodiscard]] virtual Result<std::int64_t, ProviderError> save_entry(
      std::string_view token, std::int64_t media_id, ListStatus status, std::uint32_t progress,
      std::uint32_t score, ScoreFormat format) const = 0;
};

// The real implementation, delegating to anilist::pull_list/pull_entry/
// push_entry over a bound http::Client (sync.rs's `impl AniListSync for
// AniList`). The client reference must outlive this wrapper.
class HttpAniListSync final : public AniListSync {
 public:
  explicit HttpAniListSync(const http::Client& client) : client_(client) {}

  [[nodiscard]] Result<std::vector<RemoteEntry>, ProviderError> fetch_list(
      std::string_view token, std::int64_t user_id, ScoreFormat format) const override {
    return anilist::pull_list(client_, token, user_id, format);
  }

  [[nodiscard]] Result<std::optional<ListEntry>, ProviderError> fetch_entry(
      std::string_view token, std::int64_t user_id, std::int64_t media_id,
      ScoreFormat format) const override {
    return anilist::pull_entry(client_, token, user_id, media_id, format);
  }

  [[nodiscard]] Result<std::int64_t, ProviderError> save_entry(
      std::string_view token, std::int64_t media_id, ListStatus status, std::uint32_t progress,
      std::uint32_t score, ScoreFormat format) const override {
    return anilist::push_entry(client_, token, media_id, status, progress, score, format);
  }

 private:
  const http::Client& client_;
};

// Blocking waits, behind a callable so tests assert the 2s/60s schedule
// without real sleeps (sync.rs's Sleeper trait, ported as std::function —
// one method, no state worth a vtable, same idiom as login.hpp's Verifier).
using Sleeper = std::function<void(std::chrono::milliseconds)>;

// The real sleeper.
inline void thread_sleep(std::chrono::milliseconds dur) { std::this_thread::sleep_for(dur); }

// No-wait sleeper for the quit flush (04 §11): teardown bounds the run by its
// own deadline, so spacing/backoff would only waste that budget.
inline void no_sleep(std::chrono::milliseconds) {}

// Terminal reason for a sync run. Store-level failures are Err, not a
// variant here.
enum class SyncOutcome {
  Completed,
  Disabled,          // master switch off or not connected: a no-op run.
  NoToken,
  Expired,
  NoUserId,          // token without a user id (mangled auth.json).
  PullUnauthorized,  // 401 on the pull; push skipped (06 §5.2).
  PullRateLimited,   // 429 on the pull; push skipped (06 §5.2).
  Unauthorized,      // 401 during push; run stopped (06 §5.3).
  RateLimited,       // second 429 during push; run stopped, rest stay dirty.
  Failed,            // store or client construction failed (worker bridge).
};

struct SyncSummary {
  SyncOutcome outcome = SyncOutcome::Completed;
  // Transport/decode-failed pull. Not terminal: only 401/429/store errors
  // gate the push (06 §5.2); a fetch miss lets the push run.
  bool pull_failed = false;
  PullOutcome pulled;
  std::uint32_t dirty = 0;       // size of this run's push work list.
  std::uint32_t pushed = 0;
  std::uint32_t push_failed = 0;
  // Dirty rows deliberately not pushed this run because what we believe the
  // server holds for them is known-bad: left contended by the pull (ROD-500),
  // or moved on the server since it (ROD-498). They stay dirty.
  std::uint32_t push_skipped = 0;

  static SyncSummary terminal(SyncOutcome outcome) {
    SyncSummary s;
    s.outcome = outcome;
    return s;
  }
};

// The sync funnel (06 §5.2): pull, reconcile, then push unless pull_only.
// `enabled` is the master switch; the caller passes connected + enabled.
[[nodiscard]] Result<SyncSummary, StoreError> run_sync(
    const AniListSync& client, const Auth& auth, Store& store, std::int64_t now, bool enabled,
    bool pull_only, const Sleeper& sleeper);

// Quit flush (06 §5.2): push only, no pull. The caller skips this while a
// pull is inflight and bounds it (04 §11) via the sleeper it passes.
[[nodiscard]] Result<SyncSummary, StoreError> flush_push(const AniListSync& client,
                                                          const Auth& auth, Store& store,
                                                          std::int64_t now, bool enabled,
                                                          const Sleeper& sleeper);

// --- Internals exposed for the golden tests (sync.rs tests mod) -----------
namespace detail {

// Push the dirty set (06 §5.3): 2s between rows, 401 stops the run, a first
// 429 backs off once and retries the row, a second 429 stops the run leaving
// the rest dirty. Other per-row errors count and continue. A success advances
// the snapshot only after AniList returned a non-null id (enforced in
// save_entry).
//
// Two gates hold a row back as push_skipped, leaving it dirty, rather than
// overwrite a server value we cannot account for:
//
// - `contended` are ids the pull failed to reconcile this run (ROD-500).
//   Their snapshot never advanced, so the live pair is pre-merge and pushing
//   it would overwrite the remote change the pull was carrying.
// - The per-row guard below (ROD-498). SaveMediaListEntry has no
//   compare-and-set, so the only defence against overwriting a newer remote
//   edit is to shrink the window between learning the server's value and
//   overwriting it. Checking here rather than reusing the pull keeps that
//   window at one round trip whatever the size of the dirty set; the pull is
//   PUSH_SPACING per row further behind by the time the tail is written.
[[nodiscard]] Result<Unit, StoreError> push_dirty(const AniListSync& client,
                                                   std::string_view token, std::int64_t user_id,
                                                   Store& store, const Sleeper& sleeper,
                                                   const std::vector<std::int64_t>& contended,
                                                   SyncSummary& summary, ScoreFormat format);

}  // namespace detail

}  // namespace shigoku::sync
