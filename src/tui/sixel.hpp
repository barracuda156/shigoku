// sixel.hpp — the SIXEL cover emitter, the third backend beside kitty.hpp and
// iterm.hpp. It exists for the terminals the legacy target actually runs:
// mlterm speaks SIXEL and nothing else, and xterm/foot answer SIXEL but not
// the Kitty graphics protocol. Pure like the other two emitters: RGBA bytes +
// geometry in, one escape string out; no I/O, no state.
//
// Wire shape (DCS, ST-terminated):
//   ESC P 7 ; 0 ; q " 1 ; 1 ; <px_w> ; <px_h>
//     #<n>;2;<r>;<g>;<b>   (once per palette entry, 0-100 per channel)
//     <sixel data, six pixel rows per band, '$' = CR, '-' = LF>
//   ESC backslash                                 (ST, the string terminator)
// The `0` is background-opaque mode (not transparent); "1;1 is the 1:1 pixel
// aspect every modern terminal expects.
//
// Architecturally this is a TWIN of the iTerm2 backend, not of kitty: there is
// NO image id and NO delete command. A SIXEL is painted into the cells it
// covers, so overwriting those cells erases it, and the compose layer's
// "delete = the forced repaint of the vacated rect" path — built for OSC 1337
// — is exactly what this backend wants. Hence no delete counterpart here
// either.
//
// The ONE way it differs from both: a SIXEL is sized in PIXELS, not cells.
// kitty takes c=/r= and iTerm2 takes width=/height= in cells and each scales
// for us; a SIXEL is drawn at its literal pixel size and simply covers however
// many cells that comes to. So transmit() takes the per-cell pixel geometry
// (CoverCaps::cell, already probed) and multiplies. A terminal that reports no
// cell geometry cannot be sized for safely — an oversized SIXEL scrolls the
// screen out from under the TUI — so backend_from never selects this backend
// without it (cover_probe.hpp), and transmit() returns "" if asked anyway.
//
// Quantization (adaptive ≤256-colour palette + Sierra-Filter-Lite error
// diffusion) and the SIXEL byte packing come from the vendored sayaka encoder,
// third_party/sixel_sayaka. sixel.cpp is the only TU that includes it.
//
// ENDIANNESS (§3): the encoder's own missing_endian.h handles the big-endian
// case internally (it packs RGB into a uint32 through RGBToU32, which is
// byte-order aware). On our side of the seam we only memcpy RGBA bytes in and
// concatenate a string out — nothing is packed into a wider int here.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku::tui::sixel {

// Quantize, dither and encode `rgba` (w*h*4, row-major RGBA8 — the CoverPixels
// layout) into one SIXEL escape sized to fill the `cols`×`rows` cell box at
// `cell_w`×`cell_h` pixels per cell. The covers pipeline already resized the
// pixels to exactly that box, so the encoder's resample is normally a no-op;
// it is still driven from the cell box because that box, not the source, is
// what must be covered.
//
// Returns the empty string — draw nothing this frame — for a zero-size image,
// an rgba/dimension mismatch, a non-positive cell box, unknown cell geometry
// (cell_w or cell_h == 0), or an encoder failure.
[[nodiscard]] std::string transmit(const std::vector<unsigned char>& rgba,
                                   std::uint32_t w, std::uint32_t h, int cols,
                                   int rows, std::uint16_t cell_w,
                                   std::uint16_t cell_h);

// Classify a probe reply: true iff a DA1 answer in `reply` advertises SIXEL.
// Unlike kitty and OSC 1337 this needs no query of its own — SIXEL capability
// rides in the primary device attributes the probe already requests, as
// attribute 4 (e.g. ESC [ ? 62 ; 4 ; 9 c). Silence, or a DA1 without a 4,
// ⇒ false.
[[nodiscard]] bool probe_reply_is_sixel(std::string_view reply);

namespace detail {

// True iff a DA1 reply — CSI ? <n> ; <n> … c — in `reply` lists `attr` as a
// WHOLE attribute. Exposed for the tests. Tokenizing matters: a substring
// search for "4" matches 14 and 64 (both real DA1 attributes), which would
// claim SIXEL support on terminals that never advertised it.
[[nodiscard]] bool da1_has_attribute(std::string_view reply, int attr);

}  // namespace detail

}  // namespace shigoku::tui::sixel
