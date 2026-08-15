// resolve_tests.cpp — P15 DoD: resolve.rs tests ported 1:1. Pure, offline (§8):
// a FakeWorld canned view drives classify_open / open_history / route_preferred
// / the Walk state machine, pinning tier-major order, the pin asymmetries,
// stamp-before-fetch, the K-2 continuation, and the 64-provider bitmask ceiling.

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../src/resolve.hpp"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

using namespace shigoku;
using namespace shigoku::resolve;

namespace {

// FakeWorld (resolve.rs:514-596): a canned ResolveWorld built by chaining
// setters. REG is the construction order; pref reorders it if live.
class FakeWorld final : public ResolveWorld {
 public:
  explicit FakeWorld(std::vector<std::string> reg) : reg_(std::move(reg)) {}

  FakeWorld& bind(std::int64_t aid, std::string provider, std::string id) {
    bindings_[{aid, std::move(provider)}] = std::move(id);
    return *this;
  }
  FakeWorld& absent(std::int64_t aid, std::string provider) {
    absent_.insert({aid, std::move(provider)});
    return *this;
  }
  FakeWorld& key(std::string provider, std::int64_t aid, std::string k) {
    keys_[{aid, std::move(provider)}] = std::move(k);
    return *this;
  }
  FakeWorld& pin(std::int64_t aid, std::string provider) {
    pins_[aid] = std::move(provider);
    return *this;
  }
  FakeWorld& route(std::int64_t aid, std::string provider) {
    routes_[aid] = std::move(provider);
    return *this;
  }
  FakeWorld& global(std::string p) {
    global_ = std::move(p);
    return *this;
  }
  // Retire a provider (present at binding-time, no longer in the live registry).
  FakeWorld& retire(std::string provider) {
    retired_.insert(std::move(provider));
    return *this;
  }

  std::vector<std::string> ordered(std::optional<std::string_view> pref) const override {
    std::vector<std::string> out;
    if (pref.has_value() && registered(*pref)) out.emplace_back(*pref);
    for (const std::string& p : reg_) {
      if (retired_.count(p)) continue;
      if (pref.has_value() && p == *pref) continue;
      out.push_back(p);
    }
    return out;
  }
  bool registered(std::string_view provider) const override {
    if (retired_.count(std::string(provider))) return false;
    return std::find(reg_.begin(), reg_.end(), provider) != reg_.end();
  }
  std::optional<std::string> binding(std::int64_t aid, std::string_view p) const override {
    auto it = bindings_.find({aid, std::string(p)});
    if (it == bindings_.end()) return std::nullopt;
    return it->second;
  }
  bool absent_fresh(std::int64_t aid, std::string_view p) const override {
    return absent_.count({aid, std::string(p)}) != 0;
  }
  std::optional<std::string> canonical_key(std::string_view p, const Enrichment& c) const override {
    auto it = keys_.find({c.anilist_id, std::string(p)});
    if (it == keys_.end()) return std::nullopt;
    return it->second;
  }
  std::optional<std::string> pin(std::int64_t aid) const override {
    auto it = pins_.find(aid);
    if (it == pins_.end()) return std::nullopt;
    return it->second;
  }
  std::optional<std::string> route_pref(std::int64_t aid) const override {
    auto it = routes_.find(aid);
    if (it == routes_.end()) return std::nullopt;
    return it->second;
  }
  std::string global_pref() const override { return global_; }

 private:
  std::vector<std::string> reg_;
  std::map<std::pair<std::int64_t, std::string>, std::string> bindings_;
  std::map<std::pair<std::int64_t, std::string>, std::string> keys_;
  std::set<std::pair<std::int64_t, std::string>> absent_;
  std::map<std::int64_t, std::string> pins_;
  std::map<std::int64_t, std::string> routes_;
  std::set<std::string> retired_;
  std::string global_;
};

// canon(aid): anilist_id=aid, title "canon", rest default (resolve.rs:598).
Enrichment canon(std::int64_t aid) {
  Enrichment e;
  e.anilist_id = aid;
  e.title_romaji = "canon";
  return e;
}

const std::vector<std::string> REG = {"megaplay", "senshi", "allanime"};

// Drive a walk to completion collecting hops; returns the terminal Exhausted
// kind and, for Continue, drives the continuation too (one level).
std::vector<Hop> drain(const ResolveWorld& world, Walk w, Exhausted::Kind& out_kind) {
  std::vector<Hop> hops;
  Exhausted ex{Exhausted::Kind::DeadEnd, nullptr};
  for (;;) {
    auto hop = w.advance(world, ex);
    if (hop.has_value()) {
      hops.push_back(*hop);
      continue;
    }
    out_kind = ex.kind;
    if (ex.kind == Exhausted::Kind::Continue) {
      w = std::move(*ex.next);
      continue;  // drive the continuation in the same loop.
    }
    return hops;
  }
}

}  // namespace

// --- Path 1: classify_open -------------------------------------------------

TEST_CASE("classify is tier-major: binding beats earlier key (resolve.rs:609)") {
  FakeWorld w(REG);
  w.key("megaplay", 1, "mk").bind(1, "allanime", "abound");
  auto t = classify_open(w, canon(1));
  CHECK(t.kind == ResolveTarget::Kind::Bound);
  CHECK(t.provider == "allanime");
  CHECK(t.id == "abound");
}

TEST_CASE("classify path1 non-pinned binding beats unbound pin (resolve.rs:626)") {
  FakeWorld w(REG);
  w.pin(1, "megaplay").bind(1, "senshi", "sbound");  // pin unbound, senshi bound.
  auto t = classify_open(w, canon(1));
  CHECK(t.kind == ResolveTarget::Kind::Bound);
  CHECK(t.provider == "senshi");  // any binding wins tier 0; pin only reorders.
}

TEST_CASE("classify tier-A uses effective order then needs-search (resolve.rs:643)") {
  {
    FakeWorld w(REG);
    w.pin(1, "allanime").key("megaplay", 1, "mk").key("allanime", 1, "ak");
    auto t = classify_open(w, canon(1));
    CHECK(t.kind == ResolveTarget::Kind::TierA);
    CHECK(t.provider == "allanime");  // pinned pref leads the tier-A tie.
  }
  {
    FakeWorld w(REG);
    w.key("megaplay", 1, "mk").key("allanime", 1, "ak");
    auto t = classify_open(w, canon(1));
    CHECK(t.kind == ResolveTarget::Kind::TierA);
    CHECK(t.provider == "megaplay");  // no pref: construction order.
  }
  {
    FakeWorld w(REG);
    auto t = classify_open(w, canon(1));
    CHECK(t.kind == ResolveTarget::Kind::NeedsSearch);  // nothing anywhere.
  }
}

// --- Path 2: open_history --------------------------------------------------

TEST_CASE("history pin opens only its own binding (resolve.rs:695)") {
  FakeWorld w(REG);
  w.pin(1, "allanime").bind(1, "allanime", "apin").bind(1, "senshi", "ssrc");
  Enrichment c = canon(1);
  HistoryRecord rec{1, "senshi", "ssrc", &c};
  auto o = open_history(w, rec);
  CHECK(o.kind == HistoryOpen::Kind::PinBinding);
  CHECK(o.provider == "allanime");
  CHECK(o.id == "apin");
}

TEST_CASE("history unbound pin never borrows falls through to record (resolve.rs:710)") {
  FakeWorld w(REG);
  w.pin(1, "allanime").bind(1, "senshi", "ssrc").global("megaplay").route(1, "megaplay");
  Enrichment c = canon(1);
  HistoryRecord rec{1, "senshi", "ssrc", &c};
  auto o = open_history(w, rec);
  CHECK(o.kind == HistoryOpen::Kind::Record);  // pin set but unbound: no borrow,
  CHECK(o.provider == "senshi");               // and the re-route is skipped.
}

TEST_CASE("history pin equal-to-source or retired falls through (resolve.rs:730)") {
  {
    FakeWorld w(REG);
    w.pin(1, "senshi").bind(1, "senshi", "ssrc");
    Enrichment c = canon(1);
    HistoryRecord rec{1, "senshi", "ssrc", &c};
    auto o = open_history(w, rec);
    CHECK(o.kind == HistoryOpen::Kind::Record);  // pin==source: no re-route.
    CHECK(o.provider == "senshi");
  }
  {
    FakeWorld w(REG);
    w.pin(1, "retired").retire("retired").bind(1, "senshi", "ssrc");
    Enrichment c = canon(1);
    HistoryRecord rec{1, "senshi", "ssrc", &c};
    auto o = open_history(w, rec);
    CHECK(o.kind == HistoryOpen::Kind::Record);  // retired pin: no foreign fetch.
    CHECK(o.provider == "senshi");
  }
}

TEST_CASE("history unpinned routes before the record (resolve.rs:755)") {
  FakeWorld w(REG);
  w.global("senshi").bind(1, "senshi", "sbound");  // stale/never settled → force.
  Enrichment c = canon(1);
  HistoryRecord rec{1, "megaplay", "msrc", &c};
  auto o = open_history(w, rec);
  REQUIRE(o.kind == HistoryOpen::Kind::Routed);
  REQUIRE(o.routed != nullptr);
  CHECK(o.routed->stamp == std::optional<std::string>("senshi"));
  CHECK(o.routed->action.kind == RouteAction::Kind::OpenBinding);
  CHECK(o.routed->action.provider == "senshi");
}

// --- route_preferred (ROD-398) ---------------------------------------------

TEST_CASE("route pin and empty pref are no-ops (resolve.rs:778)") {
  {
    FakeWorld w(REG);
    w.global("");  // empty pref.
    auto ro = route_preferred(w, canon(1));
    CHECK(ro.action.kind == RouteAction::Kind::None);
    CHECK(!ro.stamp.has_value());
  }
  {
    FakeWorld w(REG);
    w.global("senshi").pin(1, "megaplay");  // pin set → route no-op.
    auto ro = route_preferred(w, canon(1));
    CHECK(ro.action.kind == RouteAction::Kind::None);
  }
}

TEST_CASE("route settled opens binding without restamp (resolve.rs:789)") {
  {
    FakeWorld w(REG);
    w.global("senshi").route(1, "senshi").bind(1, "senshi", "sbound");
    auto ro = route_preferred(w, canon(1));
    CHECK(ro.action.kind == RouteAction::Kind::OpenBinding);
    CHECK(ro.action.provider == "senshi");
    CHECK(!ro.stamp.has_value());  // settled: no re-stamp.
  }
  {
    FakeWorld w(REG);
    w.global("senshi").route(1, "senshi");  // settled but unbound.
    auto ro = route_preferred(w, canon(1));
    CHECK(ro.action.kind == RouteAction::Kind::None);
    CHECK(!ro.stamp.has_value());
  }
}

TEST_CASE("route stale forces once and stamps before fetch (resolve.rs:811)") {
  FakeWorld w(REG);
  w.global("senshi").key("senshi", 1, "sk");  // stale (no route), keyed.
  auto ro = route_preferred(w, canon(1));
  CHECK(ro.action.kind == RouteAction::Kind::FetchTierA);
  CHECK(ro.action.provider == "senshi");
  CHECK(ro.stamp == std::optional<std::string>("senshi"));  // stamp present.
}

TEST_CASE("route stale retired pref does not force (resolve.rs:835)") {
  FakeWorld w(REG);
  w.global("retired").retire("retired");
  auto ro = route_preferred(w, canon(1));
  CHECK(ro.action.kind == RouteAction::Kind::None);  // mis-key guard.
  CHECK(!ro.stamp.has_value());
}

TEST_CASE("route stamp before fetch breaks the loop (resolve.rs:842)") {
  FakeWorld w(REG);
  w.global("senshi").key("senshi", 1, "sk");
  auto ro = route_preferred(w, canon(1));
  REQUIRE(ro.stamp == std::optional<std::string>("senshi"));
  // Transport applies the stamp: route_pref := senshi. Second open reads
  // non-stale → None. The loop cannot recur.
  w.route(1, "senshi");
  auto ro2 = route_preferred(w, canon(1));
  CHECK(ro2.action.kind == RouteAction::Kind::None);
}

// --- Walk + K-2 law --------------------------------------------------------

TEST_CASE("fallback walk is provider-major: binding→absence→key→search (resolve.rs:862)") {
  FakeWorld w(REG);
  // megaplay failed (pre-tried); senshi absent-fresh (skipped); allanime keyed.
  w.absent(1, "senshi").key("allanime", 1, "aa-key");
  Exhausted::Kind kind = Exhausted::Kind::DeadEnd;
  auto hops = drain(w, Walk::fallback(w, canon(1), "megaplay"), kind);
  REQUIRE(hops.size() == 1);
  CHECK(hops[0].kind == Hop::Kind::Fetch);
  CHECK(hops[0].provider == "allanime");
  CHECK(hops[0].bind == std::optional<std::int64_t>(1));  // tier-A mints.
  CHECK(kind == Exhausted::Kind::DeadEnd);
}

TEST_CASE("mark_tried skips extra providers for a continuation walk (resolve.rs:883)") {
  FakeWorld w(REG);
  w.key("senshi", 1, "s-key");
  Walk walk = Walk::fallback(w, canon(1), "megaplay");  // megaplay pre-tried.
  walk.mark_tried("senshi");
  walk.mark_tried("not-registered");  // unregistered name: a no-op, never throws.
  Exhausted::Kind kind = Exhausted::Kind::DeadEnd;
  auto hops = drain(w, std::move(walk), kind);
  REQUIRE(hops.size() == 1);
  CHECK(hops[0].kind == Hop::Kind::Search);  // straight to allanime search.
  CHECK(hops[0].provider == "allanime");
}

TEST_CASE("fallback respects fresh absence (non-manual) (resolve.rs:902)") {
  FakeWorld w(REG);
  // senshi could answer via a key, but it is absent-fresh; ordinary fallback
  // respects that and searches allanime instead.
  w.absent(1, "senshi").key("senshi", 1, "s-key");
  Exhausted::Kind kind = Exhausted::Kind::DeadEnd;
  auto hops = drain(w, Walk::fallback(w, canon(1), "megaplay"), kind);
  REQUIRE(hops.size() == 1);
  CHECK(hops[0].kind == Hop::Kind::Search);
  CHECK(hops[0].provider == "allanime");  // senshi's key skipped by its absence.
  CHECK(kind == Exhausted::Kind::DeadEnd);
}

TEST_CASE("pin_flip probes through absence and keeps pin on miss (resolve.rs:921)") {
  FakeWorld w(REG);
  w.absent(1, "senshi");  // fresh absence, but pin_flip is manual.
  Exhausted::Kind kind = Exhausted::Kind::DeadEnd;
  auto hops = drain(w, Walk::pin_flip(canon(1), "senshi"), kind);
  REQUIRE(hops.size() == 1);
  CHECK(hops[0].kind == Hop::Kind::Search);  // probes THROUGH the absence.
  CHECK(hops[0].provider == "senshi");
  CHECK(kind == Exhausted::Kind::PinKept);  // miss → keep pin, no borrow.
}

TEST_CASE("forced_preferred miss triggers K-2 continuation not dead-end (resolve.rs:938)") {
  FakeWorld w(REG);
  // senshi is the forced pref, search-only (miss); allanime has a binding.
  w.global("senshi").bind(1, "allanime", "abound");
  auto ro = route_preferred(w, canon(1));
  REQUIRE(ro.action.kind == RouteAction::Kind::Walk);
  REQUIRE(ro.action.walk != nullptr);
  Exhausted::Kind kind = Exhausted::Kind::DeadEnd;
  auto hops = drain(w, std::move(*ro.action.walk), kind);
  // First: senshi search (forced miss); then the continuation's bindings-first
  // sweep finds allanime though senshi leads and is pre-tried.
  REQUIRE(hops.size() >= 2);
  CHECK(hops.front().kind == Hop::Kind::Search);
  CHECK(hops.front().provider == "senshi");
  bool found_allanime_bound = false;
  for (const Hop& h : hops) {
    if (h.provider == "allanime" && h.kind == Hop::Kind::Fetch && !h.bind.has_value()) {
      found_allanime_bound = true;
    }
  }
  CHECK(found_allanime_bound);
}

TEST_CASE("K-2 continuation skips already-tried pref and respects absence (resolve.rs:976)") {
  FakeWorld w(REG);
  // senshi forced (miss); megaplay fresh-absent; allanime keyed.
  w.global("senshi").absent(1, "megaplay").key("allanime", 1, "ak");
  auto ro = route_preferred(w, canon(1));
  REQUIRE(ro.action.kind == RouteAction::Kind::Walk);
  Exhausted::Kind kind = Exhausted::Kind::DeadEnd;
  auto hops = drain(w, std::move(*ro.action.walk), kind);
  // senshi search, then continuation: megaplay skipped (absent, non-manual),
  // allanime keyed; then DeadEnd.
  bool allanime_keyed = false;
  bool megaplay_probed = false;
  for (const Hop& h : hops) {
    if (h.provider == "allanime" && h.bind == std::optional<std::int64_t>(1)) allanime_keyed = true;
    if (h.provider == "megaplay") megaplay_probed = true;
  }
  CHECK(allanime_keyed);
  CHECK(!megaplay_probed);
  CHECK(kind == Exhausted::Kind::DeadEnd);
}

TEST_CASE("walk snapshot is stable across a pref change mid-walk (resolve.rs:1011)") {
  FakeWorld w(REG);
  w.key("megaplay", 1, "mp").key("allanime", 1, "aa");
  Walk walk = Walk::fallback(w, canon(1), std::nullopt);  // snapshot = construction order.
  Exhausted ex{Exhausted::Kind::DeadEnd, nullptr};
  auto h0 = walk.advance(w, ex);
  REQUIRE(h0.has_value());
  CHECK(h0->kind == Hop::Kind::Fetch);
  CHECK(h0->provider == "megaplay");  // construction order, no pref.
  // Even if the world now prefers allanime (via a pin), the walk keeps its
  // snapshot: next is senshi (search, no key), then allanime.
  FakeWorld w2 = w;
  w2.pin(1, "allanime");
  auto h1 = walk.advance(w2, ex);
  REQUIRE(h1.has_value());
  CHECK(h1->kind == Hop::Kind::Search);
  CHECK(h1->provider == "senshi");  // still construction order, not reshuffled.
}

TEST_CASE("walk bitmask is overflow-safe at the registry ceiling (resolve.rs:1046)") {
  // p0 is pre-tried; the rest search-miss (no keys). Walking the full 64-
  // provider ceiling must never overflow the 1<<idx shift (ROD-436).
  std::vector<std::string> big;
  for (std::size_t i = 0; i < kMaxTrackedProviders; ++i) big.push_back("p" + std::to_string(i));
  FakeWorld w(big);
  Exhausted::Kind kind = Exhausted::Kind::DeadEnd;
  auto hops = drain(w, Walk::fallback(w, canon(1), "p0"), kind);
  CHECK(hops.size() == kMaxTrackedProviders - 1);  // all but the pre-tried p0.
  CHECK(kind == Exhausted::Kind::DeadEnd);
}
