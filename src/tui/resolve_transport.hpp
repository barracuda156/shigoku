// resolve_transport.hpp — the resolve WALK transport (P15). Ported from
// sabigoku src/tui/episodes.rs (the EpisodeSession) atop the pure engine in
// src/resolve.{hpp,cpp}. This is the layer that OWNS every store write and the
// staleness guards; the engine only decides. It lives in shigoku_tui (curl+
// sqlite-gated, alongside app.cpp) — NOT the render core — because it reads the
// Store and the ProviderRegistry.
//
// App holds one EpisodeSession by value; app.hpp forward-declares it (a pointer
// would do, but the session is small and owned) so the render core never sees
// store.hpp. The session holds NO store/registry reference across events —
// everything arrives per-call in EpisodeDeps (A7: all store access UI-thread).
//
// Supersede = detach + drop, never join (A1/04 §6): every fire bumps a
// generation; the four async handlers drop any result whose (token, for_id)
// isn't current, so a stale walk's late answer can never clobber a newer one.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../domain.hpp"
#include "../event.hpp"
#include "../resolve.hpp"
#include "../store.hpp"
#include "queue.hpp"
#include "workers.hpp"

namespace shigoku {
class ProviderRegistry;
}

namespace shigoku::tui {

// Per-call borrows the session needs to touch the world + fire workers. Built
// fresh by App each time it drives the session (episodes.rs EpisodeDeps).
struct EpisodeDeps {
  Store* store = nullptr;                    // may be null (degraded, no persist).
  const ProviderRegistry* registry = nullptr;
  EventQueue* queue = nullptr;
  std::string_view global_pref;              // config preferred provider ("" = unset).
  Translation translation = Translation::Sub;
  std::int64_t unix_now = 0;
};

// The transport's user-facing outcomes (episodes.rs Feedback). App turns each
// into a toast (04 §6: the session never touches toasts). The closed enum is
// fence-guarded so a new outcome forces a copy decision.
struct Feedback {
  enum class Kind {
    Hop,             // a walk moved to `provider` (announce).
    NoMatch,         // forced-preferred miss on `provider` (K-2 reroute copy).
    PinKept,         // pin-flip miss: pin kept, grid survives, on `provider`.
    PinUnreachable,  // pin-flip provider couldn't even be reached.
    DeadEnd,         // walk exhausted, nothing stocked.
    Fail,            // a provider fetch/search failed transiently (cause set).
    PinSet,          // pin set to `provider`, already serving (no walk).
    PinCleared,      // pin cleared.
    PinNothing,      // `v` with nothing open / no source to pin.
    PinPending,      // `v` while a fetch is in flight.
    PinSaveFailed,   // set_provider_pin write failed.
  } kind;
  std::string provider;
  ProviderError cause = ProviderError::network();  // Fail only.
  bool clearing = false;                            // PinSaveFailed only.

  static Feedback simple(Kind k) { return {k, {}, ProviderError::network(), false}; }
  static Feedback with_provider(Kind k, std::string p) {
    return {k, std::move(p), ProviderError::network(), false};
  }
};

// A WorldView bridging the Store + ProviderRegistry to the pure ResolveWorld
// read-ABC. Built per call (holds borrows only); read errors degrade to
// absent/unset so the engine walks a longer road rather than wedging.
class WorldView final : public resolve::ResolveWorld {
 public:
  WorldView(const Store* store, const ProviderRegistry* registry,
            std::string_view global_pref, std::int64_t unix_now)
      : store_(store), registry_(registry), global_pref_(global_pref), unix_now_(unix_now) {}

  std::vector<std::string> ordered(std::optional<std::string_view> pref) const override;
  bool registered(std::string_view provider) const override;
  std::optional<std::string> binding(std::int64_t aid, std::string_view p) const override;
  bool absent_fresh(std::int64_t aid, std::string_view p) const override;
  std::optional<std::string> canonical_key(std::string_view p,
                                           const Enrichment& c) const override;
  std::optional<std::string> pin(std::int64_t aid) const override;
  std::optional<std::string> route_pref(std::int64_t aid) const override;
  std::string global_pref() const override { return std::string(global_pref_); }

 private:
  const Store* store_;
  const ProviderRegistry* registry_;
  std::string_view global_pref_;
  std::int64_t unix_now_;
};

// The transport. One instance per App; drives one open show's resolve at a time
// (04 §7.1). Every public entry takes EpisodeDeps by const ref and returns the
// Feedback the caller toasts (or nothing for pure reads).
class EpisodeSession {
 public:
  // Open the detail for `canonical` (episodes.rs engage): preferred re-route
  // (stamp-before-fetch) then classify_open, firing the resulting fetch/search/
  // walk. Held no-op when the same show+track is already answered/loading.
  std::vector<Feedback> engage(const Enrichment& canonical, const EpisodeDeps& deps);

  // The four staleness-guarded async result handlers. Each drops silently
  // unless (gen current AND for_id == anilist_id).
  std::vector<Feedback> on_episodes_done(const EpisodesDone& e, const EpisodeDeps& deps);
  std::vector<Feedback> on_episodes_error(const EpisodesError& e, const EpisodeDeps& deps);
  std::vector<Feedback> on_search_done(const ProviderSearchDone& e, const EpisodeDeps& deps);
  std::vector<Feedback> on_search_error(const ProviderSearchError& e, const EpisodeDeps& deps);

  // `v`: cycle unpinned → each provider → unpinned; arm a pin_flip walk when
  // the new pin isn't already serving (episodes.rs cycle_pin).
  std::vector<Feedback> cycle_pin(const EpisodeDeps& deps);

  // A play stream/resolve failure: arm a fallback walk pre-marking every
  // already-tried provider, keeping the cursor on the in-progress episode
  // (episodes.rs play_fail_over). No absence (transient).
  std::vector<Feedback> play_fail_over(const std::vector<std::string>& tried,
                                       std::string raw_label, std::uint32_t ordinal,
                                       const EpisodeDeps& deps);

  // After a play finished-record landed: re-derive watched/resume and, on a
  // completed play at the played cell, advance the cursor (episodes.rs
  // on_play_recorded — the resume-demote arm).
  void on_play_recorded(std::int64_t anilist_id, std::uint32_t episode_ix,
                        bool completed, const EpisodeDeps& deps);

  // A background availability write (prewarm) touched this show: refresh the
  // meta rail if it's the engaged one (episodes.rs on_availability_write).
  void on_availability_write(std::int64_t anilist_id, const EpisodeDeps& deps);

  // Reset to idle (bumps generation, supersedes any in-flight).
  void reset();

  // --- grid render reads (pure; App copies these into EpisodeState) --------
  std::int64_t for_id() const { return for_id_.value_or(0); }
  bool has_for_id() const { return for_id_.has_value(); }
  const std::vector<std::string>& episodes() const { return episodes_; }
  int cursor() const { return static_cast<int>(cursor_); }
  bool loading() const { return loading_; }
  bool no_source() const { return no_source_; }
  const std::optional<std::string>& serving() const { return serving_; }
  const std::optional<std::string>& pin() const { return pin_; }
  const std::vector<std::pair<std::string, ProviderAvailability>>& avail() const {
    return avail_;
  }
  // The provider_id the grid was fetched under (play reuses it verbatim).
  const std::string& serving_id() const { return serving_id_; }
  // An armed fallback/pin-flip walk is advancing (episodes.rs walk_active):
  // it owns the CDN budget, so prewarm yields to it (03 §6.4).
  bool walk_active() const { return walk_.has_value() || walk_provider_.has_value(); }

 private:
  // --- internal machinery (episodes.rs private methods) --------------------
  std::vector<Feedback> engage_classify(const Enrichment& canonical, const EpisodeDeps& deps);
  bool open_bound(std::string provider, std::string id, const EpisodeDeps& deps);
  bool fire_fetch(std::string provider, std::string provider_id, bool mint,
                  const EpisodeDeps& deps);
  bool fire_search(std::string provider, const EpisodeDeps& deps);
  std::vector<Feedback> advance_walk(bool announce, const EpisodeDeps& deps);
  std::vector<Feedback> fail_over(std::string failed, const EpisodeDeps& deps);
  std::vector<Feedback> hop_unreachable(std::string provider);
  void land(std::string provider, std::vector<std::string> episodes,
            const EpisodeDeps& deps);
  void refresh_meta(const EpisodeDeps& deps);
  [[nodiscard]] bool is_stale(Generation token, std::int64_t anilist_id) const {
    return !gen_.is_current(token) || for_id_ != std::optional<std::int64_t>(anilist_id);
  }

  // --- state (episodes.rs EpisodeSession fields) ---------------------------
  std::optional<std::int64_t> for_id_;
  std::optional<Enrichment> canonical_;
  std::optional<Translation> track_;
  std::vector<std::string> episodes_;
  std::optional<std::string> serving_;
  std::string serving_id_;
  std::size_t cursor_ = 0;
  std::optional<std::size_t> resume_ix_;
  std::uint32_t watched_ = 0;
  bool no_source_ = false;
  bool loading_ = false;          // the single in-flight guard (fetch OR search).
  bool mint_pending_ = false;     // bind_provider on the in-flight fetch's success.
  std::optional<resolve::Walk> walk_;
  std::optional<std::string> walk_provider_;         // single-provider walk name.
  std::optional<std::pair<std::string, std::uint32_t>> remap_from_;  // pre-flip cursor id.
  std::optional<std::string> pin_;
  std::vector<std::pair<std::string, ProviderAvailability>> avail_;
  GenCounter gen_;
};

}  // namespace shigoku::tui
