// anibd_live_smoke.cpp — P12a DoD: "SHIGOKU_LIVE=1 resolve against the
// (verified-alive) upstream." Not part of the offline ctest suite — invoked
// only via scripts/live_smoke.sh (which sets SHIGOKU_LIVE=1). Skips (exit 0)
// when the env var is absent so an accidental direct run doesn't hit the
// network or fail a build.
//
// anibd has no search endpoint: the flow is canonical_key (AniList id) ->
// episodes -> resolve. Known-good target: Sousou no Frieren S1, AniList id
// 154587 (same id the ported anibd.rs tests use).

#include <cstdio>
#include <cstdlib>

#include "../src/anibd.hpp"

namespace {
constexpr const char* kFrierenS1Anilist = "154587";
}

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("anibd_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;
  using namespace shigoku::anibd;

  auto provider = AniBd::create();
  if (!provider.has_value()) {
    std::fprintf(stderr, "anibd_live_smoke: FAIL AniBd::create kind=%d status=%u detail=%s\n",
                 static_cast<int>(provider.error().kind), provider.error().status,
                 provider.error().detail.c_str());
    return 1;
  }

  // canonical_key just stringifies the AniList id — sanity-check the contract.
  Enrichment show;
  show.anilist_id = 154587;
  if (provider->canonical_key(show) != std::optional<std::string>(kFrierenS1Anilist)) {
    std::fprintf(stderr, "anibd_live_smoke: FAIL canonical_key mismatch\n");
    return 1;
  }

  auto eps = provider->episodes(kFrierenS1Anilist, Translation::Sub, std::nullopt);
  if (!eps.has_value()) {
    std::fprintf(stderr, "anibd_live_smoke: FAIL episodes kind=%d status=%u detail=%s\n",
                 static_cast<int>(eps.error().kind), eps.error().status,
                 eps.error().detail.c_str());
    return 1;
  }
  if (eps->empty() || eps->front() != "1") {
    std::fprintf(stderr, "anibd_live_smoke: FAIL episode grid shape (count=%zu first=%s)\n",
                 eps->size(), eps->empty() ? "<none>" : eps->front().c_str());
    return 1;
  }

  auto link = provider->resolve(kFrierenS1Anilist, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) {
    std::fprintf(stderr, "anibd_live_smoke: FAIL resolve kind=%d status=%u detail=%s\n",
                 static_cast<int>(link.error().kind), link.error().status,
                 link.error().detail.c_str());
    return 1;
  }
  if (link->url.rfind("http", 0) != 0 || !link->cloaked_segments || link->decloak_segments) {
    std::fprintf(stderr, "anibd_live_smoke: FAIL stream link shape (url=%s cloaked=%d decloak=%d)\n",
                 link->url.c_str(), link->cloaked_segments, link->decloak_segments);
    return 1;
  }

  std::printf("anibd_live_smoke: OK %zu episode(s), url=%s%s\n", eps->size(), link->url.c_str(),
              link->sub_url.has_value() ? " (+softsub)" : "");
  return 0;
}
