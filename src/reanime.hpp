// reanime.hpp — Re:ANIME (reanime.cz) StreamProvider, streaming from
// flixcloud.cc. Tier A, AniList-keyed: the show handle is the stringified
// AniList id; episode numbers are absolute.
//
// Chain:
//   1. GET reanime /api/flix/<anilist>/<ep> -> JSON server list. Sub and dub
//      share one flixcloud embed per server: the dub is an audio rendition of
//      the same stream. An empty list is "not stocked".
//   2. GET the embed's SvelteKit page data, `/e/<id>/__data.json?v=N`
//      (devalue-encoded). Its field names are derived from a per-request
//      seed (field_names): two key fragments, an iv, a token, a ~400-byte
//      WebAssembly module generated per request, the audio layout, and soft
//      ASS subtitles.
//   3. GET flixcloud /api/m3u8/<token> -> the ciphertext and a third key
//      fragment, under keys hashed from the token.
//   4. The module's _r mixes the three fragments (seeded by _s), then
//      PBKDF2-HMAC-SHA256 -> XOR with the seed -> SHA-256 is the AES-256-CBC
//      key that opens the master url. The module's _c is the playlist key.
//
// Every playlist is base64 + XOR under that key, so links carry an Xor
// playlist_cipher and play through the loopback proxy, which also cuts the
// master to the wanted audio_language. Segments are standard HLS AES-128.
// No search: the AniList route is the whole index (megaplay's shape), and
// there is no episode listing, so `episodes` probes ep 1 and mints from the
// count hint.

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

namespace shigoku::reanime {

inline constexpr const char* kBase = "https://reanime.cz";
inline constexpr const char* kReferer = "https://reanime.cz/";

// --- Pure helpers (exposed for golden tests) --------------------------------
namespace detail {

struct Server {
  std::string name;  // "HD-1", "HD-2", "H-SUB"
  std::string link;  // the flixcloud embed
  Translation track = Translation::Sub;
  bool hardsub = false;
  friend bool operator==(const Server&, const Server&) = default;
};

// `{success, servers: [{serverName, dataLink, dataType, hardsub}]}`. Entries
// with an unknown dataType or an unusable link drop. Err(Decode) when the
// body is not that shape.
[[nodiscard]] Result<std::vector<Server>, ProviderError> parse_servers(std::string_view raw_json);

// The servers carrying `track`, in the order resolve tries them: HD-2 (the
// one with a 480p rung), HD-1, then the rest as listed.
[[nodiscard]] std::vector<Server> servers_for(const std::vector<Server>& all, Translation track);

// `<origin>/e/<id>?v=N` -> `<origin>/e/<id>/__data.json?v=N`. nullopt unless
// the link is an absolute https `/e/<id>` embed past the fetch guard.
[[nodiscard]] std::optional<std::string> data_url(std::string_view embed_link);

// The page's field names, derived from its seed by a SHA-256 hex chain.
struct FieldNames {
  std::string key_frag;   // kf_…, inside the crypto container
  std::string iv;         // ivf_…
  std::string container;  // cd_…
  std::string array;      // ad_…
  std::string object;     // od_…
  std::string token;      // top level
  std::string key_frag2;  // top level
  friend bool operator==(const FieldNames&, const FieldNames&) = default;
};
[[nodiscard]] FieldNames field_names(std::string_view seed);

struct Subtitle {
  std::string url;
  std::string label;
  bool is_default = false;
  friend bool operator==(const Subtitle&, const Subtitle&) = default;
};

// What the unlock and the link need from the embed's page data.
struct EmbedPage {
  std::string seed;
  std::vector<std::uint8_t> wasm;
  std::vector<std::uint8_t> frag1;
  std::vector<std::uint8_t> frag2;
  std::vector<std::uint8_t> iv;
  std::string token;
  std::string audio_type;  // "dual" (Japanese + English) | "native" | ""
  std::vector<Subtitle> subtitles;
};

// The page node of a `__data.json` that carries the seed, its seed-named
// fields resolved. Err(Decode) naming what is missing.
[[nodiscard]] Result<EmbedPage, ProviderError> parse_embed(std::string_view data_json);

// The token route's reply, under keys hashed from the token.
struct TokenReply {
  std::vector<std::uint8_t> ciphertext;
  std::vector<std::uint8_t> frag3;
};
[[nodiscard]] Result<TokenReply, ProviderError> parse_token_reply(std::string_view raw_json,
                                                                  std::string_view token);

struct Unlocked {
  std::string master;
  std::vector<std::uint8_t> playlist_key;
};

// Run the page's module and the key schedule; open the master url.
[[nodiscard]] Result<Unlocked, ProviderError> unlock(const EmbedPage& page,
                                                     const TokenReply& reply);

// Sub: the default English track, else an English "full" one, else the first
// English one that is not signs-only or an AI dubtitle. Dub: the English
// signs-and-songs track. Only urls past the fetch guard qualify.
[[nodiscard]] std::optional<std::string> pick_subtitle(const std::vector<Subtitle>& subs,
                                                       Translation track);

// A positive decimal id, no leading zero, at most 10 digits.
[[nodiscard]] bool valid_show_id(std::string_view id);

// Positional "1".."n", clamped to [1, kMaxEpisodeHint].
[[nodiscard]] std::vector<std::string> labels(std::uint32_t n);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class ReAnime final : public StreamProvider {
 public:
  [[nodiscard]] static Result<ReAnime, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "reanime"; }
  [[nodiscard]] std::string_view display_name() const override { return "Re:ANIME"; }

  // Tier A: a real (positive) AniList id. A MAL-only row has none.
  [[nodiscard]] std::optional<std::string> canonical_key(const Enrichment& show) const override;

  [[nodiscard]] Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view query, const SearchOptions& opts) const override;
  [[nodiscard]] bool supports_search() const override { return false; }

  // Probe ep 1 for a server on the track, then mint "1".."N" from the hint.
  [[nodiscard]] Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view provider_id, Translation translation,
      std::optional<std::uint32_t> count_hint) const override;

  // Quality is not applied here: the master goes to the player whole (its
  // audio is separate renditions). Servers are tried in servers_for order;
  // the last failure is returned.
  [[nodiscard]] Result<StreamLink, ProviderError> resolve(std::string_view provider_id,
                                                          std::string_view episode,
                                                          Translation translation,
                                                          Quality quality) const override;

  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view cover_ref) const override;

  // Test-only seam: the site at `base`; when `embed_origin` is non-empty, the
  // embed host's fetches go there instead (the fetch guard still judges the
  // real link).
  [[nodiscard]] static Result<ReAnime, ProviderError> with_endpoints(std::string base,
                                                                     std::string embed_origin);

 private:
  ReAnime(http::Client client, std::string base, std::string embed_origin)
      : http_(std::move(client)), base_(std::move(base)), embed_origin_(std::move(embed_origin)) {}

  [[nodiscard]] Result<std::vector<detail::Server>, ProviderError> fetch_servers(
      std::string_view anilist_id, std::uint32_t episode) const;
  [[nodiscard]] Result<StreamLink, ProviderError> resolve_on(const detail::Server& server,
                                                             Translation translation) const;
  [[nodiscard]] Result<std::vector<std::uint8_t>, ProviderError> get(const std::string& url,
                                                                     const std::string& referer,
                                                                     bool json) const;
  [[nodiscard]] std::string embed_fetch_url(const std::string& url) const;

  http::Client http_;
  std::string base_;
  std::string embed_origin_;
};

}  // namespace shigoku::reanime
