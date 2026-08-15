// connect_tests.cpp — P19 connect modal render tests.
//
// Ports the sabigoku tui/view/connect.rs #[test] cases 1:1 (§8: golden tests
// are the port contract). No ratatui TestBackend equivalent exists here, so
// these render into a real CellBuffer and scan its glyphs directly.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "../src/tui/cells.hpp"
#include "../src/tui/connect.hpp"

using namespace shigoku::tui;

namespace {

// The real authorize URL shape (client id + 128-bit hex state): the long
// input that made the pre-cap row plan overflow small boxes.
constexpr const char* kUrl =
    "https://anilist.co/api/v2/oauth/authorize?client_id=46529&response_type=token&"
    "state=9f2c7a1b4e6d0f3a5c8b1d2e4f6a7b9c";

// Encode a codepoint as UTF-8, appended to `out`.
void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::vector<std::string> render_rows(int w, int h, std::uint64_t elapsed_secs) {
  ConnectView view{kUrl, elapsed_secs, elapsed_secs * 1000, false};
  CellBuffer buf(w, h);
  buf.clear(theme::bg);
  draw_connect(view, Rect{0, 0, w, h}, buf);

  std::vector<std::string> rows;
  rows.reserve(static_cast<std::size_t>(h));
  for (int y = 0; y < h; ++y) {
    std::string row;
    for (int x = 0; x < w; ++x) {
      const Cell& c = buf.at(x, y);
      if (c.continuation) continue;
      append_utf8(row, c.glyph);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string joined(const std::vector<std::string>& rows) {
  std::string out;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i != 0) out += '\n';
    out += rows[i];
  }
  return out;
}

bool all_blank(const std::string& row) {
  for (char c : row) {
    if (c != ' ') return false;
  }
  return true;
}

}  // namespace

// The exit affordance always renders, and when the full float draws, the
// bottom of the row plan draws with it. Elapsed past the paste-hint
// threshold so the tallest row plan is in play. This is the guard the
// pre-cap plan failed: draw_centered clips off-box rows without a trace.
TEST_CASE("float never clips its key hints") {
  for (int w = 32; w <= 120; ++w) {
    for (int h : {12, 14, 16, 20, 30, 45}) {
      const std::vector<std::string> rows = render_rows(w, h, 25);
      const std::string screen = joined(rows);
      INFO("w=" << w << " h=" << h);
      // Reachable via the modal esc hint or the cramped fallback.
      CHECK(screen.find("stop waiting") != std::string::npos);
      // Title present means the full float drew; the status and copy hint
      // below it must have drawn too (no silent mid-plan clip).
      if (screen.find("Connect AniList") != std::string::npos) {
        CHECK(screen.find("waiting for approval") != std::string::npos);
        CHECK(screen.find("copy link") != std::string::npos);
      }
    }
  }
}

// A float, not a takeover: on a roomy pane the box floats clear of every
// edge, so Settings shows around it (DESIGN 5.5a, the ticket's whole point).
TEST_CASE("float is contained not full bleed") {
  const std::vector<std::string> rows = render_rows(100, 34, 1);
  bool has_title = false;
  for (const auto& r : rows) {
    if (r.find("Connect AniList") != std::string::npos) has_title = true;
  }
  CHECK(has_title);
  CHECK(all_blank(rows.front()));
  CHECK(all_blank(rows.back()));
  for (const auto& r : rows) {
    REQUIRE(!r.empty());
    CHECK(r.front() == ' ');
    CHECK(r.back() == ' ');
  }
}
