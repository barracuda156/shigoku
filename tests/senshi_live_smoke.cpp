// senshi_live_smoke.cpp — P4 DoD: "SHIGOKU_LIVE=1 script resolves a known
// MAL id end-to-end and prints the m3u8 URL." Not part of the offline ctest
// suite (no network in CI-shaped runs) — invoked only via
// scripts/live_smoke.sh, which sets SHIGOKU_LIVE=1.
// Skips (exit 0) when the env var is absent so an accidental direct run
// doesn't hit the network or fail a build.
//
// Mirrors sabigoku tests/senshi_live.rs: search -> episodes -> resolve,
// same known-good target (Sousou no Frieren S1, MAL id 52991).

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "../src/crypto.hpp"
#include "../src/http.hpp"
#include "../src/senshi.hpp"


int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("senshi_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  using namespace shigoku;
  using namespace shigoku::senshi;

  auto provider = Senshi::create();
  if (!provider.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL Senshi::create kind=%d status=%u detail=%s\n",
                 static_cast<int>(provider.error().kind), provider.error().status,
                 provider.error().detail.c_str());
    return 1;
  }

  SearchOptions opts;
  opts.translation = Translation::Sub;
  opts.limit = kSearchPageSize;
  opts.page = 1;

  auto hits = provider->search("frieren", opts);
  if (!hits.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL search kind=%d status=%u detail=%s\n",
                 static_cast<int>(hits.error().kind), hits.error().status,
                 hits.error().detail.c_str());
    return 1;
  }
  // The catalogue's Frieren entry moves between seasons; any hit keyed by a
  // MAL id proves the search shape, and the rest of the walk runs on it.
  if (hits->empty()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL no search results for frieren\n");
    return 1;
  }
  const SearchHit& s1 = hits->front();
  if (!s1.mal_id.has_value() || s1.provider_id != std::to_string(*s1.mal_id)) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL hit is not MAL-keyed (id=%s)\n",
                 s1.provider_id.c_str());
    return 1;
  }
  const std::string show_id = s1.provider_id;
  auto eps = provider->episodes(show_id, Translation::Sub, std::nullopt);
  if (!eps.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL episodes kind=%d status=%u detail=%s\n",
                 static_cast<int>(eps.error().kind), eps.error().status,
                 eps.error().detail.c_str());
    return 1;
  }
  if (eps->empty() || eps->front() != "1") {
    std::fprintf(stderr, "senshi_live_smoke: FAIL episode grid shape (count=%zu first=%s)\n",
                 eps->size(), eps->empty() ? "<none>" : eps->front().c_str());
    return 1;
  }

  auto link = provider->resolve(show_id, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL resolve kind=%d status=%u detail=%s\n",
                 static_cast<int>(link.error().kind), link.error().status,
                 link.error().detail.c_str());
    return 1;
  }
  if (link->url.rfind("http", 0) != 0 || !link->cloaked_segments) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL stream link shape (url=%s cloaked=%d)\n",
                 link->url.c_str(), link->cloaked_segments);
    return 1;
  }

  // The envelope: fetch the master the way the proxy would and open it with
  // the link's key — proves the scrape (or the baked copy) still matches the
  // site's current deploy.
  if (!link->playlist_cipher.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL no playlist cipher on the link\n");
    return 1;
  }
  auto client = http::Client::create();
  if (!client.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL http client\n");
    return 1;
  }
  http::Request req;
  req.method = http::Method::Get;
  req.url = link->url;
  req.user_agent = http::kBrowserUserAgent;
  req.extra_headers.push_back({"Referer", kStreamReferer});
  req.extra_headers.push_back({"Origin", kApi});
  req.accept = http::Accept::OkOnly;
  auto body = client->fetch(req);
  if (!body.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL master fetch kind=%d status=%u\n",
                 static_cast<int>(body.error().kind), body.error().status);
    return 1;
  }
  const std::string_view raw(reinterpret_cast<const char*>(body->data()), body->size());
  const std::string_view magic = link->playlist_cipher->magic;
  if (raw.substr(0, magic.size()) != magic) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL master is not enveloped (head=%.16s)\n",
                 raw.data());
    return 1;
  }
  auto plain = crypto::open_b64_gcm(raw.substr(magic.size()), link->playlist_cipher->key);
  if (!plain.has_value()) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL master did not decrypt (key rotated?)\n");
    return 1;
  }
  const std::string_view text(reinterpret_cast<const char*>(plain->data()), plain->size());
  if (text.rfind("#EXTM3U", 0) != 0) {
    std::fprintf(stderr, "senshi_live_smoke: FAIL decrypted master is not a playlist\n");
    return 1;
  }
  std::printf("senshi_live_smoke: OK %zu hit(s), %zu episode(s), url=%s, master decrypts (%zu B)%s\n",
              hits->size(), eps->size(), link->url.c_str(), plain->size(),
              link->sub_url.has_value() ? ", softsub" : "");
  return 0;
}
