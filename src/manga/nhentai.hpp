// nhentai.hpp — the nhentai source, the fourth source behind the
// multi-source seam and the first nsfw one: nsfw() is true, so the registry
// gate (`nsfw_sources` in config.json, default off) makes it invisible
// everywhere until the user opts in — pick_source can neither match nor
// fall back to it. It is a provider row in shigoku-manga, not a separate
// binary. Same layout law as the other three: pure parsers + URL builders
// as free functions in their own namespace (`nh` — NOT `detail`, which md
// owns), transport as a small class owning an http::Client.
//
// Site pins (probed live, re-probed):
//  - The v1 API is DEAD (403 "Use new API https://nhentai.net/api/v2/docs")
//    — every pre-2026 client recipe is stale. v2 is a documented FastAPI
//    (openapi spec captured with the fixtures):
//      GET /api/v2/search?query={q}&page={n} → {result[], num_pages, …}
//      GET /api/v2/galleries/{id}            → gallery detail incl. pages[]
//      GET /api/v2/cdn                       → {image_servers[], thumb_servers[]}
//    Reads are unauthenticated (auth/captcha/pow endpoints exist for WRITE
//    paths only; the API also documents an "Authorization: Key" scheme — if
//    reads ever require it, that is one config row + one extra_headers line).
//  - TLS fingerprinting FLUCTUATES at the edge: probes have observed both
//    plain curl 403'ing everywhere while staged curl-impersonate passed, and
//    plain curl succeeding cleanly (200) on every endpoint AND both CDN
//    pools. So the posture is ADAPTIVE, not hardwired: requests go out plain
//    first, and a 403 flips this source's remaining traffic onto the
//    libcurl-impersonate seam (http::Request::fingerprint = Chrome, the same
//    target as anidbapp) — sticky for the process, one wasted request per
//    flip. With the dylib absent the escalated retry surfaces its own
//    provider-level error (which names the real fix: install
//    libcurl-impersonate). Page/thumb GETs ride mapp's plain clients (fetch
//    core + covers); both CDN pools accept plain TLS today — the day that
//    flips, PageSet grows a fingerprint field, not this file.
//  - Model mapping: gallery ≡ manga with ONE synthetic oneshot chapter
//    (chapter "", title "Oneshot" — the library dir becomes "oneshot [lang
//    id8]"). chapters() and pages() are the same GET /galleries/{id}; the
//    cdn pool is fetched once per process and cached (search primes it so
//    cover_thumb_url — a UI-thread call that must never fetch — can join
//    thumb URLs from the cache; primes-failed just means no covers).
//  - Per-page extensions VARY and paths are taken VERBATIM (live fixture
//    shows "thumb.jpg.webp" doubles) — never assume .webp, though most
//    pages are WebP and ride the webp decode seam.
//  - pages() picks its image server round-robin from the cdn pool, so the
//    rotation refetch (pages.hpp) lands on a DIFFERENT server for free.
//
// Ids are numeric (gallery id; media_id is a separate numeric namespace that
// only appears inside CDN paths). Trust boundary: ids are digit-vetted at
// parse time and re-vetted before splicing into request paths (they may
// later arrive from persisted store space rather than a fresh parse); every
// pages[].path is charset-vetted whole and its last segment must pass
// filename_safe or the WHOLE gallery parse fails (a page gap is worse than a
// failed chapter — the parse_at_home posture).
//
// The search `blacklisted` flag and the account tag blacklist are real
// nhentai features — v1 IGNORES them.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../error.hpp"
#include "../http.hpp"
#include "../result.hpp"
#include "model.hpp"
#include "source.hpp"

namespace shigoku::manga {

inline constexpr const char* kNhHost = "https://nhentai.net";
// Digit-run bounds for the two numeric id namespaces (sanity bounds, not site
// limits: live ids are 6–7 digits today).
inline constexpr std::size_t kNhIdMax = 10;
// Longest pages[].path we will splice into a URL (fixture max is ~40).
inline constexpr std::size_t kNhPathMax = 256;

// --- Pure helpers, exposed for the golden tests ------------------------------
namespace nh {

// The parse-time trust boundary for gallery/media ids: 1..kNhIdMax ASCII
// digits, nothing else — they become URL path segments and the library dir's
// [id8] anchor.
[[nodiscard]] bool id_shaped(std::string_view s);

// The vet for a CDN-relative pages[].path ("galleries/4111399/1.webp"):
// non-empty, ≤ kNhPathMax, chars [A-Za-z0-9._/-] only, no leading/trailing
// '/', no "//" or ".." runs, and the last segment (the on-disk filename
// authority) passes detail::filename_safe.
[[nodiscard]] bool path_shaped(std::string_view s);

// URL builders — exact-string tested, host injectable for the fixture server.
//
//   search_url:  {host}/api/v2/search?query={enc}&page={n}
//   gallery_url: {host}/api/v2/galleries/{id}
//   cdn_url:     {host}/api/v2/cdn
//   page_url:    {server}/{path}   (server pre-normalized, no trailing '/')
[[nodiscard]] std::string search_url(std::string_view host, std::string_view query,
                                     std::uint32_t page);
[[nodiscard]] std::string gallery_url(std::string_view host, std::string_view id);
[[nodiscard]] std::string cdn_url(std::string_view host);
[[nodiscard]] std::string page_url(std::string_view server, std::string_view path);

// "1786385298" (unix UTC seconds) → "2026-08-10T18:08:18+00:00" — the
// MdChapter.publish_at spelling whose lexicographic compare IS chronological.
// Non-positive input → "".
[[nodiscard]] std::string iso_from_unix(std::int64_t secs);

// The CDN server pools (parse_cdn). Entries normalized (trailing '/'
// stripped); non-http(s) entries dropped.
struct NhCdn {
  std::vector<std::string> image_servers;
  std::vector<std::string> thumb_servers;
  friend bool operator==(const NhCdn&, const NhCdn&) = default;
};

// One parsed gallery (parse_gallery) — the shared shape behind chapters()
// and pages().
struct NhGallery {
  std::string id;         // digits (vetted).
  std::string media_id;   // digits (vetted) — the CDN path namespace.
  std::string title;      // title.pretty → .english → .japanese → "".
  std::string lang;       // from the language tags; "" when none.
  std::uint32_t num_pages = 0;
  std::string publish_at;  // iso_from_unix(upload_date); "" when absent.
  std::string thumbnail;   // CDN-relative path; "" = none/vetoed.
  std::vector<std::string> pages;  // pages[].path VERBATIM, document order.
  friend bool operator==(const NhGallery&, const NhGallery&) = default;
};

// Parsers, pure over response bytes (fixtures: tests/fixtures/nhentai/).
//
// parse_search (search_page1.json): result[] → MdManga. id = the integer id
// as a string (row DROPPED if missing/ill-typed/ill-shaped); title =
// english_title → japanese_title → "" (rows carry no pretty variant);
// cover_filename = the thumbnail path when it passes path_shaped, else ""
// (row kept — a bad thumb loses the cover, not the gallery). Duplicate ids
// collapse to the first hit. year/status/description stay empty and
// al_id/mal_id stay nullopt — nh has no tracker links, so a future library
// sync has nothing to act on here. `blacklisted` is IGNORED for now. A
// missing/ill-typed result[] → Decode; an empty one → Ok(empty).
[[nodiscard]] Result<std::vector<MdManga>, ProviderError> parse_search(
    std::string_view raw);

// parse_cdn (cdn.json): image_servers[]/thumb_servers[] → NhCdn. Entries not
// starting http(s):// are dropped; trailing '/' stripped. An empty/missing
// image pool → Decode (pages are unfetchable); an empty thumb pool is fine
// (covers degrade to none).
[[nodiscard]] Result<NhCdn, ProviderError> parse_cdn(std::string_view raw);

// parse_gallery (gallery.json): the detail object → NhGallery. id and
// media_id are REQUIRED and vetted (Decode — without them nothing is
// fetchable/anchorable). title per the pretty-first ladder; lang = the first
// tags[] row of type "language" whose name is not "translated"/"rewrite",
// mapped english→en / japanese→ja / chinese→zh, any other name verbatim, none
// → "". pages[] paths are taken verbatim in document order, each vetted by
// path_shaped — ONE bad path fails the whole parse (a page gap is worse
// than a failed gallery). A missing pages[] parses as empty (chapters()
// doesn't need it; pages() refuses an empty set at its own layer).
[[nodiscard]] Result<NhGallery, ProviderError> parse_gallery(std::string_view raw);

// The gallery ≡ oneshot-chapter adapter: chapter "" (oneshot), title
// "Oneshot", pages = pages.size() when non-empty else num_pages, id/lang/
// publish_at carried over.
[[nodiscard]] MdChapter gallery_chapter(const NhGallery& g);

}  // namespace nh

// --- Transport ----------------------------------------------------------------

// Owns the client; every method is blocking (worker-thread use, A1). Same
// politeness floor as the other sources: 429 → sleep 1s, retry ONCE, a second
// 429 surfaces as RateLimited. The adaptive fingerprint posture: a 403
// retries once through the impersonate seam and STAYS escalated for the
// process; with_host() disables escalation (fixture servers are plain HTTP
// and must be able to 403 in tests without dylib traffic).
class Nhentai final : public MangaSource {
 public:
  [[nodiscard]] static Result<Nhentai, ProviderError> create();
  // Test seam: point at a loopback fixture server (escalation off). The cdn
  // fixture's pools point wherever the fixture says — a test server lists
  // itself and serves the pages too.
  [[nodiscard]] static Result<Nhentai, ProviderError> with_host(std::string host);

  [[nodiscard]] std::string_view key() const override { return "nh"; }
  [[nodiscard]] std::string_view name() const override { return "nhentai"; }
  // The whole point of the `nsfw_sources` gate.
  [[nodiscard]] bool nsfw() const override { return true; }

  // GET search_url (page 1); parse_search verbatim. On success, best-effort
  // primes the cdn cache so cover_thumb_url can answer from the UI thread.
  [[nodiscard]] Result<std::vector<MdManga>, ProviderError> search(
      std::string_view query) const override;

  // GET gallery_url → the ONE synthetic oneshot chapter. `lang` is ignored —
  // a gallery is a single release; its own language tag rides the row.
  [[nodiscard]] Result<std::vector<MdChapter>, ProviderError> chapters(
      std::string_view manga_id, std::string_view lang) const override;

  // GET gallery_url again (chapter_id == gallery id) + the cached cdn pool →
  // PageSet. Called per read — also the rotation refetch, which lands on the
  // next server of the pool (round-robin member counter). data_saver is
  // ignored: one quality.
  [[nodiscard]] Result<PageSet, ProviderError> pages(
      std::string_view chapter_id, bool data_saver) const override;

  // {thumb_server[0]}/{cover_filename} from the cdn CACHE — never fetches
  // (UI thread). "" until search() primed the pool (server [0] always: a
  // deterministic URL keeps the disk cover cache warm).
  [[nodiscard]] std::string cover_thumb_url(const MdManga& m) const override;

 private:
  // The cross-call mutable state (sticky escalation flag, rotation cursor,
  // cdn cache + its lock). atomic/mutex members are immovable, and the
  // create()/with_host() factories return the source by VALUE through
  // Result — so the state lives behind a unique_ptr (pointee mutation
  // through a const pointer keeps the const method surface honest).
  struct State {
    // Sticky "the edge 403'd plain TLS" flag — all later requests go
    // fingerprinted directly.
    std::atomic<bool> fingerprinted{false};
    // pages() round-robin cursor over the image pool (rotation-for-free).
    std::atomic<std::uint32_t> rotate{0};
    std::mutex cdn_mu;
    nh::NhCdn cdn;  // empty image pool = not fetched yet.
  };

  Nhentai(http::Client client, std::string host, http::Fingerprint escalate)
      : http_(std::move(client)),
        host_(std::move(host)),
        escalate_(escalate),
        state_(new State) {}

  // GET url with the seam UA, Any2xx, the one-retry 429 rule, and the sticky
  // 403 → fingerprint escalation.
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> get(
      const std::string& url) const;

  // The cdn pool, fetched once per process under the lock; subsequent calls
  // answer from the cache.
  [[nodiscard]] Result<nh::NhCdn, ProviderError> ensure_cdn() const;

  http::Client http_;
  std::string host_;
  http::Fingerprint escalate_;  // None = escalation disabled (test seam).
  std::unique_ptr<State> state_;
};

}  // namespace shigoku::manga
