// manga_mangadex_live_smoke.cpp — SHIGOKU_LIVE=1 hits the real
// api.mangadex.org end-to-end — search → chapters (full offset loop, dedupe,
// external-drop) → at_home for the first chapter — and prints what it found.
// Skips (exit 0) without the env var, same convention as the anime smokes
// (scripts/live_smoke.sh sets it).
//
// Known-good target: Chainsaw Man (the committed fixture subject), which
// pins the ja-ro title fallback, links.al/.mal, and external viz.com
// chapters all at once.

#include <cstdio>
#include <cstdlib>

#include "../src/manga/mangadex.hpp"

namespace {
constexpr const char* kChainsawManUuid = "a77742b1-befd-49a4-bff5-1ad4e6b0ef7b";

void print_error(const char* stage, const shigoku::ProviderError& e) {
  std::fprintf(stderr, "manga_live_smoke: FAIL %s kind=%d status=%u detail=%s\n",
               stage, static_cast<int>(e.kind), e.status, e.detail.c_str());
}
}  // namespace

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("manga_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku::manga;

  auto md = MangaDex::create();
  if (!md.has_value()) {
    print_error("create", md.error());
    return 1;
  }

  auto results = md->search("chainsaw man");
  if (!results.has_value()) {
    print_error("search", results.error());
    return 1;
  }
  const MdManga* csm = nullptr;
  for (const auto& m : *results) {
    if (m.id == kChainsawManUuid) csm = &m;
  }
  if (csm == nullptr) {
    std::fprintf(stderr, "manga_live_smoke: FAIL Chainsaw Man not in %zu results\n",
                 results->size());
    return 1;
  }
  std::printf("search: %zu results; \"%s\" year=%d status=%s al=%lld mal=%lld cover=%s\n",
              results->size(), csm->title.c_str(), csm->year.value_or(0),
              csm->status.c_str(),
              static_cast<long long>(csm->al_id.value_or(0)),
              static_cast<long long>(csm->mal_id.value_or(0)),
              csm->cover_filename.empty() ? "NONE" : "yes");

  auto chapters = md->chapters(csm->id, "en");
  if (!chapters.has_value()) {
    print_error("chapters", chapters.error());
    return 1;
  }
  if (chapters->empty()) {
    std::fprintf(stderr, "manga_live_smoke: FAIL chapter feed empty\n");
    return 1;
  }
  // Order proof: first < last in chapter_less, and no adjacent inversion.
  bool ordered = true;
  for (std::size_t i = 1; i < chapters->size(); ++i) {
    if (detail::chapter_less((*chapters)[i].chapter, (*chapters)[i - 1].chapter)) {
      ordered = false;
      break;
    }
  }
  std::printf("chapters: %zu deduped en chapters, first=\"%s\" last=\"%s\" ordered=%s\n",
              chapters->size(), chapters->front().chapter.c_str(),
              chapters->back().chapter.c_str(), ordered ? "yes" : "NO");
  if (!ordered) return 1;

  auto at_home = md->at_home(chapters->front().id);
  if (!at_home.has_value()) {
    print_error("at_home", at_home.error());
    return 1;
  }
  std::printf("at_home: node=%s pages=%zu saver=%zu first=%s\n",
              at_home->base_url.c_str(), at_home->data.size(),
              at_home->data_saver.size(),
              detail::page_url(*at_home, 0, false).c_str());

  std::printf("manga_live_smoke: PASS\n");
  return 0;
}
