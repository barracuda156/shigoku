// hianime_live_smoke.cpp — P47 DoD: "SHIGOKU_LIVE=1 resolve against the
// (verified-alive) upstream." Not part of the offline ctest suite — invoked
// only via scripts/live_smoke.sh (or the canary), which sets SHIGOKU_LIVE=1.
// Skips (exit 0) when the env var is absent so an accidental direct run
// doesn't hit the network or fail a build.
//
// Flow: search (title -> cards) -> episodes(sub) by the site id -> resolve
// episode 1 -> a real HLS master on the ZokoAnime embed's own host. Known-
// good target: "frieren", whose top hit is Frieren S1, site id 481 (walked
// live 2026-09-20/21; PROVIDER_INTEL.md §6).

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "../src/hianime.hpp"

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("hianime_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;
  using namespace shigoku::hianime;

  auto provider = Hianime::create();
  if (!provider.has_value()) {
    std::fprintf(stderr, "hianime_live_smoke: FAIL Hianime::create kind=%d status=%u\n",
                 static_cast<int>(provider.error().kind), provider.error().status);
    return 1;
  }

  // 1) Search.
  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = 26;
  opts.page = 1;
  auto hits = provider->search("frieren", opts);
  if (!hits.has_value()) {
    std::fprintf(stderr, "hianime_live_smoke: FAIL search kind=%d status=%u detail=%s\n",
                 static_cast<int>(hits.error().kind), hits.error().status,
                 hits.error().detail.c_str());
    return 1;
  }
  if (hits->empty()) {
    std::fprintf(stderr, "hianime_live_smoke: FAIL search returned no hits\n");
    return 1;
  }

  // Prefer the known site id (481, Frieren S1); else the first hit.
  const SearchHit* pick = &(*hits)[0];
  for (const auto& h : *hits) {
    if (h.provider_id == "481") {
      pick = &h;
      break;
    }
  }
  std::printf("hianime_live_smoke: search %zu hits, using id=%s title='%s'\n", hits->size(),
              pick->provider_id.c_str(), pick->title.c_str());

  // 2) Episodes (sub).
  auto eps = provider->episodes(pick->provider_id, Translation::Sub, std::nullopt);
  if (!eps.has_value()) {
    std::fprintf(stderr, "hianime_live_smoke: FAIL episodes kind=%d status=%u detail=%s\n",
                 static_cast<int>(eps.error().kind), eps.error().status,
                 eps.error().detail.c_str());
    return 1;
  }
  if (eps->empty() || eps->front() != "1") {
    std::fprintf(stderr, "hianime_live_smoke: FAIL episode shape (count=%zu first=%s)\n",
                 eps->size(), eps->empty() ? "<none>" : eps->front().c_str());
    return 1;
  }

  // 3) Resolve episode 1 -> a real HLS master.
  auto link = provider->resolve(pick->provider_id, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) {
    std::fprintf(stderr, "hianime_live_smoke: FAIL resolve kind=%d status=%u detail=%s\n",
                 static_cast<int>(link.error().kind), link.error().status,
                 link.error().detail.c_str());
    return 1;
  }
  if (link->url.rfind("http", 0) != 0 || !link->referer.has_value()) {
    std::fprintf(stderr, "hianime_live_smoke: FAIL stream link shape (url=%s referer=%s)\n",
                 link->url.c_str(), link->referer.value_or("<none>").c_str());
    return 1;
  }

  std::printf("hianime_live_smoke: OK %zu episode(s), url=%s referer=%s\n", eps->size(),
              link->url.c_str(), link->referer->c_str());
  return 0;
}
