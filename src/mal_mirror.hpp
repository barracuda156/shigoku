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
//   - The push takes no contended set from a pull: the pull below is its
//     own call, and the push stands on its per-row guard alone.
//   - No fixed inter-call spacing/backoff schedule: sync.hpp's 2s/60s
//     schedule is an AniList-specific rate-limit accommodation (06 §5.3)
//     that the plan does not carry over here. A 429 still stops the run
//     (rather than spin), leaving the rest of the work list dirty for next
//     time — the same "retry next run" fallback A1 gives sync's own
//     RateLimited outcome, just without an in-run retry.
//   - The per-row guard reads MAL's live entry first. When it is not what the
//     mirror's snapshot says MAL holds — the entry moved on MAL's side, or the
//     row was never mirrored — the row is merged against that entry
//     (Store::rebase_mal_row, the pull's own three-way law) and only what
//     still differs afterwards is sent. Nothing is overwritten blind, and no
//     row waits on a list pull to become pushable again. A row with no entry
//     on MAL is simply sent.
//
// No drain rights (A1 stands): the caller (app.cpp) runs this behind the
// SAME AniList sync flush / quitFlush cadence, never its own worker or its
// own extra teardown budget.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "auth.hpp"
#include "domain.hpp"
#include "error.hpp"
#include "http.hpp"
#include "mal.hpp"
#include "mal_catalog.hpp"
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
  // The account's whole list, every status (the pull direction).
  [[nodiscard]] virtual Result<std::vector<mal_catalog::UserListEntry>, ProviderError> fetch_list(
      std::string_view token) const = 0;
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
  [[nodiscard]] Result<std::vector<mal_catalog::UserListEntry>, ProviderError> fetch_list(
      std::string_view token) const override {
    return mal_catalog::user_list(client_, token);
  }

 private:
  const http::Client& client_;
};

// A MAL access token lives about a month; the refresh_token stored beside it
// redeems a new pair. The refresh seam, a callable so tests fake the token
// endpoint the way FakeMal fakes the list calls: handed the stored pair and
// the clock, it answers with the fresh pair (Rolled), with MAL's refusal of
// the refresh token (Refused: the sign-in is over, only a reconnect mends
// it), or with a request that never completed (Unreachable: transient, and
// says nothing about the sign-in).
struct Refresh {
  enum class Outcome { Rolled, Refused, Unreachable };
  Outcome outcome = Outcome::Unreachable;
  MalAuth auth;  // Rolled only.

  static Refresh rolled(MalAuth a) {
    Refresh r;
    r.outcome = Outcome::Rolled;
    r.auth = std::move(a);
    return r;
  }
  static Refresh refused() {
    Refresh r;
    r.outcome = Outcome::Refused;
    return r;
  }
  static Refresh unreachable() { return Refresh{}; }
};
using Refresher = std::function<Refresh(const MalAuth& current, std::int64_t now)>;

inline constexpr std::size_t kFailureNotes = 12;

// Terminal reason for a mirror run.
enum class MirrorOutcome {
  Completed,
  Disabled,      // no usable MAL token: signed out, or bearer refused.
  Unauthorized,  // 401 (for the pull: one a refresh could not mend); run stopped, rest stay dirty.
  RateLimited,   // 429 during push; run stopped, rest stay dirty (retried next run).
};

struct MirrorSummary {
  MirrorOutcome outcome = MirrorOutcome::Completed;
  std::uint32_t dirty = 0;
  std::uint32_t pushed = 0;
  // A MAL call for the row failed (other than 401/429, which end the run):
  // the row stays dirty.
  std::uint32_t push_failed = 0;
  // Rows merged against MAL's live entry before the decision to send; each
  // then ends pushed (the local side still differed), settled (it did not)
  // or skipped.
  std::uint32_t merged = 0;
  std::uint32_t settled = 0;
  // Rows edited locally while being merged: nothing written, left dirty.
  std::uint32_t push_skipped = 0;

  // Rows this run leaves owing to MAL: every dirty row it neither sent nor
  // settled, those a 401/429 cut the run short of included.
  [[nodiscard]] std::uint32_t unsent() const { return dirty - pushed - settled; }
  // Why a row counted in push_failed did not go, one line each
  // ("myanimelist.net/anime/<id>: save failed, http 403"), the first
  // kFailureNotes of them: the command line prints these, since nothing else
  // surfaces a failing row's reason.
  std::vector<std::string> failures;

  static MirrorSummary terminal(MirrorOutcome outcome) {
    MirrorSummary s;
    s.outcome = outcome;
    return s;
  }
};

// Push-only mirror run: dirty-row walk (store's NULL-mal_id skip already
// applied), guard (merging when MAL's entry is not the snapshot's), save,
// snapshot advance on success. `enabled` is the caller's gate (mal.bearer()
// presence is folded into `Disabled` below, so the caller need only pass
// whether the mirror is switched on). `now` stamps a progress rise a merge
// adopts from MAL.
[[nodiscard]] Result<MirrorSummary, StoreError> push_mirror(const MalMirrorClient& client,
                                                             const MalAuth& auth, Store& store,
                                                             bool enabled, std::int64_t now);

// The pull direction: the account's whole MAL list reconciled into the
// library against the mirror's OWN snapshot columns (never AniList's), the
// same three-way merge the AniList pull uses. Every status imports — the
// list IS the user's library on MAL — with the AniList id bridged per row
// (idmap hit, else the synthetic negative id); a row the library already
// holds under a real id is matched by mal_id and keeps it. A transport or
// decode miss is `pull_failed` (nothing adopted, the push still runs); 401
// and 429 are the terminal outcomes the push knows.
struct MirrorPullSummary {
  MirrorOutcome outcome = MirrorOutcome::Completed;
  bool pull_failed = false;
  PullOutcome pulled;
  // The pair the run rolled the token to, when it did (the stored one was
  // past its date, or MAL met it with 401): the caller saves it over the
  // auth file's .mal block and pushes with it. Set whatever the list fetch
  // then did; a renewed pair is worth keeping even when the pull missed.
  std::optional<MalAuth> rolled;

  static MirrorPullSummary terminal(MirrorOutcome outcome) {
    MirrorPullSummary s;
    s.outcome = outcome;
    return s;
  }
};

// `refresh` renews the token before the fetch when it is dated-expired, and
// once more after a 401 (a token MAL retired early); a refusal there is
// Unauthorized without touching the list, an unreachable endpoint a failed
// pull. Without one (tests), an expired or refused token is plain
// Unauthorized, as the push reports it.
[[nodiscard]] Result<MirrorPullSummary, StoreError> pull_mirror(const MalMirrorClient& client,
                                                                 const MalAuth& auth, Store& store,
                                                                 bool enabled, std::int64_t now,
                                                                 const Refresher& refresh = {});

}  // namespace shigoku::mal_mirror
