// main.cpp — shigoku-manga bootstrap: paths (own "shigoku-manga" segment —
// the two apps must not be able to touch each other's state) → http client
// → MangaDex → shared Covers pipeline → manga::run. No CLI surface yet (a
// dispatch shaped like the anime CLI's arrives with the read/download flows
// it would drive); --version alone so packaging scripts can probe the
// binary.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../http.hpp"
#include "../paths.hpp"
#include "../tui/covers.hpp"
#include "dynasty.hpp"
#include "mangadex.hpp"
#include "mapp.hpp"
#include "mconfig.hpp"
#include "mstore.hpp"
#include "nhentai.hpp"
#include "viewer.hpp"
#include "weebcentral.hpp"

#ifndef SHIGOKU_VERSION
#define SHIGOKU_VERSION "0.0.0-dev"
#endif

namespace {
inline constexpr const char* kMangaSegment = "shigoku-manga";
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("shigoku-manga %s\n", SHIGOKU_VERSION);
    return 0;
  }

  const auto paths = shigoku::resolve_paths(kMangaSegment);
  if (!paths.has_value()) {
    std::fprintf(stderr, "shigoku-manga: HOME is not set and no XDG dirs\n");
    return 1;
  }
  shigoku::ensure_dirs(*paths);

  // MangaDex serves no challenge page, so the anime side's nghttp2 tripwire
  // (assert_http2_available) is not a boot gate here: a curl without nghttp2
  // degrades to HTTP/1.1 and everything still works.
  auto mangadex = shigoku::manga::MangaDex::create();
  auto weebcentral = shigoku::manga::WeebCentral::create();
  auto dynasty = shigoku::manga::Dynasty::create();
  auto nhentai = shigoku::manga::Nhentai::create();
  if (!mangadex.has_value() || !weebcentral.has_value() || !dynasty.has_value() ||
      !nhentai.has_value()) {
    std::fprintf(stderr, "shigoku-manga: http client init failed\n");
    return 1;
  }

  // Covers get their own client (the pipeline seam wants a const Client& it
  // can use from the cover worker while MangaDex's client serves API calls).
  auto cover_client = shigoku::http::Client::create();
  if (!cover_client.has_value()) {
    std::fprintf(stderr, "shigoku-manga: http client init failed\n");
    return 1;
  }
  shigoku::tui::Covers covers(shigoku::tui::make_http_fetch(*cover_client),
                              paths->cache + "/covers");

  // The read flow: its own client for page transfers (one blocking worker
  // per client, the Covers precedent), config-lite, and the durable chapter
  // store under the DATA dir (offline-first — downloads, not cache).
  auto pages_client = shigoku::http::Client::create();
  if (!pages_client.has_value()) {
    std::fprintf(stderr, "shigoku-manga: http client init failed\n");
    return 1;
  }
  const std::string config_path = shigoku::config_file_path(*paths);
  shigoku::manga::save_default_if_missing(config_path);

  // AniList sync: its own client (one blocking worker per client) and its own
  // auth.json under the manga config dir — same account as the anime app,
  // never the same file.
  auto sync_client = shigoku::http::Client::create();
  if (!sync_client.has_value()) {
    std::fprintf(stderr, "shigoku-manga: http client init failed\n");
    return 1;
  }

  // manga.db lives beside the chapter library under the DATA dir. A store
  // that refuses to open is fatal, not degraded: silently dropping every read
  // mark for a session is worse than not starting.
  auto store = shigoku::manga::MangaStore::open(paths->data + "/manga.db");
  if (!store.has_value()) {
    std::fprintf(stderr, "shigoku-manga: %s\n",
                 shigoku::manga::mstore_error_text(store.error()).c_str());
    return 1;
  }

  shigoku::manga::MgDeps deps;
  deps.config = shigoku::manga::load_manga_config(config_path);
  // Registry-lite: every compiled-in source, config picks the active one
  // (unknown key degrades to the first eligible; nsfw gated). The eligible
  // list rides into deps so `s` can cycle it.
  const std::vector<const shigoku::manga::MangaSource*> sources = {
      &mangadex.value(), &weebcentral.value(), &dynasty.value(),
      &nhentai.value()};
  deps.source = shigoku::manga::pick_source(sources, deps.config.source,
                                            deps.config.nsfw_sources);
  if (deps.source == nullptr) {
    std::fprintf(stderr, "shigoku-manga: no manga source enabled\n");
    return 1;
  }
  deps.sources =
      shigoku::manga::eligible_sources(sources, deps.config.nsfw_sources);
  deps.config_path = config_path;
  deps.covers = &covers;
  deps.pages_client = &pages_client.value();
  deps.pages_root = paths->data + "/library";
  deps.legacy_pages_root = paths->data + "/chapters";  // legacy layout, read-only.
  deps.viewer_builtin = shigoku::manga::resolve_builtin_viewer(argv[0]);
  deps.store = &store.value();
  deps.sync_client = &sync_client.value();
  deps.auth_file = shigoku::auth_file_path(*paths);
  return shigoku::manga::run(deps);
}
