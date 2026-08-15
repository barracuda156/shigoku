// dynasty.hpp — the Dynasty Reader source, the third source behind the
// multi-source seam and the first one whose pages are WebP (they ride the
// webp decode seam). Same layout law as mangadex.hpp / weebcentral.hpp: pure
// parsers + URL builders as free functions in their own namespace (`dy` —
// NOT `detail`, which md owns, and NOT `wc`), transport as a small class
// owning an http::Client.
//
// Site pins (probed live, re-probed with zero drift):
//  - JSON where it counts, no Cloudflare, plain nginx, no auth:
//    GET /series/{slug}.json    → taggings[] = chapter rows
//                                 {title, permalink, released_on} interleaved
//                                 with {"header": …} volume markers (ignored)
//    GET /chapters/{perma}.json → pages[{name, url}], url site-RELATIVE.
//  - Search is the one HTML surface: GET /search?q={q}&classes[]=Series. The
//    class filter matters (probed: unfiltered returns chapter/author links and
//    ZERO /series/ links). There is no JSON twin — /search.json 500s (probed
//    live). v1 is series-only; Dynasty's loose chapters / anthologies /
//    doujins are a later lens.
//  - Pages: {host}{url} — WebP (probed 200, 516KB), so this source is why
//    the webp decode seam exists. Referer probed NOT required, carried
//    anyway (cheap insurance).
//
// Ids are slugs, and Dynasty's are HIERARCHICAL: a chapter permalink is
// "{series_slug}_ch01" / "{series_slug}_rival_and_kase_san". Two consequences
// this file (and pages.cpp) had to answer:
//   1. Chapter numbers come from the permalink's trailing "chNN" run FIRST,
//      title text second, oneshot last — because Dynasty reuses one
//      displayed number across release variants ("Chapter 17" appears three
//      times as ch17 / ch17_1 / ch17_2) and the library dir would collide if
//      we trusted the title. The permalink's "_" run becomes a decimal:
//      ch5_5 → "5.5", ch17_2 → "17.2" — distinct AND correctly ordered by
//      chapter_less.
//   2. library_chapter_dir's 8-char anchor drops the manga-id prefix (see
//      pages.cpp) — a raw first-8 of every kase_san chapter is the literal
//      string "kase_san".
//
// Trust boundary (the weebcentral.hpp posture): every slug is charset-vetted
// AT PARSE TIME and re-vetted before it splices into a request path (ids may
// later arrive from persisted store space rather than a fresh parse); a row
// with a bad slug is dropped, a bad page filename fails the WHOLE chapter (a
// page gap is worse than a failed chapter).
//
// No cover / no description in v1: the search HTML carries neither (the
// series JSON carries both, but cover_thumb_url is called on the UI thread
// from mapp's cover_tick and may not fetch).

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

inline constexpr const char* kDyHost = "https://dynasty-scans.com";
// Longest slug we will splice into a URL path (the fixture's longest is 50;
// this is a sanity bound, not a site limit).
inline constexpr std::size_t kDySlugMax = 128;

// --- Pure helpers, exposed for the golden tests ------------------------------
namespace dy {

// The parse-time trust boundary: a Dynasty slug/permalink is exactly what
// filename_safe() admits ([A-Za-z0-9._-], no ".." run, no leading '.') and at
// most kDySlugMax bytes — it becomes a URL path segment, and its head becomes
// the library dir's uniqueness anchor.
[[nodiscard]] bool slug_shaped(std::string_view s);

// URL builders — exact-string tested, host injectable for the fixture server.
//
//   search_url:  {host}/search?q={enc}&classes%5B%5D=Series
//   series_url:  {host}/series/{slug}.json
//   chapter_url: {host}/chapters/{permalink}.json
//
// The brackets are emitted pre-encoded (the mangadex builder idiom) so the
// exact string is what goes on the wire; the query text rides url_encode.
[[nodiscard]] std::string search_url(std::string_view host, std::string_view query);
[[nodiscard]] std::string series_url(std::string_view host, std::string_view slug);
[[nodiscard]] std::string chapter_url(std::string_view host,
                                      std::string_view permalink);

// The chapter number, permalink first: the trailing "_chNN" run, with each
// following "_N" group becoming a decimal place — "…_ch01" → "1", "…_ch5_5"
// → "5.5", "…_ch17_2" → "17.2" (leading zeros stripped from the integer part
// so chapter_less ties never depend on spelling). A tail carrying anything
// but digits and '_' separators is refused, and the title text is tried
// next: "Chapter 5.5: …" → "5.5". Neither → "" (oneshot).
[[nodiscard]] std::string chapter_number(std::string_view permalink,
                                         std::string_view title);

// The display title with the redundant "Chapter N:" prefix removed —
// "Chapter 1: Morning Glory and Kase-san" → "Morning Glory and Kase-san";
// an unnumbered extra ("Rival and Kase-san") passes through whole.
[[nodiscard]] std::string chapter_title(std::string_view title);

// Parsers, pure over response bytes (fixtures: tests/fixtures/dynasty/).
//
// parse_search (search_kase_series.html): one MdManga per
// href="/series/{slug}" link (the nav's bare "/series" never matches), title
// = the anchor text, entity-unescaped; duplicate slugs collapse to the first
// hit. Rows whose slug fails slug_shaped are dropped. year/status/
// description/cover_filename stay empty and al_id/mal_id stay nullopt — the
// search page carries none of them. Zero results → Ok(empty): a no-results
// page is a legit answer.
[[nodiscard]] Result<std::vector<MdManga>, ProviderError> parse_search(
    std::string_view raw);

// parse_series (series_kase_san.json): taggings[] → MdChapter, skipping the
// {"header": …} volume markers (including the null-valued one the fixture
// pins at the tail). chapter/title per chapter_number/chapter_title above;
// publish_at = released_on ("2012-06-08" — date-only, still lexicographic ==
// chronological); lang fixed "en" (EN-only site, the chapters() lang argument
// is ignored); pages unknown here (0 — the chapter fetch is the count
// authority). Output sorted ascending by chapter_less with publish_at then id
// as tie-breaks. NO dedupe: Dynasty's release variants are distinct chapters
// (md's dedupe_chapters would eat two thirds of "Chapter 17"). A missing or
// ill-typed taggings[] → Decode; an empty one → Ok(empty).
[[nodiscard]] Result<std::vector<MdChapter>, ProviderError> parse_series(
    std::string_view raw);

// parse_chapter (chapter_kase_san_ch01.json): pages[] → PageSet in document
// order. url is site-relative ("/system/releases/…"), so it is joined to
// `host` (absolute http(s) urls, should the site ever emit them, pass
// through); referer = host; name_hint = the URL's last path segment (query
// stripped), which must pass filename_safe or the WHOLE parse fails. A
// missing/empty pages[] or an unjoinable url → Decode.
[[nodiscard]] Result<PageSet, ProviderError> parse_chapter(std::string_view raw,
                                                           std::string_view host);

}  // namespace dy

// --- Transport ----------------------------------------------------------------

// Owns the client; every method is blocking (worker-thread use, A1). Same
// politeness floor as the other two sources: 429 → sleep 1s, retry ONCE, a
// second 429 surfaces as RateLimited. No challenge sniff: Dynasty is plain
// nginx with no Cloudflare in front of it (probed twice) — the day that
// changes, this source grows needs_fingerprint like nhentai, not a parser
// change.
class Dynasty final : public MangaSource {
 public:
  [[nodiscard]] static Result<Dynasty, ProviderError> create();
  // Test seam: point at a loopback fixture server. The page URLs are joined
  // against this same host, so a fixture server serves pages too.
  [[nodiscard]] static Result<Dynasty, ProviderError> with_host(std::string host);

  [[nodiscard]] std::string_view key() const override { return "dy"; }
  [[nodiscard]] std::string_view name() const override { return "Dynasty Reader"; }

  // GET search_url; parse_search verbatim.
  [[nodiscard]] Result<std::vector<MdManga>, ProviderError> search(
      std::string_view query) const override;

  // GET series_url; parse_series verbatim. `lang` is ignored — EN-only site.
  [[nodiscard]] Result<std::vector<MdChapter>, ProviderError> chapters(
      std::string_view manga_id, std::string_view lang) const override;

  // GET chapter_url; parse_chapter verbatim. Called per read (and as the
  // rotation refetch). data_saver is ignored: the site has one quality.
  [[nodiscard]] Result<PageSet, ProviderError> pages(
      std::string_view chapter_id, bool data_saver) const override;

  // Always "" in v1 — the search HTML carries no cover and this call may not
  // fetch (UI thread).
  [[nodiscard]] std::string cover_thumb_url(const MdManga& m) const override;

 private:
  Dynasty(http::Client client, std::string host)
      : http_(std::move(client)), host_(std::move(host)) {}

  // GET url with the seam UA, Any2xx, the one-retry 429 rule.
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> get(
      const std::string& url) const;

  http::Client http_;
  std::string host_;
};

}  // namespace shigoku::manga
