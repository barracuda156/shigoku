// resolve_transport.cpp — the resolve walk transport (P15). See the header.
//
// Faithful port of sabigoku src/tui/episodes.rs. The load-bearing invariants
// (episodes.rs port checklist): every fire bumps the generation; the staleness
// predicate is exactly `gen.is_current(token) && for_id == aid` at the head of
// all four async handlers; on_done writes bind_provider (if mint_pending)
// BEFORE set_episode_cache (ROD-327 FK order); engage stamps route_pref BEFORE
// firing a forced action; search scoring is best_id_match then
// best_provider_match; the three walk-exhaust arms map Continue→recurse+NoMatch,
// PinKept→toast+free, DeadEnd→toast+free+no_source.

#include "resolve_transport.hpp"

#include "../provider.hpp"
#include "../resolver.hpp"

namespace shigoku::tui {

// --- WorldView --------------------------------------------------------------

std::vector<std::string> WorldView::ordered(std::optional<std::string_view> pref) const {
  // Registry order, pref first when live; else construction order.
  std::vector<std::string> out;
  if (registry_ == nullptr) return out;
  if (pref.has_value() && registered(*pref)) out.emplace_back(*pref);
  for (std::size_t i = 0; i < registry_->size(); ++i) {
    std::string_view name = registry_->at(i)->name();
    if (pref.has_value() && name == *pref) continue;
    out.emplace_back(name);
  }
  return out;
}

bool WorldView::registered(std::string_view provider) const {
  return registry_ != nullptr && registry_->by_name(provider) != nullptr;
}

std::optional<std::string> WorldView::binding(std::int64_t aid, std::string_view p) const {
  if (store_ == nullptr) return std::nullopt;
  auto rows = store_->bindings_for(aid);
  if (!rows.has_value()) return std::nullopt;  // read error → unset.
  for (const Binding& b : *rows) {
    if (b.provider == p) return b.provider_id;
  }
  return std::nullopt;
}

bool WorldView::absent_fresh(std::int64_t aid, std::string_view p) const {
  if (store_ == nullptr) return false;
  auto r = store_->provider_absent_fresh(aid, p, unix_now_);
  return r.has_value() && *r;  // read error → false (unchecked).
}

std::optional<std::string> WorldView::canonical_key(std::string_view p,
                                                    const Enrichment& c) const {
  if (registry_ == nullptr) return std::nullopt;
  const StreamProvider* prov = registry_->by_name(p);
  if (prov == nullptr) return std::nullopt;
  return prov->canonical_key(c);
}

std::optional<std::string> WorldView::pin(std::int64_t aid) const {
  if (store_ == nullptr) return std::nullopt;
  auto r = store_->get_provider_pin(aid);
  if (!r.has_value()) return std::nullopt;
  return *r;
}

std::optional<std::string> WorldView::route_pref(std::int64_t aid) const {
  if (store_ == nullptr) return std::nullopt;
  auto r = store_->get_route_pref(aid);
  if (!r.has_value()) return std::nullopt;
  return *r;
}

// --- EpisodeSession ---------------------------------------------------------

namespace {

// Build a WorldView from the per-call deps.
WorldView world_of(const EpisodeDeps& deps) {
  return WorldView(deps.store, deps.registry, deps.global_pref, deps.unix_now);
}

}  // namespace

void EpisodeSession::reset() {
  gen_.bump();  // supersede any in-flight FIRST.
  for_id_.reset();
  canonical_.reset();
  track_.reset();
  episodes_.clear();
  serving_.reset();
  serving_id_.clear();
  cursor_ = 0;
  resume_ix_.reset();
  watched_ = 0;
  no_source_ = false;
  loading_ = false;
  mint_pending_ = false;
  walk_.reset();
  walk_provider_.reset();
  remap_from_.reset();
  pin_.reset();
  avail_.clear();
}

std::vector<Feedback> EpisodeSession::engage(const Enrichment& canonical,
                                             const EpisodeDeps& deps) {
  const std::int64_t aid = canonical.anilist_id;

  // Held no-op: same show+track already answered / loading / dead-ended.
  if (for_id_ == std::optional<std::int64_t>(aid) &&
      track_ == std::optional<Translation>(deps.translation) &&
      (loading_ || !episodes_.empty() || no_source_)) {
    return {};
  }

  reset();
  for_id_ = aid;
  canonical_ = canonical;
  track_ = deps.translation;
  refresh_meta(deps);

  // Preferred re-route runs before the classifier (03 §6.1 step 4).
  WorldView world = world_of(deps);
  resolve::RouteOutcome routed = resolve::route_preferred(world, canonical);

  // Stamp-before-fetch (ROD-398): write route_pref FIRST; only on success keep
  // the forcing action, else degrade to None (open normally, never unstamped).
  resolve::RouteAction action = std::move(routed.action);
  if (routed.stamp.has_value()) {
    bool ok = false;
    if (deps.store != nullptr) {
      ok = deps.store->set_route_pref(canonical, *routed.stamp).has_value();
    }
    if (!ok) action = resolve::RouteAction{};  // None.
  }

  switch (action.kind) {
    case resolve::RouteAction::Kind::None:
      return engage_classify(canonical, deps);
    case resolve::RouteAction::Kind::OpenBinding:
      open_bound(action.provider, action.id, deps);
      return {};
    case resolve::RouteAction::Kind::FetchTierA:
      fire_fetch(action.provider, action.id, /*mint=*/true, deps);
      return {};
    case resolve::RouteAction::Kind::Walk:
      walk_provider_ = std::string(deps.global_pref);
      walk_ = std::move(*action.walk);
      return advance_walk(/*announce=*/false, deps);
  }
  return {};  // unreachable (closed enum).
}

std::vector<Feedback> EpisodeSession::engage_classify(const Enrichment& canonical,
                                                      const EpisodeDeps& deps) {
  WorldView world = world_of(deps);
  resolve::ResolveTarget t = resolve::classify_open(world, canonical);
  switch (t.kind) {
    case resolve::ResolveTarget::Kind::Bound:
      open_bound(t.provider, t.id, deps);
      return {};
    case resolve::ResolveTarget::Kind::TierA:
      fire_fetch(t.provider, t.id, /*mint=*/true, deps);
      return {};
    case resolve::ResolveTarget::Kind::NeedsSearch:
      // A plain fallback walk (no walk_provider → misses read as DeadEnd).
      walk_ = resolve::Walk::fallback(world, canonical, std::nullopt);
      return advance_walk(/*announce=*/false, deps);
  }
  return {};  // unreachable.
}

bool EpisodeSession::open_bound(std::string provider, std::string id,
                                const EpisodeDeps& deps) {
  const std::int64_t aid = for_id_.value_or(0);
  // Cache-first (a bound hop paints from the episode_cache when unexpired).
  if (deps.store != nullptr) {
    auto cached = deps.store->get_cached_episodes(aid, provider, deps.translation,
                                                  deps.unix_now);
    if (cached.has_value() && cached->has_value()) {
      serving_id_ = id;  // play reuses the bound provider_id.
      land(std::move(provider), std::move(**cached), deps);
      return true;
    }
  }
  // Already bound → no mint.
  return fire_fetch(std::move(provider), std::move(id), /*mint=*/false, deps);
}

namespace {

// Spawn an episodes-fetch worker: episodes() on `provider`, posting EpisodesDone
// (with the provider name + provider_id) or EpisodesError. Off the UI thread.
void spawn_episodes_fetch(EventQueue& queue, const ProviderRegistry& registry,
                          std::string provider, std::string provider_id,
                          Translation tr, std::optional<std::uint32_t> count_hint,
                          std::int64_t for_id, Generation token) {
  spawn_detached([&queue, &registry, provider = std::move(provider),
                  provider_id = std::move(provider_id), tr, count_hint, for_id, token]() {
    const StreamProvider* prov = registry.by_name(provider);
    if (prov == nullptr) {
      queue.try_post(Event{EpisodesError{ProviderError::unsupported(), for_id, token, provider}});
      return;
    }
    auto eps = prov->episodes(provider_id, tr, count_hint);
    if (!eps.has_value()) {
      queue.try_post(Event{EpisodesError{eps.error(), for_id, token, provider}});
      return;
    }
    queue.try_post(Event{EpisodesDone{std::move(*eps), for_id, token,
                                      std::move(provider), std::move(provider_id)}});
  });
}

// Spawn a provider-search worker: search(title) on `provider`, posting
// ProviderSearchDone (hits) or ProviderSearchError. Off the UI thread.
void spawn_provider_search(EventQueue& queue, const ProviderRegistry& registry,
                           std::string provider, std::string query, Translation tr,
                           std::int64_t for_id, Generation token) {
  spawn_detached([&queue, &registry, provider = std::move(provider),
                  query = std::move(query), tr, for_id, token]() {
    const StreamProvider* prov = registry.by_name(provider);
    if (prov == nullptr) {
      queue.try_post(Event{ProviderSearchError{ProviderError::unsupported(), for_id, token, provider}});
      return;
    }
    SearchOptions opts;
    opts.translation = tr;
    auto hits = prov->search(query, opts);
    if (!hits.has_value()) {
      queue.try_post(Event{ProviderSearchError{hits.error(), for_id, token, provider}});
      return;
    }
    queue.try_post(Event{ProviderSearchDone{std::move(*hits), for_id, token, std::move(provider)}});
  });
}

}  // namespace

bool EpisodeSession::fire_fetch(std::string provider, std::string provider_id, bool mint,
                                const EpisodeDeps& deps) {
  if (!canonical_.has_value()) return false;
  if (deps.registry == nullptr || deps.queue == nullptr) return false;
  const Generation token = gen_.bump();  // generation bump + token minted here.
  mint_pending_ = mint;
  loading_ = true;
  // count_hint: listing-less providers mint a 1..N grid off the metadata total.
  std::optional<std::uint32_t> count_hint = canonical_->total_episodes;
  spawn_episodes_fetch(*deps.queue, *deps.registry, std::move(provider),
                       std::move(provider_id), deps.translation, count_hint,
                       canonical_->anilist_id, token);
  return true;
}

bool EpisodeSession::fire_search(std::string provider, const EpisodeDeps& deps) {
  if (!canonical_.has_value()) return false;
  if (deps.registry == nullptr || deps.queue == nullptr) return false;
  const Generation token = gen_.bump();
  mint_pending_ = false;  // a search never mints directly; the fetch it fires does.
  loading_ = true;
  spawn_provider_search(*deps.queue, *deps.registry, std::move(provider),
                        canonical_->title_romaji, deps.translation,
                        canonical_->anilist_id, token);
  return true;
}

std::vector<Feedback> EpisodeSession::advance_walk(bool announce, const EpisodeDeps& deps) {
  if (!walk_.has_value()) {
    if (episodes_.empty()) no_source_ = true;
    return {};
  }
  WorldView world = world_of(deps);
  resolve::Exhausted ex{resolve::Exhausted::Kind::DeadEnd, nullptr};
  std::optional<resolve::Hop> hop = walk_->advance(world, ex);

  std::vector<Feedback> fb;
  if (hop.has_value()) {
    if (hop->kind == resolve::Hop::Kind::Fetch) {
      if (announce) fb.push_back(Feedback::with_provider(Feedback::Kind::Hop, hop->provider));
      // A bound hop (bind == nullopt) can paint from cache; a fresh tier-A key
      // must fetch + mint.
      const bool running =
          hop->bind.has_value()
              ? fire_fetch(hop->provider, hop->id, /*mint=*/true, deps)
              : open_bound(hop->provider, hop->id, deps);
      if (!running) {
        auto extra = hop_unreachable(hop->provider);
        fb.insert(fb.end(), extra.begin(), extra.end());
      }
      return fb;
    }
    // Search.
    if (announce) fb.push_back(Feedback::with_provider(Feedback::Kind::Hop, hop->provider));
    if (!fire_search(hop->provider, deps)) {
      auto extra = hop_unreachable(hop->provider);
      fb.insert(fb.end(), extra.begin(), extra.end());
    }
    return fb;
  }

  // Exhausted.
  switch (ex.kind) {
    case resolve::Exhausted::Kind::Continue: {
      std::string missed = walk_provider_.has_value() ? *walk_provider_ : std::string{};
      walk_provider_.reset();
      walk_ = std::move(*ex.next);
      fb.push_back(Feedback::with_provider(Feedback::Kind::NoMatch, std::move(missed)));
      auto more = advance_walk(/*announce=*/true, deps);  // drive the continuation.
      fb.insert(fb.end(), more.begin(), more.end());
      return fb;
    }
    case resolve::Exhausted::Kind::PinKept: {
      std::string target = walk_provider_.has_value() ? *walk_provider_ : std::string{};
      walk_provider_.reset();
      walk_.reset();
      loading_ = false;
      fb.push_back(Feedback::with_provider(Feedback::Kind::PinKept, std::move(target)));
      return fb;
    }
    case resolve::Exhausted::Kind::DeadEnd:
      walk_.reset();
      walk_provider_.reset();
      loading_ = false;
      if (episodes_.empty()) no_source_ = true;
      fb.push_back(Feedback::simple(Feedback::Kind::DeadEnd));
      return fb;
  }
  return fb;  // unreachable.
}

std::vector<Feedback> EpisodeSession::hop_unreachable(std::string provider) {
  // Read origin BEFORE clearing the walk.
  std::optional<WalkOrigin> origin;
  if (walk_.has_value()) origin = walk_->origin();
  walk_.reset();
  walk_provider_.reset();
  if (origin == std::optional<WalkOrigin>(WalkOrigin::PinFlip)) {
    return {Feedback::with_provider(Feedback::Kind::PinUnreachable, std::move(provider))};
  }
  if (episodes_.empty()) no_source_ = true;
  return {};  // silent for non-flip (matches fire-fail policy).
}

std::vector<Feedback> EpisodeSession::fail_over(std::string failed, const EpisodeDeps& deps) {
  if (!walk_.has_value() && canonical_.has_value()) {
    WorldView world = world_of(deps);
    walk_ = resolve::Walk::fallback(world, *canonical_, std::string_view(failed));
  }
  return advance_walk(/*announce=*/true, deps);
}

std::vector<Feedback> EpisodeSession::on_episodes_done(const EpisodesDone& e,
                                                       const EpisodeDeps& deps) {
  if (is_stale(e.gen, e.for_id)) return {};
  loading_ = false;
  if (!canonical_.has_value()) return {};
  const Enrichment canonical = *canonical_;

  // Empty listing → authoritative not-stocked: mark absence, never bind.
  if (e.episodes.empty()) {
    if (deps.store != nullptr) {
      (void)deps.store->mark_provider_absent(canonical, e.provider, deps.unix_now);
    }
    mint_pending_ = false;
    refresh_meta(deps);
    return fail_over(e.provider, deps);
  }

  // Non-empty: ROD-327 FK order — bind (if pending) BEFORE the episode cache.
  if (deps.store != nullptr) {
    if (mint_pending_) {
      (void)deps.store->bind_provider(canonical, e.provider, e.provider_id, deps.unix_now);
      mint_pending_ = false;
    }
    (void)deps.store->set_episode_cache(
        e.for_id, e.provider, deps.translation, e.episodes,
        canonical.status.has_value() ? std::optional<std::string_view>(*canonical.status)
                                     : std::nullopt,
        deps.unix_now);
  }
  walk_.reset();          // landed hop clears the walk — no ping-pong.
  walk_provider_.reset();
  serving_id_ = e.provider_id;
  refresh_meta(deps);
  land(e.provider, e.episodes, deps);
  return {};
}

std::vector<Feedback> EpisodeSession::on_episodes_error(const EpisodesError& e,
                                                        const EpisodeDeps& deps) {
  if (is_stale(e.gen, e.for_id)) return {};
  loading_ = false;
  std::vector<Feedback> fb;
  if (e.cause.kind != ProviderError::Kind::Unsupported) {
    fb.push_back({Feedback::Kind::Fail, e.provider, e.cause, false});
  }
  // No absence on a transient failure. Advance the walk.
  auto more = fail_over(e.provider, deps);
  fb.insert(fb.end(), more.begin(), more.end());
  return fb;
}

std::vector<Feedback> EpisodeSession::on_search_done(const ProviderSearchDone& e,
                                                     const EpisodeDeps& deps) {
  if (is_stale(e.gen, e.for_id)) return {};
  loading_ = false;
  if (!canonical_.has_value()) return {};
  const Enrichment& canonical = *canonical_;

  // Tier-B id match outranks tier-C fuzzy; below the floors is a miss.
  std::optional<std::size_t> ix = resolver::best_id_match(canonical, e.hits);
  if (!ix.has_value()) ix = resolver::best_provider_match(canonical, e.hits);
  if (ix.has_value()) {
    fire_fetch(e.provider, e.hits[*ix].provider_id, /*mint=*/true, deps);
    return {};
  }
  return advance_walk(/*announce=*/true, deps);  // re-drive on a miss.
}

std::vector<Feedback> EpisodeSession::on_search_error(const ProviderSearchError& e,
                                                      const EpisodeDeps& deps) {
  if (is_stale(e.gen, e.for_id)) return {};
  loading_ = false;
  std::vector<Feedback> fb;
  if (e.cause.kind != ProviderError::Kind::Unsupported) {
    fb.push_back({Feedback::Kind::Fail, e.provider, e.cause, false});
  }
  // No fail_over / no absence — a search failure just advances the existing walk.
  auto more = advance_walk(/*announce=*/true, deps);
  fb.insert(fb.end(), more.begin(), more.end());
  return fb;
}

void EpisodeSession::land(std::string provider, std::vector<std::string> episodes,
                          const EpisodeDeps& deps) {
  no_source_ = false;
  loading_ = false;
  serving_ = provider;
  // A landed grid clears the walk — no ping-pong (episodes.rs on_done). Done
  // HERE (the single landing point) so the cache-hit open_bound path clears it
  // too, not just the fetch-success path — else a bound-from-cache hop would
  // leave a stale walk_ that permanently trips play_fail_over's single-flight
  // guard once that entry point is wired (Fable gate #3, P22).
  walk_.reset();
  walk_provider_.reset();
  const std::int64_t aid = for_id_.value_or(0);

  watched_ = 0;
  if (deps.store != nullptr) {
    auto p = deps.store->get_progress(aid);
    if (p.has_value() && p->has_value()) watched_ = **p;
  }
  const std::size_t len = episodes.size();
  const std::size_t next = watched_;
  cursor_ = (next >= len) ? 0 : next;

  // Resume position overrides the next-unwatched cursor.
  resume_ix_.reset();
  if (deps.store != nullptr) {
    auto lr = deps.store->latest_resume(aid, deps.translation);
    if (lr.has_value() && lr->has_value()) {
      const std::string& label = (*lr)->first;
      for (std::size_t i = 0; i < len; ++i) {
        if (episodes[i] == label) {
          resume_ix_ = i;
          cursor_ = i;
          break;
        }
      }
    }
  }

  // A flip/hop keeps the cursor on the in-progress episode (05 §10.5).
  if (remap_from_.has_value()) {
    auto [label, ordinal] = *remap_from_;
    remap_from_.reset();
    auto mapped = map_episode_index(episodes, label, ordinal);
    if (mapped.has_value()) cursor_ = *mapped;
  }

  episodes_ = std::move(episodes);
}

void EpisodeSession::refresh_meta(const EpisodeDeps& deps) {
  const std::int64_t aid = for_id_.value_or(0);
  pin_.reset();
  avail_.clear();
  if (deps.store == nullptr) return;
  if (auto p = deps.store->get_provider_pin(aid); p.has_value()) pin_ = *p;
  if (deps.registry != nullptr) {
    for (std::size_t i = 0; i < deps.registry->size(); ++i) {
      std::string_view name = deps.registry->at(i)->name();
      ProviderAvailability a = ProviderAvailability::Unchecked;
      if (auto r = deps.store->provider_availability(aid, name, deps.unix_now); r.has_value()) {
        a = *r;
      }
      avail_.emplace_back(std::string(name), a);
    }
  }
}

std::vector<Feedback> EpisodeSession::cycle_pin(const EpisodeDeps& deps) {
  if (!for_id_.has_value()) return {Feedback::simple(Feedback::Kind::PinNothing)};
  const std::int64_t aid = *for_id_;
  if (loading_) return {Feedback::simple(Feedback::Kind::PinPending)};
  if (!serving_.has_value()) return {Feedback::simple(Feedback::Kind::PinNothing)};
  if (deps.registry == nullptr) return {Feedback::simple(Feedback::Kind::PinNothing)};

  // Construction order.
  std::vector<std::string> order;
  for (std::size_t i = 0; i < deps.registry->size(); ++i) {
    order.emplace_back(deps.registry->at(i)->name());
  }

  // Compute the next pin: unpinned → first → next → … → unpinned (wrap).
  std::optional<std::string> next;
  if (!pin_.has_value()) {
    if (!order.empty()) next = order.front();
  } else {
    std::size_t at = order.size();
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == *pin_) { at = i; break; }
    }
    if (at < order.size() && at + 1 < order.size()) next = order[at + 1];
    // at == size (retired pin) or last → wrap to unpinned (next stays nullopt).
  }

  // Clear arm.
  if (!next.has_value()) {
    if (deps.store != nullptr && !deps.store->set_provider_pin(aid, std::nullopt).has_value()) {
      return {{Feedback::Kind::PinSaveFailed, {}, ProviderError::network(), true}};
    }
    pin_.reset();
    return {Feedback::simple(Feedback::Kind::PinCleared)};
  }

  // Set arm.
  const std::string target = *next;
  if (deps.store != nullptr &&
      !deps.store->set_provider_pin(aid, std::string_view(target)).has_value()) {
    return {{Feedback::Kind::PinSaveFailed, {}, ProviderError::network(), false}};
  }
  pin_ = target;

  // Already serving → pin written, no walk.
  if (serving_ == std::optional<std::string>(target)) {
    return {Feedback::with_provider(Feedback::Kind::PinSet, target)};
  }
  if (!canonical_.has_value()) {
    return {Feedback::with_provider(Feedback::Kind::PinSet, target)};
  }

  // Arm a pin_flip walk, keeping the cursor on the in-progress episode.
  if (cursor_ < episodes_.size()) {
    remap_from_ = std::make_pair(episodes_[cursor_], static_cast<std::uint32_t>(cursor_) + 1);
  }
  walk_provider_ = target;
  walk_ = resolve::Walk::pin_flip(*canonical_, target);
  return advance_walk(/*announce=*/true, deps);
}

std::vector<Feedback> EpisodeSession::play_fail_over(const std::vector<std::string>& tried,
                                                     std::string raw_label,
                                                     std::uint32_t ordinal,
                                                     const EpisodeDeps& deps) {
  // Single-flight: a live fetch/search or an existing walk owns the CDN budget.
  if (loading_ || walk_.has_value()) return {};
  if (!canonical_.has_value()) return {};
  WorldView world = world_of(deps);
  resolve::Walk walk = resolve::Walk::fallback(world, *canonical_, std::nullopt);
  for (const std::string& p : tried) walk.mark_tried(p);
  remap_from_ = std::make_pair(std::move(raw_label), ordinal);
  walk_provider_.reset();
  walk_ = std::move(walk);
  return advance_walk(/*announce=*/true, deps);
}

void EpisodeSession::on_play_recorded(std::int64_t anilist_id, std::uint32_t episode_ix,
                                      bool completed, const EpisodeDeps& deps) {
  if (for_id_ != std::optional<std::int64_t>(anilist_id) || episodes_.empty()) return;
  const std::int64_t aid = anilist_id;
  watched_ = 0;
  if (deps.store != nullptr) {
    auto p = deps.store->get_progress(aid);
    if (p.has_value() && p->has_value()) watched_ = **p;
  }
  resume_ix_.reset();
  if (deps.store != nullptr) {
    auto lr = deps.store->latest_resume(aid, deps.translation);
    if (lr.has_value() && lr->has_value()) {
      const std::string& label = (*lr)->first;
      for (std::size_t i = 0; i < episodes_.size(); ++i) {
        if (episodes_[i] == label) { resume_ix_ = i; break; }
      }
    }
  }
  const std::uint32_t played = episode_ix > 0 ? episode_ix - 1 : 0;
  if (completed && cursor_ == played) {
    const std::size_t len = episodes_.size();
    const std::size_t nxt = static_cast<std::size_t>(played) + 1;
    cursor_ = (nxt < len) ? nxt : (len > 0 ? len - 1 : 0);
  }
}

void EpisodeSession::on_availability_write(std::int64_t anilist_id, const EpisodeDeps& deps) {
  if (for_id_ == std::optional<std::int64_t>(anilist_id)) refresh_meta(deps);
}

}  // namespace shigoku::tui
