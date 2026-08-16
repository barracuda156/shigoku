// sixel_tests.cpp — the SIXEL cover backend: the emitter's wire shape, the
// DA1 attribute classifier, the backend ranking with a third contender, and
// the string-backed FILE* seam the vendored encoder writes through.
//
// There is no Rust counterpart to port from: sabigoku pins kitty only, and
// this backend is a shigoku extension. So these are structural assertions,
// not goldens — a byte-golden would pin one compiler's qsort tie-breaks in
// the palette builder, which is not a contract worth freezing. What IS
// asserted is determinism: identical input must give identical bytes on THIS
// build, which is what makes the frame short-circuit in compose_cover_apc
// sound.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/tui/cover_probe.hpp"
#include "../src/tui/memstream_compat.hpp"
#include "../src/tui/sixel.hpp"

using namespace shigoku;
using namespace shigoku::tui;

namespace {

// A w*h RGBA gradient with a colour checker — enough distinct colours to
// exercise the adaptive palette rather than collapsing to a flat fill.
std::vector<unsigned char> gradient(std::uint32_t w, std::uint32_t h) {
  std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4u);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      unsigned char* p = &px[(static_cast<std::size_t>(y) * w + x) * 4u];
      p[0] = static_cast<unsigned char>((x * 255u) / (w > 1 ? w - 1 : 1));
      p[1] = static_cast<unsigned char>((y * 255u) / (h > 1 ? h - 1 : 1));
      p[2] = static_cast<unsigned char>(((x ^ y) & 4u) ? 220 : 30);
      p[3] = 255;
    }
  }
  return px;
}

}  // namespace

// --- the emitter -----------------------------------------------------------

TEST_CASE("transmit: introducer, raster attributes, palette, ST") {
  const auto px = gradient(40, 60);
  // 5 cols x 10 rows at 8x6 px per cell = a 40x60 destination.
  const std::string s = sixel::transmit(px, 40, 60, 5, 10, 8, 6);
  REQUIRE(!s.empty());
  // ESC P <macro-param> ; <background mode> ; q " <ar_num> ; <ar_den> ; W ; H
  // Mode 0 = paint the background (not transparent); 1;1 = square pixels.
  CHECK(s.starts_with("\x1bP7;0;q\"1;1;40;60"));
  CHECK(s.ends_with("\x1b\\"));  // ST terminates the DCS string.
  // At least one palette definition, "#<n>;2;<r>;<g>;<b>" (2 = RGB).
  CHECK(s.find("#0;2;") != std::string::npos);
}

TEST_CASE("transmit: the body is sixel-range bytes and control words only") {
  const auto px = gradient(24, 24);
  const std::string s = sixel::transmit(px, 24, 24, 3, 4, 8, 6);
  REQUIRE(s.size() > 4);
  const std::string body = s.substr(2, s.size() - 4);  // strip ESC P … ESC \.
  for (const char c : body) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool data = u >= 0x3f && u <= 0x7e;              // '?'..'~'
    const bool control = c == '#' || c == ';' || c == '$'  // colour, CR
                         || c == '-' || c == '!' || c == '"';  // LF, repeat
    const bool digit = c >= '0' && c <= '9';
    CHECK(((data || control || digit)));
  }
}

TEST_CASE("transmit: deterministic for identical input") {
  const auto px = gradient(32, 48);
  const std::string a = sixel::transmit(px, 32, 48, 4, 8, 8, 6);
  const std::string b = sixel::transmit(px, 32, 48, 4, 8, 8, 6);
  REQUIRE(!a.empty());
  CHECK(a == b);
}

TEST_CASE("transmit: the destination box follows the cell geometry") {
  const auto px = gradient(20, 20);
  // Same cell box, different per-cell pixels ⇒ different raster dimensions.
  CHECK(sixel::transmit(px, 20, 20, 2, 3, 10, 20).starts_with("\x1bP7;0;q\"1;1;20;60"));
  CHECK(sixel::transmit(px, 20, 20, 2, 3, 4, 8).starts_with("\x1bP7;0;q\"1;1;8;24"));
}

TEST_CASE("transmit: guards return the empty string, never a partial escape") {
  const auto px = gradient(16, 16);
  CHECK(sixel::transmit(px, 0, 16, 2, 2, 8, 8).empty());   // zero source width.
  CHECK(sixel::transmit(px, 16, 0, 2, 2, 8, 8).empty());   // zero source height.
  CHECK(sixel::transmit(px, 16, 17, 2, 2, 8, 8).empty());  // rgba/dims mismatch.
  CHECK(sixel::transmit({}, 16, 16, 2, 2, 8, 8).empty());  // no pixels at all.
  CHECK(sixel::transmit(px, 16, 16, 0, 2, 8, 8).empty());  // empty cell box.
  CHECK(sixel::transmit(px, 16, 16, 2, 0, 8, 8).empty());
  CHECK(sixel::transmit(px, 16, 16, -1, 2, 8, 8).empty());
  // Unknown cell geometry: SIXEL cannot be sized, so nothing is drawn. This
  // is the guard behind backend_from's cell.known() precondition.
  CHECK(sixel::transmit(px, 16, 16, 2, 2, 0, 8).empty());
  CHECK(sixel::transmit(px, 16, 16, 2, 2, 8, 0).empty());
}

TEST_CASE("transmit: an absurd destination box is railed, not attempted") {
  const auto px = gradient(16, 16);
  // 400x400 cells at 64px each would be 25600x25600 = 655M pixels. The rails
  // scale it down proportionally rather than stalling the frame; what must
  // hold is that something valid and bounded comes out.
  const std::string s = sixel::transmit(px, 16, 16, 400, 400, 64, 64);
  REQUIRE(!s.empty());
  CHECK(s.starts_with("\x1bP7;0;q\"1;1;"));
  CHECK(s.ends_with("\x1b\\"));
  // Squareness is preserved by the proportional shrink (halved to 1600x1600,
  // the first size under both the 2560-a-side and 4M-pixel rails).
  CHECK(s.find("\"1;1;1600;1600") != std::string::npos);
}

// --- the DA1 classifier ----------------------------------------------------

TEST_CASE("da1_has_attribute: whole tokens only") {
  using sixel::detail::da1_has_attribute;
  // xterm with sixel: 62 = VT220, 4 = sixel, 9 = national replacement charsets.
  CHECK(da1_has_attribute("\x1b[?62;4;9c", 4));
  CHECK(da1_has_attribute("\x1b[?4c", 4));        // sole attribute.
  CHECK(da1_has_attribute("\x1b[?62;9;4c", 4));   // last attribute.
  CHECK(da1_has_attribute("\x1b[?62;4;9c", 62));  // any attribute, not just 4.
  // The whole point of tokenizing: these contain '4' but do not advertise it.
  CHECK_FALSE(da1_has_attribute("\x1b[?64;14;21c", 4));
  CHECK_FALSE(da1_has_attribute("\x1b[?41c", 4));
  CHECK_FALSE(da1_has_attribute("\x1b[?62;9c", 4));
  // Malformed or absent replies never claim support.
  CHECK_FALSE(da1_has_attribute("", 4));
  CHECK_FALSE(da1_has_attribute("\x1b[?62;4", 4));   // unterminated.
  CHECK_FALSE(da1_has_attribute("\x1b[?62;x4c", 4));  // non-numeric body.
  CHECK_FALSE(da1_has_attribute("\x1b[4;600;800t", 4));  // CSI 14t, not DA1.
}

TEST_CASE("probe_reply_is_sixel: finds the DA1 among the other answers") {
  // The probe collects every reply in one window; the DA1 comes last.
  CHECK(sixel::probe_reply_is_sixel(
      "\x1b[4;600;800t\x1b_Gi=1;OK\x1b\\\x1b[?62;4;9c"));
  CHECK_FALSE(sixel::probe_reply_is_sixel(
      "\x1b[4;600;800t\x1b_Gi=1;OK\x1b\\\x1b[?62;9c"));
  CHECK_FALSE(sixel::probe_reply_is_sixel(""));  // a mute terminal.
}

// --- the ranking -----------------------------------------------------------

TEST_CASE("backend_from: sixel is the last resort, above placeholder only") {
  CoverProbe p;
  p.cell = CellPx{8, 16};
  CHECK(backend_from(p) == CoverBackend::None);
  p.sixel = true;
  CHECK(backend_from(p) == CoverBackend::Sixel);
  p.iterm = true;  // lossless 24-bit beats a 256-colour palette.
  CHECK(backend_from(p) == CoverBackend::Iterm);
  p.kitty = true;  // and kitty beats that.
  CHECK(backend_from(p) == CoverBackend::Kitty);
}

TEST_CASE("backend_from: sixel needs the cell geometry it is sized in") {
  // mlterm-shaped verdict but no pixel geometry anywhere: a SIXEL cannot be
  // fitted to its cell box, so this degrades to placeholder mode rather than
  // guessing a size that could scroll the screen.
  CoverProbe p;
  p.sixel = true;
  CHECK(backend_from(p) == CoverBackend::None);
  p.cell = CellPx{8, 16};
  CHECK(backend_from(p) == CoverBackend::Sixel);
  // The precondition is sixel's alone — the other two size in cells.
  CoverProbe q;
  q.kitty = true;
  CHECK(backend_from(q) == CoverBackend::Kitty);
  q.kitty = false;
  q.iterm = true;
  CHECK(backend_from(q) == CoverBackend::Iterm);
}

TEST_CASE("backend_from: existing verdicts are unchanged by the new arm") {
  CoverProbe p;  // no cell geometry: must not disturb kitty/iterm at all.
  p.kitty = true;
  p.sixel = true;  // kitty terminals answer DA1 attribute 4 too (foot, wezterm).
  CHECK(backend_from(p) == CoverBackend::Kitty);
  p.iterm2 = true;  // the iTerm2 exception still outranks everything.
  CHECK(backend_from(p) == CoverBackend::Iterm);
}

// --- the FILE* seam --------------------------------------------------------

TEST_CASE("StringFile: bytes written through stdio round-trip") {
  StringFile sf;
  REQUIRE(sf.ok());
  std::fputs("\x1bP7;0;q", sf.file());
  std::fputc('#', sf.file());
  const char raw[] = {'0', ';', '2', '\0', '\x1b'};  // embedded NUL survives.
  std::fwrite(raw, 1, sizeof(raw), sf.file());
  const std::string out = sf.take();
  CHECK(out == std::string("\x1bP7;0;q#0;2\0\x1b", 13));
  CHECK(out.size() == 13);
  // take() is idempotent and closes the stream.
  CHECK(sf.take().empty());
  CHECK(sf.file() == nullptr);
}

TEST_CASE("StringFile: survives a write larger than one stdio buffer") {
  StringFile sf;
  REQUIRE(sf.ok());
  const std::string chunk(1000, 'x');
  for (int i = 0; i < 64; ++i) {
    REQUIRE(std::fwrite(chunk.data(), 1, chunk.size(), sf.file()) == chunk.size());
  }
  const std::string out = sf.take();
  CHECK(out.size() == 64000);
  CHECK(out.find_first_not_of('x') == std::string::npos);
}

TEST_CASE("StringFile: an untaken stream is closed by the destructor") {
  // No assertion beyond "this does not leak or crash" — the value is that
  // sanitizer/valgrind runs of this suite cover the early-return path in
  // sixel::transmit, where the encoder fails and take() is never called.
  StringFile sf;
  REQUIRE(sf.ok());
  std::fputs("abandoned", sf.file());
}
