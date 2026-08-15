// prewarm.hpp — the eager sibling-provider warm walk (P15). Ported from
// sabigoku src/tui/prewarm.rs. After a successful add-to-library or once mpv
// comes up during play, this background-probes the providers a show is NOT yet
// bound to (and not freshly absent) and MINTS the result into the store — a
// hidden binding (Found) or a fresh-absence row (Absent) — so a later provider
// flip or fallback hop lands tier-0 with no network. Silent: no toast, no
// spinner. The precomputed artifact is a store row, never an in-memory cache.
//
// Lives in shigoku_tui (reads Store + registry). App holds it by unique_ptr so
// app.hpp stays off store.hpp. Single-flight: one active Run at a time; a 32-
// slot round-robin ring dedups attempted anilist_ids (freeze-faithful — a show
// re-attempts after 32 others); a 30s app-wide floor spaces walk starts.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../domain.hpp"
#include "../event.hpp"
#include "queue.hpp"
#include "toast.hpp"    // TickCount.
#include "workers.hpp"  // GenCounter.

namespace shigoku {
class ProviderRegistry;
class Store;
}

namespace shigoku::tui {

// Timing (prewarm.rs:17-21), in TickCount (100ms ticks, 04 §8 clock).
inline constexpr TickCount kPrewarmSpacingTicks = 300;  // 30s floor between walk starts.
inline constexpr TickCount kPrewarmHopGapTicks = 15;    // 1.5s between candidate probes.
inline constexpr std::size_t kPrewarmRingSlots = 32;    // dedup ring size.

// Fire-time gates sampled from playback + the resolve walk (prewarm.rs:26-32).
// A prewarm yields to any user-facing resolve and to an active fallback walk
// (03 §6.4: an advancing fallback owns the CDN budget).
struct PrewarmGates {
  bool play_resolving = false;
  bool fallback_active = false;
};

// Everything the worker + on_result need per call (borrows only; A7 store on
// the UI thread). Built fresh by App each time it drives prewarm.
struct PrewarmDeps {
  Store* store = nullptr;
  const ProviderRegistry* registry = nullptr;
  EventQueue* queue = nullptr;
  Translation translation = Translation::Sub;
  std::int64_t unix_now = 0;
  TickCount now_tick = 0;
};

// One active warm walk over a show's not-yet-checked providers (prewarm.rs
// Run). Private detail exposed only for the header's inline-free members.
struct PrewarmRun {
  Enrichment canonical;
  std::vector<std::string> candidates;  // provider names still to probe.
  std::size_t ix = 0;                   // next candidate to spawn.
  bool inflight = false;                // a probe worker is out.
  std::optional<TickCount> gap_until;   // HOP_GAP pacing deadline.
  bool yielding = false;                // cancelled: honor in-flight, spawn no more.
};

class PrewarmState {
 public:
  // Arm a warm walk for `canonical` if not blocked (prewarm.rs fire). Blocked
  // when: a walk is already active, a gate is up, the id is in the ring, or the
  // 30s spacing floor hasn't elapsed. Spawns the first paced probe.
  void fire(const Enrichment& canonical, const PrewarmGates& gates, const PrewarmDeps& deps);

  // Consume a probe result (prewarm.rs on_result): mint Found→bind /
  // Absent→mark_absent (Found never steals another show's binding — a store
  // read error there fails CLOSED). Returns true iff a store row changed (so
  // App can refresh the availability rail). Drops stale tokens.
  bool on_result(const PrewarmResult& r, const PrewarmDeps& deps);

  // Pace the next probe once HOP_GAP passes (prewarm.rs tick). Called each loop
  // tick. Spawns the next candidate or finishes the run.
  void tick(const PrewarmDeps& deps);

  // Cancel: honor an in-flight probe's result but spawn nothing more; drop the
  // run outright if none is in flight (prewarm.rs cancel — fallback yield).
  void cancel();

  [[nodiscard]] bool active() const { return run_.has_value(); }

 private:
  [[nodiscard]] bool blocked(std::int64_t anilist_id, const PrewarmGates& gates,
                             const PrewarmDeps& deps) const;
  void mark_attempted(std::int64_t anilist_id);
  [[nodiscard]] bool in_ring(std::int64_t anilist_id) const;
  void spawn_next(const PrewarmDeps& deps);

  std::optional<PrewarmRun> run_;
  std::array<std::optional<std::int64_t>, kPrewarmRingSlots> attempted_{};
  std::size_t attempted_next_ = 0;
  std::optional<TickCount> last_start_;
  GenCounter gen_;
};

}  // namespace shigoku::tui
