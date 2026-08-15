// megaplay.hpp — megaplay.buzz StreamProvider (P13, ROD-445). Ported from
// sabigoku src/providers/megaplay.rs. Tier-A, MAL-keyed:
// `/stream/mal/{mal}/{ep}/{lang}` — the show handle is the stringified MAL id,
// episode labels are true MAL numbers (senshi-shaped key, zero resolve changes).
//
// No catalog: the MAL route IS the index, so `search` is structurally
// Unsupported (never an absence verdict — a later mal_id must not be blocked by
// a stale negative). No listing endpoint either: `episodes` probes ep 1 for
// existence, then mints "1".."N" from `count_hint`.
//
// Two-step resolve, no decryption:
//   1. GET embed  -> scrape `data-id` (the only sub/dub fork). 200 with no
//      data-id = not stocked.
//   2. GET /stream/getSources?id=... -> cleartext JSON (master m3u8 + softsubs).
//
// Segments are BOTH fake-extension cloaked (.jpg) and PNG-header cloaked, so the
// link sets cloaked_segments=true AND decloak_segments=true: playback routes
// through the P12 de-cloak proxy (unlike senshi/anibd which set decloak false).
// The whole CDN chain 403s without the megaplay referer + browser UA.
//
// Like anibd.hpp/senshi.hpp, the pure helpers are free functions in `detail`
// over string_view/bytes so the golden contract runs offline; the .cpp keeps
// the getSources JSON DTO walking private.

#pragma once

#include <cstddef>
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

namespace shigoku::megaplay {

inline constexpr const char* kHost = "https://megaplay.buzz";
// Every downstream CDN host gates on this exact origin.
inline constexpr const char* kStreamReferer = "https://megaplay.buzz/";
// Current Chrome UA; a no-UA request 403s on every megaplay host.
inline constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
// Bound on a scraped data-id before URL splice. Real ids are 4-6 digits today.
inline constexpr std::size_t kMaxDataIdLen = 20;
// Cap English subtitle probes per resolve (hostile getSources flood guard).
inline constexpr std::size_t kMaxSubtitleProbes = 6;

// --- Pure helpers (megaplay.rs file-private fns), exposed for golden tests ---
namespace detail {

// A getSources track that survived argv-vetting (non-null, absolute, clean
// file). `kind` "thumbnails" is the seekbar sprite, not a subtitle.
struct Track {
  std::string file;
  std::optional<std::string> label;
  std::optional<std::string> kind;
  bool is_default = false;
  friend bool operator==(const Track&, const Track&) = default;
};

// The mapped stream: the mpv-ready link plus the vetted tracks the softsub pick
// draws from (megaplay.rs Sources).
struct Sources {
  StreamLink link;
  std::vector<Track> tracks;
};

// Pure over getSources response bytes (03 §8.4). Softsub only rides a `sub`
// resolve. Err(Decode) on a missing/unsafe stream url; unsafe tracks are
// dropped, never fatal. The picked sub_url is SSRF-guarded here (it reaches mpv
// --sub-file unproxied) (megaplay.rs map_sources).
[[nodiscard]] Result<Sources, ProviderError> map_sources(std::string_view raw, Translation tt);

// Host `default` wins, else an english-labeled track, else the first
// subtitle-shaped track (ROD-354). Tracks are already argv-vetted. Returns an
// index into `tracks`; nullopt when none qualify (megaplay.rs pick_subtitle).
[[nodiscard]] std::optional<std::size_t> pick_subtitle(const std::vector<Track>& tracks);

// `captions` or kind-less counts as a subtitle; `thumbnails` and any unknown
// kind do not (never a wrong --sub-file) (megaplay.rs is_subtitle_track).
[[nodiscard]] bool is_subtitle_track(const Track& t);

// English-labeled captions in host order, capped (ROD-377). Returns the files
// (borrowed views into `tracks`) for the cue probe (megaplay.rs
// english_captions).
[[nodiscard]] std::vector<std::string_view> english_captions(const std::vector<Track>& tracks);

// MAL-keyed embed URL. `{sub|dub}` is Translation's wire tag (megaplay.rs
// embed_url).
[[nodiscard]] std::string embed_url(std::string_view host, std::string_view mal_id,
                                    std::string_view ep_label, Translation tt);

// First numeric `data-id` (quoted or bare), bounded. nullopt when none. Wins
// over sibling `data-realid`/`data-mediaid` (megaplay.rs parse_data_id).
[[nodiscard]] std::optional<std::string_view> parse_data_id(std::string_view html);

// Positional "1".."n". A hint-less caller degrades to 1, never 0 (0 collides
// with the not-stocked verdict). Re-clamps to kMaxEpisodeHint (megaplay.rs
// labels).
[[nodiscard]] std::vector<std::string> labels(std::uint32_t n);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class MegaPlay final : public StreamProvider {
 public:
  [[nodiscard]] static Result<MegaPlay, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "megaplay"; }
  [[nodiscard]] std::string_view display_name() const override { return "MegaPlay"; }

  // Tier A: mal_id -> free key. No mal_id -> nullopt; no tier-C recovery.
  [[nodiscard]] std::optional<std::string> canonical_key(const Enrichment& show) const override;

  // Structurally unsupported: the MAL route is the whole index.
  [[nodiscard]] Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view query, const SearchOptions& opts) const override;
  [[nodiscard]] bool supports_search() const override { return false; }

  // Listing-less: probe ep 1 for existence, then mint "1".."N" from count_hint.
  [[nodiscard]] Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view provider_id, Translation translation,
      std::optional<std::uint32_t> count_hint) const override;

  [[nodiscard]] Result<StreamLink, ProviderError> resolve(std::string_view provider_id,
                                                          std::string_view episode,
                                                          Translation translation,
                                                          Quality quality) const override;

  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view cover_ref) const override;

  // Test-only seam: point at a fixture server instead of the live host.
  [[nodiscard]] static Result<MegaPlay, ProviderError> with_host(std::string host);

 private:
  explicit MegaPlay(http::Client client, std::string host)
      : http_(std::move(client)), host_(std::move(host)) {}

  // GET the embed page (existence + sub/dub fork). Referer only; any 2xx.
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> embed_get(
      const std::string& url) const;
  // GET getSources as XHR (the host gates the JSON on these headers).
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> xhr_get(
      const std::string& url) const;

  // Count " --> " cue markers in one vtt. SSRF-guarded, body counted only.
  // nullopt on any failure (a dead sidecar drops the candidate, not the
  // decision) (megaplay.rs probe_cues).
  [[nodiscard]] std::optional<std::size_t> probe_cues(const std::string& url) const;
  // Upgrade the metadata pick only when a candidate has strictly more cues.
  // nullopt keeps the baseline (megaplay.rs refine_subtitle_by_cues).
  [[nodiscard]] std::optional<std::string> refine_subtitle_by_cues(
      const std::vector<std::string_view>& candidates, const std::string& baseline) const;

  http::Client http_;
  std::string host_;
};

}  // namespace shigoku::megaplay
