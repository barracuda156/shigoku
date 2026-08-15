// anilibria.hpp — AniLibria (AniLiberty API v1) StreamProvider. Greenfield (no
// sabigoku reference); shape follows the anidbapp/anibd provider pattern.
//
// Chain: /api/v1/app/search/releases?query= (bare JSON array of releases) ->
// /api/v1/anime/releases/{id} (episodes[] with DIRECT per-quality HLS urls:
// hls_480/hls_720/hls_1080). No crypto, no cloaking, no TLS fingerprinting, no
// auth; plain VOD HLS on the provider's own CDN.
//
// The API exposes no MAL/AniList ids anywhere, so canonical_key is nullopt and
// tier-C title search binds it (romaji lives in name.english, the main title
// is Russian). Single-track Russian voiceover: translation selects nothing and
// both tracks list the same episodes.
//
// Quality is real here — three separate media playlists per episode — so
// resolve picks a field instead of capping a master ladder.
//
// Like the sibling providers, the pure helpers are free functions in `detail`
// over string_view so the golden contract runs offline; the .cpp keeps the
// JSON DTO walking + transport private.

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

namespace shigoku::anilibria {

inline constexpr const char* kApi = "https://api.anilibria.app";
// Ordinal sanity ceiling: episodes are numbered in the hundreds at most; past
// this a value is API junk, and %g formatting could go scientific.
inline constexpr double kMaxOrdinal = 100000.0;

// --- Pure helpers, exposed for golden tests --------------------------------
namespace detail {

// One search hit from the releases array.
struct Hit {
  std::int64_t id = 0;
  std::string title_main;                        // name.main (Russian).
  std::optional<std::string> title_english;      // name.english (romaji).
  std::optional<std::uint32_t> year;
  std::optional<std::uint32_t> episodes_total;
  friend bool operator==(const Hit&, const Hit&) = default;
};

// One episodes[] row. Ordinals are fractional for specials (11.5), so the
// number is kept as-is and label formatting owns the string form.
struct Episode {
  double ordinal = 0.0;
  std::string name;
  std::optional<std::string> hls480;
  std::optional<std::string> hls720;
  std::optional<std::string> hls1080;
  friend bool operator==(const Episode&, const Episode&) = default;
};

// A parsed release: its episodes plus the API's own availability verdicts.
struct Release {
  std::vector<Episode> episodes;
  bool blocked_geo = false;        // is_blocked_by_geo (vantage-dependent).
  bool blocked_copyright = false;  // is_blocked_by_copyrights.
};

// Parse the search array. Rows missing a positive id or a main title are
// dropped; a body that is valid JSON but not an array is Err(Decode) — an API
// change must NOT read as "no results".
[[nodiscard]] Result<std::vector<Hit>, ProviderError> parse_search(
    std::string_view raw_json);

// Parse a release object. Episodes are sorted ascending by ordinal and
// deduplicated by label (first listed wins); rows with a negative,
// non-finite, or absurd ordinal are dropped. Err(Decode) when the body is not
// a JSON object.
[[nodiscard]] Result<Release, ProviderError> parse_release(std::string_view raw_json);

// Canonical label for an ordinal: integral -> "7", fractional -> "11.5".
[[nodiscard]] std::string format_ordinal(double ordinal);

// Whether a user/stored label names this ordinal. Digit-only labels compare
// numerically ("01" matches 1); anything else must equal the formatted form
// ("11.5"). Empty never matches.
[[nodiscard]] bool label_matches(double ordinal, std::string_view label);

// The episode's playlist for a requested quality: exact rung first, then the
// nearest below, then upward — Best walks 1080/720/480, Worst walks
// 480/720/1080. nullopt when the row carries no urls at all.
[[nodiscard]] std::optional<std::string> pick_hls(const Episode& ep, Quality quality);

// Percent-encode into an application/x-www-form-urlencoded value (unreserved
// pass, ' ' -> '+', else %XX). Used for the search query= param; Cyrillic
// queries are multi-byte UTF-8 and every byte encodes.
[[nodiscard]] std::string form_urlencode(std::string_view s);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class AniLibria final : public StreamProvider {
 public:
  [[nodiscard]] static Result<AniLibria, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "anilibria"; }
  [[nodiscard]] std::string_view display_name() const override { return "AniLibria"; }

  // The API carries no MAL/AniList ids: nullopt sends the walk to tier C, and
  // the match stays C-confidence (title/year scoring only).
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

  // Poster paths arrive site-relative ("/storage/..."); a relative ref joins
  // the API origin, an absolute http(s) ref passes through.
  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view cover_ref) const override;

  // Test-only seam: point at a loopback fixture server instead of the live API.
  [[nodiscard]] static Result<AniLibria, ProviderError> with_endpoint(std::string api);

 private:
  explicit AniLibria(http::Client client, std::string api)
      : http_(std::move(client)), api_(std::move(api)) {}

  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> json_get(
      const std::string& url) const;
  // Release fetch + the block gate: a geo- or copyright-blocked release is
  // Forbidden so the walk hops — an empty listing would stamp a durable
  // absence for what is only a vantage problem.
  [[nodiscard]] Result<detail::Release, ProviderError> fetch_release(
      std::string_view show_id) const;

  http::Client http_;
  std::string api_;
};

}  // namespace shigoku::anilibria
