// animehub_live_smoke.cpp — resolve against the live site. Not part of the
// offline ctest suite: run by hand or by scripts/provider_canary.sh, which set
// SHIGOKU_LIVE=1. Skips (exit 0) when the variable is absent.
//
// Flow: search the romaji title (as the resolve walk does) -> the Frieren S1
// hit -> episodes (sub) -> resolve episode 1 -> fetch the master it names,
// so a dead CDN fails here rather than only at play time.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "../src/animehub.hpp"
#include "../src/http.hpp"

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("animehub_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;
  using namespace shigoku::animehub;

  auto provider = AnimeHub::create();
  if (!provider.has_value()) {
    std::fprintf(stderr, "animehub_live_smoke: FAIL AnimeHub::create kind=%d\n",
                 static_cast<int>(provider.error().kind));
    return 1;
  }

  SearchOptions opts;
  opts.translation = Translation::Sub;
  auto hits = provider->search("Sousou no Frieren", opts);
  if (!hits.has_value()) {
    std::fprintf(stderr, "animehub_live_smoke: FAIL search kind=%d status=%u detail=%s\n",
                 static_cast<int>(hits.error().kind), hits.error().status,
                 hits.error().detail.c_str());
    return 1;
  }
  const SearchHit* pick = nullptr;
  for (const auto& h : *hits) {
    if (h.provider_id == "sousou-no-frieren") pick = &h;
  }
  if (pick == nullptr) {
    std::fprintf(stderr, "animehub_live_smoke: FAIL no sousou-no-frieren among %zu hit(s)\n",
                 hits->size());
    return 1;
  }
  std::printf("animehub_live_smoke: search %zu hits, using '%s' (%s)\n", hits->size(),
              pick->title.c_str(), pick->title_english.value_or("-").c_str());

  auto eps = provider->episodes(pick->provider_id, Translation::Sub, std::nullopt);
  if (!eps.has_value()) {
    std::fprintf(stderr, "animehub_live_smoke: FAIL episodes kind=%d status=%u detail=%s\n",
                 static_cast<int>(eps.error().kind), eps.error().status,
                 eps.error().detail.c_str());
    return 1;
  }
  if (eps->empty() || eps->front() != "1") {
    std::fprintf(stderr, "animehub_live_smoke: FAIL episode shape (count=%zu first=%s)\n",
                 eps->size(), eps->empty() ? "<none>" : eps->front().c_str());
    return 1;
  }

  auto link = provider->resolve(pick->provider_id, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) {
    std::fprintf(stderr, "animehub_live_smoke: FAIL resolve kind=%d status=%u detail=%s\n",
                 static_cast<int>(link.error().kind), link.error().status,
                 link.error().detail.c_str());
    return 1;
  }

  auto client = http::Client::create();
  if (!client.has_value()) {
    std::fprintf(stderr, "animehub_live_smoke: FAIL http client\n");
    return 1;
  }
  http::Request req;
  req.url = link->url;
  if (link->referer) req.extra_headers.push_back({"Referer", *link->referer});
  auto master = client->fetch(req);
  if (!master.has_value()) {
    std::fprintf(stderr, "animehub_live_smoke: FAIL master fetch kind=%d status=%u\n",
                 static_cast<int>(master.error().kind), master.error().status);
    return 1;
  }
  const std::string body(master->begin(), master->end());
  if (body.rfind("#EXTM3U", 0) != 0) {
    std::fprintf(stderr, "animehub_live_smoke: FAIL master is not a playlist\n");
    return 1;
  }

  std::printf("animehub_live_smoke: OK %zu episode(s), master %zu bytes, referer=%s\n",
              eps->size(), body.size(), link->referer.value_or("-").c_str());
  return 0;
}
