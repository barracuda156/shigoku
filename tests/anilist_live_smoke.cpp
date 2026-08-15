// anilist_live_smoke.cpp — P3 DoD: "SHIGOKU_LIVE=1 smoke returns hits for
// 'frieren'". Not part of the offline ctest suite (no network in CI-shaped
// runs) — invoked only via scripts/live_smoke.sh, which sets SHIGOKU_LIVE=1.
// Skips (exit 0) when the env var is absent so an accidental direct run
// doesn't hit the network or fail a build.

#include <cstdio>
#include <cstdlib>

#include "../src/anilist.hpp"

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("anilist_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;

  auto client = http::Client::create();
  if (!client.has_value()) {
    std::fprintf(stderr, "anilist_live_smoke: FAIL client::create\n");
    return 1;
  }

  auto page = anilist::search(*client, "frieren", 1);
  if (!page.has_value()) {
    std::fprintf(stderr, "anilist_live_smoke: FAIL search kind=%d status=%u detail=%s\n",
                 static_cast<int>(page.error().kind), page.error().status,
                 page.error().detail.c_str());
    return 1;
  }
  if (page->entries.empty()) {
    std::fprintf(stderr, "anilist_live_smoke: FAIL no hits for 'frieren'\n");
    return 1;
  }

  std::printf("anilist_live_smoke: OK %zu hit(s), first: id=%lld title=%s\n",
             page->entries.size(), static_cast<long long>(page->entries[0].anilist_id),
             page->entries[0].title_romaji.c_str());

  // The manga half of the same surface: the widened documents have to be
  // accepted by the real API, not just by the golden tests. Read-only — the
  // list calls that write need a token and are the user's account, not a
  // smoke's business.
  auto manga = anilist::search(*client, "berserk", 1, anilist::MediaKind::Manga);
  if (!manga.has_value()) {
    std::fprintf(stderr, "anilist_live_smoke: FAIL manga search kind=%d status=%u detail=%s\n",
                 static_cast<int>(manga.error().kind), manga.error().status,
                 manga.error().detail.c_str());
    return 1;
  }
  if (manga->entries.empty()) {
    std::fprintf(stderr, "anilist_live_smoke: FAIL no manga hits for 'berserk'\n");
    return 1;
  }

  const std::int64_t manga_id = manga->entries[0].anilist_id;
  auto by_id = anilist::enrich(*client, manga_id, anilist::MediaKind::Manga);
  if (!by_id.has_value()) {
    std::fprintf(stderr, "anilist_live_smoke: FAIL manga by-id kind=%d status=%u detail=%s\n",
                 static_cast<int>(by_id.error().kind), by_id.error().status,
                 by_id.error().detail.c_str());
    return 1;
  }
  if (!by_id->has_value()) {
    std::fprintf(stderr, "anilist_live_smoke: FAIL manga id %lld has no Media\n",
                 static_cast<long long>(manga_id));
    return 1;
  }

  std::printf("anilist_live_smoke: OK manga %zu hit(s), first: id=%lld title=%s\n",
             manga->entries.size(), static_cast<long long>(manga_id),
             (*by_id)->title_romaji.c_str());
  return 0;
}
