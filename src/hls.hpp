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

// Master playlist entry: variant URI (verbatim, possibly relative) + vertical
// resolution when STREAM-INF advertised one.
struct Variant {
  std::string url;
  std::optional<std::uint32_t> resolution;

  friend bool operator==(const Variant&, const Variant&) = default;
};

// Master playlist: each `#EXT-X-STREAM-INF:` (resolution) paired with the
// next non-comment URI (verbatim). Network caller joins relatives against the
// playlist URL. No STREAM-INF -> media playlist, empty vec; caller treats the
// link as one stream.
[[nodiscard]] std::vector<Variant> parse_master_playlist(std::string_view text);

// Pick by quality preference, or nullptr if empty (ROD-152). Cap policy:
// best/worst take the highest/lowest resolution; a rung takes the highest ≤
// cap, and if every variant exceeds it, the lowest available (never invent a
// ceiling breach; always return something the source offers). Returned
// pointer aliases an element of `variants`.
[[nodiscard]] const StreamLink* select_variant(const std::vector<StreamLink>& variants,
                                               Quality quality);

}  // namespace shigoku::hls
