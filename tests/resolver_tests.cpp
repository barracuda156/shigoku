// resolver_tests.cpp — P15 DoD: resolver.rs + anilist.rs title_score tests
// ported 1:1. Pure, offline (§8): no store, no registry, no network. Each case
// mirrors a sabigoku #[test] and pins the exact selection/score the Rust does.

#include <optional>
#include <string>
#include <vector>

#include "../src/resolver.hpp"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

using namespace shigoku;
using namespace shigoku::resolver;

namespace {

// canonical(name): anilist_id=1, title_romaji=name, rest default (resolver.rs:183).
Enrichment canonical(std::string name) {
  Enrichment e;
  e.anilist_id = 1;
  e.title_romaji = std::move(name);
  return e;
}

// hit(provider_id, title): those two, rest default (resolver.rs:191).
SearchHit hit(std::string provider_id, std::string title) {
  SearchHit h;
  h.provider_id = std::move(provider_id);
  h.title = std::move(title);
  return h;
}

}  // namespace

// --- title_score / normalize_title / canon_season (anilist.rs tests) -------

TEST_CASE("title_score prefers exact over prefix over substring (anilist.rs:1081)") {
  CHECK(title_score("Frieren", std::optional<std::string_view>("Frieren")) == 1600);
  CHECK(title_score("Frieren", std::optional<std::string_view>("Frieren Season 2")) == 1250);
  CHECK(title_score("Frieren", std::optional<std::string_view>("The World of Frieren")) == 900);
  CHECK(title_score("Frieren", std::nullopt) == -5000);
  CHECK(title_score("", std::optional<std::string_view>("Frieren")) == -5000);
  CHECK(title_score("Frieren", std::optional<std::string_view>("Naruto")) == -5000);
  // Ordering: exact > prefix > substring.
  CHECK(title_score("Frieren", std::optional<std::string_view>("Frieren")) >
        title_score("Frieren", std::optional<std::string_view>("Frieren Season 2")));
  CHECK(title_score("Frieren", std::optional<std::string_view>("Frieren Season 2")) >
        title_score("Frieren", std::optional<std::string_view>("The World of Frieren")));
}

TEST_CASE("title_score reconciles Season N vs Nth Season (anilist.rs:1106)") {
  CHECK(title_score("Sousou no Frieren Season 2",
                    std::optional<std::string_view>("Sousou no Frieren 2nd Season")) == 1600);
  // LHS -> "frierens2", RHS -> "frieren"; frierens2 starts_with frieren -> 1250.
  CHECK(title_score("Frieren Season 2", std::optional<std::string_view>("Frieren")) < 1600);
}

TEST_CASE("canon_season reconciles season forms, leaves the rest (anilist.rs:1095)") {
  CHECK(canon_season("frierenseason2") == "frierens2");
  CHECK(canon_season("frieren2ndseason") == "frierens2");
  CHECK(canon_season("k3rdseason") == "ks3");
  CHECK(canon_season("title2season") == "titles2");
  CHECK(canon_season("frieren") == "frieren");
  CHECK(canon_season("loghorizon2") == "loghorizon2");
  CHECK(canon_season("seasonsoflife") == "seasonsoflife");
}

TEST_CASE("normalize_title lowercases ASCII keeps unicode (anilist.rs:1118)") {
  CHECK(normalize_title("Re:Zero 2nd Season") == "rezero2ndseason");
  CHECK(normalize_title("\xE8\x91\xAC\xE9\x80\x81\xE3\x81\xAE\xE3\x83\x95\xE3\x83\xAA\xE3\x83\xBC\xE3\x83\xAC\xE3\x83\xB3") ==
        "\xE8\x91\xAC\xE9\x80\x81\xE3\x81\xAE\xE3\x83\x95\xE3\x83\xAA\xE3\x83\xBC\xE3\x83\xAC\xE3\x83\xB3");
  CHECK(normalize_title("  !!  ").empty());
}

// --- best_id_match (Tier-B) ------------------------------------------------

TEST_CASE("id match binds on MAL agreement regardless of title (resolver.rs:200)") {
  Enrichment canon;
  canon.anilist_id = 154587;
  canon.mal_id = 52991;
  canon.title_romaji = "Sousou no Frieren";
  std::vector<SearchHit> cands;
  {
    SearchHit h = hit("1443", "Frieren: Beyond Journey's End Season 2");
    h.mal_id = 58305;
    cands.push_back(h);
  }
  {
    SearchHit h = hit("2454", "Frieren: Beyond Journey's End");
    h.mal_id = 52991;
    cands.push_back(h);
  }
  auto idx = best_id_match(canon, cands);
  REQUIRE(idx.has_value());
  CHECK(cands[*idx].provider_id == "2454");
}

TEST_CASE("id match binds on anilist id when the provider embeds one (resolver.rs:224)") {
  Enrichment canon = canonical("X");
  canon.anilist_id = 999;
  std::vector<SearchHit> cands;
  {
    SearchHit h = hit("a", "a");
    h.anilist_id = 998;
    cands.push_back(h);
  }
  {
    SearchHit h = hit("b", "b");
    h.anilist_id = 999;
    cands.push_back(h);
  }
  auto idx = best_id_match(canon, cands);
  REQUIRE(idx.has_value());
  CHECK(cands[*idx].provider_id == "b");
}

TEST_CASE("id match skips candidate whose metadata contradicts the id (resolver.rs:245)") {
  Enrichment canon;
  canon.anilist_id = 16498;
  canon.mal_id = 16498;
  canon.title_romaji = "Attack on Titan";
  canon.total_episodes = 25;
  canon.year = 2013;
  canon.status = "FINISHED";

  SearchHit decoy = hit("666", "decoy");
  decoy.mal_id = 16498;
  decoy.total_episodes = 1;
  decoy.year = 1998;

  CHECK(!best_id_match(canon, std::vector<SearchHit>{decoy}).has_value());

  SearchHit real = hit("42", "Shingeki no Kyojin");
  real.mal_id = 16498;
  real.total_episodes = 25;
  real.year = 2013;
  std::vector<SearchHit> cands{decoy, real};
  auto idx = best_id_match(canon, cands);
  REQUIRE(idx.has_value());
  CHECK(cands[*idx].provider_id == "42");
}

TEST_CASE("id match prefers corroborated survivor over earlier bare (resolver.rs:279)") {
  Enrichment canon;
  canon.anilist_id = 5114;
  canon.mal_id = 5114;
  canon.title_romaji = "Fullmetal Alchemist: Brotherhood";
  canon.total_episodes = 64;
  canon.year = 2009;
  canon.status = "FINISHED";

  SearchHit sparse = hit("sparse-decoy", "sparse");
  sparse.mal_id = 5114;

  SearchHit real = hit("real", "real");
  real.mal_id = 5114;
  real.total_episodes = 64;
  real.year = 2009;

  std::vector<SearchHit> cands{sparse, real};
  auto idx = best_id_match(canon, cands);
  REQUIRE(idx.has_value());
  CHECK(cands[*idx].provider_id == "real");

  // A bare id alone still binds.
  CHECK(best_id_match(canon, std::vector<SearchHit>{sparse}).has_value());
}

TEST_CASE("id match veto spares bare metadata and releasing partials (resolver.rs:311)") {
  Enrichment canon;
  canon.anilist_id = 1;
  canon.mal_id = 52991;
  canon.total_episodes = 28;
  canon.year = 2023;
  canon.status = "FINISHED";
  SearchHit bare = hit("2454", "bare");
  bare.mal_id = 52991;
  CHECK(best_id_match(canon, std::vector<SearchHit>{bare}).has_value());

  Enrichment airing;
  airing.anilist_id = 1;
  airing.mal_id = 59978;
  airing.total_episodes = 28;
  airing.year = 2026;
  airing.status = "RELEASING";
  SearchHit partial = hit("1443", "partial");
  partial.mal_id = 59978;
  partial.total_episodes = 4;
  partial.year = 2026;
  CHECK(best_id_match(airing, std::vector<SearchHit>{partial}).has_value());

  SearchHit wrong_year = hit("9", "wrong");
  wrong_year.mal_id = 59978;
  wrong_year.year = 1998;
  CHECK(!best_id_match(airing, std::vector<SearchHit>{wrong_year}).has_value());
}

TEST_CASE("id match is a no-op without id agreement (resolver.rs:358)") {
  Enrichment no_mal;
  no_mal.anilist_id = 999;
  SearchHit mal_only = hit("52991", "x");
  mal_only.mal_id = 52991;
  CHECK(!best_id_match(no_mal, std::vector<SearchHit>{mal_only}).has_value());

  Enrichment with_mal;
  with_mal.anilist_id = 999;
  with_mal.mal_id = 52991;
  CHECK(!best_id_match(with_mal, std::vector<SearchHit>{hit("a", "X")}).has_value());
  CHECK(!best_id_match(with_mal, std::vector<SearchHit>{}).has_value());
}

// --- best_provider_match (Tier-C) ------------------------------------------

TEST_CASE("provider match binds exact title with eps+year agreement (resolver.rs:386)") {
  Enrichment canon = canonical("Sousou no Frieren");
  canon.total_episodes = 28;
  canon.year = 2023;
  std::vector<SearchHit> cands;
  {
    SearchHit h = hit("999", "Unrelated Show");
    h.total_episodes = 12;
    h.year = 2019;
    cands.push_back(h);
  }
  {
    SearchHit h = hit("52991", "Sousou no Frieren");
    h.total_episodes = 28;
    h.year = 2023;
    cands.push_back(h);
  }
  auto idx = best_provider_match(canon, cands);
  REQUIRE(idx.has_value());
  CHECK(cands[*idx].provider_id == "52991");
}

TEST_CASE("provider match rejects when no candidate clears the floor (resolver.rs:411)") {
  Enrichment canon = canonical("Sousou no Frieren");
  std::vector<SearchHit> cands{hit("a", "Naruto"), hit("b", "Bleach")};
  CHECK(!best_provider_match(canon, cands).has_value());
}

TEST_CASE("provider match rejects an ambiguous near tie (resolver.rs:418)") {
  Enrichment canon = canonical("Frieren");
  canon.year = 2023;
  std::vector<SearchHit> cands;
  {
    SearchHit h = hit("x", "Frieren");
    h.year = 2023;
    cands.push_back(h);
  }
  {
    SearchHit h = hit("y", "Frieren");
    h.year = 2023;
    cands.push_back(h);
  }
  CHECK(!best_provider_match(canon, cands).has_value());
}

TEST_CASE("provider match reconciles Season N title forms (resolver.rs:438)") {
  Enrichment canon = canonical("Re:Zero 2nd Season");
  canon.total_episodes = 25;
  std::vector<SearchHit> cands;
  {
    SearchHit h = hit("hit", "Re:Zero Season 2");
    h.total_episodes = 25;
    cands.push_back(h);
  }
  {
    SearchHit h = hit("miss", "Completely Different");
    h.total_episodes = 12;
    cands.push_back(h);
  }
  auto idx = best_provider_match(canon, cands);
  REQUIRE(idx.has_value());
  CHECK(cands[*idx].provider_id == "hit");
}

TEST_CASE("provider match rejects lone same-title different-work by eps veto (resolver.rs:458)") {
  Enrichment canon = canonical("Given");
  canon.total_episodes = 25;
  canon.status = "FINISHED";
  SearchHit movie = hit("movie", "Given");
  movie.total_episodes = 1;
  CHECK(!best_provider_match(canon, std::vector<SearchHit>{movie}).has_value());
}

TEST_CASE("provider match picks the series when its movie is also listed (resolver.rs:477)") {
  Enrichment canon = canonical("Given");
  canon.total_episodes = 11;
  canon.status = "FINISHED";
  std::vector<SearchHit> cands;
  {
    SearchHit h = hit("movie", "Given");
    h.total_episodes = 1;
    cands.push_back(h);
  }
  {
    SearchHit h = hit("series", "Given");
    h.total_episodes = 11;
    cands.push_back(h);
  }
  auto idx = best_provider_match(canon, cands);
  REQUIRE(idx.has_value());
  CHECK(cands[*idx].provider_id == "series");
}

TEST_CASE("provider match spares still-airing canonical with fewer listed eps (resolver.rs:498)") {
  Enrichment canon = canonical("One Piece");
  canon.total_episodes = 1100;
  canon.status = "RELEASING";
  SearchHit op = hit("op", "One Piece");
  op.total_episodes = 1050;
  auto idx = best_provider_match(canon, std::vector<SearchHit>{op});
  REQUIRE(idx.has_value());
  CHECK(op.provider_id == "op");
}

TEST_CASE("provider match veto is symmetric and covers null status (resolver.rs:513)") {
  Enrichment finished = canonical("X");
  finished.total_episodes = 12;
  finished.status = "FINISHED";
  SearchHit long_runner = hit("long-runner", "X");
  long_runner.total_episodes = 500;
  CHECK(!best_provider_match(finished, std::vector<SearchHit>{long_runner}).has_value());

  Enrichment null_status = canonical("Given");
  null_status.total_episodes = 25;  // status default nullopt.
  SearchHit movie = hit("movie", "Given");
  movie.total_episodes = 1;
  CHECK(!best_provider_match(null_status, std::vector<SearchHit>{movie}).has_value());
}

TEST_CASE("provider match survives garbage huge episode count (resolver.rs:546)") {
  Enrichment canon = canonical("X");
  canon.total_episodes = 100;
  canon.status = "FINISHED";
  SearchHit garbage = hit("garbage", "X");
  garbage.total_episodes = 0xFFFFFFFFu;  // u32::MAX — abs_diff must not overflow.
  CHECK(!best_provider_match(canon, std::vector<SearchHit>{garbage}).has_value());
}

TEST_CASE("provider match empty page is none (resolver.rs:563)") {
  CHECK(!best_provider_match(canonical("Anything"), std::vector<SearchHit>{}).has_value());
}

TEST_CASE("candidate_episodes prefers total falls back to larger track (resolver.rs:568)") {
  {
    SearchHit h;
    h.total_episodes = 24;
    h.eps_sub = 12;
    CHECK(candidate_episodes(h) == 24);
  }
  {
    SearchHit h;
    h.eps_sub = 12;
    h.eps_dub = 6;
    CHECK(candidate_episodes(h) == 12);
  }
  CHECK(candidate_episodes(SearchHit{}) == 0);
}
