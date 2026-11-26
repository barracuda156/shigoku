// miruro.hpp — Miruro (miruro.cx) StreamProvider. Tier C: a show is found by
// title search, and every hit names its AniList and MAL ids, so the binding
// is an id match rather than a title guess.
//
// The site is a SvelteKit app that ships its load data inside the HTML, as
// JavaScript value expressions (uneval.hpp reads them):
//   - `kit.start(app, element, {…, data: [ … ]})` — the page's own data;
//   - `<global>.resolve(<n>, () => <expr>)` — one script per streamed part.
// A part is recognised by what it holds, never by its number.
//
// Chain:
//   1. GET /search?q=<title> -> `items`: {id, external_ids:{anilist:["227"],
//      mal:["227"]}, title:{romaji,english,native}, episode_count,
//      episode_counts:{sub,dub}, season_year}. `id` is the site's own opaque
//      show handle, the provider id here.
//   2. GET /watch/<id>/<slug>?ep=<n> -> `episodes`: [{number, …}] (the whole
//      listing, whatever <n> is) and, for episode <n>, `tracks`:
//      [{track:"ssub"|"sub"|"dub", providers:[{provider, servers:[{server,
//      headers:{Referer,Origin?}, streams:[{url, format:"hls"}]}],
//      subtitles:[{language,label,file,default}]}]}].
//      "ssub" is a clean video with subtitle files beside it, "sub" has them
//      burned in. A server with no streams is an embed for the site's own
//      player and is of no use here. Only the id is known from a search:
//      the page is asked for under a placeholder slug and the site
//      redirects to the show's own, which is then remembered for the run.
//   3. The streams are on third-party hosts and some are dead at any given
//      time, so resolve fetches a candidate's master playlist (with the
//      server's Referer and Origin) and takes the first that answers one.
//
// Some stream hosts refuse a request without the Origin header the server
// entry names: the link carries it (StreamLink::origin).
//
// The site sits behind Cloudflare, which challenges some networks whatever
// the client: a plain request goes first, a 403 escalates the provider to
// the browser handshake (libcurl-impersonate) for the rest of the run.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"
#include "error.hpp"
#include "http.hpp"
#include "provider.hpp"
#include "result.hpp"

namespace shigoku::miruro {

inline constexpr const char* kBase = "https://miruro.cx";
inline constexpr const char* kReferer = "https://miruro.cx/";
// Master playlists resolve may fetch before it gives up on an episode.
inline constexpr std::size_t kMaxProbes = 6;
inline constexpr long kProbeTimeoutSecs = 6;
// Redirects a site page may send before the fetch gives up.
inline constexpr int kMaxRedirects = 3;

// --- Pure helpers (exposed for golden tests) --------------------------------
namespace detail {

// A show handle: 8 to 64 of [A-Za-z0-9_-].
[[nodiscard]] bool valid_show_id(std::string_view id);

// An episode label as it goes into `?ep=`: 1 to 4 digits, optionally a dot
// and 1 or 2 more ("7", "12.5"). No sign, no leading dot.
[[nodiscard]] bool valid_episode(std::string_view label);

// An episode number as its label: 7 -> "7", 12.5 -> "12.5", at most two
// decimals with trailing zeros dropped. nullopt for a negative, non-finite
// or five-digit number.
[[nodiscard]] std::optional<std::string> episode_label(double number);

// The search page's `items`, in page order. An item without a valid id or
// any title is dropped. title is the romaji one, else the English, else the
// native; ids are the first entry of external_ids.anilist / .mal when that
// is a positive decimal; total_episodes is episode_count when positive;
// eps_sub / eps_dub are episode_counts.sub / .dub; year is season_year.
// Ok(empty) when the page carries an empty `items`; Err(Decode) when it
// carries none at all; Err(Forbidden 403) when it is Cloudflare's challenge
// page ("<title>Just a moment").
[[nodiscard]] Result<std::vector<SearchHit>, ProviderError> parse_search(std::string_view html);

// The watch page's episode labels, ascending by number, duplicates dropped.
// Entries whose number has no label (episode_label) are skipped. Errors as
// parse_search; an empty `episodes` is Ok(empty).
[[nodiscard]] Result<std::vector<std::string>, ProviderError> parse_episodes(
    std::string_view html);

struct Subtitle {
  std::string url;
  std::string language;  // as written: "en", "en-US", "eng", "English", …
  std::string label;
  bool is_default = false;
  friend bool operator==(const Subtitle&, const Subtitle&) = default;
};

// One stream of one server.
struct Stream {
  std::string server;  // the server's name, for messages only
  std::string url;
  bool hls = false;    // format "hls"
  std::optional<std::string> referer;
  std::optional<std::string> origin;
  friend bool operator==(const Stream&, const Stream&) = default;
};

// One entry of a track's `providers`: a stream host family ("sun", "bee").
struct Source {
  std::string name;
  std::vector<Stream> streams;  // servers in page order, their streams in order
  std::vector<Subtitle> subtitles;
  friend bool operator==(const Source&, const Source&) = default;
};

struct Track {
  std::string kind;  // "ssub" | "sub" | "dub"
  std::vector<Source> sources;
  friend bool operator==(const Track&, const Track&) = default;
};

// The watch page's `tracks`, as written: nothing is filtered here except
// entries that are not the expected shape (a stream without a url string,
// a subtitle without a file string), which drop. A server's `headers` may be
// null; its Referer / Origin keys are matched without regard to case.
// Errors as parse_search; a page whose sources part has not loaded (no
// `tracks` anywhere) is Err(Decode).
[[nodiscard]] Result<std::vector<Track>, ProviderError> parse_tracks(std::string_view html);

// The English subtitle to play: the default English one, else the first
// English one. English is a language of "en", "eng", "english" or "en-…"
// (any case), or, when the language is empty, a label starting "English".
// Only urls past the fetch guard that are clean argv qualify.
[[nodiscard]] std::optional<std::string> pick_subtitle(const std::vector<Subtitle>& subs);

// A stream resolve may hand out, with what goes on the link beside it.
struct Candidate {
  Stream stream;
  std::optional<std::string> sub_url;
  friend bool operator==(const Candidate&, const Candidate&) = default;
};

// The streams to try for a translation, best first:
//   Sub: "ssub" sources that have an English subtitle (sub_url set), then
//        the "sub" track's (burned in, no sub_url), then "ssub" sources with
//        no English subtitle (sub_url unset — better than nothing);
//   Dub: the "dub" track's, never with a sub_url.
// Within one of those groups, sources in page order and their streams in
// order. A stream qualifies when its url is https, passes the fetch guard
// and is clean argv, and its referer and origin (when present) are clean
// argv too; an origin that is not `scheme://host[:port]` drops alone, the
// stream stays. A url already listed is not listed again.
[[nodiscard]] std::vector<Candidate> candidates(const std::vector<Track>& tracks,
                                                Translation translation);

// The slug of a watch address, `<origin>/watch/<id>/<slug>[?…]`, for that
// show id: 1 to 200 of [A-Za-z0-9._~%-]. nullopt for any other address.
[[nodiscard]] std::optional<std::string> watch_slug(std::string_view url,
                                                    std::string_view show_id);

// Whether a fetched body is an HLS playlist: "#EXTM3U" after an optional
// UTF-8 byte order mark and leading whitespace.
[[nodiscard]] bool is_playlist(std::string_view body);

// The audio language a dub's master has to be cut to: "eng" when the master
// carries an English audio rendition beside one in another language (a dub
// that is an audio track of the Japanese stream), else nullopt.
[[nodiscard]] std::optional<std::string> dub_audio_language(std::string_view master);

}  // namespace detail

// --- The provider -----------------------------------------------------------

class Miruro final : public StreamProvider {
 public:
  [[nodiscard]] static Result<Miruro, ProviderError> create();

  [[nodiscard]] std::string_view name() const override { return "miruro"; }
  [[nodiscard]] std::string_view display_name() const override { return "Miruro"; }

  // No id-keyed route: the show handle only comes out of a search.
  [[nodiscard]] std::optional<std::string> canonical_key(const Enrichment& show) const override;

  // Page 1 only (the site pages by cursor): a later page answers empty.
  [[nodiscard]] Result<std::vector<SearchHit>, ProviderError> search(
      std::string_view query, const SearchOptions& opts) const override;

  // The listing off the episode-1 watch page. For Dub, a page whose tracks
  // read and hold no dub stream answers Ok(empty): the show has no dub.
  // A page whose tracks do not read still answers the listing.
  [[nodiscard]] Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view provider_id, Translation translation,
      std::optional<std::uint32_t> count_hint) const override;

  // The first candidate that answers a playlist, at most kMaxProbes tried;
  // a stream that is not HLS is taken unprobed. The link: the stream's url,
  // referer and origin, the browser user agent, cloaked_segments on an HLS
  // stream (several hosts name segments .png or .jpg), the candidate's
  // sub_url, and for Dub the audio_language dub_audio_language reads off
  // the master. Quality is not applied: the master goes out whole. No
  // candidates = Decode("no stream for this track"); none answering = the
  // last probe's error.
  [[nodiscard]] Result<StreamLink, ProviderError> resolve(std::string_view provider_id,
                                                          std::string_view episode,
                                                          Translation translation,
                                                          Quality quality) const override;

  // No covers of its own: a clean absolute url passes through.
  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view cover_ref) const override;

  // Test-only seam: the site at `base`, no handshake escalation; when
  // `stream_origin` is non-empty, a probe goes to that origin with the real
  // url's path and query (the fetch guard still judges the real url).
  [[nodiscard]] static Result<Miruro, ProviderError> with_endpoints(std::string base,
                                                                    std::string stream_origin);

 private:
  struct State;
  Miruro(http::Client client, std::string base, std::string stream_origin,
         http::Fingerprint escalate);

  // A site page. Plain first; a 403 retries once with `escalate_` (when it
  // is not None) and, if that passes, keeps it for later requests. A retry
  // that fails answers the 403. A redirect is followed while it stays on
  // the site's own origin, kMaxRedirects at most; `landed`, when given,
  // receives the address that answered if that is not `url`.
  [[nodiscard]] Result<std::string, ProviderError> page(const std::string& url,
                                                        std::string* landed = nullptr) const;
  [[nodiscard]] Result<std::string, ProviderError> watch_page(std::string_view show_id,
                                                              std::string_view episode) const;
  [[nodiscard]] Result<std::string, ProviderError> probe(const detail::Stream& stream) const;

  http::Client http_;
  std::string base_;
  std::string stream_origin_;
  http::Fingerprint escalate_;
  std::shared_ptr<State> state_;
};

}  // namespace shigoku::miruro
