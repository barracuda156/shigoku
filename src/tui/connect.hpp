// connect.hpp — AniList connect modal render (P19, DESIGN 5.5a). Ported 1:1
// from sabigoku tui/view/connect.rs: a captured overlay drawn last as a
// compact centered elevated float over Settings, never a full-pane fill.
// Pure render (no I/O) — the connect session's live state (elapsed time,
// url, copied flag) lives in app.hpp's App::connect; this header owns only
// the draw function + its render-input struct, mirroring settings.hpp/
// views.cpp's split.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "cells.hpp"

namespace shigoku::tui {

// Render inputs, owned app-side as the connect session.
struct ConnectView {
  std::string_view url;
  std::uint64_t elapsed_secs = 0;
  std::uint64_t elapsed_millis = 0;  // for the spinner phase; wraps fine at u64.
  bool copied = false;
};

void draw_connect(const ConnectView& view, const Rect& area, CellBuffer& buf);

}  // namespace shigoku::tui
