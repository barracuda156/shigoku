// connect.cpp — ported from sabigoku tui/view/connect.rs (DESIGN 5.5a, P19).

#include "connect.hpp"

#include <algorithm>
#include <vector>

namespace shigoku::tui {

namespace {

// Preferred float size (DESIGN 5.5a), capped to the pane with a 2-col / 1-row
// margin. Fixed like zigoku's box so the panel size is stable across URL wrap.
constexpr int kBoxW = 68;
constexpr int kBoxH = 19;

// The URL band is capped to this many lines (zigoku parity); `c` copies the
// whole URL regardless, so a truncated band bounds the row plan losslessly.
constexpr std::size_t kMaxBandLines = 3;
// Below this width the float reads as clutter; draw one bare line instead.
// The height floor is the row plan itself: fall back before the box clips a
// hint.
constexpr int kMinCols = 28;
// The paste-hint fallback appears once the wait crosses this (DESIGN 5.5a).
constexpr std::uint64_t kPasteHintSecs = 10;
// Spinner escalates focus -> hot past this (§4.8 slow-path convention).
constexpr std::uint64_t kEscalateSecs = 3;

// Hard char-chunk wrap: a URL has no spaces to break on. Byte-based (URLs are
// ASCII), matching the Rust char-chunk wrap closely enough for this input
// class; str_width/put_str handle any incidental multibyte content safely.
std::vector<std::string> wrap(std::string_view s, int width) {
  std::vector<std::string> out;
  if (width <= 0) {
    out.emplace_back(s);
    return out;
  }
  const auto w = static_cast<std::size_t>(width);
  for (std::size_t i = 0; i < s.size(); i += w) {
    out.emplace_back(s.substr(i, w));
  }
  if (out.empty()) out.emplace_back(std::string{});
  return out;
}

// Center `line` on row `y` within `area`, matching views.cpp's centering
// idiom (put_str at (area.x + (w - lw)/2, y)).
void draw_centered(CellBuffer& buf, const Rect& area, int y, const std::string& line, Rgb fg,
                    Rgb bg, Style style = Style::None) {
  if (y < 0 || y >= area.h) return;
  const int lw = str_width(line);
  const int x = area.x + std::max(0, (area.w - lw) / 2);
  buf.put_str(x, area.y + y, line, fg, bg, style);
}

// The URL in its own surface inset band (DESIGN 5.5a): a real fallback
// action, not near-invisible dim text.
void draw_band(CellBuffer& buf, const Rect& area, int y, int band_w,
               const std::vector<std::string>& lines) {
  if (y >= area.h) return;
  const int x = area.x + std::max(0, (area.w - band_w) / 2);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const int row = y + static_cast<int>(i);
    if (row >= area.h) break;
    const Rect rect{x, area.y + row, std::min(band_w, area.w), 1};
    buf.fill(rect, theme::surface);
    buf.put_str(rect.x, rect.y, "  " + lines[i], theme::fg2, theme::surface);
  }
}

void draw_key_hint(CellBuffer& buf, const Rect& area, int y, std::string_view key,
                   std::string_view action, Rgb action_color) {
  // Build as one line so draw_centered can measure/center it as a unit
  // (views.cpp has no multi-span Line type; this hand-assembles the text and
  // colors each half with two put_str calls at the same centered x).
  const std::string full = std::string(key) + "  " + std::string(action);
  if (y < 0 || y >= area.h) return;
  const int lw = str_width(full);
  int x = area.x + std::max(0, (area.w - lw) / 2);
  x = buf.put_str(x, area.y + y, key, theme::focus, theme::elevated, Style::Bold);
  x = buf.put_str(x, area.y + y, "  ", theme::fg2, theme::elevated);
  buf.put_str(x, area.y + y, action, action_color, theme::elevated);
}

}  // namespace

void draw_connect(const ConnectView& view, const Rect& area, CellBuffer& buf) {
  if (area.w == 0 || area.h == 0) return;

  // Compact centered float, capped to the pane with a 2-col / 1-row margin.
  // Never a full-pane fill: Settings stays visible around the box, and the
  // box's elevated bg is the only overlay signal (DESIGN 5.5a, borderless).
  const int bw = std::min(kBoxW, std::max(0, area.w - 4));
  const int bh = std::min(kBoxH, std::max(0, area.h - 2));

  // Wrap and cap the URL band against the box width, then size the row plan.
  // band_h is capped, so `total` is bounded and the fit check below is exact.
  const int band_w = std::min(std::max(0, bw - 6), 72);
  const int inner_w = std::max(band_w - 4, 8);
  std::vector<std::string> wrapped = wrap(view.url, inner_w);
  if (wrapped.size() > kMaxBandLines) wrapped.resize(kMaxBandLines);
  const int band_h = static_cast<int>(wrapped.size());
  const int total = 13 + band_h;

  if (bw < kMinCols || bh < total) {
    // Too cramped to hold every row: a bare one-line hint on its own elevated
    // strip. Gating on `total` (not a fixed floor) is what keeps the centered
    // draw from silently clipping a key hint off the box bottom.
    const int row = area.h / 2;
    const Rect strip{area.x, area.y + row, area.w, 1};
    buf.fill(strip, theme::elevated);
    draw_centered(buf, area, row, "connect: esc to stop waiting", theme::fg2, theme::elevated);
    return;
  }

  const Rect modal{area.x + std::max(0, (area.w - bw) / 2), area.y + std::max(0, (area.h - bh) / 2),
                    bw, bh};

  buf.fill(modal, theme::elevated);

  const std::uint64_t secs = view.elapsed_secs;

  // Fixed row plan (offsets from the modal top); the paste slot is reserved
  // whether or not the hint shows, so the key hints never jump when it
  // appears.
  const int top = std::max(0, (modal.h - total) / 2);
  const auto at = [&](int offset) { return top + offset; };

  draw_centered(buf, modal, at(0), "Connect AniList", theme::hot, theme::elevated, Style::Bold);
  draw_centered(buf, modal, at(2), "approve access in your browser to continue", theme::fg2,
               theme::elevated);
  draw_centered(buf, modal, at(4), "browser didn't open? use this link:", theme::fg2,
               theme::elevated, Style::Italic);
  draw_band(buf, modal, at(5), band_w, wrapped);

  const char* spin = kSpinnerFrames[(view.elapsed_millis / 100) % static_cast<std::uint64_t>(kSpinnerFrameCount)];
  const Rgb spin_color = secs < kEscalateSecs ? theme::focus : theme::hot;
  {
    const std::string line = std::string(spin) + " waiting for approval\xE2\x80\xA6 " +
                              std::to_string(secs) + "s";
    const int y = at(6 + band_h);
    if (y >= 0 && y < modal.h) {
      const int lw = str_width(line);
      int x = modal.x + std::max(0, (modal.w - lw) / 2);
      x = buf.put_str(x, modal.y + y, std::string(spin) + " ", spin_color, theme::elevated);
      buf.put_str(x, modal.y + y,
                  "waiting for approval\xE2\x80\xA6 " + std::to_string(secs) + "s", theme::fg2,
                  theme::elevated);
    }
  }

  if (secs >= kPasteHintSecs) {
    const std::string line = "no callback? run  shigoku login --paste  in a terminal";
    const int y = at(8 + band_h);
    if (y >= 0 && y < modal.h) {
      const int lw = str_width(line);
      int x = modal.x + std::max(0, (modal.w - lw) / 2);
      x = buf.put_str(x, modal.y + y, "no callback? run  ", theme::warn, theme::elevated);
      x = buf.put_str(x, modal.y + y, "shigoku login --paste", theme::warn, theme::elevated,
                      Style::Bold);
      buf.put_str(x, modal.y + y, "  in a terminal", theme::warn, theme::elevated);
    }
  }

  const std::string copy_action = view.copied ? "copied \xE2\x9C\x93" : "copy link";
  const Rgb copy_color = view.copied ? theme::fg : theme::fg2;
  draw_key_hint(buf, modal, at(11 + band_h), "c", copy_action, copy_color);
  // "stop waiting", not "cancel": a callback already landing still completes;
  // esc only stops the wait (ROD-448 review).
  draw_key_hint(buf, modal, at(12 + band_h), "esc", "stop waiting", theme::fg2);
}

}  // namespace shigoku::tui
