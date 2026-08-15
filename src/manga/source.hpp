// source.hpp — the multi-source seam, extracted from exactly the MangaDex
// surface mapp calls. One abstract class + the page unit the fetch core
// consumes; a source implements ~4 methods and the rest of the app (search
// UI, chapter list, fetch-to-cache, viewer, library layout) never learns
// which site it is talking to.
//
// PageUnit is the referer-carrying page unit: every page is an absolute URL
// plus the Referer some sites demand plus a filename hint for the on-disk
// extension. The source resolves everything site-specific (MangaDex's
// at-home grant + data-saver fallback, a scraper's <img> list) into this
// shape; rotation/re-scrape is the refetch callback in pages.hpp, whose
// semantics each source keeps for itself.
//
// Registry-lite (v1 = one ACTIVE source at a time): main constructs every
// compiled-in source, pick_source() resolves the config `source` key against
// it — honoring the `nsfw_sources` gate, an nsfw source is invisible unless
// opted into — and MgDeps carries the winner plus the eligible_sources()
// cycle list. In-app switching (`s`) walks the list, re-arms the search
// surface, and persists the key.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "../error.hpp"
#include "../result.hpp"
#include "model.hpp"

namespace shigoku::manga {

// The identifying UA every manga HTTP call sends (MangaDex API policy asks
// for one — browser UAs are what they throttle; the other sources accept it
// or a per-source override becomes an attr the day one objects).
inline constexpr const char* kUserAgent = "shigoku-manga/0.1";

// One page to fetch: absolute URL (the ORCHESTRATOR guards it — the core
// never re-guards, the pages.hpp contract), the Referer to send ("" = none),
// and the source-side filename whose extension names the file on disk.
struct PageUnit {
  std::string url;
  std::string referer;
  std::string name_hint;
  friend bool operator==(const PageUnit&, const PageUnit&) = default;
};

// A chapter's worth of pages, in reading order.
struct PageSet {
  std::vector<PageUnit> units;
  friend bool operator==(const PageSet&, const PageSet&) = default;
};

// A manga source. All operations are blocking (worker-thread use, A1) and
// const (workers share one instance across threads; implementations keep
// their http::Client's one-blocking-user-per-worker discipline).
class MangaSource {
 public:
  virtual ~MangaSource() = default;

  // Id prefix ("md", "wc", "dy", "nh") — stable forever, it is the store
  // scoping (scoped_id below) and never shown to the user.
  [[nodiscard]] virtual std::string_view key() const = 0;
  // Display name for status bars / hints ("MangaDex").
  [[nodiscard]] virtual std::string_view name() const = 0;
  // An nsfw source is filtered out of pick_source unless the config gate
  // (`nsfw_sources`) opted in.
  [[nodiscard]] virtual bool nsfw() const { return false; }

  [[nodiscard]] virtual Result<std::vector<MdManga>, ProviderError> search(
      std::string_view query) const = 0;

  // The full deduped chapter list for one manga, sorted in reading order.
  [[nodiscard]] virtual Result<std::vector<MdChapter>, ProviderError> chapters(
      std::string_view manga_id, std::string_view lang) const = 0;

  // Resolve a chapter to its fetchable pages. Called per read (grants
  // expire; scrapes go stale) — also the rotation refetch. data_saver is
  // advisory: sources without a compressed variant ignore it.
  [[nodiscard]] virtual Result<PageSet, ProviderError> pages(
      std::string_view chapter_id, bool data_saver) const = 0;

  // Absolute cover-thumb URL for the shared Covers::load path; "" = none.
  [[nodiscard]] virtual std::string cover_thumb_url(const MdManga& m) const = 0;
};

// "{key}:{native_id}" — the only id spelling that may enter store space. The
// md-specific scoped_id() in mangadex.hpp is this, pre-applied.
[[nodiscard]] inline std::string scoped_id(const MangaSource& s,
                                           std::string_view native) {
  std::string out(s.key());
  out += ':';
  out += native;
  return out;
}

// The in-app cycle list (`s`): every source the nsfw gate admits, in
// registry order.
[[nodiscard]] inline std::vector<const MangaSource*> eligible_sources(
    const std::vector<const MangaSource*>& sources, bool allow_nsfw) {
  std::vector<const MangaSource*> out;
  for (const MangaSource* s : sources) {
    if (s == nullptr || (s->nsfw() && !allow_nsfw)) continue;
    out.push_back(s);
  }
  return out;
}

// The active source: exact key match wins; otherwise the first eligible
// entry (nullptr only for an empty/fully-gated list). An nsfw source with
// allow_nsfw == false is invisible — it can neither match nor fall back.
[[nodiscard]] inline const MangaSource* pick_source(
    const std::vector<const MangaSource*>& sources, std::string_view key,
    bool allow_nsfw) {
  const MangaSource* fallback = nullptr;
  for (const MangaSource* s : sources) {
    if (s == nullptr || (s->nsfw() && !allow_nsfw)) continue;
    if (s->key() == key) return s;
    if (fallback == nullptr) fallback = s;
  }
  return fallback;
}

}  // namespace shigoku::manga
