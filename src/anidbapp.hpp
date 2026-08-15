// anidbapp.hpp — anidb.app StreamProvider (P25, ROD-516). Ported from sabigoku
// src/providers/anidbapp.rs. Seam tier C, confidence tier A: canonical_key
// returns nullopt (no id-keyed endpoint), so the walk reaches it via tier-C
// title search — but the detail page yields AniList/MAL ids, giving the match
// tier-A confidence.
//
// Chain: /search/suggestions (HTML cards) -> /anime/{slug} (external ids) ->
// /api/frontend/anime/{siteId}/episodes -> /api/frontend/episode/{id}/languages
// -> embed HTML -> jwplayer HLS master. Referer + UA required.
//
// anidb.app's edge fingerprints the TLS ClientHello (a default handshake 403s),
// so every fetch runs through the P25 impersonate seam: Request::fingerprint =
// Chrome (see http.hpp / project_p25_clienthello_decision). This is the only
// provider that sets it.
//
// Like anibd.hpp, the pure helpers are free functions in `detail` over
// string_view so the golden contract runs offline; the .cpp keeps the JSON/HTML
// DTO walking + transport private.

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

namespace shigoku::anidbapp {

inline constexpr const char* kApi = "https://anidb.app";
inline constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
inline constexpr const char* kReferer = "https://anidb.app/";
// Detail-page probes per search: the suggestions endpoint returns at most 8
// cards, so in practice every candidate reaches the scorer id-annotated.
inline constexpr std::size_t kMaxProbe = 8;
// The suggestions endpoint answers nothing below this query length.
inline constexpr std::size_t kMinQueryLen = 2;
// Audio codes. `jpn` carries burned-in subs (no sidecar track); `eng` is dub.
inline constexpr const char* kCodeSub = "jpn";
inline constexpr const char* kCodeDub = "eng";

// --- Pure helpers (anidbapp.rs file-private fns), exposed for golden tests ----
namespace detail {

// One episode: the site's id and its (site-absolute) number.
struct Episode {
  std::int64_t id = 0;
  std::uint32_t number = 0;
  friend bool operator==(const Episode&, const Episode&) = default;
};

// A parsed suggestion card: slug + trailing site id, title, optional year.
struct Card {
  std::string slug;
  std::string site_id;
  std::string title;
  std::optional<std::uint32_t> year;
  friend bool operator==(const Card&, const Card&) = default;
};

// One languages[] row: audio code ("jpn"/"eng") + its embed page url.
struct LanguageRow {
  std::optional<std::string> code;
  std::optional<std::string> embed_url;
};

// -- episode listing --
// Rows to episodes, ascending by number, one row per number (first listed
// wins on a dup). Drops rows with a non-positive id or an out-of-range number.
// Err(Decode) when the body is not the expected JSON (e.g. an HTML 404 page).
[[nodiscard]] Result<std::vector<Episode>, ProviderError> parse_episodes(
    std::string_view raw_json);

// Distance between the site's numbering and the canonical's: a season the site
// numbers franchise-absolute (Frieren S2 = 29..38) maps to an AniList entry
// numbering 1..10. Labels are canonical numbers; the offset converts back on
// resolve. 0 for empty or already-1-based.
[[nodiscard]] std::uint32_t base_offset(const std::vector<Episode>& eps);

// Canonical label for an episode given the offset (site number - offset).
[[nodiscard]] std::string label(const Episode& ep, std::uint32_t offset);

// -- search card parsing --
// Suggestion cards from the search HTML: each is an `<a ... data-search-item>`
// wrapping a poster and two `<p>` (title, then a `TYPE · YEAR` line).
[[nodiscard]] std::vector<Card> parse_cards(std::string_view html);

// Slug and trailing site id from a card href. Only the last path segment is
// kept (host is a constant), so a forged href cannot move the probe off our
// origin; the charset guard rejects a segment that would corrupt the built URL.
// nullopt if there is no `/anime/` segment, no trailing numeric id, or the
// segment carries anything but [A-Za-z0-9_-].
[[nodiscard]] std::optional<std::pair<std::string, std::string>> split_slug(
    std::string_view href);

// Last 4-digit run in a `TYPE · YEAR` line (nullopt if none).
[[nodiscard]] std::optional<std::uint32_t> trailing_year(std::string_view meta);

// Single-pass HTML entity decode (named amp/lt/gt/quot/apos/nbsp + &#NN; /
// &#xNN;). Unknown entities stay literal; must NOT over-decode a double-escaped
// literal (`Journey&amp;#039;s` -> `Journey&#039;s`, one pass).
[[nodiscard]] std::string decode_entities(std::string_view s);

// -- detail page + embed --
// AniList and MAL ids from the detail page's external-link block (the integer
// right after `anilist.co/anime/` and `myanimelist.net/anime/`).
[[nodiscard]] std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>
parse_external_ids(std::string_view html);

// A Cloudflare interstitial served at 200. An empty parse would read as "no
// results" and stamp a 7-day absence, so this gates it. Neither obvious marker
// works bare: `/cdn-cgi/challenge-platform` ships on good embeds, and "Just a
// moment" is ordinary loading copy — so it counts only in the <title>.
[[nodiscard]] bool is_challenge(std::string_view html);

// HLS master out of the jwplayer setup: the `file:` value first (only if it
// contains .m3u8), then any quoted `.m3u8` so a config rename does not break
// extraction. Only an absolute http(s) url survives. nullopt otherwise.
[[nodiscard]] std::optional<std::string> extract_hls(std::string_view html);

// Vet a scraped stream url before it becomes the play url: absolute + argv-clean
// + past the SSRF guard (a quality cap would fetch it ourselves).
[[nodiscard]] bool stream_url_ok(std::string_view url);

// Percent-encode into an application/x-www-form-urlencoded value (unreserved
// pass, ' ' -> '+', else %XX). Used for the /search/suggestions q= param.
[[nodiscard]] std::string form_urlencode(std::string_view s);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class AniDbApp final : public StreamProvider {
 public:
  [[nodiscard]] static Result<AniDbApp, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "anidbapp"; }
  [[nodiscard]] std::string_view display_name() const override { return "AniDB"; }

  // No canonical-keyed endpoint exists: nullopt sends the walk to tier C, where
  // the detail-page ids give the match tier-A confidence.
  [[nodiscard]] std::optional<std::string> canonical_key(
      const Enrichment& show) const override;

  [[nodiscard]] Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view query, const SearchOptions& opts) const override;

  [[nodiscard]] Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view provider_id, Translation translation,
      std::optional<std::uint32_t> count_hint) const override;

  [[nodiscard]] Result<StreamLink, ProviderError> resolve(
      std::string_view provider_id, std::string_view episode,
      Translation translation, Quality quality) const override;

  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view cover_ref) const override;

  // Test-only seam: point at a loopback fixture server instead of the live API.
  // The fixture speaks plain HTTP and does not fingerprint the TLS handshake, so
  // it forces Fingerprint::None — this exercises the full transport logic
  // (chain, dub bisection, parsing, guards) offline on any host, WITHOUT
  // requiring the impersonate dylib. The real Chrome-shaped handshake is proven
  // separately by anidbapp_live_smoke against the live edge.
  [[nodiscard]] static Result<AniDbApp, ProviderError> with_endpoint(std::string api);

 private:
  explicit AniDbApp(http::Client client, std::string api,
                    http::Fingerprint fingerprint)
      : http_(std::move(client)), api_(std::move(api)), fingerprint_(fingerprint) {}

  // JSON GET (Accept: application/json). Chrome-fingerprinted like every fetch.
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> json_get(
      const std::string& url) const;
  // HTML GET; a challenge interstitial becomes Forbidden{403} so the walk hops
  // instead of reading it as an answer.
  [[nodiscard]] Result<std::string, ProviderError> page_get(
      const std::string& url) const;

  [[nodiscard]] Result<std::vector<detail::Episode>, ProviderError> fetch_episodes(
      std::string_view site_id) const;
  [[nodiscard]] Result<std::vector<detail::LanguageRow>, ProviderError>
  fetch_languages(std::int64_t ep_id) const;
  [[nodiscard]] Result<bool, ProviderError> has_dub(std::int64_t ep_id) const;
  // Index of the last dubbed episode (dub availability assumed a prefix; a
  // per-episode probe is what a 1000-episode show cannot afford). nullopt = no
  // dub. See anidbapp.rs dub_prefix for the perforation caveats.
  [[nodiscard]] Result<std::optional<std::size_t>, ProviderError> dub_prefix(
      const std::vector<detail::Episode>& eps) const;
  [[nodiscard]] Result<std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>,
                        ProviderError>
  probe_ids(std::string_view slug) const;
  // Variant matching the quality cap, or nullopt so resolve keeps the master.
  [[nodiscard]] std::optional<std::string> cap_variant(const std::string& master_url,
                                                       Quality quality) const;

  http::Client http_;
  std::string api_;
  // Chrome for the live provider (anidb.app fingerprints the ClientHello);
  // None for the fixture-server test seam. Set once at construction.
  http::Fingerprint fingerprint_ = http::Fingerprint::Chrome;
};

}  // namespace shigoku::anidbapp
