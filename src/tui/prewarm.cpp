// prewarm.cpp — the eager sibling-provider warm walk (P15). See prewarm.hpp.
//
// Faithful port of sabigoku src/tui/prewarm.rs. Candidate = unchecked only
// (not bound, not fresh-absent). Each probe mirrors the resolve walk's
// classification (canonical_key → episodes; else search → score → episodes) and
// posts one PrewarmResult. on_result mints Found→bind (steal-safe, fails CLOSED)
// / Absent→mark_absent / Nothing→nothing. Store is the cache — a later flip or
// fallback reads these rows and lands tier-0.

#include "prewarm.hpp"

#include "../provider.hpp"
#include "../resolver.hpp"
#include "../store.hpp"

namespace shigoku::tui {
namespace {

// The providers this show is not yet checked on: skip bound + fresh-absent.
// A store read here fails OPEN (a missed skip costs one wasted probe, not a
// wrong mint) — the mint path (on_result) is the one that fails closed.
std::vector<std::string> candidates(const Store* store, const ProviderRegistry& registry,
                                    std::int64_t aid, std::int64_t unix_now) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < registry.size(); ++i) {
    const StreamProvider* p = registry.at(i);
    const std::string_view name = p->name();
    bool bound = false;
    if (store != nullptr) {
      if (auto rows = store->bindings_for(aid); rows.has_value()) {
        for (const Binding& b : *rows) {
          if (b.provider == name) { bound = true; break; }
        }
      }
    }
    if (bound) continue;
    bool absent = false;
    if (store != nullptr) {
      if (auto a = store->provider_absent_fresh(aid, name, unix_now); a.has_value()) {
        absent = *a;
      }
    }
    if (absent) continue;
    out.emplace_back(name);
  }
  return out;
}

// Classify one provider's stock for a canonical (workers.rs probe_candidate):
// tier-A key → episodes; else search → best_id/best_provider → episodes.
// Non-empty listing → Found{provider_id}; authoritative empty → Absent; any
// error/miss → Nothing.
PrewarmVerdict probe_candidate(const StreamProvider& prov, const Enrichment& canonical,
                               Translation tr) {
  std::string provider_id;
  if (auto key = prov.canonical_key(canonical); key.has_value()) {
    provider_id = std::move(*key);
  } else {
    SearchOptions opts;
    opts.translation = tr;
    auto hits = prov.search(canonical.title_romaji, opts);
    if (!hits.has_value()) return PrewarmVerdict{PrewarmVerdict::Kind::Nothing, {}};
    std::optional<std::size_t> ix = resolver::best_id_match(canonical, *hits);
    if (!ix.has_value()) ix = resolver::best_provider_match(canonical, *hits);
    if (!ix.has_value()) return PrewarmVerdict{PrewarmVerdict::Kind::Nothing, {}};
    provider_id = (*hits)[*ix].provider_id;
  }
  auto eps = prov.episodes(provider_id, tr, canonical.total_episodes);
  if (!eps.has_value()) return PrewarmVerdict{PrewarmVerdict::Kind::Nothing, {}};
  if (eps->empty()) return PrewarmVerdict{PrewarmVerdict::Kind::Absent, {}};
  return PrewarmVerdict{PrewarmVerdict::Kind::Found, std::move(provider_id)};
}

}  // namespace

bool PrewarmState::in_ring(std::int64_t anilist_id) const {
  for (const auto& slot : attempted_) {
    if (slot == std::optional<std::int64_t>(anilist_id)) return true;
  }
  return false;
}

void PrewarmState::mark_attempted(std::int64_t anilist_id) {
  attempted_[attempted_next_] = anilist_id;
  attempted_next_ = (attempted_next_ + 1) % kPrewarmRingSlots;
}

bool PrewarmState::blocked(std::int64_t anilist_id, const PrewarmGates& gates,
                           const PrewarmDeps& deps) const {
  if (run_.has_value()) return true;                       // a walk is active.
  if (gates.play_resolving || gates.fallback_active) return true;  // yield.
  if (in_ring(anilist_id)) return true;                    // recently attempted.
  if (last_start_.has_value() &&
      deps.now_tick - *last_start_ < kPrewarmSpacingTicks) {
    return true;  // inside the 30s spacing floor.
  }
  return false;
}

void PrewarmState::fire(const Enrichment& canonical, const PrewarmGates& gates,
                        const PrewarmDeps& deps) {
  const std::int64_t aid = canonical.anilist_id;
  if (blocked(aid, gates, deps)) return;
  if (deps.registry == nullptr) return;
  std::vector<std::string> cands =
      candidates(deps.store, *deps.registry, aid, deps.unix_now);
  if (cands.empty()) return;  // all bound/absent: no run, ring/last_start unstamped.

  mark_attempted(aid);
  last_start_ = deps.now_tick;
  PrewarmRun run;
  run.canonical = canonical;
  run.candidates = std::move(cands);
  run_ = std::move(run);
  spawn_next(deps);
}

namespace {

// Spawn a single detached probe worker posting exactly one PrewarmResult.
void spawn_probe(EventQueue& queue, const ProviderRegistry& registry, std::string provider,
                 Enrichment canonical, Translation tr, Generation token) {
  spawn_detached([&queue, &registry, provider = std::move(provider),
                  canonical = std::move(canonical), tr, token]() {
    const std::int64_t aid = canonical.anilist_id;
    const StreamProvider* prov = registry.by_name(provider);
    PrewarmVerdict verdict = (prov == nullptr)
                                 ? PrewarmVerdict{PrewarmVerdict::Kind::Nothing, {}}
                                 : probe_candidate(*prov, canonical, tr);
    queue.try_post(Event{PrewarmResult{aid, std::move(provider), std::move(verdict), token}});
  });
}

}  // namespace

void PrewarmState::spawn_next(const PrewarmDeps& deps) {
  if (!run_.has_value()) return;
  PrewarmRun& run = *run_;
  if (run.yielding) { run_.reset(); return; }  // cancelled between hops.
  if (run.ix >= run.candidates.size()) { run_.reset(); return; }  // done.
  if (deps.registry == nullptr || deps.queue == nullptr) { run_.reset(); return; }

  const std::string provider = run.candidates[run.ix];
  ++run.ix;
  run.inflight = true;
  run.gap_until.reset();
  const Generation token = gen_.bump();
  spawn_probe(*deps.queue, *deps.registry, provider, run.canonical, deps.translation, token);
}

bool PrewarmState::on_result(const PrewarmResult& r, const PrewarmDeps& deps) {
  if (!gen_.is_current(r.token)) return false;  // stale probe: drop.
  bool changed = false;
  if (r.verdict.kind == PrewarmVerdict::Kind::Found) {
    // Steal-safety: never re-point another show's (provider, provider_id) edge.
    // A store read error here fails CLOSED (mint nothing) — a wrong steal is
    // worse than a missed warm.
    bool safe = false;
    if (deps.store != nullptr) {
      auto owner = deps.store->show_id_for_binding(r.provider, r.verdict.provider_id);
      if (owner.has_value()) {
        safe = !owner->has_value() || **owner == r.anilist_id;
      }
      // owner read error (!has_value) → safe stays false (fail closed).
    }
    if (safe && run_.has_value()) {
      (void)deps.store->bind_provider(run_->canonical, r.provider, r.verdict.provider_id,
                                      deps.unix_now);
      changed = true;
    }
  } else if (r.verdict.kind == PrewarmVerdict::Kind::Absent) {
    if (deps.store != nullptr && run_.has_value()) {
      (void)deps.store->mark_provider_absent(run_->canonical, r.provider, deps.unix_now);
      changed = true;
    }
  }
  // Nothing: write nothing.

  // Pace the next hop: mark the in-flight probe home and arm the HOP_GAP.
  if (run_.has_value()) {
    run_->inflight = false;
    if (run_->yielding) {
      run_.reset();  // cancelled: honored this result, spawn no more.
    } else if (run_->ix >= run_->candidates.size()) {
      run_.reset();  // last candidate done.
    } else {
      run_->gap_until = deps.now_tick + kPrewarmHopGapTicks;
    }
  }
  return changed;
}

void PrewarmState::tick(const PrewarmDeps& deps) {
  if (!run_.has_value()) return;
  PrewarmRun& run = *run_;
  if (run.inflight) return;  // waiting on a probe.
  if (run.yielding) { run_.reset(); return; }
  if (!run.gap_until.has_value()) return;      // no pacing armed.
  if (deps.now_tick < *run.gap_until) return;  // still inside HOP_GAP.
  spawn_next(deps);
}

void PrewarmState::cancel() {
  if (!run_.has_value()) return;
  if (run_->inflight) {
    run_->yielding = true;  // honor the sunk in-flight result, spawn no more.
  } else {
    run_.reset();  // nothing out: drop the run.
  }
}

}  // namespace shigoku::tui
