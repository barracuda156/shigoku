// catalog_live_smoke.cpp — the dispatcher in auto mode
// against the live network, with the production backends. Whichever source
// answers is fine — the point is that a search and a Discover page come back
// at all, and the smoke prints who served them. Not a ctest; gated on
// SHIGOKU_LIVE=1 (skips otherwise). SHIGOKU_MAL_CLIENT_ID overrides the
// baked id.

#include <cstdio>
#include <cstdlib>

#include "../src/catalog.hpp"
#include "../src/mal_catalog.hpp"

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("catalog_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;

  auto client = http::Client::create();
  if (!client.has_value()) {
    std::fprintf(stderr, "catalog_live_smoke: FAIL client::create\n");
    return 1;
  }

  const char* env_id = std::getenv("SHIGOKU_MAL_CLIENT_ID");
  const std::string client_id = mal_catalog::effective_client_id(env_id != nullptr ? env_id : "");
  std::optional<catalog::Backend> mal;
  if (!client_id.empty()) mal = catalog::mal_backend(*client, client_id);
  catalog::Catalog cat(catalog::Mode::Auto, catalog::anilist_backend(*client), std::move(mal));

  auto fail = [](const char* what, const ProviderError& e) {
    std::fprintf(stderr, "catalog_live_smoke: FAIL %s kind=%d status=%u detail=%s\n", what,
                 static_cast<int>(e.kind), e.status, e.detail.c_str());
    return 1;
  };

  auto page = cat.search("frieren", 1);
  if (!page.has_value()) return fail("search", page.error());
  if (page->entries.empty()) {
    std::fprintf(stderr, "catalog_live_smoke: FAIL no hits for 'frieren'\n");
    return 1;
  }
  const Enrichment& first = page->entries[0];
  std::printf("catalog_live_smoke: OK search via %s: %zu hit(s), first: anilist=%lld mal=%lld %s\n",
              std::string(catalog::source_name(page->source)).c_str(), page->entries.size(),
              static_cast<long long>(first.anilist_id),
              static_cast<long long>(first.mal_id.value_or(0)), first.title_romaji.c_str());

  auto feed = cat.discover(DiscoverAxis::Trending, 1, DiscoverFilters{});
  if (!feed.has_value()) return fail("discover", feed.error());
  if (feed->entries.empty()) {
    std::fprintf(stderr, "catalog_live_smoke: FAIL empty Trending feed\n");
    return 1;
  }
  std::printf("catalog_live_smoke: OK discover via %s: %zu row(s), first: %s\n",
              std::string(catalog::source_name(feed->source)).c_str(), feed->entries.size(),
              feed->entries[0].title_romaji.c_str());

  // Refresh-on-view of the first hit, through the same latch.
  auto e = cat.enrich(first.anilist_id, first.mal_id);
  if (!e.has_value()) return fail("enrich", e.error());
  if (!e->has_value()) {
    std::fprintf(stderr, "catalog_live_smoke: FAIL enrich says no such show\n");
    return 1;
  }
  const auto st = cat.status();
  std::printf("catalog_live_smoke: OK enrich keeps id %lld; active=%s mal_available=%d\n",
              static_cast<long long>((*e)->anilist_id),
              std::string(catalog::source_name(st.active)).c_str(), st.mal_available ? 1 : 0);
  return 0;
}
