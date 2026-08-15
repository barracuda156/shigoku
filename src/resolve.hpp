// resolve.hpp — the pure headless resolve engine (P15). Ported 1:1 from
// sabigoku src/resolve.rs. No store, no threads, no network in this file: every
// decision is a pure function over a ResolveWorld read-view, returning an
// *action* the transport (tui/app.cpp) executes and whose store writes the
// transport owns. This is the resolve.rs / episodes.rs split (03 §4-§6, 05 §10):
// the engine decides, the transport writes + guards staleness.
//
// The A8 walk vocabulary (Tier::Bound/BIdMatch, WalkOrigin::*) becomes
// constructed machinery here (PORT_PARITY §3). The K-2 fence lives in
// Walk::exhaust: a forced-preferred miss continues to a full bindings-first
// walk; a pin-flip miss stops and keeps the pin; a fallback miss dead-ends.
//
// Providers depend on domain only (01 §5); this engine depends on domain +
// resolver (scoring) and NEVER on store/tui, so it links into the render-core-
// free library and unit-tests fully offline against a FakeWorld (§8).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "provider.hpp"  // WalkOrigin, Tier vocabulary.

namespace shigoku::resolve {

// --- ResolveWorld: the read-only view the engine consults (resolve.rs:22-49) --
//
// The live impl (WorldView, in app.cpp) reads the Store + ProviderRegistry; the
// test fake (FakeWorld) is canned. Every method is a pure read — the engine
// never writes through this.
class ResolveWorld {
 public:
  virtual ~ResolveWorld() = default;

  // Registry order, `pref` first when it names a live provider; else plain
  // construction order. Empty/unknown pref → construction order.
  [[nodiscard]] virtual std::vector<std::string> ordered(
      std::optional<std::string_view> pref) const = 0;

  // Live registry membership — a retired pin name is NOT registered (guards
  // against fetching a foreign id on primary).
  [[nodiscard]] virtual bool registered(std::string_view provider) const = 0;

  // Stored provider_id for (anilist_id, provider), or nullopt.
  [[nodiscard]] virtual std::optional<std::string> binding(
      std::int64_t anilist_id, std::string_view provider) const = 0;

  // Within-TTL absence for (anilist_id, provider) (the store owns the 7-day TTL;
  // a stale absence reads as false = unchecked).
  [[nodiscard]] virtual bool absent_fresh(std::int64_t anilist_id,
                                          std::string_view provider) const = 0;

  // Tier-A key = pure derivation from the canonical. nullopt = provider doesn't
  // id-key on canonical (NOT "not stocked").
  [[nodiscard]] virtual std::optional<std::string> canonical_key(
      std::string_view provider, const Enrichment& canonical) const = 0;

  // Per-show pin (at most one provider), or nullopt.
  [[nodiscard]] virtual std::optional<std::string> pin(std::int64_t anilist_id) const = 0;

  // The preferred_provider this show last settled under (ROD-398), or nullopt.
  [[nodiscard]] virtual std::optional<std::string> route_pref(
      std::int64_t anilist_id) const = 0;

  // Global config preferred provider; empty = follow-leader / unset.
  [[nodiscard]] virtual std::string global_pref() const = 0;
};

// --- effective_pref (resolve.rs:53-59) -------------------------------------
// Pin overrides global; an empty global reads as nullopt. This is the
// pin-folds-into-preference rule (Path 1 side of the pin asymmetry).
[[nodiscard]] std::optional<std::string> effective_pref(const ResolveWorld& world,
                                                        std::int64_t anilist_id);

// --- Path 1: classify_open (resolve.rs:65-114) -----------------------------
//
// Tier-major (ROD-343): a binding on ANY provider (in effective order) beats a
// tier-A key on an earlier provider. No tier-B arm here — id-match is a
// search-result step in the transport, reached via NeedsSearch.
struct ResolveTarget {
  enum class Kind { Bound, TierA, NeedsSearch } kind;
  std::string provider;      // Bound/TierA.
  std::string id;            // Bound: provider_id; TierA: canonical key.
  std::int64_t anilist_id = 0;

  static ResolveTarget bound(std::string p, std::string id, std::int64_t aid) {
    return {Kind::Bound, std::move(p), std::move(id), aid};
  }
  static ResolveTarget tier_a(std::string p, std::string key, std::int64_t aid) {
    return {Kind::TierA, std::move(p), std::move(key), aid};
  }
  static ResolveTarget needs_search(std::int64_t aid) {
    return {Kind::NeedsSearch, {}, {}, aid};
  }
  friend bool operator==(const ResolveTarget&, const ResolveTarget&) = default;
};

[[nodiscard]] ResolveTarget classify_open(const ResolveWorld& world,
                                          const Enrichment& canonical);

// --- Walk: the fallback state machine (resolve.rs:265-488) -----------------

// One hop the transport executes (resolve.rs:276-286).
struct Hop {
  enum class Kind { Fetch, Search } kind;
  std::string provider;
  std::string id;                      // Fetch: provider_id (bound or key).
  std::optional<std::int64_t> bind;    // Fetch: Some(aid) → mint on success.
  std::int64_t anilist_id = 0;         // Search: the aid to search + bind under.

  static Hop fetch(std::string p, std::string id, std::optional<std::int64_t> bind) {
    return {Kind::Fetch, std::move(p), std::move(id), bind, 0};
  }
  static Hop search(std::string p, std::int64_t aid) {
    return {Kind::Search, std::move(p), {}, std::nullopt, aid};
  }
  friend bool operator==(const Hop&, const Hop&) = default;
};

class Walk;
struct RouteOutcome;  // fwd — route_preferred() is a friend of Walk.

// What an exhausted walk does (resolve.rs:290-299). Continue carries the K-2
// continuation walk (a fresh full bindings-first walk); PinKept stops and keeps
// the pin; DeadEnd is the ordinary end.
struct Exhausted {
  enum class Kind { Continue, PinKept, DeadEnd } kind;
  std::unique_ptr<Walk> next;  // Continue only.
};

// Skip-mask ceiling (resolve.rs:494): a u64 bitmask tracks tried providers.
inline constexpr std::size_t kMaxTrackedProviders = 64;

// The fallback walk (resolve.rs:306-488). Snapshot-immutable provider list; a
// forward-only cursor with a tried bitmask. Constructed by fallback()/pin_flip()
// / the private forced_preferred() (via route_preferred) / the K-2 continuation.
class Walk {
 public:
  // Ordinary failure-driven fallback (resolve.rs:367-387): full ordered
  // snapshot under effective_pref, origin Fallback, respects fresh absence.
  // `failed` (the provider whose fetch/stream just failed) is marked tried at
  // construction so the walk never re-probes it; nullopt = no provider burned
  // yet (a from-scratch fallback).
  [[nodiscard]] static Walk fallback(const ResolveWorld& world,
                                     const Enrichment& canonical,
                                     std::optional<std::string_view> failed);

  // Manual pin flip (resolve.rs:350-363): single-provider [target], probes
  // THROUGH fresh absence, origin PinFlip. `target` MUST be a live registry
  // name — the caller guarantees it (this ctor takes no world to check).
  [[nodiscard]] static Walk pin_flip(const Enrichment& canonical, std::string target);

  // Mark a provider already-tried (resolve.rs:397-399): an unregistered name is
  // a no-op (never throws). Used to seed a continuation with burned providers.
  void mark_tried(std::string_view provider);

  WalkOrigin origin() const { return origin_; }

  // One hop, or Exhausted (resolve.rs:413-461). Advances the cursor forward,
  // skipping tried providers and (unless probe_through_absence) fresh absences.
  // Returns nullopt with `out_exhausted` filled when the walk runs off the end.
  [[nodiscard]] std::optional<Hop> advance(const ResolveWorld& world,
                                           Exhausted& out_exhausted);

  const Enrichment& canonical() const { return canonical_; }

 private:
  Walk() = default;

  // Stale-stamp re-route (resolve.rs:328-341): single-provider [pref],
  // probes THROUGH fresh absence, origin ForcedPreferred. Private — only
  // route_preferred arms it.
  [[nodiscard]] static Walk forced_preferred(const Enrichment& canonical,
                                             std::string pref);

  // K-2 exhaust (resolve.rs:466-488): forced_preferred → Continue(full
  // bindings-first walk, pref pre-tried); pin_flip → PinKept; fallback →
  // DeadEnd.
  [[nodiscard]] Exhausted exhaust(const ResolveWorld& world);

  [[nodiscard]] bool is_tried(std::size_t idx) const;

  Enrichment canonical_;
  std::vector<std::string> providers_;  // snapshot, immutable across the walk.
  std::size_t next_ = 0;                 // forward-only cursor.
  std::uint64_t tried_ = 0;              // bitmask of tried provider indices.
  WalkOrigin origin_ = WalkOrigin::Fallback;
  bool probe_through_absence_ = false;   // "manual" bit (§5.3, orthogonal to origin).
  bool bindings_first_ = false;          // K-2 continuation: sweep bindings once.
  bool swept_bindings_ = false;          // latch so the sweep runs once.

  friend RouteOutcome route_preferred(const ResolveWorld&, const Enrichment&);
};

// --- Path 2: open_history (resolve.rs:119-166) -----------------------------

struct HistoryRecord {
  std::optional<std::int64_t> anilist_id;
  std::string source;      // the owning provider on the History row.
  std::string source_id;   // its provider_id.
  const Enrichment* canonical = nullptr;
};

struct HistoryOpen {
  enum class Kind { PinBinding, Routed, Record } kind;
  std::string provider;  // PinBinding/Record.
  std::string id;        // PinBinding/Record.
  std::shared_ptr<RouteOutcome> routed;  // Routed only.
};

// --- Path (route): route_preferred (resolve.rs:173-259) --------------------

// The action a preferred re-route resolves to (resolve.rs:181-196).
struct RouteAction {
  enum class Kind { OpenBinding, FetchTierA, Walk, None } kind = Kind::None;
  std::string provider;
  std::string id;               // OpenBinding: provider_id; FetchTierA: key.
  std::int64_t anilist_id = 0;
  std::shared_ptr<Walk> walk;   // Walk only (shared: RouteOutcome is copyable).
};

// stamp = the pref to write to route_pref BEFORE firing action (stamp-before-
// fetch, ROD-398); nullopt = no re-stamp (resolve.rs:173-178).
struct RouteOutcome {
  std::optional<std::string> stamp;
  RouteAction action;
};

[[nodiscard]] RouteOutcome route_preferred(const ResolveWorld& world,
                                           const Enrichment& canonical);

[[nodiscard]] HistoryOpen open_history(const ResolveWorld& world,
                                       const HistoryRecord& rec);

}  // namespace shigoku::resolve
