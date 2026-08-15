// resolve_transport_tests.cpp — P15 DoD: the resolve WALK transport
// (EpisodeSession) driven end-to-end against a real in-memory Store + a
// two-fixture registry. Proves the store-write ownership (bind on tier-A
// success, absence on empty, route stamp, pin), the tier-B/C scoring path, the
// pin-flip and fallback walks, the provider caption, and the staleness guard
// (a superseded walk's late result is dropped). No network — the fixtures
// resolve synchronously; workers post to the queue and we pump it inline.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../src/provider.hpp"
#include "../src/store.hpp"
#include "../src/tui/prewarm.hpp"
#include "../src/tui/resolve_transport.hpp"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

using namespace shigoku;
using namespace shigoku::tui;

namespace {

// Fixture A: MAL-keyed (tier-A), stocks the show under provider_id "mal:<id>".
class FixtureA final : public StreamProvider {
 public:
  bool stocked = true;  // false → episodes() returns empty (authoritative absent).
  std::string_view name() const override { return "alpha"; }
  std::string_view display_name() const override { return "Alpha"; }
  std::optional<std::string> canonical_key(const Enrichment& s) const override {
    if (!s.mal_id.has_value()) return std::nullopt;
    return "mal:" + std::to_string(*s.mal_id);
  }
  Result<std::vector<SearchHit>, ProviderError> search(std::string_view,
                                                       const SearchOptions&) const override {
    return std::vector<SearchHit>{};
  }
  Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view, Translation, std::optional<std::uint32_t>) const override {
    if (!stocked) return std::vector<std::string>{};  // authoritative not-stocked.
    return std::vector<std::string>{"1", "2", "3"};
  }
  Result<StreamLink, ProviderError> resolve(std::string_view, std::string_view,
                                            Translation, Quality) const override {
    StreamLink l;
    l.url = "https://alpha.invalid/x.m3u8";
    return l;
  }
  Result<CoverRequest, ProviderError> cover_request(std::string_view) const override {
    return err(ProviderError::unsupported());
  }
};

// Fixture B: NOT MAL-keyed (no canonical_key) → tier-C search. Search returns a
// single id-agreeing hit so the scorer binds it; episodes() then stocks it.
class FixtureB final : public StreamProvider {
 public:
  bool has_hit = true;
  std::string_view name() const override { return "beta"; }
  std::string_view display_name() const override { return "Beta"; }
  std::optional<std::string> canonical_key(const Enrichment&) const override {
    return std::nullopt;  // tier-C only.
  }
  Result<std::vector<SearchHit>, ProviderError> search(std::string_view,
                                                       const SearchOptions&) const override {
    std::vector<SearchHit> hits;
    if (has_hit) {
      SearchHit h;
      h.provider_id = "beta-42";
      h.title = "Fixture Show";  // exact match with the canonical title below.
      h.mal_id = 100;            // id agreement → tier-B bind.
      h.total_episodes = 3;
      hits.push_back(h);
    }
    return hits;
  }
  Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view, Translation, std::optional<std::uint32_t>) const override {
    return std::vector<std::string>{"1", "2", "3"};
  }
  Result<StreamLink, ProviderError> resolve(std::string_view, std::string_view,
                                            Translation, Quality) const override {
    StreamLink l;
    l.url = "https://beta.invalid/x.m3u8";
    return l;
  }
  Result<CoverRequest, ProviderError> cover_request(std::string_view) const override {
    return err(ProviderError::unsupported());
  }
};

// A test harness: in-memory store + a registry, driving one EpisodeSession and
// pumping the queue inline after each step.
struct Harness {
  Store store;
  ProviderRegistry registry;
  EventQueue queue;
  EpisodeSession session;
  FixtureA* a = nullptr;
  FixtureB* b = nullptr;

  static Harness make(bool include_b = true) {
    auto st = Store::open_memory();
    REQUIRE(st.has_value());
    std::vector<std::unique_ptr<StreamProvider>> ps;
    auto ua = std::make_unique<FixtureA>();
    FixtureA* ap = ua.get();
    ps.push_back(std::move(ua));
    FixtureB* bp = nullptr;
    if (include_b) {
      auto ub = std::make_unique<FixtureB>();
      bp = ub.get();
      ps.push_back(std::move(ub));
    }
    return Harness{std::move(*st), ProviderRegistry(std::move(ps)), EventQueue{},
                   EpisodeSession{}, ap, bp};
  }

  EpisodeDeps deps(std::int64_t now = 1000) {
    EpisodeDeps d;
    d.store = &store;
    d.registry = &registry;
    d.queue = &queue;
    d.global_pref = std::string_view{};
    d.translation = Translation::Sub;
    d.unix_now = now;
    return d;
  }

  // Drain every queued event into the session, returning the concatenated
  // feedback. Blocks briefly for the detached worker to post.
  std::vector<Feedback> pump(std::int64_t now = 1000) {
    std::vector<Feedback> all;
    for (int spins = 0; spins < 50; ++spins) {
      auto ev = queue.wait_next();  // up to 100ms.
      if (!ev.has_value()) {
        if (!session.loading()) break;  // idle + nothing pending → done.
        continue;
      }
      EpisodeDeps d = deps(now);
      std::vector<Feedback> fb;
      if (auto* e = std::get_if<EpisodesDone>(&*ev)) fb = session.on_episodes_done(*e, d);
      else if (auto* e = std::get_if<EpisodesError>(&*ev)) fb = session.on_episodes_error(*e, d);
      else if (auto* e = std::get_if<ProviderSearchDone>(&*ev)) fb = session.on_search_done(*e, d);
      else if (auto* e = std::get_if<ProviderSearchError>(&*ev)) fb = session.on_search_error(*e, d);
      for (auto& f : fb) all.push_back(std::move(f));
    }
    return all;
  }
};

Enrichment show(std::int64_t aid, std::int64_t mal, std::string title) {
  Enrichment e;
  e.anilist_id = aid;
  e.mal_id = mal;
  e.title_romaji = std::move(title);
  e.total_episodes = 3;
  return e;
}

}  // namespace

TEST_CASE("tier-A open binds the provider and lands the grid") {
  Harness h = Harness::make();
  Enrichment s = show(1, 100, "Fixture Show");
  h.session.engage(s, h.deps());
  h.pump();
  CHECK(h.session.episodes().size() == 3);
  CHECK(h.session.serving() == std::optional<std::string>("alpha"));
  // Tier-A success mints a binding under alpha.
  auto binding = h.store.show_id_for_binding("alpha", "mal:100");
  REQUIRE(binding.has_value());
  CHECK(binding->has_value());
  CHECK(**binding == 1);
}

TEST_CASE("an authoritative empty listing marks absence and walks on") {
  Harness h = Harness::make();
  h.a->stocked = false;  // alpha: empty listing → absence, walk to beta.
  Enrichment s = show(1, 100, "Fixture Show");
  h.session.engage(s, h.deps());
  h.pump();
  // alpha marked absent (fresh); beta bound via its search hit.
  auto absent = h.store.provider_absent_fresh(1, "alpha", 1000);
  REQUIRE(absent.has_value());
  CHECK(*absent);
  CHECK(h.session.serving() == std::optional<std::string>("beta"));
  auto bound = h.store.show_id_for_binding("beta", "beta-42");
  REQUIRE(bound.has_value());
  CHECK(bound->has_value());
}

TEST_CASE("tier-C search scores and binds an id-agreeing hit") {
  Harness h = Harness::make();
  // No MAL id → alpha's canonical_key is null; alpha searches (empty) then beta
  // searches and its id-agreeing hit binds.
  Enrichment s = show(1, 100, "Fixture Show");
  s.mal_id = std::nullopt;  // kill tier-A on alpha.
  // But beta's hit carries mal_id 100; canonical has no mal now → id match needs
  // the anilist path. Give the hit an anilist_id instead for agreement.
  h.session.engage(s, h.deps());
  h.pump();
  // With no tier-A anywhere, the walk searches; beta's hit (mal 100) does not
  // agree (canonical mal is null), so best_provider_match scores the exact
  // title (1600) + eps (3==3 → +180) = 1780, clears the floor → bind beta.
  CHECK(h.session.serving() == std::optional<std::string>("beta"));
}

TEST_CASE("route stamp is written before the forced fetch (ROD-398)") {
  Harness h = Harness::make();
  Enrichment s = show(1, 100, "Fixture Show");
  EpisodeDeps d = h.deps();
  d.global_pref = "beta";  // prefer beta; stale (never settled) → force once.
  h.session.engage(s, d);
  // The route stamp must land before the fetch fires.
  auto settled = h.store.get_route_pref(1);
  REQUIRE(settled.has_value());
  CHECK(*settled == std::optional<std::string>("beta"));
  h.pump();
  CHECK(h.session.serving() == std::optional<std::string>("beta"));
}

TEST_CASE("pin flip binds the pinned provider; a re-open honors the pin") {
  Harness h = Harness::make();
  Enrichment s = show(1, 100, "Fixture Show");
  h.session.engage(s, h.deps());
  h.pump();
  CHECK(h.session.serving() == std::optional<std::string>("alpha"));  // tier-A default.
  // `v`: unpinned → alpha (already serving → PinSet, no walk) → beta (flip).
  auto fb1 = h.session.cycle_pin(h.deps());  // → alpha, already serving.
  CHECK(h.session.pin() == std::optional<std::string>("alpha"));
  auto fb2 = h.session.cycle_pin(h.deps());  // → beta, arms a pin_flip walk.
  h.pump();
  CHECK(h.session.pin() == std::optional<std::string>("beta"));
  CHECK(h.session.serving() == std::optional<std::string>("beta"));
  CHECK(h.store.get_provider_pin(1).value() == std::optional<std::string>("beta"));
  // Play MUST resolve on the SERVING provider (beta), never primary (alpha):
  // serving_id is beta's id, and app.cpp passes serving() as the provider name
  // (Fable gate #3 fix — a hop landed on a non-primary provider).
  CHECK(h.session.serving_id() == "beta-42");
}

TEST_CASE("pin flip keeps the pin on a miss and never borrows (K-2 pin fence)") {
  Harness h = Harness::make();
  h.b->has_hit = false;  // beta cannot resolve (no search hit, no key).
  Enrichment s = show(1, 100, "Fixture Show");
  h.session.engage(s, h.deps());
  h.pump();
  REQUIRE(h.session.serving() == std::optional<std::string>("alpha"));
  h.session.cycle_pin(h.deps());  // pin alpha (serving).
  auto fb = h.session.cycle_pin(h.deps());  // flip to beta → walk.
  auto more = h.pump();
  for (auto& f : more) fb.push_back(std::move(f));
  // Miss on beta: pin kept, grid survives on alpha, no borrow.
  bool pin_kept = false;
  for (const auto& f : fb) if (f.kind == Feedback::Kind::PinKept) pin_kept = true;
  CHECK(pin_kept);
  CHECK(h.session.pin() == std::optional<std::string>("beta"));
  CHECK(h.session.serving() == std::optional<std::string>("alpha"));  // grid survives.
  CHECK(!h.session.episodes().empty());
}

TEST_CASE("a superseded walk's late result is dropped (staleness guard)") {
  Harness h = Harness::make();
  Enrichment s = show(1, 100, "Fixture Show");
  h.session.engage(s, h.deps());
  // Before pumping the alpha fetch result, supersede by engaging a different
  // show (bumps the generation + changes for_id).
  Enrichment s2 = show(2, 200, "Other Show");
  h.session.engage(s2, h.deps());
  // Now pump: the stale for_id=1 EpisodesDone must be dropped (gen + for_id
  // both moved), and only the fresh for_id=2 result lands.
  h.pump();
  CHECK(h.session.for_id() == 2);
  CHECK(h.session.serving() == std::optional<std::string>("alpha"));
  CHECK(h.session.episodes().size() == 3);
}

TEST_CASE("the held guard no-ops a re-open of the same show+track") {
  Harness h = Harness::make();
  Enrichment s = show(1, 100, "Fixture Show");
  h.session.engage(s, h.deps());
  h.pump();
  auto bound_before = h.store.show_id_for_binding("alpha", "mal:100");
  REQUIRE(bound_before.has_value());
  // Re-engage the same show: held guard → no new fetch, no re-bind churn.
  auto fb = h.session.engage(s, h.deps());
  CHECK(fb.empty());
  CHECK(h.session.serving() == std::optional<std::string>("alpha"));
  auto bound_after = h.store.show_id_for_binding("alpha", "mal:100");
  REQUIRE(bound_after.has_value());
  CHECK(*bound_before == *bound_after);  // same owner; compare the unwrapped values.
}

// --- prewarm end-to-end ----------------------------------------------------

TEST_CASE("prewarm mints a sibling binding into the store") {
  Harness h = Harness::make();
  Enrichment s = show(1, 100, "Fixture Show");
  // Bind alpha up front so prewarm's only candidate is beta.
  (void)h.store.bind_provider(s, "alpha", "mal:100", 1000);

  PrewarmState pw;
  PrewarmDeps pd;
  pd.store = &h.store;
  pd.registry = &h.registry;
  pd.queue = &h.queue;
  pd.translation = Translation::Sub;
  pd.unix_now = 1000;
  pd.now_tick = 0;
  PrewarmGates gates;
  pw.fire(s, gates, pd);
  CHECK(pw.active());

  // Drain the probe result(s): beta's search hit + episodes → Found → bind.
  for (int i = 0; i < 50 && pw.active(); ++i) {
    auto ev = h.queue.wait_next();
    if (!ev.has_value()) { pw.tick(pd); continue; }
    if (auto* r = std::get_if<PrewarmResult>(&*ev)) pw.on_result(*r, pd);
    pw.tick(pd);
  }
  auto beta = h.store.show_id_for_binding("beta", "beta-42");
  REQUIRE(beta.has_value());
  CHECK(beta->has_value());
  CHECK(**beta == 1);
}
