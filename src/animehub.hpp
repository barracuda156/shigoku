// animehub.hpp — 123animehub.cc StreamProvider. No canonical-keyed endpoint
// (nullopt), so the walk binds it via title search.
//
// Chain, all under kBase: /search?keyword= (HTML cards) -> /ajax/film/sv?id=
// <slug> (JSON envelope around the episode grid + server tabs) ->
// /ajax/episode/info?epr=<slug>/<ep>/<server> (JSON, `target` = an embed on a
// third-party player host) -> the embed page's `var zrpart2 = '...'` ->
// <embed origin>/hs/<zrpart2>?pl_usn=1, whose `div#sources` holds the HLS
// master as JSON. The master's segments are plain MPEG-TS on extensionless
// paths served as image/jpeg, hence cloaked_segments. The /hs/ player burns
// the subtitles in; the embed's soft-sub player is a megaplay iframe.
//
// The dub is its own title on the site: the same slug with "-dub" appended.
// A title (or its dub twin) that does not exist answers the episode-grid call
// with an empty 500; the show page's 404 tells that apart from an outage.
//
// Episode labels are per-title, as the grid lists them ("12", "1128.5").

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "http.hpp"
#include "provider.hpp"
#include "result.hpp"

namespace shigoku::animehub {

inline constexpr const char* kBase = "https://123animehub.cc";
inline constexpr const char* kReferer = "https://123animehub.cc/";
inline constexpr std::string_view kDubSuffix = "-dub";
// The server tried when the grid names none.
inline constexpr const char* kDefaultServer = "0";

// --- Pure helpers (exposed for golden tests) --------------------------------
namespace detail {

// One search-result card, as the page printed it: the track's own slug (a
// dub card carries the "-dub" twin), its display name, which track it is, and
// the poster's "Ep N" (0 when it is not a number).
struct Card {
  std::string slug;
  std::string name;
  Translation track = Translation::Sub;
  std::uint32_t episodes = 0;
  friend bool operator==(const Card&, const Card&) = default;
};

// Cards inside the page's `film-list`, up to the paging bar or the sidebar,
// whichever comes first — the sidebar's ranking rail reuses `class="name"`.
// No `film-list` -> scan up to the sidebar. The "No Results Found" card has
// no href and drops out.
[[nodiscard]] std::vector<Card> parse_search_cards(std::string_view html);

// Sub and dub cards of one title folded into one hit keyed by the base slug,
// in first-seen order. `title` is the slug title-cased (the site's own
// romaji spelling); `title_english` is the card's name when it differs —
// which language the site prints flips with the query.
[[nodiscard]] std::vector<SearchHit> merge_cards(const std::vector<Card>& cards);

// The site's search finds nothing once the keyword carries punctuation
// (`Re:Zero`, `Dr. Stone`, `Fate/Zero`): every byte that is not an ASCII
// letter, digit or '-' becomes a space, runs collapse, ends trim.
[[nodiscard]] std::string search_query(std::string_view query);

// Lowercase ASCII letters, digits and inner '-', at most kMaxSlugLen.
inline constexpr std::size_t kMaxSlugLen = 200;
[[nodiscard]] bool valid_slug(std::string_view slug);

// "kusuriya-no-hitorigoto" -> "Kusuriya No Hitorigoto".
[[nodiscard]] std::string slug_title(std::string_view slug);

// The slug a track lives under: the dub twin for Dub.
[[nodiscard]] std::string track_slug(std::string_view slug, Translation translation);

// The episode grid: labels (ascending, deduplicated) and server ids in tab
// order.
struct Film {
  std::vector<std::string> episodes;
  std::vector<std::string> servers;
  friend bool operator==(const Film&, const Film&) = default;
};

// `/ajax/film/sv` answers `{"html": "..."}`. Episodes are the `data-id`
// values `<track_slug>/<label>`; servers are the tab `data-name`s (digits).
// Err(Decode) when the body is not that envelope.
[[nodiscard]] Result<Film, ProviderError> parse_film(std::string_view raw_json,
                                                     std::string_view track_slug);

// Digits, optionally one '.' and more digits; at most 16 bytes.
[[nodiscard]] bool valid_label(std::string_view label);

// Numeric order over valid labels ("2" < "10" < "10.5" < "11").
[[nodiscard]] bool label_less(std::string_view a, std::string_view b);

// The `target` embed url of an `/ajax/episode/info` body; nullopt otherwise.
[[nodiscard]] std::optional<std::string> parse_target(std::string_view raw_json);

// The embed page's `zrpart2` token (base64 alphabet only); nullopt if absent.
[[nodiscard]] std::optional<std::string> extract_zrpart2(std::string_view html);

// The HLS master out of the player page's `div#sources` JSON: `sources` as a
// string, or the first entry of a list (a string or a `{file}` object).
[[nodiscard]] std::optional<std::string> parse_sources(std::string_view html);

// Absolute + argv-clean + past the SSRF guard.
[[nodiscard]] bool stream_url_ok(std::string_view url);

// Single-pass HTML entity decode (amp/lt/gt/quot/apos/nbsp + &#NN; / &#xNN;).
// Unknown entities stay literal.
[[nodiscard]] std::string decode_entities(std::string_view s);

// Percent-encode a query value (RFC 3986 unreserved bytes pass).
[[nodiscard]] std::string url_encode(std::string_view s);

// `scheme://host[:port]` of an absolute url, no trailing slash.
[[nodiscard]] std::optional<std::string> origin_of(std::string_view absolute_url);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class AnimeHub final : public StreamProvider {
 public:
  [[nodiscard]] static Result<AnimeHub, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "animehub"; }
  [[nodiscard]] std::string_view display_name() const override { return "AnimeHub"; }

  // No canonical-keyed endpoint: nullopt sends the walk to tier C.
  [[nodiscard]] std::optional<std::string> canonical_key(
      const Enrichment& show) const override;

  [[nodiscard]] Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view query, const SearchOptions& opts) const override;

  // Dub: a title without a dub twin is an authoritative empty list.
  [[nodiscard]] Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view provider_id, Translation translation,
      std::optional<std::uint32_t> count_hint) const override;

  // Quality is ignored: the master goes to the player whole. Each grid
  // server is tried in tab order; the last failure is returned.
  [[nodiscard]] Result<StreamLink, ProviderError> resolve(
      std::string_view provider_id, std::string_view episode,
      Translation translation, Quality quality) const override;

  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view cover_ref) const override;

  // Test-only seam: the site at a loopback fixture server. The embed hop
  // follows the absolute `target` it is handed, so it never lands there.
  [[nodiscard]] static Result<AnimeHub, ProviderError> with_endpoint(std::string base);

 private:
  explicit AnimeHub(http::Client client, std::string base)
      : http_(std::move(client)), base_(std::move(base)) {}

  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> json_get(
      const std::string& url, const std::string& referer) const;
  [[nodiscard]] Result<std::string, ProviderError> page_get(const std::string& url,
                                                            const std::string& referer) const;

  [[nodiscard]] std::string show_url(std::string_view track_slug) const;
  [[nodiscard]] Result<detail::Film, ProviderError> fetch_film(
      const std::string& track_slug) const;
  [[nodiscard]] Result<StreamLink, ProviderError> resolve_on(const std::string& track_slug,
                                                             std::string_view episode,
                                                             const std::string& server) const;

  http::Client http_;
  std::string base_;
};

}  // namespace shigoku::animehub
