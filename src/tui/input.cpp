// input.cpp — byte → Event decoder (P6, A3; mouse P32).

#include "input.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "../debug_log.hpp"

namespace shigoku::tui {

namespace {

using Special = KeyEvent::Special;

KeyEvent special(Special s) {
  KeyEvent k;
  k.codepoint = 0;
  k.special = s;
  return k;
}

KeyEvent printable(char32_t cp) {
  KeyEvent k;
  k.codepoint = cp;
  k.special = Special::None;
  return k;
}

KeyEvent ctrl_key(char32_t cp) {
  KeyEvent k;
  k.codepoint = cp;
  k.special = Special::None;
  k.ctrl = true;
  return k;
}

// How many bytes a UTF-8 sequence with this lead byte occupies (1..4), or 0 if
// the byte is not a valid lead.
int utf8_len(unsigned char b0) {
  if (b0 < 0x80) return 1;
  if ((b0 & 0xE0u) == 0xC0u) return 2;
  if ((b0 & 0xF0u) == 0xE0u) return 3;
  if ((b0 & 0xF8u) == 0xF0u) return 4;
  return 0;
}

char32_t decode_utf8_fixed(const std::string& s, int len) {
  const auto b0 = static_cast<unsigned char>(s[0]);
  auto cont = [&](int k) -> char32_t {
    return static_cast<unsigned char>(s[k]) & 0x3Fu;
  };
  switch (len) {
    case 1: return b0;
    case 2: return ((b0 & 0x1Fu) << 6) | cont(1);
    case 3: return ((b0 & 0x0Fu) << 12) | (cont(1) << 6) | cont(2);
    case 4:
      return ((b0 & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
    default: return b0;
  }
}

// --- P32 mouse -------------------------------------------------------------

// Map an xterm button code (already de-based: SGR's raw Pb, or the X10 byte
// minus 32) + 0-based cell coords to a MouseEvent, or nothing for the reports
// dropped on the floor (wheel tilt, motion, malformed). `sgr_release` is the
// SGR final-byte distinction ('m'); X10 encodes any release as button code 3
// and forgets which button it was.
bool emit_mouse(int cb, int x, int y, bool sgr_release, std::vector<Event>& out) {
  if (cb < 0 || x < 0 || y < 0) return false;  // malformed / underflowed.
  if (cb & 64) {
    // Wheel (the P32 ledger's deferred alias, paid): low bits 0/1 are the
    // vertical notches; 2/3 are tilt-left/right — dropped. Terminals report
    // wheel as press-form only; a stray SGR 'm' wheel is dropped too rather
    // than invented into a second notch.
    if (sgr_release || (cb & 2)) return false;
    MouseEvent m;
    m.kind = (cb & 1) ? MouseEvent::Kind::WheelDown : MouseEvent::Kind::WheelUp;
    m.button = MouseEvent::Button::None;
    m.x = x;
    m.y = y;
    out.push_back(Event{m});
    return true;
  }
  if (cb & 32) return false;  // motion (?1002 drag): decoded, dropped.
  const int b = cb & 3;       // modifier bits (4/8/16) deliberately ignored.
  MouseEvent m;
  if (sgr_release || b == 3) {
    m.kind = MouseEvent::Kind::Release;
    // SGR releases keep the button number; the X10 "3" means unknown.
    m.button = (sgr_release && b != 3)
                   ? static_cast<MouseEvent::Button>(b)
                   : MouseEvent::Button::None;
  } else {
    m.kind = MouseEvent::Kind::Press;
    m.button = static_cast<MouseEvent::Button>(b);  // 0/1/2 = Left/Middle/Right.
  }
  m.x = x;
  m.y = y;
  out.push_back(Event{m});
  return true;
}

// True for the byte after ESC that introduces a string sequence — OSC (]),
// DCS (P), APC (_), PM (^), SOS (X). These carry an arbitrary payload up to a
// BEL or ST terminator, and terminals SEND them as replies (XTVERSION answers
// as a DCS; OSC queries answer as an OSC). They must be swallowed whole: fed
// to the key path, the introducer decodes as a bare Escape — which the views
// bind to "back" — and the payload as a burst of garbage keystrokes.
bool is_string_introducer(unsigned char b) {
  return b == ']' || b == 'P' || b == '_' || b == '^' || b == 'X';
}

// Parse the SGR params "<Pb;Px;Py" (the '<' already checked by the caller)
// into (cb, x, y) with x/y normalized 1-based → 0-based. nullopt = malformed.
std::optional<std::array<int, 3>> parse_sgr_params(std::string_view params) {
  std::array<int, 3> vals{};
  std::size_t field = 0;
  int acc = 0;
  bool any = false;
  for (std::size_t i = 1; i < params.size(); ++i) {  // skip '<'.
    const char c = params[i];
    if (c == ';') {
      if (!any || field >= 2) return std::nullopt;
      vals[field++] = acc;
      acc = 0;
      any = false;
    } else if (c >= '0' && c <= '9') {
      if (acc > 99999) return std::nullopt;  // absurd — refuse to overflow.
      acc = acc * 10 + (c - '0');
      any = true;
    } else {
      return std::nullopt;
    }
  }
  if (!any || field != 2) return std::nullopt;
  vals[2] = acc;
  vals[1] -= 1;  // Px: 1-based → 0-based.
  vals[2] -= 1;  // Py.
  return vals;
}

}  // namespace

int InputDecoder::decode_one(std::vector<Event>& out) {
  if (pending_.empty()) return 0;
  const auto b0 = static_cast<unsigned char>(pending_[0]);

  // --- ESC: bare Escape, or the introducer of a CSI/SS3 sequence. ----------
  if (b0 == 0x1b) {
    if (pending_.size() == 1) return 0;  // hold: could be lone Esc or CSI.
    const auto b1 = static_cast<unsigned char>(pending_[1]);
    if (b1 == '[' || b1 == 'O') {
      // CSI (ESC [) or SS3 (ESC O). Need the final byte.
      if (pending_.size() < 3) return 0;  // wait for at least the final.
      // CSI form: ESC [ [params] final. Scan to the final byte (0x40..0x7e).
      std::size_t i = 2;
      while (i < pending_.size()) {
        const auto c = static_cast<unsigned char>(pending_[i]);
        if (c >= 0x40 && c <= 0x7e) break;  // final byte.
        ++i;
      }
      if (i >= pending_.size()) return 0;  // final not arrived yet.
      const char final = pending_[i];

      // X10 mouse (P32): CSI M b x y. The scan stops AT the 'M' (0x4D is in
      // the final range) with empty params, but the three payload bytes that
      // follow are raw data, not CSI bytes — they must be consumed here or
      // they'd decode as garbage keys on a terminal that never learned SGR
      // (?1006 unsupported → X10 encoding despite our request).
      if (i == 2 && final == 'M' && b1 == '[') {
        if (pending_.size() < 6) return 0;  // payload not arrived yet.
        const int cb = static_cast<unsigned char>(pending_[3]) - 32;
        const int mx = static_cast<unsigned char>(pending_[4]) - 33;  // 1-based+32.
        const int my = static_cast<unsigned char>(pending_[5]) - 33;
        pending_.erase(0, 6);
        return emit_mouse(cb, mx, my, /*sgr_release=*/false, out) ? 6 : -1;
      }

      // Parameters between b2..i (e.g. "3" for Delete "ESC [ 3 ~").
      const std::string params = pending_.substr(2, i - 2);
      const int consumed = static_cast<int>(i + 1);
      pending_.erase(0, static_cast<std::size_t>(consumed));

      // SGR mouse (P32): CSI < Pb ; Px ; Py M (press) / m (release).
      if (!params.empty() && params[0] == '<' &&
          (final == 'M' || final == 'm')) {
        const auto vals = parse_sgr_params(params);
        if (!vals.has_value()) return -1;  // malformed: consumed, no event.
        return emit_mouse((*vals)[0], (*vals)[1], (*vals)[2],
                          /*sgr_release=*/final == 'm', out)
                   ? consumed
                   : -1;
      }

      // urxvt ?1015 mouse (HW #4): CSI Pb ; Px ; Py M — like SGR but no '<'
      // and Pb still carries the X10 +32 offset (releases are Pb=35, which
      // de-bases to the X10 "unknown button" 3 emit_mouse already maps).
      // parse_sgr_params skips params[0] unconditionally (the SGR '<'), so
      // feed it a re-prefixed copy rather than teach it a second shape.
      if (!params.empty() && final == 'M' && params[0] >= '0' &&
          params[0] <= '9') {
        const auto vals = parse_sgr_params("<" + params);
        if (!vals.has_value()) return -1;  // malformed: consumed, no event.
        return emit_mouse((*vals)[0] - 32, (*vals)[1], (*vals)[2],
                          /*sgr_release=*/false, out)
                   ? consumed
                   : -1;
      }

      Special s = Special::None;
      switch (final) {
        case 'A': s = Special::Up; break;
        case 'B': s = Special::Down; break;
        case 'C': s = Special::Right; break;
        case 'D': s = Special::Left; break;
        case 'H': s = Special::Home; break;
        case 'F': s = Special::End; break;
        case '~':
          if (params == "1" || params == "7") s = Special::Home;
          else if (params == "4" || params == "8") s = Special::End;
          else if (params == "3") s = Special::Delete;
          else if (params == "5") s = Special::PageUp;
          else if (params == "6") s = Special::PageDown;
          break;
        default: break;
      }
      if (s != Special::None) {
        out.push_back(Event{special(s)});
        return consumed;
      }
      // Unrecognized CSI: consumed, no event (avoid key spam). Logged so a
      // hardware terminal speaking a mouse encoding this decoder doesn't
      // know shows its exact wire bytes in SHIGOKU_DEBUG_LOG (HW #4 round 2:
      // the report said "still nothing" and we had nothing observable).
      if (debug_log_enabled()) {
        debug_log("input: dropped CSI params='" + params + "' final='" +
                  std::string(1, final) + "'");
      }
      return -1;
    }
    // String sequences (OSC/DCS/APC/PM/SOS): swallow the whole thing, emit
    // nothing. Terminals send these as REPLIES (a stray probe answer, an
    // XTVERSION DCS), and only string-capable terminals ever will — decoding
    // one as Escape+keystrokes is exactly the iTerm2-only garbage burst this
    // arm exists to prevent.
    if (is_string_introducer(b1)) {
      // Find the terminator: BEL, or ST (ESC \). An ESC inside the body that
      // is NOT part of an ST means the sequence was cut off by a new escape —
      // drop what came before it and let the new sequence decode.
      std::size_t i = 2;
      while (i < pending_.size()) {
        const auto c = static_cast<unsigned char>(pending_[i]);
        if (c == 0x07) {  // BEL terminator.
          if (debug_log_enabled()) {
            debug_log("input: swallowed string seq " +
                      debug_escape(std::string_view(pending_).substr(0, i + 1)));
          }
          pending_.erase(0, i + 1);
          return -1;
        }
        if (c == 0x1b) {
          if (i + 1 >= pending_.size()) return 0;  // ST split — wait for '\'.
          if (pending_[i + 1] == '\\') {           // ST terminator.
            if (debug_log_enabled()) {
              debug_log("input: swallowed string seq " +
                        debug_escape(std::string_view(pending_).substr(0, i + 2)));
            }
            pending_.erase(0, i + 2);
            return -1;
          }
          // A new escape sequence begins mid-body: the string sequence never
          // terminated. Drop it, keep the fresh ESC for the next iteration.
          if (debug_log_enabled()) {
            debug_log("input: dropped unterminated string seq " +
                      debug_escape(std::string_view(pending_).substr(0, i)));
          }
          pending_.erase(0, i);
          return -1;
        }
        ++i;
      }
      // No terminator yet. Hold — but cap the body so a broken stream can't
      // eat keystrokes forever; past the cap the whole buffer is garbage.
      constexpr std::size_t kStringSeqCap = 4096;
      if (pending_.size() > kStringSeqCap) {
        if (debug_log_enabled()) {
          debug_log("input: dropped oversize string seq (" +
                    std::to_string(pending_.size()) + "B)");
        }
        pending_.clear();
        return -1;
      }
      return 0;
    }

    // ESC followed by a non-CSI byte: treat the ESC as a standalone Escape and
    // let the following byte decode on its own next iteration.
    pending_.erase(0, 1);
    out.push_back(Event{special(Special::Escape)});
    return 1;
  }

  // --- Single-byte controls ------------------------------------------------
  switch (b0) {
    case '\r':
    case '\n':
      pending_.erase(0, 1);
      out.push_back(Event{special(Special::Enter)});
      return 1;
    case '\t':
      pending_.erase(0, 1);
      out.push_back(Event{special(Special::Tab)});
      return 1;
    case 0x7f:  // DEL
    case 0x08:  // BS
      pending_.erase(0, 1);
      out.push_back(Event{special(Special::Backspace)});
      return 1;
    case 0x03:  // Ctrl-C
      pending_.erase(0, 1);
      out.push_back(Event{ctrl_key(U'c')});
      return 1;
    default: break;
  }

  // Other C0 controls (Ctrl-A..Ctrl-Z except the ones handled above): map to a
  // ctrl key on the corresponding letter, so the app can bind e.g. Ctrl-D.
  if (b0 < 0x20) {
    pending_.erase(0, 1);
    out.push_back(Event{ctrl_key(static_cast<char32_t>(b0 + 'a' - 1))});
    return 1;
  }

  // --- Printable UTF-8 -----------------------------------------------------
  const int len = utf8_len(b0);
  if (len == 0) {
    pending_.erase(0, 1);  // stray continuation / invalid lead — drop it.
    return -1;
  }
  if (static_cast<int>(pending_.size()) < len) return 0;  // partial — wait.
  const char32_t cp = decode_utf8_fixed(pending_, len);
  pending_.erase(0, static_cast<std::size_t>(len));
  out.push_back(Event{printable(cp)});
  return len;
}

namespace {

// One-line render of a decoded event for SHIGOKU_DEBUG_LOG (the HW seam): the
// only way to see what a hardware terminal's bytes actually decoded to.
std::string describe_event(const Event& e) {
  if (const auto* k = std::get_if<KeyEvent>(&e)) {
    static constexpr const char* kSpecialNames[] = {
        "None", "Enter",  "Escape", "Backspace", "Tab",    "Up",     "Down",
        "Left", "Right",  "Home",   "End",       "PageUp", "PageDown",
        "Delete"};
    std::string s = "key ";
    if (k->ctrl) s += "ctrl+";
    if (k->special == Special::None) {
      s += "cp=" + std::to_string(static_cast<std::uint32_t>(k->codepoint));
      if (k->codepoint >= 0x20 && k->codepoint < 0x7f) {
        s += " '";
        s += static_cast<char>(k->codepoint);
        s += '\'';
      }
    } else {
      s += kSpecialNames[static_cast<int>(k->special)];
    }
    return s;
  }
  if (const auto* m = std::get_if<MouseEvent>(&e)) {
    static constexpr const char* kKindNames[] = {"press", "release", "wheelup",
                                                 "wheeldown"};
    return std::string("mouse ") + kKindNames[static_cast<int>(m->kind)] +
           " b=" + std::to_string(static_cast<int>(m->button)) +
           " x=" + std::to_string(m->x) + " y=" + std::to_string(m->y);
  }
  return "other";
}

}  // namespace

std::size_t InputDecoder::feed(std::string_view bytes,
                               std::vector<Event>& out) {
  pending_.append(bytes.data(), bytes.size());
  const std::size_t before = out.size();
  for (;;) {
    const int n = decode_one(out);
    if (n == 0) break;  // need more bytes.
    // n>0 emitted, n<0 dropped garbage — either way keep draining.
    if (pending_.empty()) break;
  }
  if (debug_log_enabled()) {
    for (std::size_t i = before; i < out.size(); ++i) {
      debug_log("input: " + describe_event(out[i]));
    }
  }
  return out.size() - before;
}

bool InputDecoder::flush_pending(std::vector<Event>& out) {
  // Only a lone ESC (exactly one byte, the introducer) resolves to Escape on
  // timeout. A partial CSI (ESC [ …) is still growing and must not be flushed.
  if (pending_.size() == 1 &&
      static_cast<unsigned char>(pending_[0]) == 0x1b) {
    pending_.clear();
    out.push_back(Event{special(Special::Escape)});
    return true;
  }
  return false;
}

bool InputDecoder::esc_pending() const {
  return pending_.size() == 1 &&
         static_cast<unsigned char>(pending_[0]) == 0x1b;
}

}  // namespace shigoku::tui
