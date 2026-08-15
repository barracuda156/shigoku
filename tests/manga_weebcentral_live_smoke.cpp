// manga_weebcentral_live_smoke.cpp — SHIGOKU_LIVE=1 hits the real
// weebcentral.com end-to-end — search berserk → full chapter list → images
// for the first chapter → GET of the first page's bytes (proving the
// varying per-series CDN host accepts our UA with no referer requirement) —
// and prints what it found. Skips (exit 0) without the env var, same
// convention as the other smokes.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/http.hpp"
#include "../src/manga/weebcentral.hpp"

namespace {

void print_error(const char* stage, const shigoku::ProviderError& e) {
  std::fprintf(stderr, "wc_live_smoke: FAIL %s kind=%d status=%u detail=%s\n",
               stage, static_cast<int>(e.kind), e.status, e.detail.c_str());
}

}  // namespace

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("wc_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku::manga;

  auto wc = WeebCentral::create();
  if (!wc.has_value()) {
    print_error("create", wc.error());
    return 1;
  }

  auto results = wc->search("berserk");
  if (!results.has_value()) {
    print_error("search", results.error());
    return 1;
  }
  if (results->empty()) {
    std::fprintf(stderr, "wc_live_smoke: FAIL zero search results\n");
    return 1;
  }
  const MdManga* berserk = nullptr;
  for (const auto& m : *results) {
    if (m.title == "Berserk") berserk = &m;
  }
  if (berserk == nullptr) berserk = &results->front();
  std::printf("search: %zu results; \"%s\" id=%s year=%d status=%s\n",
              results->size(), berserk->title.c_str(), berserk->id.c_str(),
              berserk->year.value_or(0), berserk->status.c_str());
  std::printf("cover:  %s\n", wc->cover_thumb_url(*berserk).c_str());

  auto chapters = wc->chapters(berserk->id, "en");
  if (!chapters.has_value()) {
    print_error("chapters", chapters.error());
    return 1;
  }
  if (chapters->empty()) {
    std::fprintf(stderr, "wc_live_smoke: FAIL chapter list empty\n");
    return 1;
  }
  std::printf("chapters: %zu rows, first=\"%s\" (%s) last=\"%s\"\n",
              chapters->size(), chapters->front().chapter.c_str(),
              chapters->front().id.c_str(), chapters->back().chapter.c_str());

  auto pages = wc->pages(chapters->front().id, false);
  if (!pages.has_value()) {
    print_error("pages", pages.error());
    return 1;
  }
  if (pages->units.empty()) {
    std::fprintf(stderr, "wc_live_smoke: FAIL zero page units\n");
    return 1;
  }
  std::printf("pages: %zu units, first=%s hint=%s\n", pages->units.size(),
              pages->units.front().url.c_str(),
              pages->units.front().name_hint.c_str());

  // The DoD bite: the per-series CDN host must serve actual page bytes to
  // our UA (referer carried, per the PageUnit contract).
  auto client = shigoku::http::Client::create();
  if (!client.has_value()) {
    print_error("client", client.error());
    return 1;
  }
  shigoku::http::Request req;
  req.method = shigoku::http::Method::Get;
  req.url = pages->units.front().url;
  req.user_agent = kUserAgent;
  req.accept = shigoku::http::Accept::Any2xx;
  if (!pages->units.front().referer.empty()) {
    req.extra_headers.push_back({"Referer", pages->units.front().referer});
  }
  auto page = client->fetch(req);
  if (!page.has_value()) {
    print_error("page-get", page.error());
    return 1;
  }
  std::printf("page-get: %zu bytes\n", page->size());
  if (page->size() < 1024) {
    std::fprintf(stderr, "wc_live_smoke: FAIL suspiciously small page body\n");
    return 1;
  }

  std::printf("wc_live_smoke: PASS\n");
  return 0;
}
