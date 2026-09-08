// mal_catalog_live_smoke.cpp — search -> ranking -> by-id
// against the live MAL API v2. Not a ctest; gated on SHIGOKU_LIVE=1 (skips
// otherwise). The client id comes from SHIGOKU_MAL_CLIENT_ID or the baked
// one; with neither the smoke FAILS loudly (the canary must not read a
// missing id as a pass).

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "../src/mal_catalog.hpp"

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("mal_catalog_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;

  const char* env_id = std::getenv("SHIGOKU_MAL_CLIENT_ID");
  const std::string client_id = mal_catalog::effective_client_id(env_id != nullptr ? env_id : "");
  if (client_id.empty()) {
    std::fprintf(stderr, "mal_catalog_live_smoke: FAIL no client id (set SHIGOKU_MAL_CLIENT_ID)\n");
    return 1;
  }

  auto client = http::Client::create();
  if (!client.has_value()) {
    std::fprintf(stderr, "mal_catalog_live_smoke: FAIL client::create\n");
    return 1;
  }

  auto fail = [](const char* what, const ProviderError& e) {
    std::fprintf(stderr, "mal_catalog_live_smoke: FAIL %s kind=%d status=%u detail=%s\n", what,
                 static_cast<int>(e.kind), e.status, e.detail.c_str());
    return 1;
  };

  auto page = mal_catalog::search(*client, client_id, "frieren", 1);
  if (!page.has_value()) return fail("search", page.error());
  if (page->entries.empty()) {
    std::fprintf(stderr, "mal_catalog_live_smoke: FAIL no hits for 'frieren'\n");
    return 1;
  }
  const Enrichment& first = page->entries[0];
  std::printf("mal_catalog_live_smoke: OK search %zu hit(s), first: mal=%lld anilist=%lld title=%s\n",
              page->entries.size(), static_cast<long long>(first.mal_id.value_or(0)),
              static_cast<long long>(first.anilist_id), first.title_romaji.c_str());

  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
  const DiscoverAxis axes[] = {DiscoverAxis::Trending, DiscoverAxis::Popular,
                               DiscoverAxis::TopRated, DiscoverAxis::ThisSeason};
  for (const auto axis : axes) {
    auto feed = mal_catalog::discover(*client, client_id, axis, 1, now);
    if (!feed.has_value()) return fail("discover", feed.error());
    if (feed->entries.empty()) {
      std::fprintf(stderr, "mal_catalog_live_smoke: FAIL empty feed for axis %d\n",
                   static_cast<int>(axis));
      return 1;
    }
    std::printf("mal_catalog_live_smoke: OK discover axis %d: %zu row(s), has_next=%d, first=%s\n",
                static_cast<int>(axis), feed->entries.size(), feed->has_next ? 1 : 0,
                feed->entries[0].title_romaji.c_str());
  }

  auto detail = mal_catalog::by_id(*client, client_id, first.mal_id.value_or(0));
  if (!detail.has_value()) return fail("by_id", detail.error());
  if (!detail->has_value()) {
    std::fprintf(stderr, "mal_catalog_live_smoke: FAIL by_id says no such anime\n");
    return 1;
  }
  const auto& d = **detail;
  std::printf("mal_catalog_live_smoke: OK by_id title=%s eps=%u score=%u recs=%zu\n",
              d.show.title_romaji.c_str(), d.show.total_episodes.value_or(0),
              d.show.score.value_or(0), d.recommendations.size());

  auto missing = mal_catalog::by_id(*client, client_id, 999999999);
  if (!missing.has_value()) return fail("by_id(missing)", missing.error());
  if (missing->has_value()) {
    std::fprintf(stderr, "mal_catalog_live_smoke: FAIL id 999999999 unexpectedly exists\n");
    return 1;
  }
  std::printf("mal_catalog_live_smoke: OK missing id -> confirmed absent\n");
  return 0;
}
