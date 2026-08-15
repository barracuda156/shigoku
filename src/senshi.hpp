// senshi.hpp — senshi.live StreamProvider (P4, 03 §8.2). Ported from
// sabigoku src/providers/senshi.rs: plain REST JSON keyed by MAL id, no
// crypto/captcha. API surface: POST /anime/filter (search), GET
// /episodes/{mal} (list), GET /episode-embeds/{mal}/{ep} (resolve),
// /posters/{mal}.webp (cover, via cover_request).
//
// Pure helpers are free functions over bytes/string_view (mirrors the Rust
// file-private fns) so the ~20-test golden contract runs offline without a
// network fixture, per PORT_CPP.md P4.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "hls.hpp"
#include "http.hpp"
#include "provider.hpp"
#include "result.hpp"

namespace shigoku::senshi {

inline constexpr const char* kApi = "https://senshi.live";
// Stream CDN (ninstream) 403s a refererless GET; gate on this origin.
inline constexpr const char* kStreamReferer = "https://senshi.live/";

// --- Pure helpers (senshi.rs file-private fns), exposed for golden tests --
namespace detail {

// One /anime/filter row -> tier-C candidate (senshi.rs map_anime). Needs the
// raw JSON row plus its already-decoded fields since C++ has no derive; the
// .cpp keeps the DTO shape private and calls this from the JSON walk.
struct FilterRow {
  std::uint64_t id = 0;
  std::optional<std::string> title;
  std::optional<std::string> title_english;
  std::optional<std::string> ani_episodes;  // JSON string ("16"), may be "?".
  std::optional<std::string> ani_status;
  std::optional<std::uint32_t> ani_year;
};

[[nodiscard]] SearchHit map_anime(const FilterRow& row);

// Fold senshi prose onto the canonical airing vocab (ROD-296).
[[nodiscard]] std::optional<std::string> map_status(std::optional<std::string_view> s);

// Leading digit run only ("23 min per ep" -> 23). nullopt on no leading digit.
[[nodiscard]] std::optional<std::uint32_t> parse_leading_uint(std::string_view s);

// Integral drops the decimal ("1"); fractional keeps it ("13.5"). Defensive
// non-finite/negative branch returns "0" (parse_episodes filters those first).
[[nodiscard]] std::string ep_label(double n);

// Parse /episodes JSON into numerically-sorted labels. Drops non-finite and
// non-positive ep_id (phantom zero AND hostile negatives) before ep_label
// ever sees them (ROD-441 review: the drop is the single gate).
[[nodiscard]] Result<std::vector<std::string>, ProviderError> parse_episodes(
    std::string_view raw_json);

// One /episode-embeds row (senshi.rs Embed).
struct Embed {
  std::optional<std::string> url;
  std::optional<std::string> status;
  std::optional<std::string> server_fm;  // carries "sub.info=..." (ROD-378).
};

// One track in the sidecar sub.info JSON (senshi.rs SubTrack).
struct SubTrack {
  std::optional<std::string> src;
  std::optional<std::string> label;
  bool is_default = false;
};

// Rank a status label for a track (0 = wrong/unranked track).
[[nodiscard]] std::uint8_t match_score(std::optional<std::string_view> status,
                                       Translation tt);

// Best embed for the track; nullopt when not offered. First-wins on a score
// tie (strict >, matches v0.4.7 fold order, not a last-wins max).
[[nodiscard]] std::optional<Embed> pick_embed(const std::vector<Embed>& embeds, Translation tt);

// Percent-decode a query value. Malformed '%' kept literal; '+' stays literal
// (raw URL, not form data).
[[nodiscard]] std::string percent_decode(std::string_view s);

// Pull the percent-decoded sub.info= value out of serverFM. nullopt when
// absent (a true HardSub) or the value is empty.
[[nodiscard]] std::optional<std::string> sub_info_url(std::optional<std::string_view> server_fm);

// Prefer host `default`, then an English-labeled track, then the first.
[[nodiscard]] std::optional<std::string> pick_sub_track(const std::vector<SubTrack>& tracks);

// Pick + guard: absolute, argv-clean, AND past the SSRF guard (the src
// reaches mpv --sub-file unproxied, so argv-vet alone is not enough).
[[nodiscard]] std::optional<std::string> guarded_sub_track(const std::vector<SubTrack>& tracks);

// Episode label shape: digits, at most one '.'. Rejects path tricks before a
// URL splice.
[[nodiscard]] Result<Unit, ProviderError> guard_ep_label(std::string_view s);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class Senshi final : public StreamProvider {
 public:
  [[nodiscard]] static Result<Senshi, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "senshi"; }
  [[nodiscard]] std::string_view display_name() const override { return "Senshi"; }

  [[nodiscard]] std::optional<std::string> canonical_key(const Enrichment& show) const override;

  [[nodiscard]] Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view query, const SearchOptions& opts) const override;

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
  [[nodiscard]] static Result<Senshi, ProviderError> with_endpoint(std::string api);

 private:
  explicit Senshi(http::Client client, std::string api)
      : http_(std::move(client)), api_(std::move(api)) {}

  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> request(
      http::Method method, const std::string& url, std::optional<std::string_view> body) const;
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> cdn_get(
      const std::string& url) const;
  [[nodiscard]] std::optional<std::string> fetch_subtitle(
      std::optional<std::string_view> server_fm) const;
  // Fetch the adaptive master and return the variant matching the quality
  // cap, or nullopt so resolve falls back to the master ladder.
  [[nodiscard]] std::optional<std::string> cap_variant(const std::string& master_url,
                                                        Quality quality) const;

  http::Client http_;
  std::string api_;
};

}  // namespace shigoku::senshi
