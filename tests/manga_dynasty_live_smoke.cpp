// manga_dynasty_live_smoke.cpp — SHIGOKU_LIVE=1 hits the real
// dynasty-scans.com end-to-end — search kase → series chapter list → pages of
// the first chapter → GET of the first page's bytes (a WebP body, riding
// the webp decode seam) — and prints what it found. Skips (exit 0) without the env
// var, same convention as the other smokes.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/http.hpp"
#include "../src/manga/dynasty.hpp"

namespace {

void print_error(const char* stage, const shigoku::ProviderError& e) {
  std::fprintf(stderr, "dy_live_smoke: FAIL %s kind=%d status=%u detail=%s\n",
               stage, static_cast<int>(e.kind), e.status, e.detail.c_str());
}

}  // namespace

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("dy_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku::manga;

  auto dyn = Dynasty::create();
  if (!dyn.has_value()) {
    print_error("create", dyn.error());
    return 1;
  }

  auto results = dyn->search("kase");
  if (!results.has_value()) {
    print_error("search", results.error());
    return 1;
  }
  if (results->empty()) {
    std::fprintf(stderr, "dy_live_smoke: FAIL zero search results\n");
    return 1;
  }
  const MdManga* series = nullptr;
  for (const auto& m : *results) {
    if (m.id == "kase_san") series = &m;
  }
  if (series == nullptr) series = &results->front();
  std::printf("search: %zu results; \"%s\" id=%s\n", results->size(),
              series->title.c_str(), series->id.c_str());

  auto chapters = dyn->chapters(series->id, "en");
  if (!chapters.has_value()) {
    print_error("chapters", chapters.error());
    return 1;
  }
  if (chapters->empty()) {
    std::fprintf(stderr, "dy_live_smoke: FAIL chapter list empty\n");
    return 1;
  }
  std::printf("chapters: %zu rows, first=\"%s\" (%s) \"%s\" last=\"%s\" (%s)\n",
              chapters->size(), chapters->front().chapter.c_str(),
              chapters->front().id.c_str(), chapters->front().title.c_str(),
              chapters->back().chapter.c_str(), chapters->back().id.c_str());

  auto pages = dyn->pages(chapters->front().id, false);
  if (!pages.has_value()) {
    print_error("pages", pages.error());
    return 1;
  }
  if (pages->units.empty()) {
    std::fprintf(stderr, "dy_live_smoke: FAIL zero page units\n");
    return 1;
  }
  std::printf("pages: %zu units, first=%s hint=%s\n", pages->units.size(),
              pages->units.front().url.c_str(),
              pages->units.front().name_hint.c_str());

  // The DoD bite: real page bytes, and they must be a WebP body (the RIFF
  // magic) — that is what routes through the webp decode seam on read.
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
  const bool webp = page->size() >= 12 && (*page)[0] == 'R' && (*page)[1] == 'I' &&
                    (*page)[2] == 'F' && (*page)[3] == 'F' && (*page)[8] == 'W' &&
                    (*page)[9] == 'E' && (*page)[10] == 'B' && (*page)[11] == 'P';
  std::printf("page-get: %zu bytes, webp=%s\n", page->size(), webp ? "yes" : "no");
  if (page->size() < 1024) {
    std::fprintf(stderr, "dy_live_smoke: FAIL suspiciously small page body\n");
    return 1;
  }
  if (!webp) {
    std::fprintf(stderr, "dy_live_smoke: FAIL first page is not a WebP body\n");
    return 1;
  }

  std::printf("dy_live_smoke: PASS\n");
  return 0;
}
