// manga_nhentai_live_smoke.cpp — SHIGOKU_LIVE=1 hits the real
// nhentai.net end-to-end — search → gallery-as-oneshot chapter → pages
// (which exercises the cdn pool + round-robin) → GET of the first page's
// bytes — and prints what it found, including whether the transport had to
// escalate onto the libcurl-impersonate seam (the edge's TLS scoring
// fluctuates between probes). Skips (exit 0) without the env var,
// same convention as the other smokes.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/http.hpp"
#include "../src/manga/nhentai.hpp"

namespace {

void print_error(const char* stage, const shigoku::ProviderError& e) {
  std::fprintf(stderr, "nh_live_smoke: FAIL %s kind=%d status=%u detail=%s\n",
               stage, static_cast<int>(e.kind), e.status, e.detail.c_str());
}

}  // namespace

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("nh_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku::manga;

  auto nh = Nhentai::create();
  if (!nh.has_value()) {
    print_error("create", nh.error());
    return 1;
  }

  auto results = nh->search("touhou");
  if (!results.has_value()) {
    print_error("search", results.error());
    return 1;
  }
  if (results->empty()) {
    std::fprintf(stderr, "nh_live_smoke: FAIL zero search results\n");
    return 1;
  }
  const MdManga& first = results->front();
  std::printf("search: %zu results; \"%s\" id=%s thumb=%s\n", results->size(),
              first.title.c_str(), first.id.c_str(),
              nh->cover_thumb_url(first).c_str());

  auto chapters = nh->chapters(first.id, "en");
  if (!chapters.has_value()) {
    print_error("chapters", chapters.error());
    return 1;
  }
  if (chapters->size() != 1) {
    std::fprintf(stderr, "nh_live_smoke: FAIL expected ONE oneshot row, got %zu\n",
                 chapters->size());
    return 1;
  }
  std::printf("chapters: oneshot \"%s\" lang=%s pages=%u publish=%s\n",
              chapters->front().title.c_str(), chapters->front().lang.c_str(),
              chapters->front().pages, chapters->front().publish_at.c_str());

  auto pages = nh->pages(chapters->front().id, false);
  if (!pages.has_value()) {
    print_error("pages", pages.error());
    return 1;
  }
  if (pages->units.empty() ||
      pages->units.size() != chapters->front().pages) {
    std::fprintf(stderr, "nh_live_smoke: FAIL %zu units vs %u advertised pages\n",
                 pages->units.size(), chapters->front().pages);
    return 1;
  }
  std::printf("pages: %zu units, first=%s hint=%s\n", pages->units.size(),
              pages->units.front().url.c_str(),
              pages->units.front().name_hint.c_str());

  // The DoD bite: real page bytes off the image CDN — through a PLAIN client,
  // exactly the one mapp's fetch core uses (the CDN accepted plain TLS in
  // every probe; the day it fingerprints, this is the line that goes red).
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
    std::fprintf(stderr, "nh_live_smoke: FAIL suspiciously small page body\n");
    return 1;
  }

  std::printf("nh_live_smoke: PASS\n");
  return 0;
}
