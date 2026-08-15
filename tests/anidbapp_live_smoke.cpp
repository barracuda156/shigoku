// anidbapp_live_smoke.cpp — P25 DoD: "SHIGOKU_LIVE=1 resolve against the
// (verified-alive) upstream, through the REAL Chrome-shaped TLS handshake."
// Not part of the offline ctest suite — invoked only via scripts/live_smoke.sh
// (which sets SHIGOKU_LIVE=1). Skips (exit 0) when the env var is absent so an
// accidental direct run doesn't hit the network or fail a build.
//
// This is the one place the impersonate seam is exercised end-to-end against a
// live fingerprinting edge: AniDbApp::create() uses Fingerprint::Chrome, so a
// stock handshake would 403 here. Requires libcurl-impersonate present at
// runtime (dev box: ~/.local/lib via LD_LIBRARY_PATH; user target:
// /opt/local/lib). If the dylib is missing, every fetch is a Network error and
// this smoke fails loudly — which is the correct signal ("impersonate not
// installed"), distinct from an upstream outage.
//
// Flow: search (title -> cards + scraped ids) -> episodes(sub) by the site id
// -> resolve episode 1 -> a real HLS master. Known-good target: "frieren",
// whose top hits include Frieren S1 (AniList 154587).

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "../src/anidbapp.hpp"

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("anidbapp_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;
  using namespace shigoku::anidbapp;

  auto provider = AniDbApp::create();
  if (!provider.has_value()) {
    std::fprintf(stderr, "anidbapp_live_smoke: FAIL AniDbApp::create kind=%d status=%u\n",
                 static_cast<int>(provider.error().kind), provider.error().status);
    return 1;
  }

  // 1) Search. A stock ClientHello 403s here; a pass proves the Chrome shape.
  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = 26;
  opts.page = 1;
  auto hits = provider->search("frieren", opts);
  if (!hits.has_value()) {
    std::fprintf(stderr,
                 "anidbapp_live_smoke: FAIL search kind=%d status=%u detail=%s "
                 "(403 here usually means libcurl-impersonate is missing)\n",
                 static_cast<int>(hits.error().kind), hits.error().status,
                 hits.error().detail.c_str());
    return 1;
  }
  if (hits->empty()) {
    std::fprintf(stderr, "anidbapp_live_smoke: FAIL search returned no hits\n");
    return 1;
  }

  // Prefer the hit that scraped AniList id 154587 (Frieren S1); else the first.
  const SearchHit* pick = &(*hits)[0];
  for (const auto& h : *hits) {
    if (h.anilist_id == std::optional<std::int64_t>(154587)) {
      pick = &h;
      break;
    }
  }
  std::printf("anidbapp_live_smoke: search %zu hits, using id=%s title='%s' anilist=%lld\n",
              hits->size(), pick->provider_id.c_str(), pick->title.c_str(),
              static_cast<long long>(pick->anilist_id.value_or(-1)));

  // 2) Episodes (sub lists every episode; no language probe).
  auto eps = provider->episodes(pick->provider_id, Translation::Sub, std::nullopt);
  if (!eps.has_value()) {
    std::fprintf(stderr, "anidbapp_live_smoke: FAIL episodes kind=%d status=%u detail=%s\n",
                 static_cast<int>(eps.error().kind), eps.error().status,
                 eps.error().detail.c_str());
    return 1;
  }
  if (eps->empty() || eps->front() != "1") {
    std::fprintf(stderr, "anidbapp_live_smoke: FAIL episode shape (count=%zu first=%s)\n",
                 eps->size(), eps->empty() ? "<none>" : eps->front().c_str());
    return 1;
  }

  // 3) Resolve episode 1 -> a real HLS master.
  auto link = provider->resolve(pick->provider_id, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) {
    std::fprintf(stderr, "anidbapp_live_smoke: FAIL resolve kind=%d status=%u detail=%s\n",
                 static_cast<int>(link.error().kind), link.error().status,
                 link.error().detail.c_str());
    return 1;
  }
  if (link->url.rfind("http", 0) != 0 || !link->cloaked_segments || link->decloak_segments) {
    std::fprintf(stderr,
                 "anidbapp_live_smoke: FAIL stream link shape (url=%s cloaked=%d decloak=%d)\n",
                 link->url.c_str(), link->cloaked_segments, link->decloak_segments);
    return 1;
  }

  std::printf("anidbapp_live_smoke: OK %zu episode(s), url=%s\n", eps->size(),
              link->url.c_str());
  return 0;
}
