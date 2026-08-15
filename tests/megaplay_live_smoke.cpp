// megaplay_live_smoke.cpp — P13 DoD: "SHIGOKU_LIVE=1 resolves + plays through
// the full pipeline." Not part of the offline ctest suite — invoked only via
// scripts/live_smoke.sh (which sets SHIGOKU_LIVE=1). Skips (exit 0) when the
// env var is absent so an accidental direct run doesn't hit the network.
//
// megaplay is MAL-keyed with no search endpoint: the flow is canonical_key
// (MAL id) -> episodes (mint from a hint) -> resolve. Known-good target:
// Sousou no Frieren S1, MAL id 52991 (the id the ported megaplay.rs tests use).
// The resolved link must carry decloak_segments=true (the reason megaplay is in
// M2): playback then routes through the P12 proxy.

#include <cstdio>
#include <cstdlib>

#include "../src/megaplay.hpp"

namespace {
constexpr const char* kFrierenS1Mal = "52991";
}

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("megaplay_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;
  using namespace shigoku::megaplay;

  auto provider = MegaPlay::create();
  if (!provider.has_value()) {
    std::fprintf(stderr, "megaplay_live_smoke: FAIL MegaPlay::create kind=%d status=%u detail=%s\n",
                 static_cast<int>(provider.error().kind), provider.error().status,
                 provider.error().detail.c_str());
    return 1;
  }

  // canonical_key stringifies the MAL id — sanity-check the contract.
  Enrichment show;
  show.anilist_id = 154587;
  show.mal_id = 52991;
  if (provider->canonical_key(show) != std::optional<std::string>(kFrierenS1Mal)) {
    std::fprintf(stderr, "megaplay_live_smoke: FAIL canonical_key mismatch\n");
    return 1;
  }

  auto eps = provider->episodes(kFrierenS1Mal, Translation::Sub, std::optional<std::uint32_t>(28));
  if (!eps.has_value()) {
    std::fprintf(stderr, "megaplay_live_smoke: FAIL episodes kind=%d status=%u detail=%s\n",
                 static_cast<int>(eps.error().kind), eps.error().status,
                 eps.error().detail.c_str());
    return 1;
  }
  if (eps->empty() || eps->front() != "1") {
    std::fprintf(stderr, "megaplay_live_smoke: FAIL episode grid shape (count=%zu first=%s)\n",
                 eps->size(), eps->empty() ? "<none>" : eps->front().c_str());
    return 1;
  }

  auto link = provider->resolve(kFrierenS1Mal, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) {
    std::fprintf(stderr, "megaplay_live_smoke: FAIL resolve kind=%d status=%u detail=%s\n",
                 static_cast<int>(link.error().kind), link.error().status,
                 link.error().detail.c_str());
    return 1;
  }
  // The megaplay signature: cloaked AND decloak (routes through the proxy).
  if (link->url.rfind("http", 0) != 0 || !link->cloaked_segments || !link->decloak_segments) {
    std::fprintf(stderr,
                 "megaplay_live_smoke: FAIL stream link shape (url=%s cloaked=%d decloak=%d)\n",
                 link->url.c_str(), link->cloaked_segments, link->decloak_segments);
    return 1;
  }

  std::printf("megaplay_live_smoke: OK %zu episode(s), url=%s%s\n", eps->size(), link->url.c_str(),
              link->sub_url.has_value() ? " (+softsub)" : "");
  return 0;
}
