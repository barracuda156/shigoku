// senshi_live_smoke.cpp — P4 DoD: "SHIGOKU_LIVE=1 script resolves a known
// MAL id end-to-end and prints the m3u8 URL." Not part of the offline ctest
// suite (no network in CI-shaped runs) — invoked only via
// scripts/live_smoke.sh, which sets SHIGOKU_LIVE=1.
// Skips (exit 0) when the env var is absent so an accidental direct run
// doesn't hit the network or fail a build.
//
// Mirrors sabigoku tests/senshi_live.rs: search -> episodes -> resolve,
// same known-good target (Sousou no Frieren S1, MAL id 52991).

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "../src/senshi.hpp"

namespace {
constexpr const char* kFrierenS1Mal = "52991";
}

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("senshi_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;
  using namespace shigoku::senshi;

  auto provider = Senshi::create();
  if (!provider.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL Senshi::create kind=%d status=%u detail=%s\n",
                 static_cast<int>(provider.error().kind), provider.error().status,
                 provider.error().detail.c_str());
    return 1;
  }

  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = kSearchPageSize;
  opts.page = 1;

  auto hits = provider->search("frieren", opts);
  if (!hits.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL search kind=%d status=%u detail=%s\n",
                 static_cast<int>(hits.error().kind), hits.error().status,
                 hits.error().detail.c_str());
    return 1;
  }
  const auto s1 = std::find_if(hits->begin(), hits->end(), [](const SearchHit& h) {
    return h.provider_id == kFrierenS1Mal;
  });
  if (s1 == hits->end()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL frieren s1 (mal=%s) not in search results\n",
                 kFrierenS1Mal);
    return 1;
  }
  if (s1->mal_id != 52991) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL mal_id mismatch\n");
    return 1;
  }

  auto eps = provider->episodes(kFrierenS1Mal, Translation::Sub, std::nullopt);
  if (!eps.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL episodes kind=%d status=%u detail=%s\n",
                 static_cast<int>(eps.error().kind), eps.error().status,
                 eps.error().detail.c_str());
    return 1;
  }
  if (eps->size() < 28 || eps->front() != "1") {
    std::fprintf(stderr, "senshi_live_smoke: FAIL episode grid shape (count=%zu first=%s)\n",
                 eps->size(), eps->empty() ? "<none>" : eps->front().c_str());
    return 1;
  }

  auto link = provider->resolve(kFrierenS1Mal, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL resolve kind=%d status=%u detail=%s\n",
                 static_cast<int>(link.error().kind), link.error().status,
                 link.error().detail.c_str());
    return 1;
  }
  if (link->url.rfind("http", 0) != 0 || !link->cloaked_segments) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL stream link shape (url=%s cloaked=%d)\n",
                 link->url.c_str(), link->cloaked_segments);
    return 1;
  }

  std::printf("senshi_live_smoke: OK %zu hit(s), %zu episode(s), url=%s\n", hits->size(),
              eps->size(), link->url.c_str());
  return 0;
}
