// mangahub.hpp — the MangaHub source. Everything but the images is one
// GraphQL endpoint (api.mghcdn.com) behind a Cloudflare edge that refuses a
// plain TLS handshake, so every API call goes out through the
// libcurl-impersonate seam (http::Request::fingerprint = Chrome). Pages and
// covers are plain GETs on mghcdn and ride mapp's plain clients.
//
//   search  → search(x:m01,q:$q){rows{…,isSafe}}
//   chapters → manga(x:m01,slug:$s){chapters{number,title,date}}
//   pages   → chapter(x:m01,slug:$s,number:$n){pages}, where `pages` is a JSON
//             STRING {"p":"<prefix>/","i":["1.jpg",…]} joined onto imgx.
//
// User text and ids travel as GraphQL variables, never spliced into the query.
// The page prefix is not the slug ("dr-stone_111" pages live under
// "dr-stone/1/"), so it is taken verbatim after a vet. A missing title or
// chapter answers 200 with data.<field> = null (Http 404 here); a throttled
// call answers the same way with a "rate limit" error (RateLimited).
//
// Ids: manga = the slug; chapter = "<slug>_<number>" (split at the last '_',
// which a number never carries). Rows whose isSafe is not true are dropped
// unless the source was created with allow_unsafe (the `nsfw_sources` gate).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../error.hpp"
#include "../http.hpp"
#include "../result.hpp"
#include "model.hpp"
#include "source.hpp"

namespace shigoku::manga {

inline constexpr const char* kMhApi = "https://api.mghcdn.com/graphql";
inline constexpr const char* kMhReferer = "https://mangahub.io/";
inline constexpr const char* kMhImageHost = "https://imgx.mghcdn.com";
inline constexpr const char* kMhThumbHost = "https://thumb.mghcdn.com";
inline constexpr std::size_t kMhSlugMax = 200;
inline constexpr std::size_t kMhPathMax = 256;
inline constexpr std::size_t kMhNumberMax = 16;
inline constexpr std::uint32_t kMhSearchLimit = 30;

namespace mh {

// [a-z0-9_-], 1..kMhSlugMax, alphanumeric at both ends.
[[nodiscard]] bool slug_shaped(std::string_view s);

// Digits, optionally '.' and more digits ("12", "93.5"), at most kMhNumberMax.
[[nodiscard]] bool number_shaped(std::string_view s);

// A page prefix ("dr-stone/1/"): [A-Za-z0-9._/-], ends with '/', no leading
// '/', no "//" or "..", at most kMhPathMax.
[[nodiscard]] bool prefix_shaped(std::string_view s);

// A cover path ("md/sousou-no-frieren.jpg"): [A-Za-z0-9._/-], no leading or
// trailing '/', no "//" or "..", last segment filename_safe, at most
// kMhPathMax.
[[nodiscard]] bool cover_shaped(std::string_view s);

[[nodiscard]] std::string chapter_id(std::string_view slug, std::string_view number);
// Inverse of chapter_id, both halves re-vetted; nullopt when either fails.
[[nodiscard]] std::optional<std::pair<std::string, std::string>> split_chapter_id(
    std::string_view id);

// "Chapter 12" for number "12" (any case, any spacing) is the site's filler
// title; it becomes "". Anything else passes through trimmed.
[[nodiscard]] std::string chapter_title(std::string_view title, std::string_view number);

// GraphQL request bodies, exact-string tested. `number` must be number_shaped.
[[nodiscard]] std::string search_body(std::string_view query, std::uint32_t limit);
[[nodiscard]] std::string manga_body(std::string_view slug);
[[nodiscard]] std::string chapter_body(std::string_view slug, std::string_view number);

[[nodiscard]] std::string page_url(std::string_view image_host, std::string_view prefix,
                                   std::string_view file);
[[nodiscard]] std::string thumb_url(std::string_view thumb_host, std::string_view image);

// rows[] → MdManga: id = slug (row dropped when not slug_shaped, duplicates
// collapse to the first), title (slug when empty), status verbatim,
// cover_filename = image when cover_shaped. isSafe != true drops the row
// unless allow_unsafe. No data.search → Decode carrying the first error.
[[nodiscard]] Result<std::vector<MdManga>, ProviderError> parse_search(
    std::string_view raw, bool allow_unsafe);

// manga.chapters[] → MdChapter (id per chapter_id, lang "en", publish_at =
// the API date, pages unknown), dedupe_chapters order. A row whose number
// fails number_shaped is dropped. data.manga null → Http 404.
[[nodiscard]] Result<std::vector<MdChapter>, ProviderError> parse_manga(
    std::string_view raw, std::string_view slug);

// chapter.pages → PageSet: `p` must pass prefix_shaped and every `i` entry
// filename_safe, or the whole chapter fails; empty `i` → Decode. referer =
// kMhReferer. data.chapter null → Http 404.
[[nodiscard]] Result<PageSet, ProviderError> parse_chapter(std::string_view raw,
                                                           std::string_view image_host);

}  // namespace mh

class MangaHub final : public MangaSource {
 public:
  [[nodiscard]] static Result<MangaHub, ProviderError> create(bool allow_unsafe);
  // Test seam: a plain-HTTP fixture endpoint, fingerprinting off.
  [[nodiscard]] static Result<MangaHub, ProviderError> with_endpoint(std::string api,
                                                                    bool allow_unsafe);

  [[nodiscard]] std::string_view key() const override { return "mh"; }
  [[nodiscard]] std::string_view name() const override { return "MangaHub"; }

  [[nodiscard]] Result<std::vector<MdManga>, ProviderError> search(
      std::string_view query) const override;
  // `lang` is ignored: the catalogue is English only.
  [[nodiscard]] Result<std::vector<MdChapter>, ProviderError> chapters(
      std::string_view manga_id, std::string_view lang) const override;
  // data_saver is ignored: one quality.
  [[nodiscard]] Result<PageSet, ProviderError> pages(std::string_view chapter_id,
                                                     bool data_saver) const override;
  [[nodiscard]] std::string cover_thumb_url(const MdManga& m) const override;

 private:
  MangaHub(http::Client client, std::string api, http::Fingerprint fingerprint,
           bool allow_unsafe)
      : http_(std::move(client)),
        api_(std::move(api)),
        fingerprint_(fingerprint),
        allow_unsafe_(allow_unsafe) {}

  // POST one GraphQL body; 429 sleeps 1s and retries once.
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> post(
      std::string body) const;

  http::Client http_;
  std::string api_;
  http::Fingerprint fingerprint_;
  bool allow_unsafe_;
};

}  // namespace shigoku::manga
