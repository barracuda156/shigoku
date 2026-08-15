// iterm.hpp — the OSC 1337 (iTerm2 inline images) emitter, the second cover
// backend beside kitty.hpp. It exists for terminals that draw images but do
// not speak the Kitty graphics protocol — iTerm2 itself first among them —
// and is chosen by the startup probe only when the Kitty query goes
// unanswered (cover_probe.hpp). Pure like the kitty emitter: RGBA bytes +
// cell geometry in, one escape string out; no I/O, no state.
//
// Wire shape (BEL-terminated — the form the protocol's documentation uses and
// every adopter accepts):
//   ESC ] 1337 ; File=inline=1;size=<bytes>;width=<cols>;height=<rows>;
//     preserveAspectRatio=0;doNotMoveCursor=1 : <base64 of a PNG> BEL
//   - OSC 1337 carries image FILES, not raw pixels, so the RGBA is wrapped in
//     a PNG (stb_image_write). PNG is the one format every adopter decodes.
//   - width/height are in CELLS; preserveAspectRatio=0 stretches to fill the
//     box exactly, matching kitty's c=/r= clamp (the cover pipeline pre-crops
//     to the box aspect, so neither backend visibly distorts).
//   - doNotMoveCursor=1 is kitty's C=1 analogue (iTerm2 3.5+): without it the
//     terminal walks the cursor down through every image row as if the rows
//     were typed, which perturbs the cursor the CellBuffer owns; adopters
//     that predate the key ignore it. The caller still CUPs to the origin
//     first, and the next diff re-CUPs regardless.
//   - The whole file goes in ONE escape: the protocol has no chunking. A
//     cover-sized PNG is well under what any adopter buffers.
//
// The load-bearing difference from kitty: there is NO image id and NO delete
// command. An inline image lives IN the cells it covers, so overwriting those
// cells erases it. The compose layer leans on that — "delete" is the repaint
// of the vacated rect that mark_all_dirty already forces, and a resize's ED
// clear wipes placements with the cells they sat in. This emitter therefore
// exports no delete counterpart at all.
//
// ENDIANNESS: stb_image_write consumes the RGBA byte-wise (comp=4) and base64
// is byte-wise over the PNG; no pixel is packed into a wider int, so the wire
// bytes are identical big- and little-endian.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku::tui::iterm {

// PNG-encode `rgba` (w*h*4, row-major RGBA8 — the CoverPixels layout). Empty
// vector on zero dimensions, a size mismatch, or an encoder failure.
[[nodiscard]] std::vector<unsigned char> encode_png(
    const std::vector<unsigned char>& rgba, std::uint32_t w, std::uint32_t h);

// The transmit-and-display escape: PNG-encode, base64, wrap in OSC 1337 with
// the image clamped into the `cols`×`rows` cell box, doNotMoveCursor=1 so the
// draw is position-neutral. Returns the empty string for a zero-size image
// (nothing to draw).
[[nodiscard]] std::string transmit(const std::vector<unsigned char>& rgba,
                                   std::uint32_t w, std::uint32_t h, int cols,
                                   int rows);

// The XTVERSION query (CSI > 0 q): asks the terminal to name itself via a
// DCS > | <name> <version> ST reply. Emitted inside the startup probe's DA1
// window — an active probe, never env sniffing (the same law as the kitty
// a=q query; $TERM_PROGRAM does not survive ssh).
[[nodiscard]] std::string query_xtversion();

// The ReportCellSize query (OSC 1337 ; ReportCellSize BEL): the protocol's own
// capability probe — a terminal that answers it demonstrably parses OSC 1337,
// the exact analogue of kitty's a=q ("ask the protocol, not the name").
// iTerm2 versions differ on whether they answer XTVERSION (3.5.14 does; the
// first hardware round saw none), so this is the query that reliably detects
// it; the reply doubles as per-cell pixel geometry. The reply:
//   OSC 1337 ; ReportCellSize=<h>;<w>[;<scale>] ST
// with h/w floating-point cell size in points and scale = pixels per point
// (absent on older versions ⇒ 1.0).
[[nodiscard]] std::string query_cell_size();

// Parse a ReportCellSize reply out of the probe's read window: on success
// writes the per-cell size IN PIXELS (points × scale) and returns true. False
// when no well-formed reply is present.
[[nodiscard]] bool parse_cell_size_reply(std::string_view reply, double& w_px,
                                         double& h_px);

// Classify a probe reply: true iff the terminal answered ReportCellSize (it
// speaks OSC 1337 — the definitive signal) or an XTVERSION DCS reply names a
// terminal known to draw OSC 1337 inline images (iTerm2, WezTerm, mintty,
// Konsole — case-insensitive prefix match; the fallback for adopters that
// answer XTVERSION but not ReportCellSize). A reply naming anything else, a
// bare DA1 answer, or silence ⇒ false.
[[nodiscard]] bool probe_reply_is_iterm(std::string_view reply);

// Narrower classification: the terminal is iTerm2 ITSELF — it answered
// ReportCellSize (iTerm2's proprietary report; no other terminal implements
// it) or its XTVERSION reply names iTerm2. Needed because iTerm2 3.5 ALSO
// acks the kitty a=q graphics query, but its kitty implementation accepts our
// transmits (replies OK) without rendering them — on iTerm2 the native
// OSC 1337 path is the one that draws, so this verdict outranks the kitty
// ack in backend selection. WezTerm/Konsole answer a=q too and render kitty
// fine; neither answers ReportCellSize nor names iTerm2, so they keep the
// kitty backend.
[[nodiscard]] bool probe_reply_is_iterm2(std::string_view reply);

}  // namespace shigoku::tui::iterm
