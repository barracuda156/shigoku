// anilibria_live_smoke.cpp — real search/episodes/resolve against the live
// AniLiberty API, gated on SHIGOKU_LIVE=1 (not a ctest — driven by hand). Skips
// (exit 0) when the env var is absent so an accidental direct run doesn't hit
// the network or fail a build.
//
// Flow: search (romaji query -> ids) -> episodes by the release id -> resolve
// episode 1 -> a real per-quality HLS playlist url. Known-good target:
// "frieren" (Sousou no Frieren, release 9542).

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/anilibria.hpp"

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("anilibria_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;
  using namespace shigoku::anilibria;

  auto provider = AniLibria::create();
  if (!provider.has_value()) {
    std::fprintf(stderr, "anilibria_live_smoke: FAIL create kind=%d status=%u\n",
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
    std::fprintf(stderr, "anilibria_live_smoke: FAIL search kind=%d status=%u detail=%s\n",
                 static_cast<int>(hits.error().kind), hits.error().status,
                 hits.error().detail.c_str());
    return 1;
  }
  if (hits->empty()) {
    std::fprintf(stderr, "anilibria_live_smoke: FAIL search returned no hits\n");
    return 1;
  }
  const std::string id = (*hits)[0].provider_id;
  std::printf("anilibria_live_smoke: search ok, %zu hits, top id=%s title=%s (%s)\n",
              hits->size(), id.c_str(), (*hits)[0].title.c_str(),
              (*hits)[0].title_english.value_or("-").c_str());

  // 2) Episodes.
  auto eps = provider->episodes(id, Translation::Sub, std::nullopt);
  if (!eps.has_value()) {
    std::fprintf(stderr, "anilibria_live_smoke: FAIL episodes kind=%d status=%u detail=%s\n",
                 static_cast<int>(eps.error().kind), eps.error().status,
                 eps.error().detail.c_str());
    return 1;
  }
  if (eps->empty()) {
    std::fprintf(stderr, "anilibria_live_smoke: FAIL no episodes listed\n");
    return 1;
  }
  std::printf("anilibria_live_smoke: episodes ok, %zu listed, first=%s last=%s\n",
              eps->size(), eps->front().c_str(), eps->back().c_str());

  // 3) Resolve the first listed episode.
  auto link = provider->resolve(id, eps->front(), Translation::Sub, Quality::Best);
  if (!link.has_value()) {
    std::fprintf(stderr, "anilibria_live_smoke: FAIL resolve kind=%d status=%u detail=%s\n",
                 static_cast<int>(link.error().kind), link.error().status,
                 link.error().detail.c_str());
    return 1;
  }
  if (link->url.rfind("https://", 0) != 0 ||
      link->url.find(".m3u8") == std::string::npos) {
    std::fprintf(stderr, "anilibria_live_smoke: FAIL suspicious stream url: %s\n",
                 link->url.c_str());
    return 1;
  }
  std::printf("anilibria_live_smoke: resolve ok, url=%s\n", link->url.c_str());
  std::printf("anilibria_live_smoke: PASS\n");
  return 0;
}
