// manga_mangahub_live_smoke.cpp — SHIGOKU_LIVE=1 hits the real MangaHub API
// through the impersonate seam: search → chapter list → pages of a
// fractional chapter → a plain GET of the first page and the cover. Skips
// (exit 0) without the env var.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/http.hpp"
#include "../src/manga/mangahub.hpp"

namespace {

void print_error(const char* stage, const shigoku::ProviderError& e) {
  std::fprintf(stderr, "mh_live_smoke: FAIL %s kind=%d status=%u detail=%s\n", stage,
               static_cast<int>(e.kind), e.status, e.detail.c_str());
}

bool plain_get(const shigoku::http::Client& client, const std::string& url,
               const std::string& referer, const char* stage) {
  shigoku::http::Request req;
  req.method = shigoku::http::Method::Get;
  req.url = url;
  req.user_agent = shigoku::manga::kUserAgent;
  req.accept = shigoku::http::Accept::Any2xx;
  if (!referer.empty()) req.extra_headers.push_back({"Referer", referer});
  auto body = client.fetch(req);
  if (!body.has_value()) {
    print_error(stage, body.error());
    return false;
  }
  std::printf("%s: %zu bytes\n", stage, body->size());
  if (body->size() < 1024) {
    std::fprintf(stderr, "mh_live_smoke: FAIL %s suspiciously small body\n", stage);
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("mh_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku::manga;

  auto mh = MangaHub::create(false);
  if (!mh.has_value()) {
    print_error("create", mh.error());
    return 1;
  }

  auto results = mh->search("Frieren: Beyond Journey's End");
  if (!results.has_value()) {
    print_error("search", results.error());
    return 1;
  }
  const MdManga* frieren = nullptr;
  for (const auto& m : *results) {
    if (m.id == "sousou-no-frieren") frieren = &m;
  }
  if (frieren == nullptr) {
    std::fprintf(stderr, "mh_live_smoke: FAIL sousou-no-frieren not among %zu hits\n",
                 results->size());
    return 1;
  }
  std::printf("search: %zu hits; \"%s\" cover=%s\n", results->size(), frieren->title.c_str(),
              mh->cover_thumb_url(*frieren).c_str());

  auto chapters = mh->chapters(frieren->id, "en");
  if (!chapters.has_value()) {
    print_error("chapters", chapters.error());
    return 1;
  }
  const MdChapter* half = nullptr;
  for (const auto& c : *chapters) {
    if (c.chapter == "93.5") half = &c;
  }
  if (chapters->size() < 150 || half == nullptr) {
    std::fprintf(stderr, "mh_live_smoke: FAIL %zu chapters, 93.5 %s\n", chapters->size(),
                 half != nullptr ? "present" : "missing");
    return 1;
  }
  std::printf("chapters: %zu, first \"%s\" (%s), last %s\n", chapters->size(),
              chapters->front().title.c_str(), chapters->front().publish_at.c_str(),
              chapters->back().chapter.c_str());

  auto pages = mh->pages(half->id, false);
  if (!pages.has_value()) {
    print_error("pages", pages.error());
    return 1;
  }
  if (pages->units.empty()) {
    std::fprintf(stderr, "mh_live_smoke: FAIL no page units\n");
    return 1;
  }
  std::printf("pages: %zu units, first=%s\n", pages->units.size(),
              pages->units.front().url.c_str());

  auto client = shigoku::http::Client::create();
  if (!client.has_value()) {
    print_error("client", client.error());
    return 1;
  }
  if (!plain_get(*client, pages->units.front().url, pages->units.front().referer,
                 "page-get") ||
      !plain_get(*client, mh->cover_thumb_url(*frieren), "", "cover-get")) {
    return 1;
  }

  std::printf("mh_live_smoke: PASS\n");
  return 0;
}
