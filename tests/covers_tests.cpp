// covers_tests.cpp — P8/P26 covers + kitty-emitter tests (A4, 04 §7.3, 05 §12,
// DESIGN §3.3).
//
// Coverage:
//   - kitty emitter goldens: q=2 everywhere, C=1 + c=/r= on transmit, d=I
//     delete, NO o=, m=1/m=0 chunking incl. non-4-byte-aligned payloads,
//     4096-char chunk boundary, one-chunk (m=0 alone) path, empty image.
//   - base64 (RFC 4648) round-trip incl. 1/2-byte tail padding.
//   - cover geometry: tier split at 80, poster_rows formula, detail caps/floors,
//     cell_px_from_window.
//   - ByteLru (cache.rs port): get promotes, byte-bounded eviction, oversize
//     decline, replace settles byte accounting.
//   - disk_cache (disk.rs port): roundtrip/miss, stable full-hex stem with no
//     collisions, no temp droppings survive a write, oversize file reads as a
//     miss.
//   - Covers::load full pipeline (mod.rs port): decoded-LRU/raw-LRU/disk hits
//     each short-circuit further work and warm the layers above them; a
//     corrupt disk body heals by refetching; network success warms every
//     layer; a box change re-crops from the cached decode without refetching.
//   - decode_cover + crop_and_resize: real PNG decode, decode-bomb guard,
//     center-crop-to-fill aspect.
//   - CoverState (05 §12): the detail.rs decision table, ported 1:1.
//   - CSI 14t parse.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/tui/cover_geom.hpp"
#include "../src/tui/cover_probe.hpp"
#include "../src/tui/covers.hpp"
#include "../src/tui/iterm.hpp"
#include "../src/tui/kitty.hpp"

using namespace shigoku;       // CoverPixels lives in shigoku (event.hpp).
using namespace shigoku::tui;  // kitty::, CoverState, cover geometry.

namespace {

// True if `hay` contains `needle` (substring).
bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// Count non-overlapping occurrences of `needle` in `hay`.
int count(const std::string& hay, const std::string& needle) {
  int n = 0;
  std::size_t p = 0;
  while ((p = hay.find(needle, p)) != std::string::npos) {
    ++n;
    p += needle.size();
  }
  return n;
}

kitty::Image solid(std::uint32_t w, std::uint32_t h, unsigned char v) {
  kitty::Image img;
  img.w = w;
  img.h = h;
  img.rgba.assign(static_cast<std::size_t>(w) * h * 4u, v);
  return img;
}

std::vector<unsigned char> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
}

// A scratch disk-cache dir under the OS temp dir, wiped before use so tests
// never see a stale prior run's files (disk.rs test_dir). Test-only: unlike
// covers.cpp (shipped to the legacy MacPorts target), this TU never leaves the
// dev box, so std::filesystem is fine here.
std::string test_dir(const std::string& name) {
  const char* tmp = std::getenv("TMPDIR");
  const std::string base = (tmp != nullptr && tmp[0] != '\0') ? tmp : "/tmp";
  const std::string dir = base + "/shigoku-covers-disk-tests/" + name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return dir;
}

std::vector<unsigned char> body_of(std::size_t n, unsigned char fill) {
  return std::vector<unsigned char>(n, fill);
}

}  // namespace

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------

TEST_CASE("base64 matches RFC 4648 incl. tail padding") {
  auto enc = [](std::initializer_list<unsigned char> b) {
    return kitty::base64_encode(std::vector<unsigned char>(b));
  };
  CHECK(enc({}) == "");
  // "M" = 0x4D -> "TQ==" ; "Ma" -> "TWE=" ; "Man" -> "TWFu".
  CHECK(enc({'M'}) == "TQ==");
  CHECK(enc({'M', 'a'}) == "TWE=");
  CHECK(enc({'M', 'a', 'n'}) == "TWFu");
  // 4 RGBA bytes (one pixel) -> a full 3-byte group ("AAAA") + a 1-byte tail
  // ("AA==") = 8 chars. (3 bytes would be "AAAA"; 4 bytes is NOT a clean
  // quantum — this pins that the chunker never assumes 4|payload.)
  CHECK(kitty::base64_encode({0, 0, 0}) == "AAAA");        // exactly 3 bytes.
  CHECK(kitty::base64_encode({0, 0, 0, 0}) == "AAAAAA==");  // 4 bytes, padded.
  CHECK(kitty::base64_encode({255, 255, 255, 255}) == "/////w==");
}

// ---------------------------------------------------------------------------
// kitty emitter goldens (A4)
// ---------------------------------------------------------------------------

TEST_CASE("transmit: single-chunk carries the full A4 control block") {
  const std::string out = kitty::transmit(solid(1, 1, 0), /*id=*/7, /*cols=*/20,
                                          /*rows=*/13);
  // APC envelope.
  CHECK(has(out, "\x1b_G"));
  CHECK(has(out, "\x1b\\"));
  // A4 keys, exactly.
  CHECK(has(out, "a=T"));
  CHECK(has(out, "f=32"));      // raw RGBA
  CHECK(has(out, "i=7"));       // stable id
  CHECK(has(out, "s=1"));       // source pixel width (MANDATORY for f=32 —
  CHECK(has(out, "v=1"));       // its omission was a live no-covers bug)
  CHECK(has(out, "c=20"));      // explicit cell box width
  CHECK(has(out, "r=13"));      // explicit cell box rows
  CHECK(has(out, "C=1"));       // do not move the cursor
  CHECK(has(out, "q=2"));       // quiet
  // A single small image fits one chunk: m=0 alone (no m=1 run before it).
  CHECK(has(out, "m=0"));
  CHECK_FALSE(has(out, "m=1"));
  // A4: compression is NEVER emitted.
  CHECK_FALSE(has(out, "o=z"));
  CHECK_FALSE(has(out, "o="));
  // t defaults to direct — never f/t/s transmission (contour rejects them).
  CHECK_FALSE(has(out, "t=f"));
  CHECK_FALSE(has(out, "t=t"));
  CHECK_FALSE(has(out, "t=s"));
}

TEST_CASE("transmit: multi-chunk payload uses m=1 … m=0, 4096-char fragments") {
  // A payload big enough to need several 4096-char base64 fragments. 4096 b64
  // chars decode from 3072 raw bytes; make the RGBA well over 3 fragments.
  // 40x40 RGBA = 6400 px * 4 = 25600 bytes -> base64 ~34136 chars -> ~9 chunks.
  const std::string out = kitty::transmit(solid(40, 40, 128), /*id=*/1, 20, 13);

  // Every fragment is one APC (ESC_G … ESC\). Count them.
  const int apc_open = count(out, "\x1b_G");
  const int apc_close = count(out, "\x1b\\");
  CHECK(apc_open == apc_close);
  CHECK(apc_open >= 3);  // multi-chunk.

  // Exactly one m=0 (the final, closing chunk); the rest are m=1.
  CHECK(count(out, "m=0") == 1);
  CHECK(count(out, "m=1") == apc_open - 1);

  // The first fragment carries the control block; continuations carry only m=.
  CHECK(count(out, "a=T") == 1);
  CHECK(count(out, "s=40") == 1);  // pixel dims ride the control block only.
  CHECK(count(out, "v=40") == 1);
  CHECK(count(out, "c=20") == 1);
  CHECK(count(out, "q=2") == 1);  // control block only on the first fragment.

  // No base64 fragment exceeds the 4096-char chunk bound. Slice on APC opens
  // and check each payload (after the ';') length.
  std::size_t p = 0;
  int checked = 0;
  while ((p = out.find("\x1b_G", p)) != std::string::npos) {
    const std::size_t semi = out.find(';', p);
    const std::size_t end = out.find("\x1b\\", p);
    REQUIRE(semi != std::string::npos);
    REQUIRE(end != std::string::npos);
    REQUIRE(semi < end);
    const std::size_t payload_len = end - semi - 1;
    CHECK(payload_len <= kitty::kChunkBase64Bytes);
    ++checked;
    p = end + 2;
  }
  CHECK(checked == apc_open);
}

TEST_CASE("transmit: non-4-byte-aligned payload still chunks cleanly") {
  // 1x7 RGBA = 28 bytes -> base64 40 chars (divisible by 4, one chunk). Use a
  // pixel count whose RGBA size is not a multiple of 3 to exercise tail
  // padding inside the single fragment: 1x5 = 20 bytes -> ceil(20/3)=7 groups
  // -> 28 base64 chars with '=' padding (20 % 3 == 2 -> one '=').
  const std::string out = kitty::transmit(solid(1, 5, 90), 1, 4, 3);
  CHECK(count(out, "\x1b_G") == 1);   // fits one fragment.
  CHECK(has(out, "s=1"));             // pixel dims present.
  CHECK(has(out, "v=5"));
  CHECK(has(out, "m=0"));
  CHECK(has(out, "="));               // tail padding present in the payload.
}

TEST_CASE("transmit: empty / zero-size image emits nothing") {
  CHECK(kitty::transmit(kitty::Image{}, 1, 20, 13) == "");
  kitty::Image z;
  z.w = 0;
  z.h = 5;
  z.rgba.assign(0, 0);
  CHECK(kitty::transmit(z, 1, 20, 13) == "");
}

TEST_CASE("delete_image: a=d, capital d=I, by id, quiet, no payload") {
  const std::string out = kitty::delete_image(7);
  CHECK(has(out, "\x1b_G"));
  CHECK(has(out, "\x1b\\"));
  CHECK(has(out, "a=d"));
  CHECK(has(out, "d=I"));   // CAPITAL I — frees stored data + honors the quota.
  CHECK_FALSE(has(out, "d=i"));
  CHECK(has(out, "i=7"));
  CHECK(has(out, "q=2"));
  // No payload after the control data (no ';').
  CHECK_FALSE(has(out, ";"));
}

TEST_CASE("query_apc / detection_probe: a=t transmit-ack probe, DA1 framing") {
  const std::string q = kitty::query_apc(1);
  CHECK(has(q, "\x1b_G"));
  CHECK(has(q, "a=t"));            // transmit-only: forces an OK ack contour returns.
  CHECK_FALSE(has(q, "a=q"));      // NOT a query — contour_ppc ignores a=q (false-negative).
  CHECK_FALSE(has(q, "q=2"));      // NOT quiet — quiet would suppress the ack we detect on.
  CHECK_FALSE(has(q, "\x1b[c"));   // query_apc has no DA1 of its own.

  const std::string probe = kitty::detection_probe(1);
  CHECK(has(probe, "a=t"));
  CHECK(has(probe, "\x1b[c"));     // detection_probe frames with a DA1.
}

TEST_CASE("probe_reply_is_kitty: graphics APC ack vs bare DA1") {
  // A graphics APC anywhere in the reply => kitty.
  CHECK(kitty::probe_reply_is_kitty("\x1b_Gi=1;OK\x1b\\\x1b[?62;c"));
  // A bare DA1 answer, no graphics APC => placeholder.
  CHECK_FALSE(kitty::probe_reply_is_kitty("\x1b[?62;22;c"));
  CHECK_FALSE(kitty::probe_reply_is_kitty(""));
}

// ---------------------------------------------------------------------------
// OSC 1337 emitter (iterm.hpp) — the second backend, chosen when the kitty
// query goes unanswered but XTVERSION names an inline-image adopter.
// ---------------------------------------------------------------------------

TEST_CASE("iterm transmit: OSC 1337 shape, cell box, PNG payload") {
  // 2x1: one red, one green pixel.
  const std::vector<unsigned char> rgba = {255, 0, 0, 255, 0, 255, 0, 255};
  const std::string s = tui::iterm::transmit(rgba, 2, 1, 10, 5);
  REQUIRE(!s.empty());
  CHECK(s.rfind("\x1b]1337;File=inline=1;size=", 0) == 0);
  CHECK(s.find(";width=10;height=5;preserveAspectRatio=0;doNotMoveCursor=1:") !=
        std::string::npos);
  CHECK(s.back() == '\x07');  // BEL-terminated, the documented form.
  // The payload is a PNG: base64 of the 8-byte magic \x89PNG\r\n\x1a\n is a
  // fixed prefix.
  const std::size_t colon = s.find(':');
  REQUIRE(colon != std::string::npos);
  CHECK(s.compare(colon + 1, 11, "iVBORw0KGgo") == 0);
}

TEST_CASE("iterm transmit: zero-size / mismatched buffer -> empty") {
  CHECK(tui::iterm::transmit({}, 0, 0, 10, 5).empty());
  CHECK(tui::iterm::transmit({1, 2, 3, 4}, 2, 2, 10, 5).empty());  // 2x2 needs 16.
}

TEST_CASE("iterm encode_png round-trips through decode_cover") {
  // 2x2 distinct opaque pixels survive PNG encode -> stb decode byte-exact.
  const std::vector<unsigned char> rgba = {
      255, 0,   0,   255,  0,   255, 0,   255,
      0,   0,   255, 255,  128, 64,  32,  255};
  const std::vector<unsigned char> png = tui::iterm::encode_png(rgba, 2, 2);
  REQUIRE(!png.empty());
  const auto decoded = tui::decode_cover(png);
  REQUIRE(decoded.has_value());
  CHECK(decoded->w == 2);
  CHECK(decoded->h == 2);
  CHECK(decoded->rgba == rgba);
}

TEST_CASE("probe_reply_is_iterm: ReportCellSize answer or XTVERSION adopter") {
  // The definitive signal: a ReportCellSize answer (only iTerm2 implements
  // the query; whether it also answers XTVERSION varies by version).
  CHECK(tui::iterm::probe_reply_is_iterm(
      "\x1b]1337;ReportCellSize=17.50;8.00;2.0\x1b\\\x1b[?62;4c"));
  // The XTVERSION fallback, adopters (case-insensitive on the name).
  CHECK(tui::iterm::probe_reply_is_iterm("\x1bP>|iTerm2 3.5.9\x1b\\\x1b[?62;c"));
  CHECK(tui::iterm::probe_reply_is_iterm(
      "\x1bP>|WezTerm 20230712-072601-f4abf8fd\x1b\\"));
  CHECK(tui::iterm::probe_reply_is_iterm("\x1bP>|mintty 3.7.0\x1b\\"));
  // A terminal that names itself but is not an OSC 1337 adopter.
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm("\x1bP>|contour 0.4.3.6442\x1b\\"));
  // Bare DA1 / kitty-only / silence.
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm("\x1b[?62;22;c"));
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm("\x1b_Gi=1;OK\x1b\\"));
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm(""));
}

TEST_CASE("probe_reply_is_iterm2: only iTerm2 itself, not the other adopters") {
  // ReportCellSize is iTerm2's proprietary report — an answer IS iTerm2.
  CHECK(tui::iterm::probe_reply_is_iterm2(
      "\x1b]1337;ReportCellSize=17.50;8.00;2.0\x1b\\\x1b[?62;4c"));
  // The XTVERSION name pins it too.
  CHECK(tui::iterm::probe_reply_is_iterm2("\x1bP>|iTerm2 3.5.9\x1b\\\x1b[?62;c"));
  // iTerm2 3.5.14's real reply: kitty OK ack + CSI 14t + ReportCellSize +
  // XTVERSION + DA1, all in one window. Both classifiers must fire.
  const char* kIterm3514 =
      "\x1b[4;690;718t\x1b_GOK\x1b\\\x1b]1337;ReportCellSize=17.0;7.0;1.0\x1b\\"
      "\x1bP>|iTerm2 3.5.14\x1b\\\x1b[?64;1;2;4;6;17;18;21;22c";
  CHECK(tui::iterm::probe_reply_is_iterm2(kIterm3514));
  CHECK(tui::iterm::probe_reply_is_iterm(kIterm3514));
  // The other OSC 1337 adopters are NOT iTerm2: their kitty support is real,
  // so they must keep the kitty backend.
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm2(
      "\x1bP>|WezTerm 20230712-072601-f4abf8fd\x1b\\"));
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm2("\x1bP>|mintty 3.7.0\x1b\\"));
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm2("\x1bP>|konsole 24.02.2\x1b\\"));
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm2("\x1b_Gi=1;OK\x1b\\"));
  CHECK_FALSE(tui::iterm::probe_reply_is_iterm2(""));
}

TEST_CASE("parse_cell_size_reply: points x scale -> pixels") {
  double w = 0, h = 0;
  // Retina: 8.00pt x 17.50pt at scale 2.0 -> 16 x 35 px.
  REQUIRE(tui::iterm::parse_cell_size_reply(
      "\x1b]1337;ReportCellSize=17.50;8.00;2.0\x1b\\", w, h));
  CHECK(w == doctest::Approx(16.0));
  CHECK(h == doctest::Approx(35.0));
  // Older reply without the scale field -> scale 1.0.
  REQUIRE(tui::iterm::parse_cell_size_reply(
      "\x1b]1337;ReportCellSize=20;9\x1b\\", w, h));
  CHECK(w == doctest::Approx(9.0));
  CHECK(h == doctest::Approx(20.0));
  // Malformed / absent.
  CHECK_FALSE(tui::iterm::parse_cell_size_reply("\x1b]1337;ReportCellSize=\x1b\\", w, h));
  CHECK_FALSE(tui::iterm::parse_cell_size_reply("\x1b[?62;4c", w, h));
  CHECK_FALSE(tui::iterm::parse_cell_size_reply("", w, h));
  // Zero-size refused (a degenerate reply must not poison the geometry).
  CHECK_FALSE(tui::iterm::parse_cell_size_reply(
      "\x1b]1337;ReportCellSize=0;8.0;2.0\x1b\\", w, h));
}

TEST_CASE("backend_from: kitty wins, iterm falls back, else none") {
  tui::CoverProbe p;
  CHECK(tui::backend_from(p) == tui::CoverBackend::None);
  p.iterm = true;
  CHECK(tui::backend_from(p) == tui::CoverBackend::Iterm);
  p.kitty = true;  // both answered: the richer wire wins.
  CHECK(tui::backend_from(p) == tui::CoverBackend::Kitty);
}

TEST_CASE("backend_from: iTerm2 itself outranks its own kitty ack") {
  // iTerm2 3.5 acks a=q but its kitty path doesn't render; the native
  // OSC 1337 backend must win there — and only there.
  tui::CoverProbe p;
  p.kitty = true;
  p.iterm = true;
  p.iterm2 = true;
  CHECK(tui::backend_from(p) == tui::CoverBackend::Iterm);
  // WezTerm-shaped verdict (adopter, but not iTerm2): kitty keeps the seat.
  p.iterm2 = false;
  CHECK(tui::backend_from(p) == tui::CoverBackend::Kitty);
}

// ---------------------------------------------------------------------------
// cover geometry (sizing.rs port)
// ---------------------------------------------------------------------------

TEST_CASE("cover_tier splits at 80 effective cols") {
  CHECK(cover_tier(80) == CoverTier{true, 20, 22});
  CHECK(cover_tier(120) == CoverTier{true, 20, 22});
  CHECK(cover_tier(79) == CoverTier{false, 14, 16});
  CHECK(cover_tier(40) == CoverTier{false, 14, 16});
}

TEST_CASE("poster_rows: 2:3 aspect from cell geometry (sizing.rs)") {
  // Ghostty 9x20 px cells, large tier (cover_w=20): (20*9*3/2)/20 = 270/20 = 13.
  CHECK(poster_rows(20, CellPx{9, 20}) == 13);
  // Degenerate cell height floors to 1 (no divide-by-zero).
  CHECK(poster_rows(20, CellPx{9, 0}) >= 0);
}

TEST_CASE("poster_cols inverts poster_rows (zoom scaling)") {
  // Ghostty 9x20: 13 rows back to columns — floor round-trip stays ≤ the
  // width poster_rows(20) started from.
  CHECK(poster_cols(13, CellPx{9, 20}) == 19);
  // The half-canvas growth target on a 40-row frame: 18 rows → 26 cols.
  CHECK(poster_cols(18, CellPx{9, 20}) == 26);
  // Degenerate cell width floors to 1 (no divide-by-zero).
  CHECK(poster_cols(10, CellPx{0, 20}) >= 0);
}

TEST_CASE("detail_cover_rows: adaptive, clamped to caps; fallback = cap") {
  // Reported geometry -> adaptive (ghostty 9x20 large tier = 13).
  CHECK(detail_cover_rows(cover_tier(100), CellPx{9, 20}) == 13);
  // No geometry -> the fixed caps double as the fallback (28 large, 20 small).
  CHECK(detail_cover_rows(cover_tier(100), CellPx{}) == 28);
  CHECK(detail_cover_rows(cover_tier(60), CellPx{}) == 20);
  // Square cells derive 30 rows for the large tier -> clamped to 28.
  CHECK(detail_cover_rows(cover_tier(100), CellPx{10, 10}) == 28);
}

TEST_CASE("card_cover_rows: adaptive, floors 7/5, NO upper cap (P17, sizing.rs)") {
  // Ghostty 9x20: large tier = 13, small tier = 9 (the ratified heights).
  CHECK(card_cover_rows(cover_tier(100), CellPx{9, 20}) == 13);
  CHECK(card_cover_rows(cover_tier(60), CellPx{9, 20}) == 9);
  // No geometry -> the fixed floors (7 large, 5 small), NOT the detail caps.
  CHECK(card_cover_rows(cover_tier(100), CellPx{}) == 7);
  CHECK(card_cover_rows(cover_tier(60), CellPx{}) == 5);
  // Wide flat cells push the derived height under the floor -> floored.
  CHECK(card_cover_rows(cover_tier(100), CellPx{4, 40}) == 7);
  CHECK(card_cover_rows(cover_tier(60), CellPx{4, 40}) == 5);
  // Square cells derive 30 rows for the large tier -> NOT capped (unlike detail).
  CHECK(card_cover_rows(cover_tier(100), CellPx{10, 10}) == 30);
  // Degenerate cell height is survived (never below the floor).
  CHECK(card_cover_rows(cover_tier(100), CellPx{9, 0}) >= 7);
}

TEST_CASE("cell_px_from_window: window pixels / grid, 0 on any missing input") {
  CHECK(cell_px_from_window(800, 600, 100, 30) == CellPx{8, 20});
  CHECK(cell_px_from_window(0, 600, 100, 30) == CellPx{});    // no x pixels.
  CHECK(cell_px_from_window(800, 600, 0, 30) == CellPx{});    // no cols.
  CHECK_FALSE(cell_px_from_window(800, 600, 100, 30).known() == false);
}

// ---------------------------------------------------------------------------
// CSI 14t parse
// ---------------------------------------------------------------------------

TEST_CASE("parse_csi_14t reads height;width in pixels") {
  std::uint16_t w = 0, h = 0;
  // ESC [ 4 ; <height> ; <width> t  (height precedes width in the report).
  CHECK(cover_probe_detail::parse_csi_14t("\x1b[4;600;800t", w, h));
  CHECK(h == 600);
  CHECK(w == 800);
  // Embedded in a longer reply (with a DA1 answer alongside).
  w = h = 0;
  CHECK(cover_probe_detail::parse_csi_14t("\x1b[?62;c\x1b[4;480;640t", w, h));
  CHECK(h == 480);
  CHECK(w == 640);
  // Not a 14t report.
  CHECK_FALSE(cover_probe_detail::parse_csi_14t("\x1b[?62;c", w, h));
  CHECK_FALSE(cover_probe_detail::parse_csi_14t("\x1b[4;0;0t", w, h));  // zero.
}

// ---------------------------------------------------------------------------
// ByteLru (cache.rs port): byte-bounded, url-keyed, get() promotes.
// ---------------------------------------------------------------------------

TEST_CASE("ByteLru: get promotes so eviction takes the cold entry") {
  RawLru lru(10, &byte_len);
  CHECK(lru.insert("a", body_of(4, 1)));
  CHECK(lru.insert("b", body_of(4, 2)));
  CHECK(lru.get("a").has_value());  // promotes "a".
  CHECK(lru.insert("c", body_of(4, 3)));  // evicts the cold "b".
  CHECK_FALSE(lru.get("b").has_value());
  CHECK(lru.get("a").has_value());
  CHECK(lru.get("c").has_value());
  CHECK(lru.total_bytes() == 8);
}

TEST_CASE("ByteLru: oversize is declined and nothing changes") {
  RawLru lru(10, &byte_len);
  CHECK(lru.insert("a", body_of(4, 1)));
  CHECK_FALSE(lru.insert("big", body_of(11, 9)));
  CHECK(lru.size() == 1);
  CHECK(lru.total_bytes() == 4);
  CHECK(lru.get("a").has_value());
}

TEST_CASE("ByteLru: replacing a key settles the byte accounting") {
  RawLru lru(10, &byte_len);
  CHECK(lru.insert("a", body_of(4, 1)));
  CHECK(lru.insert("a", body_of(6, 2)));
  CHECK(lru.size() == 1);
  CHECK(lru.total_bytes() == 6);
  CHECK(lru.get("a").value() == body_of(6, 2));
}

TEST_CASE("ByteLru: eviction frees until it fits") {
  RawLru lru(10, &byte_len);
  CHECK(lru.insert("a", body_of(4, 1)));
  CHECK(lru.insert("b", body_of(4, 2)));
  CHECK(lru.insert("c", body_of(9, 3)));
  CHECK(lru.size() == 1);
  CHECK(lru.get("c").has_value());
  CHECK(lru.total_bytes() == 9);
}

TEST_CASE("ByteLru: decoded tier accounts pixel bytes (2x2 RGBA = 16 bytes)") {
  CoverPixels img;
  img.w = 2;
  img.h = 2;
  img.rgba.assign(16, 7);
  DecodedLru lru15(15, &byte_len);
  CHECK_FALSE(lru15.insert("u", img));  // 15-byte cap declines a 16-byte image.
  DecodedLru lru16(16, &byte_len);
  CHECK(lru16.insert("u", img));
  CHECK(lru16.total_bytes() == 16);
}

// ---------------------------------------------------------------------------
// disk_cache (disk.rs port): url-keyed on-disk bodies, atomic writes.
// ---------------------------------------------------------------------------

TEST_CASE("disk_cache: roundtrip and miss") {
  const std::string dir = test_dir("roundtrip");
  const std::string url = "https://cdn.example/a.png";
  CHECK_FALSE(disk_cache::read(dir, url).has_value());
  disk_cache::write(dir, url, {'b', 'o', 'd', 'y'});
  auto got = disk_cache::read(dir, url);
  REQUIRE(got.has_value());
  CHECK(*got == std::vector<unsigned char>{'b', 'o', 'd', 'y'});
  CHECK_FALSE(disk_cache::read(dir, "https://cdn.example/other.png").has_value());
}

TEST_CASE("disk_cache: stem is stable full hex and urls do not collide") {
  const std::string dir = test_dir("stem");
  const std::string p1 = disk_cache::cover_path(dir, "https://cdn.example/a.png");
  const std::string p2 = disk_cache::cover_path(dir, "https://cdn.example/a.png");
  const std::string p3 = disk_cache::cover_path(dir, "https://cdn.example/b.png");
  CHECK(p1 == p2);
  CHECK(p1 != p3);
  // 64 hex chars + "/" + ".jpg" — the full SHA-256, not a truncated hex-16
  // (disk.rs's deliberate widen: refs cross the provider trust boundary).
  const std::string stem = p1.substr(dir.size() + 1, 64);
  CHECK(stem.size() == 64);
  for (char c : stem) CHECK(std::isxdigit(static_cast<unsigned char>(c)));
}

TEST_CASE("disk_cache: write leaves no temp droppings and heals a stale body") {
  const std::string dir = test_dir("no-temps");
  const std::string url = "https://cdn.example/a.png";
  disk_cache::write(dir, url, {'x'});
  disk_cache::write(dir, url, {'y'});
  auto got = disk_cache::read(dir, url);
  REQUIRE(got.has_value());
  CHECK(*got == std::vector<unsigned char>{'y'});
  // Exactly one file for this url: the final cover_path, no .tmp leftovers.
  CHECK(disk_cache::cover_path(dir, url).find(".tmp") == std::string::npos);
}

TEST_CASE("disk_cache: oversize file reads as a miss, not a truncation") {
  const std::string dir = test_dir("oversize");
  const std::string url = "https://cdn.example/big.png";
  disk_cache::write(dir, url, std::vector<unsigned char>(kMaxEncodedBytes + 1, 0));
  CHECK_FALSE(disk_cache::read(dir, url).has_value());
}

// ---------------------------------------------------------------------------
// decode + crop/resize (mod.rs decode_cover + cover_crop)
// ---------------------------------------------------------------------------

TEST_CASE("decode_cover decodes a real PNG to RGBA8") {
  const auto body = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.png");
  REQUIRE_FALSE(body.empty());
  auto img = decode_cover(body);
  REQUIRE(img.has_value());
  CHECK(img->w == 2);
  CHECK(img->h == 3);
  CHECK(img->rgba.size() == 2u * 3u * 4u);
  // First pixel is the top-left red we encoded.
  CHECK(img->rgba[0] == 200);
  CHECK(img->rgba[1] == 0);
  CHECK(img->rgba[2] == 0);
  CHECK(img->rgba[3] == 255);
}

TEST_CASE("decode_cover rejects empty / garbage bodies") {
  CHECK_FALSE(decode_cover({}).has_value());
  CHECK_FALSE(decode_cover({'n', 'o', 't', ' ', 'p', 'n', 'g'}).has_value());
}

// WeebCentral/Dynasty/nhentai serve WebP pages/covers; stb_image
// cannot read them, so decode_cover routes a RIFF….WEBP body to libwebp.
// cover_2x3.webp is a lossless re-encode of the same 2x3 pixels as
// cover_2x3.png (top-left red 200,0,0,255), so this asserts the WebP branch
// decodes to the identical RGBA8 result as the PNG branch above.
TEST_CASE("decode_cover decodes a real WebP to RGBA8" *
          doctest::skip(!SHIGOKU_HAVE_WEBP)) {
  const auto body = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.webp");
  REQUIRE_FALSE(body.empty());
  auto img = decode_cover(body);
  REQUIRE(img.has_value());
  CHECK(img->w == 2);
  CHECK(img->h == 3);
  CHECK(img->rgba.size() == 2u * 3u * 4u);
  CHECK(img->rgba[0] == 200);
  CHECK(img->rgba[1] == 0);
  CHECK(img->rgba[2] == 0);
  CHECK(img->rgba[3] == 255);
}

TEST_CASE("decode_cover: a WebP body without libwebp fails cleanly, not a crash" *
          doctest::skip(SHIGOKU_HAVE_WEBP)) {
  const auto body = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.webp");
  REQUIRE_FALSE(body.empty());
  CHECK_FALSE(decode_cover(body).has_value());
}

TEST_CASE("crop_and_resize center-crops to the target aspect and resizes") {
  // A 100x50 source, wider than a square 10x10 block: keep full height, crop
  // width to the block aspect, then resize to exactly 10x10.
  CoverPixels src;
  src.w = 100;
  src.h = 50;
  src.rgba.assign(100u * 50u * 4u, 128);
  auto out = crop_and_resize(src, 10, 10);
  REQUIRE(out.has_value());
  CHECK(out->w == 10);
  CHECK(out->h == 10);
  CHECK(out->rgba.size() == 10u * 10u * 4u);
}

TEST_CASE("crop_and_resize rejects zero dims") {
  CoverPixels src;
  src.w = 10;
  src.h = 10;
  src.rgba.assign(10u * 10u * 4u, 0);
  CHECK_FALSE(crop_and_resize(src, 0, 10).has_value());
  CHECK_FALSE(crop_and_resize(src, 10, 0).has_value());
  CoverPixels zero;
  CHECK_FALSE(crop_and_resize(zero, 10, 10).has_value());
}

// ---------------------------------------------------------------------------
// Covers::load — the full pipeline (mod.rs load_with port): decoded-LRU ->
// raw-LRU -> disk -> guarded network, warming every layer on the way back.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kPipelineUrl = "https://cdn.example/cover.png";

// A counting fetch stub: serves `body` and counts calls (mod.rs CountingFetch).
CoverFetchFn counting_fetch(std::vector<unsigned char> body, int* calls) {
  return [body = std::move(body), calls](
             std::string_view, const std::optional<std::string>&,
             const std::optional<std::string>&)
             -> Result<std::vector<unsigned char>, CoverFetchError> {
    ++*calls;
    return body;
  };
}

}  // namespace

TEST_CASE("Covers::load: decoded-LRU hit short-circuits the fetch") {
  const std::string dir = test_dir("pipeline-decoded-hit");
  const auto png = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.png");
  REQUIRE_FALSE(png.empty());
  int calls = 0;
  Covers covers(counting_fetch(png, &calls), dir);
  // Warm the decoded tier the same way a real fetch would (network path),
  // then reset the fetch counter to isolate the decoded-hit branch.
  auto first = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(first.has_value());
  calls = 0;
  auto second = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(second.has_value());
  CHECK(calls == 0);
}

TEST_CASE("Covers::load: disk hit warms both memory tiers and skips the network") {
  const std::string dir = test_dir("pipeline-disk-hit");
  const auto png = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.png");
  REQUIRE_FALSE(png.empty());
  disk_cache::write(dir, kPipelineUrl, png);
  int calls = 0;
  Covers covers(counting_fetch({}, &calls), dir);
  auto img = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(img.has_value());
  CHECK(calls == 0);
  // Second call must not touch disk or network either (decoded-LRU hit now).
  auto again = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(again.has_value());
  CHECK(calls == 0);
}

TEST_CASE("Covers::load: corrupt disk body heals by refetching") {
  const std::string dir = test_dir("pipeline-corrupt-disk");
  disk_cache::write(dir, kPipelineUrl, {'n', 'o', 't', ' ', 'p', 'n', 'g'});
  const auto png = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.png");
  REQUIRE_FALSE(png.empty());
  int calls = 0;
  Covers covers(counting_fetch(png, &calls), dir);
  auto img = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(img.has_value());
  CHECK(calls == 1);
  auto healed = disk_cache::read(dir, kPipelineUrl);
  REQUIRE(healed.has_value());
  CHECK(*healed == png);
}

TEST_CASE("Covers::load: network success warms disk + both memory tiers once") {
  const std::string dir = test_dir("pipeline-network");
  const auto png = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.png");
  REQUIRE_FALSE(png.empty());
  int calls = 0;
  Covers covers(counting_fetch(png, &calls), dir);
  auto img = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(img.has_value());
  CHECK(calls == 1);
  auto on_disk = disk_cache::read(dir, kPipelineUrl);
  REQUIRE(on_disk.has_value());
  CHECK(*on_disk == png);
  auto second = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(second.has_value());
  CHECK(calls == 1);  // second load is a cache hit, not a refetch.
}

TEST_CASE("Covers::load: a box change re-crops the cached decode, no refetch") {
  const std::string dir = test_dir("pipeline-box-change");
  const auto png = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.png");
  REQUIRE_FALSE(png.empty());
  int calls = 0;
  Covers covers(counting_fetch(png, &calls), dir);
  auto a = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(a.has_value());
  CHECK(calls == 1);
  // A different box (e.g. a terminal resize) must re-crop from the cached
  // decode, not refetch or re-decode from scratch.
  auto b = covers.load(nullptr, kPipelineUrl, 4, 4);
  REQUIRE(b.has_value());
  CHECK(calls == 1);
  CHECK(b->w == 4);
  CHECK(b->h == 4);
}

TEST_CASE("Covers::load: memory-only (empty covers_dir) never touches disk") {
  int calls = 0;
  const auto png = read_file(std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/cover_2x3.png");
  REQUIRE_FALSE(png.empty());
  Covers covers(counting_fetch(png, &calls), /*covers_dir=*/"");
  auto img = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(img.has_value());
  CHECK(calls == 1);
  auto second = covers.load(nullptr, kPipelineUrl, 2, 3);
  REQUIRE(second.has_value());
  CHECK(calls == 1);  // memory tiers alone still short-circuit the refetch.
}

// ---------------------------------------------------------------------------
// CoverState — the 05 §12 decision table (detail.rs, ported 1:1)
// ---------------------------------------------------------------------------

namespace {
constexpr std::int64_t ID7 = 7, ID8 = 8;
constexpr const char* URL = "https://cdn.example/a.png";
constexpr std::uint64_t COOLDOWN = kCoverCooldownTicks;  // 100 ticks (10s).
}  // namespace

TEST_CASE("cover: no target is None regardless of state") {
  CoverState s;
  CHECK(s.decide(std::nullopt, std::nullopt, 0) == CoverAction::None);
  s.begin_fetch(ID7, URL);
  CHECK(s.decide(std::nullopt, URL, 0) == CoverAction::None);
}

TEST_CASE("cover: urlless target clears only foreign art") {
  CoverState s;
  CHECK(s.decide(ID7, std::nullopt, 0) == CoverAction::None);
  s.begin_fetch(ID7, URL);
  CHECK(s.on_done(ID7));
  CHECK(s.decide(ID7, std::nullopt, 0) == CoverAction::None);
  CHECK(s.decide(ID8, std::nullopt, 0) == CoverAction::Clear);
}

TEST_CASE("cover: loading or pixels for target is UpToDate") {
  CoverState s;
  s.begin_fetch(ID7, URL);
  CHECK(s.decide(ID7, URL, 0) == CoverAction::UpToDate);
  CHECK(s.on_done(ID7));
  CHECK(s.decide(ID7, URL, 0) == CoverAction::UpToDate);
  CHECK(s.decide(ID8, URL, 0) == CoverAction::Fetch);
}

TEST_CASE("cover: failure suppresses same id+url within cooldown") {
  CoverState s;
  s.begin_fetch(ID7, URL);
  CHECK(s.on_error(ID7, 0));
  CHECK_FALSE(s.is_loading());  // error clears loading for retry.
  CHECK(s.decide(ID7, URL, 0) == CoverAction::Suppress);
  // A url change recovers immediately; another id never suppresses.
  CHECK(s.decide(ID7, "https://cdn.example/b.png", 0) == CoverAction::Fetch);
  CHECK(s.decide(ID8, URL, 0) == CoverAction::Fetch);
}

TEST_CASE("cover: cooldown expiry readmits at the boundary") {
  CoverState s;
  s.begin_fetch(ID7, URL);
  CHECK(s.on_error(ID7, 0));
  CHECK(s.decide(ID7, URL, COOLDOWN - 1) == CoverAction::Suppress);
  CHECK(s.decide(ID7, URL, COOLDOWN) == CoverAction::Fetch);
}

TEST_CASE("cover: failure record survives navigation but not success") {
  CoverState s;
  s.begin_fetch(ID7, URL);
  CHECK(s.on_error(ID7, 0));
  s.clear();  // navigate away.
  CHECK(s.decide(ID7, URL, 0) == CoverAction::Suppress);
  s.begin_fetch(ID7, URL);
  CHECK(s.on_done(ID7));
  s.clear();
  CHECK(s.decide(ID7, URL, 0) == CoverAction::Fetch);
}

TEST_CASE("cover: begin_fetch supersedes the failure record") {
  CoverState s;
  s.begin_fetch(ID7, URL);
  CHECK(s.on_error(ID7, 0));
  s.begin_fetch(ID7, URL);
  s.clear();
  CHECK(s.decide(ID7, URL, 0) == CoverAction::Fetch);
}

TEST_CASE("cover: failure record never leaks into the urlless branch") {
  CoverState s;
  s.begin_fetch(ID7, URL);
  CHECK(s.on_error(ID7, 0));
  CHECK(s.decide(ID7, std::nullopt, 0) == CoverAction::None);
  CHECK(s.decide(ID8, std::nullopt, 0) == CoverAction::None);
}

TEST_CASE("cover: stale done and error are dropped untouched") {
  CoverState s;
  s.begin_fetch(ID7, URL);
  CHECK_FALSE(s.on_done(9));  // stale cover_done discarded.
  CHECK(s.is_loading());
  CHECK_FALSE(s.has_pixels());
  CHECK_FALSE(s.on_error(9, 0));  // stale cover_error discarded.
  CHECK(s.is_loading());
  CHECK(s.for_id() == std::optional<std::int64_t>(ID7));
}

TEST_CASE("cover: error attributes the inflight url, not the current one") {
  CoverState s;
  s.begin_fetch(ID7, URL);
  CHECK(s.on_error(ID7, 0));
  // The record names URL, so a different url for the same id fetches.
  CHECK(s.decide(ID7, "https://cdn.example/moved.png", 0) == CoverAction::Fetch);
  CHECK(s.decide(ID7, URL, 0) == CoverAction::Suppress);
}
