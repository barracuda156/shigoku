// input.hpp — byte stream → Event decoder (P6, A3; mouse P32). The key binds
// the views need (DESIGN §6.1/§7.2): printable UTF-8, arrows + Home/End/PgUp/
// PgDn/Delete (CSI), Enter/Esc/Backspace/Tab, and Ctrl-C (hard quit) — plus
// the P32 mouse reports: SGR (CSI < b;x;y M/m) and legacy X10 (CSI M bxy),
// both normalized to MouseEvent — presses, releases, and vertical wheel
// notches. Motion/tilt/chord reports are consumed silently — consuming them
// HERE is what keeps a terminal's mouse chatter from ever decoding as
// garbage keystrokes.
//
// Incremental + stateful so a CSI sequence split across read() calls decodes
// correctly, and so a lone Esc can be disambiguated from a CSI introducer: an
// Esc that is not immediately followed by more bytes is held pending; the loop
// resolves it to a bare Escape after a short timeout via flush_pending().
// feed() is pure over (state, bytes) so the decoder unit-tests offline.

#pragma once

#include <string_view>
#include <vector>

#include "../event.hpp"

namespace shigoku::tui {

class InputDecoder {
 public:
  // Consume `bytes`, appending any fully-decoded events (KeyEvent or
  // MouseEvent alternatives, in stream order) to `out`. Bytes that form an
  // incomplete sequence (a trailing ESC, partial CSI/UTF-8, or a mouse report
  // still in flight) are retained internally for the next feed(). Returns the
  // count appended.
  std::size_t feed(std::string_view bytes, std::vector<Event>& out);

  // Resolve a held-pending lone ESC into a bare Escape key (called by the loop
  // when the Esc-vs-CSI timeout elapses with no follow-on byte). No-op if
  // nothing is pending or the pending bytes are a partial CSI still growing.
  // Returns true if it emitted an Escape.
  bool flush_pending(std::vector<Event>& out);

  // True if the decoder is holding a lone ESC awaiting disambiguation (the loop
  // arms its short timeout only then).
  [[nodiscard]] bool esc_pending() const;

 private:
  // Bytes retained between feeds: a growing escape sequence or a partial UTF-8
  // lead. Empty when the stream is at a clean boundary.
  std::string pending_;

  // Try to decode one complete event from the FRONT of pending_. Returns:
  //   >0  bytes consumed and one event pushed to out;
  //    0  need more bytes (incomplete sequence) — leave pending_ as-is;
  //   -1  the front byte was garbage or a silently-dropped report, consumed
  //       without emitting.
  int decode_one(std::vector<Event>& out);
};

}  // namespace shigoku::tui
