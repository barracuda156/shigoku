// mal_mirror.hpp — MAL mirror push (P31 §9.1 slice 4). No sabigoku reference
// (MAL support never existed there). This is the orchestration layer slice 3
// (mal.hpp) was built for: a dirty-row walk over library rows keyed by
// mal_id, pushing (list_status, progress) to MAL and advancing the mirror's
// OWN snapshot (store.hpp's mal_synced_status/mal_synced_progress, v3).
//
// Deliberately NOT a generalization of sync.hpp's AniList machinery
// (PORT_PARITY.md P31: "not a generalization of the 06 §5 sync law — that
// law stays AniList's; the mirror is separate, simpler machinery behind its
// own client trait"). Differences from sync.cpp's push_dirty that fall out
// of that ruling:
//   - No pull, ever: MAL is push-only (the plan's own words), so there is no
//     contended-set gate and no reconcile.
//   - No fixed inter-call spacing/backoff schedule: sync.hpp's 2s/60s
//     schedule is an AniList-specific rate-limit accommodation (06 §5.3)
//     that the plan does not carry over here. A 429 still stops the run
//     (rather than spin), leaving the rest of the work list dirty for next
//     time — the same "retry next run" fallback A1 gives sync's own
//     RateLimited outcome, just without an in-run retry.
//   - The per-row guard reads MAL's live entry first (mirrors sync's ROD-498
//     guard) so a value changed on MAL's side since the last mirror push is
//     never blindly overwritten; a MAL-deleted entry (my_list_status simply
//     absent, i.e. mal::get_list_entry returns nullopt) is NOT held back —
//     same rationale as sync's "no entry on the server: nothing of theirs to
//     destroy" branch, and MAL mirrors get no analogous "pull" that could
//     ever clear the local dirty flag on our side, so holding back here
//     would strand the row dirty forever.
//
// No drain rights (A1 stands): the caller (app.cpp) runs this behind the
// SAME AniList sync flush / quitFlush cadence, never its own worker or its
// own extra teardown budget.

#pragma once

#include <cstdint>
#include <string_view>
#include <tuple>

#include "auth.hpp"
#include "domain.hpp"
#include "error.hpp"
#include "http.hpp"
#include "mal.hpp"
#include "result.hpp"
#include "store.hpp"

namespace shigoku::mal_mirror {

// The MAL calls the mirror needs, behind an ABC so tests script get/put
// without a socket (sync.hpp's AniListSync precedent). Two methods used
// together per row, so a vtable over a std::function pair (mirrors
// sync.hpp's own reasoning for AniListSync vs login.hpp's single-method
// Verifier).
class MalMirrorClient {
 public:
  virtual ~MalMirrorClient() = default;

  // The triple's score is MAL's own 0..=10 int scale (mal.hpp's own scope
  // comment) — mal_mirror.cpp compares it directly against MalMirrorRow's
  // already-converted `score`/`synced_score`, no further conversion here.
  [[nodiscard]] virtual Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>,
                              ProviderError>
  fetch_entry(std::string_view token, std::int64_t mal_id) const = 0;

  // `score` nullopt = withhold the field from the PUT (a locally-unscored
  // row must never send MAL's "remove score" sentinel 0 — see mal.hpp).
  [[nodiscard]] virtual Result<Unit, ProviderError> save_entry(
      std::string_view token, std::int64_t mal_id, ListStatus status, std::uint32_t progress,
      std::optional<std::uint32_t> score) const = 0;
};

// The real implementation, delegating to mal::get_list_entry/update_list_entry
// over a bound http::Client (mirrors sync.hpp's HttpAniListSync). The client
// reference must outlive this wrapper.
class HttpMalMirrorClient final : public MalMirrorClient {
 public:
  explicit HttpMalMirrorClient(const http::Client& client) : client_(client) {}

  [[nodiscard]] Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>,
                      ProviderError>
  fetch_entry(std::string_view token, std::int64_t mal_id) const override {
    return mal::get_list_entry(client_, token, mal_id);
  }

  [[nodiscard]] Result<Unit, ProviderError> save_entry(
      std::string_view token, std::int64_t mal_id, ListStatus status, std::uint32_t progress,
      std::optional<std::uint32_t> score) const override {
    return mal::update_list_entry(client_, token, mal_id, status, progress, score);
  }

 private:
  const http::Client& client_;
};

// Terminal reason for a mirror run.
enum class MirrorOutcome {
  Completed,
  Disabled,      // no usable MAL token: signed out, expired, or bearer refused.
  Unauthorized,  // 401 during push; run stopped, rest stay dirty.
  RateLimited,   // 429 during push; run stopped, rest stay dirty (retried next run).
};

struct MirrorSummary {
  MirrorOutcome outcome = MirrorOutcome::Completed;
  std::uint32_t dirty = 0;
  std::uint32_t pushed = 0;
  std::uint32_t push_failed = 0;
  // Rows held back because MAL's live entry disagrees with what the mirror
  // believes it last pushed (ROD-498-equivalent guard) — left dirty rather
  // than overwritten.
  std::uint32_t push_skipped = 0;

  static MirrorSummary terminal(MirrorOutcome outcome) {
    MirrorSummary s;
    s.outcome = outcome;
    return s;
  }
};

// Push-only mirror run: dirty-row walk (store's NULL-mal_id skip already
// applied), guard-then-save per row, snapshot advance on success. `enabled`
// is the caller's gate (mal.bearer() presence is folded into `Disabled`
// below, so the caller need only pass whether the mirror is switched on).
[[nodiscard]] Result<MirrorSummary, StoreError> push_mirror(const MalMirrorClient& client,
                                                             const MalAuth& auth, Store& store,
                                                             bool enabled);

}  // namespace shigoku::mal_mirror
