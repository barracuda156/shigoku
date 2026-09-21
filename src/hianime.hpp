// hianime.hpp — hianime.at StreamProvider (P47). Tier-C: no canonical-keyed
// endpoint (nullopt), so the walk binds it via title search. The ZokoAnime
// server's data-hash carries the MAL id too (AniSkip fodder), but nothing in
// this v0 surfaces it — StreamLink has no id field to hand it onward.
//
// Chain, all under kApi: /search?keyword= (HTML cards) -> /api/theme/episode/
// list/<id> (HTML fragment inside a JSON envelope) -> /api/theme/episode/
// servers?episodeId=<id> (same envelope shape) -> the ZokoAnime data-hash,
// base64 of an absolute embed url on a THIRD-PARTY host (zokoanime.video) ->
// that embed's `window.__P` is base64(JSON) XORed under the repeating key
// "otaku-embed-v1" -> {src: a plain m3u8 master, subtitles[]}. The master
// wants the embed's own origin as Referer; no cloaking, no envelope on the
// media itself.
//
// Walked live 2026-09-20/21: plain requests pass today (no Cloudflare
// interstitial), so this provider does NOT set http::Request::fingerprint —
// unlike anidbapp, whose edge scores the TLS ClientHello. If the site turns a
// challenge on, that seam is the fix (see PROVIDER_INTEL.md §6).
//
// Episode numbering is per-title (a season is its own search hit with its own
// id), so — unlike anidbapp — there is no franchise-absolute offset to
// convert: the site's data-number IS the canonical label.
//
// Like anidbapp.hpp, the pure helpers are free functions in `detail` over
// string_view/bytes so the golden contract runs offline; the .cpp keeps the
// JSON/HTML DTO walking + transport private.

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

namespace shigoku::hianime {

inline constexpr const char* kApi = "https://hianime.at";
inline constexpr const char* kReferer = "https://hianime.at/";
// XOR key over the embed's `window.__P` base64 payload (ASCII literal, cheap
// to widen if the site rotates it — no libcrypto involved).
inline constexpr const char* kEmbedKey = "otaku-embed-v1";

// --- Pure helpers (exposed for golden tests) --------------------------------
namespace detail {

// A search-result card: the site's numeric id (the search page's canonical
// key into every later endpoint) + its display title.
struct Card {
  std::string id;
  std::string title;
  friend bool operator==(const Card&, const Card&) = default;
};

// Result cards on a /search page: every `<h3 class="film-name"><a href=...>`
// BEFORE the page's `id="main-sidebar"` marker — the sidebar's own trending
// rail reuses the identical card markup after it, and would otherwise read as
// more hits. No marker found -> scan the whole page (a template change should
// degrade to over-matching, not to silently losing every hit).
[[nodiscard]] std::vector<Card> parse_search_cards(std::string_view html);

// Trailing `-<digits>` of an href's last path segment (nullopt if none) — the
// site's numeric id, e.g. ".../frieren-beyond-journeys-end-481" -> "481".
[[nodiscard]] std::optional<std::string> trailing_id(std::string_view href);

// One episode row: the site's numeric episode id + its (already canonical,
// per-title) 1-based number.
struct EpisodeRow {
  std::uint32_t number = 0;
  std::string id;
  friend bool operator==(const EpisodeRow&, const EpisodeRow&) = default;
};

// `/api/theme/episode/list/<id>` answers `{"html": "<escaped fragment>"}`;
// this unwraps the JSON envelope THEN scrapes every `data-number="N"
// data-id="ID"` row. Ascending by number, one row per number (first listed
// wins a dup). Err(Decode) when the body isn't the expected JSON envelope.
[[nodiscard]] Result<std::vector<EpisodeRow>, ProviderError> parse_episode_list(
    std::string_view raw_json);

// The ZokoAnime `data-hash` for `translation` out of a
// `/api/theme/episode/servers` response (same JSON-envelope shape as
// above). nullopt = that track truly isn't offered (HD-1/Vidstream-2 rows
// are ignored — only ZokoAnime's hash is decodable). Err(Decode) on a
// malformed envelope.
[[nodiscard]] Result<std::optional<std::string>, ProviderError> find_zokoanime_hash(
    std::string_view raw_json, Translation translation);

// base64 `data-hash` -> the absolute embed url it names. nullopt on bad
// base64/utf8 or a non-absolute result (the SSRF guard runs separately,
// before the fetch).
[[nodiscard]] std::optional<std::string> decode_hash(std::string_view hash_b64);

// The base64 payload inside `window.__P="..."` on the embed page. nullopt if
// the marker is absent or unterminated.
[[nodiscard]] std::optional<std::string_view> extract_p_blob(std::string_view html);

// Repeating-key XOR (its own inverse) — `kEmbedKey` opens the embed's `__P`
// payload after base64 decode.
[[nodiscard]] std::vector<std::uint8_t> xor_repeat(const std::vector<std::uint8_t>& data,
                                                    std::string_view key);

// One subtitle track off the decoded embed JSON.
struct Subtitle {
  std::string lang;
  std::string label;
  bool is_default = false;
  std::string src;
  friend bool operator==(const Subtitle&, const Subtitle&) = default;
};

// The decoded embed payload: the HLS master (`src`, absent on a malformed/
// stripped payload) + whatever subtitle tracks it lists.
struct Embed {
  std::optional<std::string> src;
  std::vector<Subtitle> subtitles;
  friend bool operator==(const Embed&, const Embed&) = default;
};

// The `__P` plaintext (post base64+XOR) -> {src, subtitles[]}. Err(Decode) if
// it isn't a JSON object.
[[nodiscard]] Result<Embed, ProviderError> parse_embed(std::string_view plaintext);

// Vet a scraped stream/embed url before it is fetched or handed to mpv:
// absolute + argv-clean + past the SSRF guard.
[[nodiscard]] bool stream_url_ok(std::string_view url);

// Single-pass HTML entity decode (named amp/lt/gt/quot/apos/nbsp + &#NN; /
// &#xNN;). Unknown entities stay literal.
[[nodiscard]] std::string decode_entities(std::string_view s);

// Percent-encode a query value (RFC 3986 unreserved passes, everything else
// %XX; no '+' for space — this is a URL query string, not a form body).
[[nodiscard]] std::string url_encode(std::string_view s);

// `scheme://host[:port]/` off an absolute url (nullopt if not absolute) — the
// Referer the master/segments want (the embed's own origin).
[[nodiscard]] std::optional<std::string> origin_of(std::string_view absolute_url);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class Hianime final : public StreamProvider {
 public:
  [[nodiscard]] static Result<Hianime, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "hianime"; }
  [[nodiscard]] std::string_view display_name() const override { return "HiAnime"; }

  // No canonical-keyed endpoint: nullopt sends the walk to tier C.
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

  // Test-only seam: point at a loopback fixture server instead of the live
  // site. The embed hop stays independent of this (its url comes fully
  // qualified out of the decoded data-hash), so a test can aim it at a SECOND
  // fixture server without any extra plumbing here.
  [[nodiscard]] static Result<Hianime, ProviderError> with_endpoint(std::string api);

 private:
  explicit Hianime(http::Client client, std::string api)
      : http_(std::move(client)), api_(std::move(api)) {}

  // JSON GET (Accept: application/json, X-Requested-With like the site's own
  // fetch), for the two /api/theme/... endpoints.
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> json_get(
      const std::string& url) const;
  // HTML GET (search page, embed page).
  [[nodiscard]] Result<std::string, ProviderError> page_get(const std::string& url) const;

  [[nodiscard]] Result<std::vector<detail::EpisodeRow>, ProviderError> fetch_episodes(
      std::string_view id) const;
  [[nodiscard]] Result<bool, ProviderError> has_dub(const std::string& episode_id) const;
  // Index of the last dubbed episode (dub availability assumed a prefix, like
  // anidbapp's dub_prefix — a per-episode probe is what a long-running show
  // cannot afford). nullopt = no dub.
  [[nodiscard]] Result<std::optional<std::size_t>, ProviderError> dub_prefix(
      const std::vector<detail::EpisodeRow>& eps) const;

  http::Client http_;
  std::string api_;
};

}  // namespace shigoku::hianime
