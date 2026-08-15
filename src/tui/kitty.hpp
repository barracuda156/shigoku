// kitty.hpp — the hand-rolled Kitty graphics emitter (P8, A4). Pure: RGBA
// bytes + cell geometry in, an APC escape string out; no I/O, no state. The
// covers worker (covers.hpp) feeds it decoded pixels; app.cpp splices its
// output into the CellBuffer flush at the A4 composition points (delete before
// the diff, transmit after it, inside one ?2026 synchronized frame).
//
// A4 is normative for the wire subset — the intersection of what modern kitty,
// ghostty, AND the legacy target's contour_ppc verifiably accept (sabigoku's
// Rust leans on the ratatui-image crate for this; shigoku ships one protocol by
// hand, so A4 is the spec, not a Rust byte format to port):
//   - Transmission t=d only (direct base64 in-band; contour rejects f/t/s).
//   - Format f=32 raw RGBA (contour's wire default; no PNG encoder needed).
//   - No o=z compression — contour answers ENOTSUP; never emit it.
//   - Chunked base64 m=1/…/m=0, 4096 base64 bytes per chunk (contour's
//     reassembly is forensically verified against production mpv captures).
//   - Transmit+display a=T with i=<id>, s=<px_w>,v=<px_h> (source pixel dims —
//     MANDATORY for raw f=32; their omission was a transcription bug real
//     hardware caught: terminals EINVAL the transmit, silently under
//     q=2), c=<cols>,r=<rows> (explicit cell box), C=1 (do not move the
//     cursor), q=2 (quiet — no ack onto the shell).
//   - Delete via a=d,d=I,i=<id> — CAPITAL I (frees stored data, honors the
//     storage quota that lowercase deletes leak).
//
// ENDIANNESS (§3): base64 is byte-wise over the RGBA buffer; nothing packs a
// pixel into a wider int, so the wire bytes are identical big- and
// little-endian.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku::tui::kitty {

// Base64 chunk size (A4): 4096 base64 characters per m=1 fragment. A multiple
// of 4 so no fragment splits a base64 quantum (contour reassembles by
// concatenation; a split quantum would corrupt the decode).
inline constexpr std::size_t kChunkBase64Bytes = 4096;

// Standard base64 (RFC 4648) of `bytes`, no line breaks, '=' padded. The APC
// payload is base64 of the raw RGBA per A4's t=d,f=32.
[[nodiscard]] std::string base64_encode(const std::vector<unsigned char>& bytes);

// A decoded cover ready to transmit: RGBA8 pixels in memory order (R,G,B,A per
// pixel, row-major, w*h*4 bytes) plus its pixel dimensions. Mirrors
// event.hpp's CoverPixels but kept independent so the emitter has no TUI-event
// dependency (it is a leaf module).
struct Image {
  std::vector<unsigned char> rgba;  // w*h*4, row-major.
  std::uint32_t w = 0;              // pixel width.
  std::uint32_t h = 0;              // pixel height.
};

// Transmit-and-display command (a=T). `id` is the stable image id (per
// anilist_id, so a replace reuses the slot). `img.w`/`img.h` go on the wire as
// s=/v= (required for f=32: the terminal validates payload == s*v*4).
// `cols`/`rows` is the explicit cell box the image is clamped into (c=/r=).
// Emits (ST = the ESC-backslash string terminator that closes each APC):
//   ESC _G a=T,f=32,i=<id>,s=<w>,v=<h>,c=<cols>,r=<rows>,C=1,q=2,m=1;<b64> ST
//   ESC _G m=1;<b64> ST   (repeated for every middle chunk)
//   ESC _G m=0;<b64> ST   (final chunk closes the transmission)
// Exactly one chunk (m=0 alone, no m=1 prefix run) when the payload fits in one
// fragment. Returns the empty string for a zero-size image (nothing to draw).
//
// Per A4: NO o= key (compression rejected), t defaults to d (direct — the only
// transmission contour accepts), and C=1 keeps the TUI's cursor its own.
[[nodiscard]] std::string transmit(const Image& img, std::uint32_t id, int cols,
                                   int rows);

// Delete-and-free command (a=d, d=I capital, i=<id>): frees the terminal's
// stored image data for `id`, not merely a placement (A4: lowercase 'i' leaks
// the storage quota over a session). One short APC, q=2, no payload (ST = the
// ESC-backslash terminator):
//   ESC _G a=d,d=I,i=<id>,q=2 ST
[[nodiscard]] std::string delete_image(std::uint32_t id);

// The a=q graphics query APC alone (A4: active probe, never env sniffing —
// contour sets no $KITTY_WINDOW_ID). A minimal well-formed query the terminal
// acks iff it speaks the protocol (ST = the ESC-backslash terminator):
//   ESC _G i=<id>,a=q,f=32,s=1,v=1,q=2;<1px b64> ST
// A 1×1 RGBA probe pixel keeps the query well-formed on strict terminals. The
// caller frames it with a DA1 request (CSI c) so a bounded read window closes.
[[nodiscard]] std::string query_apc(std::uint32_t id);

// The full detection probe: query_apc(id) followed by a DA1 request (CSI c). A
// graphics reply BEFORE the DA1 answer ⇒ kitty path; the DA1 answer alone ⇒
// placeholder mode. Returns the bytes to WRITE; the reader parses the reply
// (see probe_reply_is_kitty).
[[nodiscard]] std::string detection_probe(std::uint32_t id);

// Classify a terminal reply captured after writing detection_probe(): true iff
// a Kitty graphics APC response (ESC _G … ESC \) appears anywhere in `reply`
// (A4: a graphics ack ⇒ the terminal speaks the protocol). A bare DA1 answer
// (CSI ? … c) with no graphics APC ⇒ false (placeholder mode).
[[nodiscard]] bool probe_reply_is_kitty(std::string_view reply);

}  // namespace shigoku::tui::kitty
