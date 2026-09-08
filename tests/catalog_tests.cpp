// catalog_tests.cpp — the catalog dispatcher offline: the FailoverState latch
// table, and Catalog over two fake backends (who gets called, in which
// order, what the composed error copy says, the synthetic-id routing). No
// network, no fixtures.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../src/catalog.hpp"

using namespace shigoku;
using namespace shigoku::catalog;

namespace {

struct Fake {
  Source tag = Source::AniList;
  bool ok = true;
  ProviderError fail = ProviderError::network();
  std::optional<ProviderError> enrich_err;  // overrides `fail` for enrich/char_recs
  int search_calls = 0;
  int discover_calls = 0;
  int enrich_calls = 0;
  int char_calls = 0;
  int genre_calls = 0;
  std::optional<std::int64_t> last_enrich_id;
  std::optional<std::int64_t> last_enrich_mal;
};

Backend make(Fake& f) {
  Backend b;
  b.search = [&f](std::string_view, std::uint32_t) -> Result<Page, ProviderError> {
    ++f.search_calls;
    if (!f.ok) return err(f.fail);
    Page p;
    p.source = f.tag;
    Enrichment e;
    e.anilist_id = f.tag == Source::AniList ? 1 : -7;
    p.entries.push_back(e);
    return p;
  };
  b.discover = [&f](DiscoverAxis, std::uint32_t, const DiscoverFilters&)
      -> Result<Page, ProviderError> {
    ++f.discover_calls;
    if (!f.ok) return err(f.fail);
    Page p;
    p.source = f.tag;
    return p;
  };
  b.enrich = [&f](std::int64_t id, std::optional<std::int64_t> mal)
      -> Result<std::optional<Enrichment>, ProviderError> {
    ++f.enrich_calls;
    f.last_enrich_id = id;
    f.last_enrich_mal = mal;
    if (f.enrich_err.has_value()) return err(*f.enrich_err);
    if (!f.ok) return err(f.fail);
    Enrichment e;
    e.anilist_id = id;
    e.mal_id = mal;
    return std::optional<Enrichment>(e);
  };
  b.char_recs = [&f](std::int64_t, std::optional<std::int64_t>)
      -> Result<std::optional<CharactersAndRecommendations>, ProviderError> {
    ++f.char_calls;
    if (f.enrich_err.has_value()) return err(*f.enrich_err);
    if (!f.ok) return err(f.fail);
    return std::optional<CharactersAndRecommendations>(CharactersAndRecommendations{});
  };
  b.genres = [&f]() -> Result<std::vector<std::string>, ProviderError> {
    ++f.genre_calls;
    if (!f.ok) return err(f.fail);
    return std::vector<std::string>{f.tag == Source::AniList ? "Action" : "Suspense"};
  };
  return b;
}

struct Rig {
  Fake anilist;
  Fake mal;
  std::int64_t now = 1000;
  std::optional<Catalog> cat;

  explicit Rig(Mode mode, bool with_mal = true) {
    anilist.tag = Source::AniList;
    mal.tag = Source::Mal;
    std::optional<Backend> mb;
    if (with_mal) mb = make(mal);
    cat.emplace(mode, make(anilist), std::move(mb), [this]() { return now; });
  }
};

}  // namespace

// ===========================================================================
// FailoverState
// ===========================================================================

TEST_CASE("FailoverState: auto mode latches MAL on an AniList failure, probes again later") {
  FailoverState st;
  st.mode = Mode::Auto;
  st.mal_available = true;
  CHECK(st.first(0) == Source::AniList);
  CHECK(st.fallback_for(Source::AniList) == Source::Mal);
  CHECK(st.fallback_for(Source::Mal) == Source::AniList);

  st.on_result(Source::AniList, false, 100);
  CHECK(st.active == Source::Mal);
  CHECK(st.anilist_retry_at == 100 + FailoverState::kRetryAfterSecs);
  CHECK(st.first(101) == Source::Mal);
  CHECK(st.first(100 + FailoverState::kRetryAfterSecs - 1) == Source::Mal);
  CHECK(st.first(100 + FailoverState::kRetryAfterSecs) == Source::AniList);  // the probe

  // MAL outcomes never move the latch.
  st.on_result(Source::Mal, false, 200);
  CHECK(st.active == Source::Mal);
  st.on_result(Source::Mal, true, 200);
  CHECK(st.active == Source::Mal);

  // A failed probe re-latches with a fresh window.
  st.on_result(Source::AniList, false, 900);
  CHECK(st.anilist_retry_at == 900 + FailoverState::kRetryAfterSecs);

  // AniList back: latch cleared.
  st.on_result(Source::AniList, true, 2000);
  CHECK(st.active == Source::AniList);
  CHECK_FALSE(st.anilist_retry_at.has_value());
  CHECK(st.first(2001) == Source::AniList);
}

TEST_CASE("FailoverState: auto without MAL never latches or falls through") {
  FailoverState st;
  st.mode = Mode::Auto;
  st.mal_available = false;
  CHECK_FALSE(st.fallback_for(Source::AniList).has_value());
  st.on_result(Source::AniList, false, 100);
  CHECK(st.active == Source::AniList);
  CHECK_FALSE(st.anilist_retry_at.has_value());
  CHECK(st.first(101) == Source::AniList);
}

TEST_CASE("FailoverState: fixed modes never fall through") {
  FailoverState a;
  a.mode = Mode::AniList;
  a.mal_available = true;
  CHECK(a.first(0) == Source::AniList);
  CHECK_FALSE(a.fallback_for(Source::AniList).has_value());
  a.on_result(Source::AniList, false, 0);
  CHECK(a.active == Source::AniList);  // no latch in a fixed mode
  CHECK(a.first(1) == Source::AniList);

  FailoverState m;
  m.mode = Mode::Mal;
  m.mal_available = true;
  CHECK(m.first(0) == Source::Mal);
  CHECK_FALSE(m.fallback_for(Source::Mal).has_value());
}

TEST_CASE("parse_mode / mode_str") {
  CHECK(parse_mode("auto") == Mode::Auto);
  CHECK(parse_mode("anilist") == Mode::AniList);
  CHECK(parse_mode("mal") == Mode::Mal);
  CHECK(parse_mode("jikan") == Mode::Auto);
  CHECK(parse_mode("") == Mode::Auto);
  CHECK(mode_str(Mode::Auto) == "auto");
  CHECK(mode_str(Mode::AniList) == "anilist");
  CHECK(mode_str(Mode::Mal) == "mal");
}

// ===========================================================================
// Catalog dispatch
// ===========================================================================

TEST_CASE("auto: AniList answers, MAL is never called") {
  Rig r(Mode::Auto);
  auto p = r.cat->search("frieren", 1);
  REQUIRE(p.has_value());
  CHECK(p->source == Source::AniList);
  CHECK(r.anilist.search_calls == 1);
  CHECK(r.mal.search_calls == 0);
  CHECK_FALSE(r.cat->status().serving_from_mal());
}

TEST_CASE("auto: AniList 403 -> MAL answers, latches, and is probed again after the window") {
  Rig r(Mode::Auto);
  r.anilist.ok = false;
  r.anilist.fail = ProviderError::forbidden(403);

  auto p = r.cat->search("frieren", 1);
  REQUIRE(p.has_value());
  CHECK(p->source == Source::Mal);
  CHECK(r.anilist.search_calls == 1);
  CHECK(r.mal.search_calls == 1);
  CHECK(r.cat->status().serving_from_mal());
  CHECK(r.cat->status().active == Source::Mal);

  // Latched: the next calls go straight to MAL.
  r.now += 10;
  auto d = r.cat->discover(DiscoverAxis::Popular, 1, DiscoverFilters{});
  REQUIRE(d.has_value());
  CHECK(d->source == Source::Mal);
  CHECK(r.anilist.discover_calls == 0);
  CHECK(r.mal.discover_calls == 1);

  // Window elapsed: AniList is probed first again; still down -> MAL.
  r.now += FailoverState::kRetryAfterSecs;
  auto p2 = r.cat->search("frieren", 2);
  REQUIRE(p2.has_value());
  CHECK(p2->source == Source::Mal);
  CHECK(r.anilist.search_calls == 2);
  CHECK(r.mal.search_calls == 2);

  // AniList recovers on the next probe: served by AniList, latch cleared.
  r.now += FailoverState::kRetryAfterSecs;
  r.anilist.ok = true;
  auto p3 = r.cat->search("frieren", 3);
  REQUIRE(p3.has_value());
  CHECK(p3->source == Source::AniList);
  CHECK_FALSE(r.cat->status().serving_from_mal());
}

TEST_CASE("auto: MAL failure while latched falls back to AniList") {
  Rig r(Mode::Auto);
  r.anilist.ok = false;
  r.anilist.fail = ProviderError::forbidden(403);
  REQUIRE(r.cat->search("x", 1).has_value());  // latches MAL
  r.mal.ok = false;
  r.mal.fail = ProviderError::server(503);
  r.anilist.ok = true;
  auto p = r.cat->search("x", 1);
  REQUIRE(p.has_value());
  CHECK(p->source == Source::AniList);
  CHECK_FALSE(r.cat->status().serving_from_mal());
}

TEST_CASE("auto without a MAL backend: the error names the missing client id") {
  Rig r(Mode::Auto, /*with_mal=*/false);
  r.anilist.ok = false;
  r.anilist.fail = ProviderError::forbidden(403);
  auto p = r.cat->search("x", 1);
  REQUIRE_FALSE(p.has_value());
  CHECK(p.error().kind == ProviderError::Kind::Forbidden);
  CHECK(p.error().status == 403);
  CHECK(p.error().detail == "AniList blocked us (403); MAL: no client id (set one in Settings)");
  CHECK(describe(p.error()) == p.error().detail);
  CHECK_FALSE(r.cat->status().mal_available);
}

TEST_CASE("auto: both sources fail -> the first source's kind, both copies") {
  Rig r(Mode::Auto);
  r.anilist.ok = false;
  r.anilist.fail = ProviderError::forbidden(403);
  r.mal.ok = false;
  r.mal.fail = ProviderError::server(503);
  auto p = r.cat->search("x", 1);
  REQUIRE_FALSE(p.has_value());
  CHECK(p.error().kind == ProviderError::Kind::Forbidden);
  CHECK(p.error().detail == "AniList blocked us (403); MAL is down (503)");
}

TEST_CASE("auto: a MAL Unsupported answer reports the AniList failure alone") {
  Rig r(Mode::Auto);
  r.anilist.ok = false;
  r.anilist.fail = ProviderError::forbidden(403);
  r.mal.enrich_err = ProviderError::unsupported();
  auto e = r.cat->enrich(154587, std::nullopt);
  REQUIRE_FALSE(e.has_value());
  CHECK(e.error().kind == ProviderError::Kind::Forbidden);
  CHECK(e.error().detail == "AniList blocked us (403)");
  CHECK(r.mal.enrich_calls == 1);
}

TEST_CASE("mode anilist: MAL is never called, even on failure") {
  Rig r(Mode::AniList);
  r.anilist.ok = false;
  r.anilist.fail = ProviderError::network();
  auto p = r.cat->search("x", 1);
  REQUIRE_FALSE(p.has_value());
  CHECK(p.error().detail == "AniList network unreachable");
  CHECK(r.mal.search_calls == 0);
  CHECK_FALSE(r.cat->status().serving_from_mal());
}

TEST_CASE("mode mal: AniList is never called; a MAL failure is reported as MAL's") {
  Rig r(Mode::Mal);
  auto p = r.cat->search("x", 1);
  REQUIRE(p.has_value());
  CHECK(p->source == Source::Mal);
  CHECK(r.anilist.search_calls == 0);
  CHECK(r.cat->status().serving_from_mal());

  r.mal.ok = false;
  r.mal.fail = ProviderError::rate_limited();
  auto p2 = r.cat->search("x", 1);
  REQUIRE_FALSE(p2.has_value());
  CHECK(p2.error().kind == ProviderError::Kind::RateLimited);
  CHECK(p2.error().detail == "MAL is rate-limiting us (429)");
  CHECK(r.anilist.search_calls == 0);
}

TEST_CASE("mode mal without a client id fails every call with the Settings hint") {
  Rig r(Mode::Mal, /*with_mal=*/false);
  auto p = r.cat->search("x", 1);
  REQUIRE_FALSE(p.has_value());
  CHECK(p.error().kind == ProviderError::Kind::Forbidden);
  CHECK(p.error().detail == "MAL: no client id (set one in Settings)");
  CHECK(r.anilist.search_calls == 0);
}

TEST_CASE("synthetic (negative) ids are always served by MAL, whatever the mode") {
  Rig r(Mode::AniList);
  auto e = r.cat->enrich(-52991, std::nullopt);
  REQUIRE(e.has_value());
  REQUIRE(e->has_value());
  CHECK((*e)->anilist_id == -52991);
  CHECK(r.anilist.enrich_calls == 0);
  CHECK(r.mal.enrich_calls == 1);
  CHECK(r.mal.last_enrich_id == -52991);

  auto c = r.cat->char_recs(-52991, std::nullopt);
  REQUIRE(c.has_value());
  CHECK(r.anilist.char_calls == 0);
  CHECK(r.mal.char_calls == 1);

  // A MAL failure on a synthetic row is MAL's alone.
  r.mal.ok = false;
  r.mal.fail = ProviderError::server(500);
  auto e2 = r.cat->enrich(-52991, std::nullopt);
  REQUIRE_FALSE(e2.has_value());
  CHECK(e2.error().detail == "MAL is down (500)");
}

TEST_CASE("synthetic id without a MAL backend: Forbidden with the Settings hint") {
  Rig r(Mode::Auto, /*with_mal=*/false);
  auto e = r.cat->enrich(-5, std::nullopt);
  REQUIRE_FALSE(e.has_value());
  CHECK(e.error().kind == ProviderError::Kind::Forbidden);
  CHECK(e.error().detail == "MAL: no client id (set one in Settings)");
  CHECK(r.anilist.enrich_calls == 0);
}

TEST_CASE("auto: a real id refreshed while AniList is down goes to MAL with the mal hint") {
  Rig r(Mode::Auto);
  r.anilist.ok = false;
  r.anilist.fail = ProviderError::server(502);
  auto e = r.cat->enrich(154587, 52991);
  REQUIRE(e.has_value());
  REQUIRE(e->has_value());
  CHECK((*e)->anilist_id == 154587);  // the row keeps its own id
  CHECK((*e)->mal_id == 52991);
  CHECK(r.anilist.enrich_calls == 1);
  CHECK(r.mal.last_enrich_id == 154587);
  CHECK(r.mal.last_enrich_mal == 52991);
}

TEST_CASE("genres fall through to MAL's static vocabulary") {
  Rig r(Mode::Auto);
  r.anilist.ok = false;
  auto g = r.cat->genres();
  REQUIRE(g.has_value());
  CHECK(*g == std::vector<std::string>{"Suspense"});
  CHECK(r.anilist.genre_calls == 1);
  CHECK(r.mal.genre_calls == 1);
}

TEST_CASE("failure_copy / describe") {
  CHECK(failure_copy(Source::AniList, ProviderError::forbidden(403)) == "AniList blocked us (403)");
  CHECK(failure_copy(Source::Mal, ProviderError::server(503)) == "MAL is down (503)");
  CHECK(failure_copy(Source::AniList, ProviderError::network()) == "AniList network unreachable");
  CHECK(failure_copy(Source::AniList, ProviderError::decode("x")) == "AniList malformed response");
  CHECK(failure_copy(Source::Mal, ProviderError::http(400)) == "MAL returned an error (400)");
  CHECK(describe(ProviderError::network()) == "network unreachable");
  ProviderError e = ProviderError::forbidden(403);
  e.detail = "AniList blocked us (403)";
  CHECK(describe(e) == "AniList blocked us (403)");
}
