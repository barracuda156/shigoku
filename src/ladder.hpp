// ladder.hpp — a resolved stream's quality ladder, read the way its player
// reads it, and a rung taken off it. The one place the download transfer, the
// download offer and capped playback go through to turn "this stream, at this
// size" into the urls a player is handed.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "domain.hpp"
#include "error.hpp"
#include "hls.hpp"
#include "http.hpp"
#include "result.hpp"

namespace shigoku::ladder {

// Whether a url names a playlist rather than a file, by its path's extension
// (query and fragment stripped, ASCII case-folded): .m3u8 / .m3u.
[[nodiscard]] bool is_playlist_url(std::string_view url);

// The same for a resolved link, which knows more than its url: a link whose
// segments are cloaked or that has to be relayed (proxy::needed) is a
// playlist whatever its url ends in.
[[nodiscard]] bool is_playlist(const StreamLink& link);

// A master playlist's text and the url it was read from (what its relative
// entries join against).
struct Master {
  std::string text;
  std::string url;
};

// The link's master playlist, fetched from `play_url` with the link's own
// headers. `play_url` is the link's url, or its proxy's loopback when the
// link is relayed — the proxy then serves the playlist opened, cut to its
// audio language and rewritten, exactly as the player will be served it.
// nullopt when the fetch fails or the body is not a master playlist (a media
// playlist, a file). `play_url` must already have passed the fetch guard, or
// be the loopback.
[[nodiscard]] std::optional<Master> read(const http::Client& client, const StreamLink& link,
                                         std::string_view play_url);

// What `rung` takes from a master.
//   Ok(pair)    -> hand the player pair.video_url, with pair.audio_url beside
//                  it when set;
//   Ok(nullopt) -> nothing to pick (hls::choose_pair's nullopt): keep the
//                  master;
//   Err         -> the picked variant or its audio failed the fetch guard.
// `relayed` says the master came through the proxy's loopback: its urls are
// loopback refs the proxy guards hop by hop, so they are not guarded here
// (the guard refuses 127.0.0.1).
[[nodiscard]] Result<std::optional<hls::Pair>, GuardError> take(const Master& master,
                                                                bool relayed, const Rung& rung);

}  // namespace shigoku::ladder
