// anibd.hpp — epeng.animeapps.top StreamProvider (P12a, ROD-515). Ported from
// sabigoku src/providers/anibd.rs. Tier-A, AniList-keyed:
// `api2.php?epid={anilistId}`. Pure JSON catalog, no search endpoint, no
// crypto. Two player types behind apilink.php: play2.php (hardsub) and
// playsub.php (softsub with jwplayer-style VTT tracks).
//
// Resolve chain: api2.php (catalog) -> apilink.php (player links) -> player
// page HTML -> regex videoUrl -> HLS master. Referer + UA required.
//
// First AniList-keyed provider: canonical_key = anilist_id string, so it needs
// no MAL id (senshi/megaplay key on MAL). cloaked_segments=true (segments served
// as .jpg), decloak_segments=false (no byte-prefix decoy — senshi-shaped, no
// proxy).
//
// Like senshi.hpp, the pure helpers are free functions in `detail` over
// string_view/bytes so the golden contract runs offline; the .cpp keeps the
// JSON DTO walking private.

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

namespace shigoku::anibd {

inline constexpr const char* kApi = "https://epeng.animeapps.top";
inline constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
inline constexpr const char* kReferer = "https://epeng.animeapps.top/";
// Player pages tried per resolve before giving up (anibd.rs MAX_PLAYER_TRIES).
inline constexpr std::size_t kMaxPlayerTries = 3;

// --- Pure helpers (anibd.rs file-private fns), exposed for golden tests -----
namespace detail {

// One (episode number, opaque catalog link) pair within an audio group.
struct EpisodeLink {
  std::uint32_t number = 0;
  std::string link;
  friend bool operator==(const EpisodeLink&, const EpisodeLink&) = default;
};

// A parsed catalog group: "sub" or "dub" and its episode links.
struct CatalogGroup {
  std::string audio;  // "sub" | "dub"
  std::vector<EpisodeLink> episodes;
};

// One tracks[] entry from a playsub.php page (jwplayer soft subtitle).
struct SubTrack {
  std::string file;
  std::string label;
};

// "dub" if server_name contains "dub" (case-insensitive), else "sub".
[[nodiscard]] std::string_view audio_kind(std::optional<std::string_view> server_name);

// Parse the api2.php catalog JSON into typed groups. Drops empty groups; drops
// episodes whose number/link is missing (anibd.rs parse_catalog).
[[nodiscard]] Result<std::vector<CatalogGroup>, ProviderError> parse_catalog(
    std::string_view raw_json);

// Sorted, deduplicated episode labels across all audio kinds (u32 numeric sort).
[[nodiscard]] std::vector<std::string> episode_labels(const std::vector<CatalogGroup>& groups);

// Find the opaque catalog link for a specific episode + translation. nullopt if
// the track/episode is not offered.
[[nodiscard]] std::optional<std::string> find_link(const std::vector<CatalogGroup>& groups,
                                                   std::uint32_t episode, Translation tt);

// Extract the ArtPlayer stream URL from player HTML: `videoUrl` first
// (legacy/Anilili), then bare `url` (live site). Relative paths resolved against
// `page_url` (anibd.rs extract_video_url).
[[nodiscard]] std::optional<std::string> extract_video_url(std::string_view html,
                                                           std::string_view page_url);

// Find `key: "value"` or `key: 'value'` in HTML and resolve relative paths
// against `page_url` (anibd.rs extract_config_value).
[[nodiscard]] std::optional<std::string> extract_config_value(std::string_view html,
                                                              std::string_view key,
                                                              std::string_view page_url);

// The origin (scheme + authority) of a URL, e.g. https://player.example:8080.
// Falls back to the input on a parse failure (anibd.rs url_origin).
[[nodiscard]] std::string url_origin(std::string_view url);

// Resolve a possibly-relative URL against a base page URL (anibd.rs resolve_url;
// url crate join semantics via the libcurl URL API).
[[nodiscard]] std::string resolve_url(std::string_view raw, std::string_view base_url);

// Parse jwplayer-style `tracks: [{...}, ...]` soft subtitles from player HTML.
// Only http(s) files with kind captions/subtitles (or no kind) (anibd.rs
// parse_subtitles + parse_sub_track).
[[nodiscard]] std::vector<SubTrack> parse_subtitles(std::string_view html);

// Extract a JSON string field: `"name": "value"` -> value (anibd.rs
// json_string_field).
[[nodiscard]] std::optional<std::string> json_string_field(std::string_view obj,
                                                           std::string_view name);

// Pick the best subtitle: English-labeled preferred, then first (anibd.rs
// pick_subtitle). Returns an index into `tracks`; nullopt when empty.
[[nodiscard]] std::optional<std::size_t> pick_subtitle(const std::vector<SubTrack>& tracks);

// Percent-encode a byte string into an application/x-www-form-urlencoded value
// (url::form_urlencoded::byte_serialize): unreserved bytes pass, ' ' -> '+',
// everything else -> %XX. Used for the apilink.php `data` param.
[[nodiscard]] std::string form_urlencode(std::string_view s);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class AniBd final : public StreamProvider {
 public:
  [[nodiscard]] static Result<AniBd, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "anibd"; }
  [[nodiscard]] std::string_view display_name() const override { return "AniBD"; }

  // Tier A: the AniList id IS the API key (first AniList-keyed provider).
  [[nodiscard]] std::optional<std::string> canonical_key(const Enrichment& show) const override;

  // anibd has no search endpoint: structurally Unsupported.
  [[nodiscard]] Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view query, const SearchOptions& opts) const override;
  [[nodiscard]] bool supports_search() const override { return false; }

  [[nodiscard]] Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view provider_id, Translation translation,
      std::optional<std::uint32_t> count_hint) const override;

  [[nodiscard]] Result<StreamLink, ProviderError> resolve(std::string_view provider_id,
                                                          std::string_view episode,
                                                          Translation translation,
                                                          Quality quality) const override;

  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view cover_ref) const override;

  // Test-only seam: point at a fixture server instead of the live API.
  [[nodiscard]] static Result<AniBd, ProviderError> with_endpoint(std::string api);

 private:
  explicit AniBd(http::Client client, std::string api)
      : http_(std::move(client)), api_(std::move(api)) {}

  // api2.php / apilink.php on the API origin (JSON, Referer + browser Accept).
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> api_get(
      const std::string& url) const;
  // Player pages: a different origin that gates on its own Referer + a
  // browser-shaped Accept (Cloudflare 301s anything else).
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> player_get(
      const std::string& url, const std::string& referer) const;

  [[nodiscard]] Result<std::vector<detail::CatalogGroup>, ProviderError> fetch_catalog(
      std::string_view anilist_id) const;
  // Parse + guard a soft subtitle from a player page: absolute, argv-clean, AND
  // past the SSRF guard (it reaches mpv --sub-file unproxied).
  [[nodiscard]] std::optional<std::string> guarded_subtitle(std::string_view html) const;

  http::Client http_;
  std::string api_;
};

}  // namespace shigoku::anibd
