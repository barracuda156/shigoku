// resolve.cpp — the pure headless resolve engine (P15). See resolve.hpp.
//
// Byte-for-byte port of sabigoku src/resolve.rs. Pure over ResolveWorld; the
// transport (app.cpp) executes the returned actions and owns every store write
// + staleness guard. The tests (tests/resolve_tests.cpp) are the 20 golden
// vectors from resolve.rs and pin every branch (tier-major, pin asymmetry,
// stamp-before-fetch, the K-2 continuation, the 64-provider bitmask ceiling).

#include "resolve.hpp"

#include <utility>

namespace shigoku::resolve {
namespace {

// mark() (resolve.rs:498-507): the bit for `name` in the provider snapshot, or
// 0 if not found / past the u64 ceiling. Overflow-safe (ROD-436).
[[nodiscard]] std::uint64_t mark(const std::vector<std::string>& providers,
                                 std::string_view name) {
  for (std::size_t i = 0; i < providers.size(); ++i) {
    if (i >= kMaxTrackedProviders) return 0;  // past the ceiling: untrackable.
    if (providers[i] == name) return std::uint64_t{1} << i;
  }
  return 0;
}

}  // namespace

std::optional<std::string> effective_pref(const ResolveWorld& world,
                                          std::int64_t anilist_id) {
  if (auto p = world.pin(anilist_id); p.has_value()) return p;  // pin overrides.
  std::string g = world.global_pref();
  if (g.empty()) return std::nullopt;  // empty global reads as none.
  return g;
}

ResolveTarget classify_open(const ResolveWorld& world, const Enrichment& canonical) {
  const std::int64_t aid = canonical.anilist_id;
  const std::optional<std::string> pref = effective_pref(world, aid);
  const std::vector<std::string> order = world.ordered(
      pref.has_value() ? std::optional<std::string_view>(*pref) : std::nullopt);

  // Tier 0 (Bound): tier-major — any binding in effective order wins (ROD-343).
  for (const std::string& p : order) {
    if (auto id = world.binding(aid, p); id.has_value()) {
      return ResolveTarget::bound(p, std::move(*id), aid);
    }
  }
  // Tier A (key): only if no binding anywhere.
  for (const std::string& p : order) {
    if (auto key = world.canonical_key(p, canonical); key.has_value()) {
      return ResolveTarget::tier_a(p, std::move(*key), aid);
    }
  }
  // Tier C: nothing anywhere.
  return ResolveTarget::needs_search(aid);
}

// --- Walk -------------------------------------------------------------------

Walk Walk::fallback(const ResolveWorld& world, const Enrichment& canonical,
                    std::optional<std::string_view> failed) {
  Walk w;
  w.canonical_ = canonical;
  const std::optional<std::string> pref = effective_pref(world, canonical.anilist_id);
  w.providers_ = world.ordered(
      pref.has_value() ? std::optional<std::string_view>(*pref) : std::nullopt);
  w.origin_ = WalkOrigin::Fallback;
  w.probe_through_absence_ = false;
  w.bindings_first_ = false;
  // The failed provider is pre-tried so the walk never re-probes it.
  if (failed.has_value()) w.tried_ = mark(w.providers_, *failed);
  return w;
}

Walk Walk::pin_flip(const Enrichment& canonical, std::string target) {
  Walk w;
  w.canonical_ = canonical;
  w.providers_ = {std::move(target)};
  w.origin_ = WalkOrigin::PinFlip;
  w.probe_through_absence_ = true;  // probe through a fresh absence.
  w.bindings_first_ = false;
  return w;
}

Walk Walk::forced_preferred(const Enrichment& canonical, std::string pref) {
  Walk w;
  w.canonical_ = canonical;
  w.providers_ = {std::move(pref)};
  w.origin_ = WalkOrigin::ForcedPreferred;
  w.probe_through_absence_ = true;  // the one-shot forced probe is manual.
  w.bindings_first_ = false;
  return w;
}

bool Walk::is_tried(std::size_t idx) const {
  if (idx >= kMaxTrackedProviders) return false;  // past the ceiling (ROD-436).
  return (tried_ & (std::uint64_t{1} << idx)) != 0;
}

void Walk::mark_tried(std::string_view provider) {
  tried_ |= mark(providers_, provider);  // unregistered name → 0, a no-op.
}

std::optional<Hop> Walk::advance(const ResolveWorld& world, Exhausted& out_exhausted) {
  const std::int64_t aid = canonical_.anilist_id;

  // K-2 continuation only: a once-only bindings-first sweep (resolve.rs:414-428)
  // so an existing binding beats a tier-A key on an earlier provider.
  if (bindings_first_ && !swept_bindings_) {
    swept_bindings_ = true;
    for (std::size_t i = 0; i < providers_.size(); ++i) {
      if (is_tried(i)) continue;
      if (auto id = world.binding(aid, providers_[i]); id.has_value()) {
        tried_ |= mark(providers_, providers_[i]);
        return Hop::fetch(providers_[i], std::move(*id), std::nullopt);
      }
    }
  }

  while (next_ < providers_.size()) {
    const std::size_t i = next_;
    const std::string provider = providers_[i];  // copy: providers_ is stable.
    ++next_;
    if (is_tried(i)) continue;  // already burned (marked from a prior sweep).
    tried_ |= mark(providers_, provider);

    // Bound: cache-first at the transport, no mint.
    if (auto id = world.binding(aid, provider); id.has_value()) {
      return Hop::fetch(provider, std::move(*id), std::nullopt);
    }
    // Absence gate: skip a fresh absence unless this is a manual walk.
    if (!probe_through_absence_ && world.absent_fresh(aid, provider)) {
      continue;
    }
    // Tier A: mint on success.
    if (auto key = world.canonical_key(provider, canonical_); key.has_value()) {
      return Hop::fetch(provider, std::move(*key), aid);
    }
    // Tier C: search + bind under aid.
    return Hop::search(provider, aid);
  }

  out_exhausted = exhaust(world);
  return std::nullopt;
}

Exhausted Walk::exhaust(const ResolveWorld& world) {
  if (origin_ == WalkOrigin::ForcedPreferred) {
    // K-2 law: continue to a fresh full bindings-first walk, the tried single
    // pref pre-marked, non-manual (respect fresh absence). Never blank a grid
    // while a binding exists on another provider.
    const std::string pref = providers_.empty() ? std::string{} : providers_.front();
    Walk cont;
    cont.canonical_ = canonical_;
    cont.providers_ = world.ordered(
        pref.empty() ? std::nullopt : std::optional<std::string_view>(pref));
    cont.origin_ = WalkOrigin::Fallback;
    cont.probe_through_absence_ = false;
    cont.bindings_first_ = true;
    cont.tried_ = mark(cont.providers_, pref);  // pre-mark the tried pref.
    return Exhausted{Exhausted::Kind::Continue, std::make_unique<Walk>(std::move(cont))};
  }
  if (origin_ == WalkOrigin::PinFlip) {
    return Exhausted{Exhausted::Kind::PinKept, nullptr};  // stop, keep the pin.
  }
  return Exhausted{Exhausted::Kind::DeadEnd, nullptr};
}

// --- route_preferred (resolve.rs:202-259) ----------------------------------

RouteOutcome route_preferred(const ResolveWorld& world, const Enrichment& canonical) {
  const std::int64_t aid = canonical.anilist_id;

  // Pin set → route is a no-op (pin overrides the route stamp).
  if (world.pin(aid).has_value()) {
    return RouteOutcome{std::nullopt, RouteAction{}};
  }

  // Empty preferred config → the route helper no-ops (follow-leader order).
  std::string g = world.global_pref();
  if (g.empty()) {
    return RouteOutcome{std::nullopt, RouteAction{}};
  }
  const std::string pref = g;

  // Settled under the live pref?
  if (auto settled = world.route_pref(aid); settled.has_value() && *settled == pref) {
    if (auto id = world.binding(aid, pref); id.has_value()) {
      // Open the pref binding, no re-stamp.
      RouteAction a;
      a.kind = RouteAction::Kind::OpenBinding;
      a.provider = pref;
      a.id = std::move(*id);
      a.anilist_id = aid;
      return RouteOutcome{std::nullopt, std::move(a)};
    }
    // Settled but unbound → fall through to normal open (no re-search loop).
    return RouteOutcome{std::nullopt, RouteAction{}};
  }

  // Stale or never settled → force the pref ONCE, but only if it is registered
  // (a retired/unknown pref cannot be forced — mis-key guard).
  if (!world.registered(pref)) {
    return RouteOutcome{std::nullopt, RouteAction{}};
  }

  RouteOutcome out;
  out.stamp = pref;  // stamp-before-fetch: the transport writes this first.
  if (auto id = world.binding(aid, pref); id.has_value()) {
    out.action.kind = RouteAction::Kind::OpenBinding;
    out.action.provider = pref;
    out.action.id = std::move(*id);
    out.action.anilist_id = aid;
  } else if (auto key = world.canonical_key(pref, canonical); key.has_value()) {
    out.action.kind = RouteAction::Kind::FetchTierA;
    out.action.provider = pref;
    out.action.id = std::move(*key);
    out.action.anilist_id = aid;
  } else {
    out.action.kind = RouteAction::Kind::Walk;
    out.action.walk = std::make_shared<Walk>(Walk::forced_preferred(canonical, pref));
  }
  return out;
}

// --- open_history (resolve.rs:142-166) -------------------------------------

HistoryOpen open_history(const ResolveWorld& world, const HistoryRecord& rec) {
  const std::int64_t aid = rec.anilist_id.value_or(0);

  // Pin arm: a HARD restriction — only the pin's OWN binding is looked up, and
  // only when the pin is registered, differs from the source, and is bound.
  if (auto pin = world.pin(aid); pin.has_value()) {
    if (*pin != rec.source && world.registered(*pin)) {
      if (auto id = world.binding(aid, *pin); id.has_value()) {
        return HistoryOpen{HistoryOpen::Kind::PinBinding, *pin, std::move(*id), nullptr};
      }
    }
    // Otherwise (retired / same-as-source / unbound pin) fall through to the
    // record — with the pin still set, the unpinned re-route is skipped
    // (route_preferred no-ops on a set pin), so an unbound pin NEVER borrows.
    return HistoryOpen{HistoryOpen::Kind::Record, rec.source, rec.source_id, nullptr};
  }

  // Unpinned: run the preferred re-route first; a non-None action routes.
  if (rec.canonical != nullptr) {
    RouteOutcome ro = route_preferred(world, *rec.canonical);
    if (ro.action.kind != RouteAction::Kind::None) {
      return HistoryOpen{HistoryOpen::Kind::Routed, {}, {},
                         std::make_shared<RouteOutcome>(std::move(ro))};
    }
  }

  // Else open the owning provider on the History row.
  return HistoryOpen{HistoryOpen::Kind::Record, rec.source, rec.source_id, nullptr};
}

}  // namespace shigoku::resolve
