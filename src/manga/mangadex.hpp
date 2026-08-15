// mangadex.hpp — the MangaDex source. Greenfield (no sabigoku reference);
// the shape follows the anime provider pattern (megaplay.hpp): pure parsers
// + URL builders as free functions in `detail` so the golden contract runs
// offline against committed fixtures, transport as a small class owning an
// http::Client.
//
// MangaDex is the one clean-API source (documented public REST JSON at
// api.mangadex.org, no auth to read, no scraping, no Cloudflare fight). The
// multi-source seam extracted from this surface lives in source.hpp;
// MangaDex implements MangaSource, with pages() adapting the at-home grant
// (base_url + hash + filenames + the data-saver fallback law) into the
// seam's absolute-URL PageUnits. at_home() stays public — it is the tested
// transport under pages() and the rotation grant.
//
// Id scoping: ids inside this lib are bare MangaDex uuids; anything that
// crosses into mapp/store space goes through scoped_id() ("md:{uuid}") so a
// second source never forces an mstore ladder bump.
//
// Trust boundary: every string that later becomes a URL component or a disk
// path (manga/chapter ids, cover fileName, at-home hash + page filenames) is
// charset-vetted AT PARSE TIME; entries failing the vet are dropped (ids) or
// blanked (cover), never "sanitized and proceeded with". The at-home
// base_url is provider-supplied and is not fetched by search(); pages() must
// pass it through guard_fetch_url before any GET.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../error.hpp"
#include "../http.hpp"
#include "../result.hpp"
#include "model.hpp"
#include "source.hpp"

namespace shigoku::manga {

inline constexpr const char* kApiHost = "https://api.mangadex.org";
inline constexpr const char* kUploadsHost = "https://uploads.mangadex.org";
// kUserAgent (the identifying UA MangaDex's API policy asks for) moved to
// source.hpp with the seam — every source's transport sends it.
// Feed page size (the API max). The fixture was captured with limit=20 to
// stay small; production always sends 500.
inline constexpr std::uint32_t kFeedLimit = 500;
// Offset-loop flood guard: 20 pages × 500 = 10k chapters, far above any real
// series; a `total` claiming more is treated as exhausted at the cap.
inline constexpr std::uint32_t kMaxFeedPages = 20;
// Search result page size (one screen of results; no pagination).
inline constexpr std::uint32_t kSearchLimit = 25;

// --- Types (pinned) -----------------------------------------------------------
// MdManga / MdChapter moved to model.hpp with the multi-source seam (they
// are the cross-source model now); MdFeedPage and MdAtHome stay here — they
// are MangaDex parser output, not model.

// One feed page + the server's total (the offset loop's terminator).
struct MdFeedPage {
  std::vector<MdChapter> chapters;
  std::uint32_t total = 0;
};

struct MdAtHome {
  std::string base_url;  // ephemeral node grant; the pages path guards + rotates it.
  std::string hash;
  std::vector<std::string> data;        // full-quality page filenames.
  std::vector<std::string> data_saver;  // compressed page filenames.
  friend bool operator==(const MdAtHome&, const MdAtHome&) = default;
};

// "md:{uuid}". The only spelling that may leave the lib.
[[nodiscard]] std::string scoped_id(std::string_view uuid);

// --- Pure helpers, exposed for the golden tests ------------------------------
namespace detail {

// RFC 3986: unreserved [A-Za-z0-9._~-] pass, everything else %XX (uppercase
// hex, UTF-8 bytes escaped individually). Space is %20, never '+'.
[[nodiscard]] std::string url_encode(std::string_view s);

// URL builders — exact-string tested, host injectable for the fixture server.
//
//   search_url:  {host}/manga?title={enc}&limit={n}&includes%5B%5D=cover_art
//   feed_url:    {host}/manga/{id}/feed?translatedLanguage%5B%5D={lang}
//                &order%5Bchapter%5D=asc&limit={limit}&offset={offset}
//   at_home_url: {host}/at-home/server/{chapter_id}
//   cover_url:   {uploads_host}/covers/{manga_id}/{fileName}.512.jpg
//                (fileName already carries its own .jpg/.png — the .512.jpg
//                thumb suffix is APPENDED; verified live, 200 with no
//                referer/UA requirement)
//   page_url:    {base_url}/data/{hash}/{file} — data-saver swaps the segment
//                to /data-saver and reads from data_saver[i].
//
// Brackets are emitted pre-encoded (%5B%5D) so the exact string is what goes
// on the wire.
[[nodiscard]] std::string search_url(std::string_view host, std::string_view query,
                                     std::uint32_t limit);
[[nodiscard]] std::string feed_url(std::string_view host, std::string_view manga_id,
                                   std::string_view lang, std::uint32_t limit,
                                   std::uint32_t offset);
[[nodiscard]] std::string at_home_url(std::string_view host, std::string_view chapter_id);
[[nodiscard]] std::string cover_url(std::string_view uploads_host,
                                    std::string_view manga_id,
                                    std::string_view filename);
[[nodiscard]] std::string page_url(const MdAtHome& at_home, std::size_t index,
                                   bool data_saver);

// Charset vets (the parse-time trust boundary). uuid_shaped: 36 chars,
// lowercase hex + dashes in the 8-4-4-4-12 pattern. filename_safe:
// non-empty, [A-Za-z0-9._-]+ only, no ".." run and no leading '.' — the
// cover fileName and every at-home page filename must pass before they can
// reach a URL or a cache path.
[[nodiscard]] bool uuid_shaped(std::string_view s);
[[nodiscard]] bool filename_safe(std::string_view s);

// Parsers, pure over response bytes (fixtures: tests/fixtures/mangadex/).
//
// parse_search (search.json): data[] → MdManga. Title rule: title.en, else
// title["ja-ro"], else the first entry, else "" (Chainsaw Man has NO en
// title — the fixture pins this). Description: description.en else "".
// links.al/.mal are STRINGS in the API; non-numeric/absent → nullopt (a
// malformed link never fails the row). cover fileName from the one
// relationships[] entry with type=="cover_art" (present because search_url
// sends includes[]=cover_art); missing rel, missing attributes, or a
// fileName failing filename_safe() → cover_filename = "". Rows whose id
// fails uuid_shaped() are dropped. Err(Decode) only for non-JSON or a
// missing/ill-typed data[].
[[nodiscard]] Result<std::vector<MdManga>, ProviderError> parse_search(
    std::string_view raw);

// parse_feed (feed.json): one feed page. DROPS external chapters —
// attributes.externalUrl non-null OR pages == 0 OR isUnavailable == true —
// they are publisher redirects (viz.com in the fixture) with no at-home
// pages. chapter/title null → "". Rows with a non-uuid id are dropped.
// `total` is the SERVER's count (externals included) — the offset loop
// terminates on raw-seen count, so parse_feed also reports how many rows it
// saw pre-drop via MdFeedPage: chapters.size() is post-drop; the loop
// advances by the request limit (next_offset), never by chapters.size().
[[nodiscard]] Result<MdFeedPage, ProviderError> parse_feed(std::string_view raw);

// parse_at_home (at_home.json): baseUrl + chapter.hash + chapter.data[] +
// chapter.dataSaver[]. hash and every filename must pass filename_safe()
// (they become URL segments in page_url and cache filenames in the
// chapter-fetch path); a failing filename fails the WHOLE parse (Decode) —
// a chapter with page gaps is worse than a failed chapter. Empty data[] →
// Decode.
[[nodiscard]] Result<MdAtHome, ProviderError> parse_at_home(std::string_view raw);

// The pinned chapter order: parse the leading decimal number of each ("10.5"
// → 10.5, strtod semantics); both numeric → compare by value, ties →
// lexicographic on the full string; numeric sorts BEFORE non-numeric
// (oneshots/"extras" trail the numbered run); both non-numeric (incl. "") →
// lexicographic.
[[nodiscard]] bool chapter_less(std::string_view a, std::string_view b);

// Duplicate scanlations of the same chapter STRING (exact match — "1" and
// "1.0" are distinct): keep the newest publish_at (lexicographic max; ties →
// first seen), drop the rest. Output sorted by chapter_less (id as the
// final tie-break for determinism). Input is one lang already (the feed
// filtered server-side).
[[nodiscard]] std::vector<MdChapter> dedupe_chapters(std::vector<MdChapter> in);

// The offset-loop step, pure so the math tests offline: given the offset
// just fetched, the request limit, the server total, and how many pages
// were fetched so far, the next offset — or nullopt when exhausted (offset +
// limit >= total) or at the kMaxFeedPages flood cap.
[[nodiscard]] std::optional<std::uint32_t> next_offset(std::uint32_t offset,
                                                       std::uint32_t limit,
                                                       std::uint32_t total,
                                                       std::uint32_t pages_fetched);

}  // namespace detail

// --- Transport ----------------------------------------------------------------

// Owns the client; every method is blocking (worker-thread use, A1). 429 →
// sleep 1s, retry ONCE per request (the politeness floor MangaDex asks for,
// ~5 req/s); a second 429 surfaces as RateLimited. All other errors surface
// unchanged. No other retry lives here (node rotation is the pages path's
// business).
class MangaDex final : public MangaSource {
 public:
  [[nodiscard]] static Result<MangaDex, ProviderError> create();
  // Test seam: point at a loopback fixture server (senshi_tests pattern).
  [[nodiscard]] static Result<MangaDex, ProviderError> with_host(std::string api_host);

  [[nodiscard]] std::string_view key() const override { return "md"; }
  [[nodiscard]] std::string_view name() const override { return "MangaDex"; }

  // GET search_url; parse_search verbatim (no post-processing).
  [[nodiscard]] Result<std::vector<MdManga>, ProviderError> search(
      std::string_view query, std::uint32_t limit) const;
  [[nodiscard]] Result<std::vector<MdManga>, ProviderError> search(
      std::string_view query) const override {
    return search(query, kSearchLimit);
  }

  // The full feed: offset loop (kFeedLimit pages, next_offset terminator),
  // parse_feed per page (externals dropped there), then dedupe_chapters over
  // the ASSEMBLED list (duplicates span page boundaries) — returned sorted.
  [[nodiscard]] Result<std::vector<MdChapter>, ProviderError> chapters(
      std::string_view manga_id, std::string_view lang) const override;

  // GET at_home_url; parse_at_home verbatim. Called per read (grants expire).
  [[nodiscard]] Result<MdAtHome, ProviderError> at_home(
      std::string_view chapter_id) const;

  // at_home adapted to the seam: absolute page_url per page, no referer
  // (MangaDex@Home wants none), name_hint = the at-home filename. The
  // data-saver fallback law (an empty dataSaver list must not brick the
  // chapter) lives HERE now, not in the fetch core.
  [[nodiscard]] Result<PageSet, ProviderError> pages(
      std::string_view chapter_id, bool data_saver) const override;

  // Absolute .512.jpg thumb URL for the shared Covers::load path (nullptr
  // provider arm — the URL is absolute). "" when the manga has no cover.
  [[nodiscard]] std::string cover_thumb_url(const MdManga& m) const override;

 private:
  MangaDex(http::Client client, std::string api_host)
      : http_(std::move(client)), api_host_(std::move(api_host)) {}

  // GET url with kUserAgent, Any2xx, the one-retry 429 rule.
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> get(
      const std::string& url) const;

  http::Client http_;
  std::string api_host_;
};

}  // namespace shigoku::manga
