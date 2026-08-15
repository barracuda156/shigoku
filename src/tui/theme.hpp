// theme.hpp — the DESIGN §1.1 palette as constants (P6, A3).
//
// v0 ships only the default "Terminal Ghost" theme (DESIGN §1.4). The
// four-theme Palette-struct machinery is a P7+ nicety; render code here reads
// these constants directly. Colors are Rgb{r,g,b} byte triples in memory order
// — NEVER a packed uint32 (§3, big-endian PPC target). SGR is emitted from the
// bytes with plain integer formatting, so it is endian-independent.

#pragma once

#include <cstdint>
#include <string>

namespace shigoku::tui {

// A 24-bit color as three bytes in memory order. No packed representation.
struct Rgb {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  friend bool operator==(const Rgb&, const Rgb&) = default;
};

// --- Terminal Ghost palette (DESIGN §1.1 / §1.2 semantic aliases) ----------
namespace theme {

// §1.2 aliases mapped to §1.1 hex.
inline constexpr Rgb bg{0x02, 0x0d, 0x06};        // bg.base — the void
inline constexpr Rgb surface{0x06, 0x14, 0x10};   // bg.surface — focused row bg
inline constexpr Rgb elevated{0x0b, 0x1f, 0x18};  // bg.elevated — toasts
inline constexpr Rgb chrome{0x1a, 0x40, 0x30};    // border.hair — hairlines
inline constexpr Rgb fg{0x39, 0xff, 0x6a};        // text.primary — phosphor green
inline constexpr Rgb fg2{0x3a, 0x80, 0x58};       // text.muted — dim phosphor (lifted: 2.7→4.2:1 on bg, was #2a6040)
inline constexpr Rgb fg3{0x2a, 0x5a, 0x42};       // text.dim — receded (lifted: 1.5→2.5:1 on bg, was #163525)
inline constexpr Rgb focus{0x20, 0xff, 0xdd};     // state.focus — cyan ghost
inline constexpr Rgb hot{0xff, 0x2d, 0x78};       // state.now — spectral magenta
inline constexpr Rgb warn{0xe5, 0xb8, 0x00};      // state.warn — amber
inline constexpr Rgb success{0x39, 0xff, 0x6a};   // state.success == fg
inline constexpr Rgb error{0xff, 0x2d, 0x78};     // state.error == hot

}  // namespace theme

// --- Spinner (DESIGN §4.8): 10 Braille frames, ~100ms/frame ----------------
// Multi-byte UTF-8 string literals (each frame is one U+28xx codepoint).
inline constexpr const char* kSpinnerFrames[10] = {
    "⠋", "⠙", "⠹", "⠸", "⠼",
    "⠴", "⠦", "⠧", "⠇", "⠏"};
inline constexpr int kSpinnerFrameCount = 10;

// --- SGR helpers (truecolor; endian-neutral integer formatting) ------------

// "\x1b[38;2;R;G;Bm" appended to `out`.
inline void append_fg(std::string& out, Rgb c) {
  out += "\x1b[38;2;";
  out += std::to_string(c.r);
  out += ';';
  out += std::to_string(c.g);
  out += ';';
  out += std::to_string(c.b);
  out += 'm';
}

// "\x1b[48;2;R;G;Bm" appended to `out`.
inline void append_bg(std::string& out, Rgb c) {
  out += "\x1b[48;2;";
  out += std::to_string(c.r);
  out += ';';
  out += std::to_string(c.g);
  out += ';';
  out += std::to_string(c.b);
  out += 'm';
}

}  // namespace shigoku::tui
