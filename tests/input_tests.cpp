// input_tests.cpp — P6 byte→KeyEvent decoder tests (A3) + P32 mouse decode.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

#include "../src/tui/input.hpp"

using namespace shigoku;
using namespace shigoku::tui;
using S = KeyEvent::Special;
using MK = MouseEvent::Kind;
using MB = MouseEvent::Button;

namespace {
std::vector<Event> decode_all(InputDecoder& d, std::string_view bytes) {
  std::vector<Event> out;
  d.feed(bytes, out);
  return out;
}

// The key events in the decoded stream (mouse filtered out) — keeps the P6
// key assertions in their original shape.
std::vector<KeyEvent> decode(InputDecoder& d, std::string_view bytes) {
  std::vector<KeyEvent> keys;
  for (const Event& e : decode_all(d, bytes)) {
    if (const auto* k = std::get_if<KeyEvent>(&e)) keys.push_back(*k);
  }
  return keys;
}

std::vector<MouseEvent> decode_mouse(InputDecoder& d, std::string_view bytes) {
  std::vector<MouseEvent> ms;
  for (const Event& e : decode_all(d, bytes)) {
    if (const auto* m = std::get_if<MouseEvent>(&e)) ms.push_back(*m);
  }
  return ms;
}
}  // namespace

TEST_CASE("printable ascii and multibyte utf-8") {
  InputDecoder d;
  auto ev = decode(d, "aZ/");
  REQUIRE(ev.size() == 3);
  CHECK(ev[0].codepoint == U'a');
  CHECK(ev[1].codepoint == U'Z');
  CHECK(ev[2].codepoint == U'/');

  // 冬 = E5 86 AC
  auto ev2 = decode(d, "\xE5\x86\xAC");
  REQUIRE(ev2.size() == 1);
  CHECK(ev2[0].codepoint == U'冬');
}

TEST_CASE("multibyte split across two feeds") {
  InputDecoder d;
  std::vector<Event> out;
  d.feed("\xE5\x86", out);  // first two bytes of 冬
  CHECK(out.empty());       // held, incomplete
  d.feed("\xAC", out);      // final byte
  REQUIRE(out.size() == 1);
  CHECK(std::get<KeyEvent>(out[0]).codepoint == U'冬');
}

TEST_CASE("enter / tab / backspace / ctrl-c") {
  InputDecoder d;
  auto ev = decode(d, "\r\t\x7f\x03");
  REQUIRE(ev.size() == 4);
  CHECK(ev[0].special == S::Enter);
  CHECK(ev[1].special == S::Tab);
  CHECK(ev[2].special == S::Backspace);
  CHECK(ev[3].ctrl);
  CHECK(ev[3].codepoint == U'c');
}

TEST_CASE("CSI arrows and navigation keys") {
  InputDecoder d;
  auto up = decode(d, "\x1b[A");
  REQUIRE(up.size() == 1);
  CHECK(up[0].special == S::Up);

  auto nav = decode(d, "\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F");
  REQUIRE(nav.size() == 5);
  CHECK(nav[0].special == S::Down);
  CHECK(nav[1].special == S::Right);
  CHECK(nav[2].special == S::Left);
  CHECK(nav[3].special == S::Home);
  CHECK(nav[4].special == S::End);

  auto tilde = decode(d, "\x1b[3~\x1b[5~\x1b[6~");
  REQUIRE(tilde.size() == 3);
  CHECK(tilde[0].special == S::Delete);
  CHECK(tilde[1].special == S::PageUp);
  CHECK(tilde[2].special == S::PageDown);
}

TEST_CASE("CSI split across feeds decodes once complete") {
  InputDecoder d;
  std::vector<Event> out;
  d.feed("\x1b", out);
  CHECK(out.empty());
  CHECK(d.esc_pending());  // lone ESC held for disambiguation
  d.feed("[", out);
  CHECK(out.empty());
  CHECK(!d.esc_pending());  // now a growing CSI, not a lone ESC
  d.feed("A", out);
  REQUIRE(out.size() == 1);
  CHECK(std::get<KeyEvent>(out[0]).special == S::Up);
}

TEST_CASE("lone ESC resolves via flush_pending on timeout") {
  InputDecoder d;
  std::vector<Event> out;
  d.feed("\x1b", out);
  CHECK(out.empty());
  CHECK(d.esc_pending());
  const bool flushed = d.flush_pending(out);
  CHECK(flushed);
  REQUIRE(out.size() == 1);
  CHECK(std::get<KeyEvent>(out[0]).special == S::Escape);
  CHECK(!d.esc_pending());
}

TEST_CASE("ESC followed by a non-CSI byte is Escape then the byte") {
  InputDecoder d;
  auto ev = decode(d, "\x1b" "a");
  REQUIRE(ev.size() == 2);
  CHECK(ev[0].special == S::Escape);
  CHECK(ev[1].codepoint == U'a');
}

TEST_CASE("unrecognized CSI is swallowed, not spammed") {
  InputDecoder d;
  // ESC [ 2 0 0 ~  (bracketed-paste start) — not a key we map.
  auto ev = decode(d, "\x1b[200~");
  CHECK(ev.empty());
}

// --- P32 mouse -------------------------------------------------------------

TEST_CASE("SGR mouse press and release decode with 0-based coords") {
  InputDecoder d;
  // Press left at column 4, row 8 (1-based on the wire) then release there.
  auto ms = decode_mouse(d, "\x1b[<0;4;8M\x1b[<0;4;8m");
  REQUIRE(ms.size() == 2);
  CHECK(ms[0].kind == MK::Press);
  CHECK(ms[0].button == MB::Left);
  CHECK(ms[0].x == 3);
  CHECK(ms[0].y == 7);
  CHECK(ms[1].kind == MK::Release);
  CHECK(ms[1].button == MB::Left);
  CHECK(ms[1].x == 3);
  CHECK(ms[1].y == 7);
}

TEST_CASE("SGR middle/right buttons carry through") {
  InputDecoder d;
  auto ms = decode_mouse(d, "\x1b[<1;10;2M\x1b[<2;10;2M");
  REQUIRE(ms.size() == 2);
  CHECK(ms[0].button == MB::Middle);
  CHECK(ms[1].button == MB::Right);
}

TEST_CASE("SGR modifier bits are ignored, button survives") {
  InputDecoder d;
  // ctrl-click: 16 | 0 = 16 → still a plain left press.
  auto ms = decode_mouse(d, "\x1b[<16;5;5M");
  REQUIRE(ms.size() == 1);
  CHECK(ms[0].kind == MK::Press);
  CHECK(ms[0].button == MB::Left);
}

TEST_CASE("SGR wheel notches decode; motion and tilt stay silent") {
  InputDecoder d;
  // wheel up (64) / down (65) surface as wheel kinds with no button; a
  // shift-modified notch (64|4) is still a notch (modifier bits ignored).
  auto ms = decode_mouse(d, "\x1b[<64;3;4M\x1b[<65;3;4M\x1b[<68;3;4M");
  REQUIRE(ms.size() == 3);
  CHECK(ms[0].kind == MK::WheelUp);
  CHECK(ms[0].button == MB::None);
  CHECK(ms[0].x == 2);
  CHECK(ms[0].y == 3);
  CHECK(ms[1].kind == MK::WheelDown);
  CHECK(ms[2].kind == MK::WheelUp);
  // Left-drag motion (32), wheel tilt (66/67), and a release-form wheel
  // ('m') are consumed silently — none may leak as keys either.
  auto rest = decode_all(d, "\x1b[<32;3;3M\x1b[<66;3;3M\x1b[<67;3;3M\x1b[<64;3;3m");
  CHECK(rest.empty());
}

TEST_CASE("urxvt 1015 mouse decodes: +32 button base, decimal coords (HW #4)") {
  InputDecoder d;
  // Press left at column 4, row 8: Pb = 0+32, coords 1-based decimal — the
  // SGR shape minus the '<', button still carrying the X10 offset.
  auto ms = decode_mouse(d, "\x1b[32;4;8M");
  REQUIRE(ms.size() == 1);
  CHECK(ms[0].kind == MK::Press);
  CHECK(ms[0].button == MB::Left);
  CHECK(ms[0].x == 3);
  CHECK(ms[0].y == 7);

  // Release is Pb = 3+32 (the X10 "unknown button" release), wheel notches
  // 64/65+32; coords past the X10 byte ceiling arrive intact in decimal.
  auto more = decode_mouse(d, "\x1b[35;4;8M\x1b[96;3;4M\x1b[97;3;4M\x1b[32;250;100M");
  REQUIRE(more.size() == 4);
  CHECK(more[0].kind == MK::Release);
  CHECK(more[1].kind == MK::WheelUp);
  CHECK(more[2].kind == MK::WheelDown);
  CHECK(more[3].kind == MK::Press);
  CHECK(more[3].x == 249);
  CHECK(more[3].y == 99);
}

TEST_CASE("X10 wheel bytes decode as wheel notches") {
  InputDecoder d;
  // X10 cb = 32 + 64 = 96 ('`') wheel up, 97 ('a') wheel down; x=33+3 ('$'),
  // y=33+5 ('&').
  auto ms = decode_mouse(d, "\x1b[M`$&\x1b[Ma$&");
  REQUIRE(ms.size() == 2);
  CHECK(ms[0].kind == MK::WheelUp);
  CHECK(ms[0].button == MB::None);
  CHECK(ms[0].x == 3);
  CHECK(ms[0].y == 5);
  CHECK(ms[1].kind == MK::WheelDown);
}

TEST_CASE("SGR mouse split across feeds decodes once complete") {
  InputDecoder d;
  std::vector<Event> out;
  d.feed("\x1b[<0;1", out);
  CHECK(out.empty());  // params still growing.
  d.feed("2;34M", out);
  REQUIRE(out.size() == 1);
  const auto& m = std::get<MouseEvent>(out[0]);
  CHECK(m.kind == MK::Press);
  CHECK(m.x == 11);
  CHECK(m.y == 33);
}

TEST_CASE("malformed SGR mouse params are swallowed, not spammed") {
  InputDecoder d;
  // empty field / too many fields / trailing separator — consumed, no event.
  auto all = decode_all(d, "\x1b[<0;;3M\x1b[<0;1;2;3M\x1b[<0;1;M");
  CHECK(all.empty());
}

TEST_CASE("X10 mouse press decodes; payload bytes never leak as keys") {
  InputDecoder d;
  // CSI M, then cb=32 (' ' = left press), x=33+3 ('$'), y=33+5 ('&').
  auto all = decode_all(d, "\x1b[M $&");
  REQUIRE(all.size() == 1);
  const auto& m = std::get<MouseEvent>(all[0]);
  CHECK(m.kind == MK::Press);
  CHECK(m.button == MB::Left);
  CHECK(m.x == 3);
  CHECK(m.y == 5);

  // Release: cb=32+3 ('#') — X10 forgets the button.
  auto rel = decode_mouse(d, "\x1b[M#!!");
  REQUIRE(rel.size() == 1);
  CHECK(rel[0].kind == MK::Release);
  CHECK(rel[0].button == MB::None);
  CHECK(rel[0].x == 0);
  CHECK(rel[0].y == 0);
}

TEST_CASE("X10 mouse split across feeds waits for the full payload") {
  InputDecoder d;
  std::vector<Event> out;
  d.feed("\x1b[M", out);
  CHECK(out.empty());  // 3-byte payload not arrived.
  d.feed(" ", out);
  CHECK(out.empty());
  d.feed("$&", out);
  REQUIRE(out.size() == 1);
  CHECK(std::get<MouseEvent>(out[0]).x == 3);
}

TEST_CASE("keys interleave with mouse reports in stream order") {
  InputDecoder d;
  auto all = decode_all(d, "j\x1b[<0;2;2Mk");
  REQUIRE(all.size() == 3);
  CHECK(std::get<KeyEvent>(all[0]).codepoint == U'j');
  CHECK(std::get<MouseEvent>(all[1]).kind == MK::Press);
  CHECK(std::get<KeyEvent>(all[2]).codepoint == U'k');
}

// --- String sequences (OSC/DCS/APC/PM/SOS) are swallowed, never keys --------
// A terminal's replies to queries arrive on stdin shaped as string sequences;
// any that outlive the probe's read window land here. Decoded naively, the
// introducer becomes Escape (bound to "back" in the views) and the payload a
// burst of garbage keystrokes — the exact failure a real OSC-answering
// terminal showed while the CSI-only repliers stayed healthy.

TEST_CASE("OSC reply is swallowed whole: ST and BEL forms, keys resume after") {
  InputDecoder d;
  // The iTerm2 ReportCellSize reply shape (ST-terminated), then a real key.
  auto keys = decode(d, "\x1b]1337;ReportCellSize=17.50;8.00;2.0\x1b\\j");
  REQUIRE(keys.size() == 1);
  CHECK(keys[0].codepoint == U'j');
  // BEL-terminated OSC (a title report), then a key.
  keys = decode(d, "\x1b]0;some title\x07k");
  REQUIRE(keys.size() == 1);
  CHECK(keys[0].codepoint == U'k');
}

TEST_CASE("DCS and APC replies are swallowed: XTVERSION, kitty ack") {
  InputDecoder d;
  // XTVERSION reply (DCS > | name ST).
  CHECK(decode_all(d, "\x1bP>|iTerm2 3.5.14\x1b\\").empty());
  // kitty graphics ack (APC G ... ST).
  CHECK(decode_all(d, "\x1b_Gi=1;OK\x1b\\").empty());
  // The decoder is clean afterwards: a normal key decodes.
  auto keys = decode(d, "q");
  REQUIRE(keys.size() == 1);
  CHECK(keys[0].codepoint == U'q');
}

TEST_CASE("string sequence split across feeds waits, then swallows") {
  InputDecoder d;
  std::vector<Event> out;
  d.feed("\x1b]1337;Report", out);
  CHECK(out.empty());  // no terminator yet — held, not decoded as keys.
  d.feed("CellSize=17;8", out);
  CHECK(out.empty());
  // ST arrives split at the ESC boundary.
  d.feed("\x1b", out);
  CHECK(out.empty());
  d.feed("\\j", out);
  REQUIRE(out.size() == 1);
  CHECK(std::get<KeyEvent>(out[0]).codepoint == U'j');
}

TEST_CASE("unterminated string seq is dropped when a new escape begins") {
  InputDecoder d;
  // A broken OSC cut off by an arrow key: the arrow must survive.
  auto all = decode_all(d, "\x1b]0;half-a-title\x1b[B");
  REQUIRE(all.size() == 1);
  CHECK(std::get<KeyEvent>(all[0]).special == S::Down);
}

TEST_CASE("oversize unterminated string seq is dropped, decoder recovers") {
  InputDecoder d;
  std::vector<Event> out;
  d.feed("\x1b]", out);
  const std::string junk(5000, 'x');  // never a terminator.
  d.feed(junk, out);
  CHECK(out.empty());  // capped and dropped, not decoded as keys.
  d.feed("j", out);
  REQUIRE(out.size() == 1);
  CHECK(std::get<KeyEvent>(out[0]).codepoint == U'j');
}

TEST_CASE("lone Esc still resolves via flush_pending, not the string arm") {
  InputDecoder d;
  std::vector<Event> out;
  d.feed("\x1b", out);
  CHECK(out.empty());
  CHECK(d.esc_pending());
  CHECK(d.flush_pending(out));
  REQUIRE(out.size() == 1);
  CHECK(std::get<KeyEvent>(out[0]).special == S::Escape);
}
