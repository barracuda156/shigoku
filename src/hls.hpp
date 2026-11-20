// hls.hpp — shared HLS master-playlist parsing + quality-cap selection
// (P12 seed + P23). Ported from sabigoku src/providers/hls.rs.
//
// join_url shipped with the P12 de-cloak proxy. parse_master_playlist and
// select_variant land with P23: providers fetch the master themselves (CDN
// headers differ per source) and feed the bytes here; cap policy and variant
// math live in one place.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"  // Quality, StreamLink

namespace shigoku::hls {

// Join a possibly-relative m3u8 URI against the playlist URL. Absolute http(s)
// passes through; `/rooted` keeps scheme+host; else relative to the playlist
// dir. `./` and `../` stay LITERAL: mpv normalizes; we don't resolve (this is
// deliberately a string join, NOT a CURLU/RFC-3986 resolver — normalizing here
// would diverge from the Rust and the proxy's rewrite goldens). nullopt when
// `base` has no `://`.
[[nodiscard]] std::optional<std::string> join_url(std::string_view base,
                                                  std::string_view reference);

// Master playlist entry: variant URI (verbatim, possibly relative) + what its
// STREAM-INF advertised. `resolution` is the height in lines; `bandwidth` is
// BANDWIDTH in bits per second (AVERAGE-BANDWIDTH is not it); `codecs` is the
// CODECS string as written; `audio_group` is AUDIO, "" when the audio is
// muxed into the variant.
struct Variant {
  std::string url;
  std::optional<std::uint32_t> resolution;
  std::optional<std::uint32_t> width;
  std::optional<std::uint64_t> bandwidth;
  std::string codecs;
  std::string audio_group;

  friend bool operator==(const Variant&, const Variant&) = default;
};

// One `#EXT-X-MEDIA:TYPE=AUDIO` entry. `uri` is verbatim (possibly relative);
// "" means the entry only labels audio muxed into the variants.
struct AudioRendition {
  std::string group;     // GROUP-ID
  std::string name;      // NAME
  std::string language;  // LANGUAGE as written, "" when absent
  std::string uri;
  bool is_default = false;  // DEFAULT=YES

  friend bool operator==(const AudioRendition&, const AudioRendition&) = default;
};

// Master playlist: each `#EXT-X-STREAM-INF:` (resolution) paired with the
// next non-comment URI (verbatim). Network caller joins relatives against the
// playlist URL. No STREAM-INF -> media playlist, empty vec; caller treats the
// link as one stream.
[[nodiscard]] std::vector<Variant> parse_master_playlist(std::string_view text);

// Whether a master carries its audio as separate renditions: an
// `#EXT-X-MEDIA` of TYPE=AUDIO with a URI. Its variants are then video-only,
// and a cap that hands the player (or ffmpeg) one variant plays — or saves —
// silent; the capping callers keep the master instead and let the player
// pick the tracks. A TYPE=AUDIO entry without a URI only labels audio that is
// muxed into the variants, so it does not count.
[[nodiscard]] bool has_separate_audio(std::string_view text);

// A master whose audio renditions are cut down to `language` (ISO 639,
// two- or three-letter): within each GROUP-ID that carries it, the other
// languages' TYPE=AUDIO entries drop and the first kept one is DEFAULT=YES,
// so mpv and ffmpeg pick it unprompted. A group without the language,
// and a master with none, pass through unchanged.
[[nodiscard]] std::string keep_audio_language(std::string_view text, std::string_view language);

// Pick by quality preference, or nullptr if empty (ROD-152). Cap policy:
// best/worst take the highest/lowest resolution; a rung takes the highest ≤
// cap, and if every variant exceeds it, the lowest available (never invent a
// ceiling breach; always return something the source offers). Returned
// pointer aliases an element of `variants`.
[[nodiscard]] const StreamLink* select_variant(const std::vector<StreamLink>& variants,
                                               Quality quality);

// The variant URL a quality cap picks from a master playlist, joined against
// `base`; nullopt when the caller should keep the master: a media playlist (no
// variants), a master with separate audio renditions (a lone variant would
// play silent), or no variant left after the join and the argv guard.
[[nodiscard]] std::optional<std::string> choose_variant(std::string_view playlist,
                                                        std::string_view base, Quality quality);

// Every TYPE=AUDIO rendition of a master, in playlist order.
[[nodiscard]] std::vector<AudioRendition> parse_audio_renditions(std::string_view text);

// The variant a rung names, or nullptr when `variants` is empty. The returned
// pointer aliases an element of `variants`.
//
// Among the variants that name a resolution:
//   lowest          -> the lowest resolution;
//   height == 0     -> the highest resolution;
//   height  > 0     -> the highest at or under it; when every one is above
//                      it, the lowest.
// Variants of one resolution: the bandwidth nearest `rung.bandwidth` when
// that is set, else the highest bandwidth (the lowest when the rule above
// was reaching for the lowest resolution); still tied, the earlier one.
// A variant that names no resolution is taken only when none does. Then:
//   lowest                       -> the lowest bandwidth;
//   bandwidth > 0                -> the highest bandwidth at or under it,
//                                   else the lowest;
//   height == 0 (the best)       -> the highest bandwidth;
//   height > 0, no bandwidth     -> the first listed (a ceiling in lines
//                                   says nothing about such a ladder).
// A missing BANDWIDTH counts as 0.
[[nodiscard]] const Variant* pick_variant(const std::vector<Variant>& variants,
                                          const Rung& rung);

// The rendition a variant's audio comes from: of the renditions in its
// audio_group that carry a URI, the DEFAULT=YES one, else the first. nullptr
// when the variant names no group or the group has no URI (audio is muxed
// in). The returned pointer aliases an element of `renditions`.
[[nodiscard]] const AudioRendition* audio_for(const Variant& variant,
                                              const std::vector<AudioRendition>& renditions);

// A rung taken off a master: the video variant and, when its audio is
// a separate rendition, that rendition's playlist. Both urls are joined against
// the master's own url. `variant` is the picked entry with its url joined.
struct Pair {
  std::string video_url;
  std::optional<std::string> audio_url;
  Variant variant;

  friend bool operator==(const Pair&, const Pair&) = default;
};

// The pair `rung` names in a master playlist read from `base`. Variants whose
// url does not join or fails the argv guard (clean_arg) are left out before
// the pick. nullopt when the caller should keep the master: a media playlist,
// no usable variant, or a picked variant whose audio rendition's url does not
// join or fails the guard (the video alone would play silent).
[[nodiscard]] std::optional<Pair> choose_pair(std::string_view playlist, std::string_view base,
                                              const Rung& rung);

}  // namespace shigoku::hls
