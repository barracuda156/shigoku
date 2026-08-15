// weebcentral.hpp — the WeebCentral source, the first scrape source behind
// the multi-source seam. HTMX site: every endpoint returns an HTML fragment
// (no JSON anywhere), so the parsers are string-scans over the exact markup
// pinned by live probes and committed as tests/fixtures/weebcentral/. Same
// layout law as mangadex.hpp: pure parsers + URL builders as free functions
// in `wc` (golden-tested offline — their own namespace, not `detail`: both
// manga sources share shigoku::manga, and md already owns
// detail::search_url/parse_search), transport as a small class owning an
// http::Client.
//
// Site pins (probe evidence, not guesses):
//  - Search is a fragment of <article class="bg-base-300"> blocks, one per
//    result: the /series/{ULID}/{slug} link + a title anchor + Year/Status
//    rows. Covers have a JPG fallback alongside webp
//    ({kCoverHost}/cover/fallback/{ULID}.jpg) — covers ride stb, no webp dep.
//  - Chapters: /series/{ULID}/full-chapter-list → <a href="/chapters/{ULID}">
//    rows with a "Chapter {n}" span + an ISO <time datetime>. EN-only site:
//    lang is fixed "en" and the chapters() lang argument is ignored.
//  - Pages: /chapters/{ULID}/images?...reading_style=long_strip → <img src>
//    list. The image host VARIES PER SERIES (scans-hot.planeptune.us,
//    official.lowee.us, …) — src is taken verbatim and the mapp orchestrator
//    guard_fetch_url's every unit; this file never assumes a host. PNG today;
//    Referer probed NOT required, carried on every PageUnit anyway (cheap
//    insurance).
//  - Cloudflare fronts the site but serves NO challenge today. The known
//    Cloudflare challenge-page markers live in looks_like_challenge(); the
//    day CF flips, the fix is routing requests through the
//    libcurl-impersonate fingerprint seam (http::Request::fingerprint), not
//    a re-scrape.
//
// Ids are ULIDs (26 chars, Crockford base32). Trust boundary: ids and page
// filename hints are charset-vetted AT PARSE TIME (they become URL path
// segments and on-disk names); a row with a bad id is dropped, a bad page
// filename fails the whole chapter (a page gap is worse than a failed
// chapter — the parse_at_home posture).

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "../error.hpp"
#include "../http.hpp"
#include "../result.hpp"
#include "model.hpp"
#include "source.hpp"

namespace shigoku::manga {

inline constexpr const char* kWcHost = "https://weebcentral.com";
// The cover CDN (probed live; the /fallback/ JPG exists alongside the webp
// variants, no referer/UA requirement).
inline constexpr const char* kWcCoverHost = "https://temp.compsci88.com";
// Search page size (the site's own default for the Full Display fragment).
inline constexpr std::uint32_t kWcSearchLimit = 32;

// --- Pure helpers, exposed for the golden tests ------------------------------
namespace wc {

// ULID vet (the parse-time trust boundary): exactly 26 chars of Crockford
// base32 — digits + uppercase letters excluding I, L, O, U.
[[nodiscard]] bool ulid_shaped(std::string_view s);

// Minimal HTML entity unescape for scraped text (titles/labels): the five
// named entities (&amp; &lt; &gt; &quot; &#39;/&apos;) plus decimal and hex
// numeric references, emitted as UTF-8. Unknown entities pass through
// verbatim (never drop text over markup trivia).
[[nodiscard]] std::string html_unescape(std::string_view s);

// CF-challenge / parked-domain sniff (known challenge/parked-domain strings
// plus the standard Cloudflare markers), case-insensitive. True = "this
// domain needs the libcurl-impersonate fingerprint seam" — the transport
// surfaces Forbidden instead of feeding a challenge page to a parser.
[[nodiscard]] bool looks_like_challenge(std::string_view body);

// URL builders — exact-string tested, host injectable for the fixture server.
//
//   search_url:       {host}/search/data?limit={n}&offset=0&text={enc}
//                     &sort=Best+Match&order=Ascending&official=Any
//                     &display_mode=Full+Display
//   chapter_list_url: {host}/series/{ULID}/full-chapter-list
//   images_url:       {host}/chapters/{ULID}/images?is_prev=False
//                     &current_page=1&reading_style=long_strip
//   cover_url:        {cover_host}/cover/fallback/{ULID}.jpg
//
// The fixed params carry the site's own '+'-encoded spellings verbatim (what
// went on the wire in the probes); only the query text is url-encoded
// (%XX, the mangadex url_encode).
[[nodiscard]] std::string search_url(std::string_view host, std::string_view query,
                                     std::uint32_t limit);
[[nodiscard]] std::string chapter_list_url(std::string_view host,
                                           std::string_view series_id);
[[nodiscard]] std::string images_url(std::string_view host,
                                     std::string_view chapter_id);
[[nodiscard]] std::string cover_url(std::string_view cover_host,
                                    std::string_view series_id);

// Parsers, pure over fragment bytes (fixtures: tests/fixtures/weebcentral/).
//
// parse_search (search_data.html): one MdManga per
// <article class="bg-base-300"> block. id = the ULID of the first
// /series/{ULID}/ link (row dropped if it fails ulid_shaped); title = the
// line-clamp-1 anchor text (fallback: the truncate overlay div), entity-
// unescaped; year/status from the "Year:"/"Status:" rows (absent → nullopt /
// ""). description stays "" (not in the fragment — this parser skips it);
// cover_filename stays "" (the cover URL derives from the id);
// al_id/mal_id stay nullopt (only MangaDex supplies tracker links). Zero
// blocks → Ok(empty): a no-results page is a legit answer, and drift/
// challenge pages are the transport sniff's business, so this parser never
// errors.
[[nodiscard]] Result<std::vector<MdManga>, ProviderError> parse_search(
    std::string_view raw);

// parse_chapters (full_chapter_list.html): one MdChapter per
// href="/chapters/{ULID}" anchor (bad id → row dropped). The label span
// ("Chapter 17", or "Special …" for unnumbered extras) splits on the
// "Chapter " prefix: numbered → chapter = the raw remainder, title = "";
// anything else → chapter = "", title = the label (chapter_less already
// sorts unnumbered rows after the numbered run). publish_at = the <time
// datetime> attr (ISO8601 Z — lexicographic compare IS chronological);
// lang fixed "en"; pages unknown here (0 — the images fetch is the count
// authority). Output sorted ascending by chapter_less, publish_at then id
// as tie-breaks (the site lists newest-first; reading order is ours).
[[nodiscard]] Result<std::vector<MdChapter>, ProviderError> parse_chapters(
    std::string_view raw);

// parse_images (chapter_images.html): every <img src="http(s)://…"> in the
// fragment, in document order, one PageUnit each — url verbatim (the host
// varies per series; the ORCHESTRATOR guards it), referer = kWcHost,
// name_hint = the URL's last path segment (query stripped), which must pass
// the mangadex filename_safe vet or the WHOLE parse fails (Decode).
// Relative srcs (static badges/placeholders) are skipped. Zero units →
// Decode — a chapter with no pages is a failed scrape, not an answer.
[[nodiscard]] Result<PageSet, ProviderError> parse_images(std::string_view raw);

}  // namespace wc

// --- Transport ----------------------------------------------------------------

// Owns the client; every method is blocking (worker-thread use, A1). Same
// politeness floor as MangaDex: 429 → sleep 1s, retry ONCE, a second 429
// surfaces as RateLimited. Every 2xx body passes looks_like_challenge before
// any parser sees it — a challenge page surfaces as Forbidden ("WeebCentral
// blocked us"), the signal to flip this source onto the fingerprint seam.
class WeebCentral final : public MangaSource {
 public:
  [[nodiscard]] static Result<WeebCentral, ProviderError> create();
  // Test seam: point at a loopback fixture server.
  [[nodiscard]] static Result<WeebCentral, ProviderError> with_host(std::string host);

  [[nodiscard]] std::string_view key() const override { return "wc"; }
  [[nodiscard]] std::string_view name() const override { return "WeebCentral"; }

  // GET search_url (kWcSearchLimit); parse_search verbatim.
  [[nodiscard]] Result<std::vector<MdManga>, ProviderError> search(
      std::string_view query) const override;

  // GET chapter_list_url; parse_chapters verbatim. `lang` is ignored —
  // EN-only site, every row is "en" already. The id is re-vetted before it
  // splices into the URL path (ids may later arrive from persisted store
  // space rather than a fresh parse; a corrupt id must not shape a request).
  [[nodiscard]] Result<std::vector<MdChapter>, ProviderError> chapters(
      std::string_view manga_id, std::string_view lang) const override;

  // GET images_url; parse_images verbatim. Called per read (scrapes go
  // stale) — also the rotation refetch. data_saver is ignored: the site has
  // one quality. Id re-vetted like chapters().
  [[nodiscard]] Result<PageSet, ProviderError> pages(
      std::string_view chapter_id, bool data_saver) const override;

  // The probed JPG fallback ({kWcCoverHost}/cover/fallback/{id}.jpg) — always
  // derivable from the vetted id, so never "".
  [[nodiscard]] std::string cover_thumb_url(const MdManga& m) const override;

 private:
  WeebCentral(http::Client client, std::string host)
      : http_(std::move(client)), host_(std::move(host)) {}

  // GET url with the seam UA, Any2xx, the one-retry 429 rule, and the
  // challenge sniff on the returned body.
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> get(
      const std::string& url) const;

  http::Client http_;
  std::string host_;
};

}  // namespace shigoku::manga
