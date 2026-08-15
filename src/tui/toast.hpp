// toast.hpp — toast queue (P6, DESIGN §4.7 / 04 §8).
//
// Cap 3 visible; non-persistent toasts auto-dismiss after 2.5s and are evicted
// oldest-first + compacted when a fourth arrives; persistent toasts are the
// source-unreachable variant (per-topic singleton, refreshed in place, cleared
// on recovery). Copy budget is 36 display columns (the box is 40, the 4-column
// "[x] " glyph prefix is fixed) — truncation is width-aware on a codepoint
// boundary with a trailing "…". MAX_BOX_COLS / GLYPH_COLS / MAX_COPY_COLS are
// the single source of truth (DESIGN §4.7).

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "cells.hpp"
#include "theme.hpp"

namespace shigoku::tui {

// Time is measured in ~100ms ticks (04 §8) so the queue is clock-injectable and
// unit-testable without wall time. 2.5s auto-dismiss = 25 ticks.
using TickCount = std::uint64_t;

enum class ToastKind {
  Info,     // "[~]" muted
  Success,  // "[✓]" success + bold
  Error,    // "[!]" now + bold
  Warning,  // "[!]" warn
};

inline constexpr int kToastMaxBoxCols = 40;
inline constexpr int kToastGlyphCols = 4;  // "[x] "
inline constexpr int kToastMaxCopyCols = kToastMaxBoxCols - kToastGlyphCols;  // 36
inline constexpr int kToastMaxVisible = 3;
inline constexpr TickCount kToastLifetimeTicks = 25;  // 2.5s at 100ms ticks.

struct Toast {
  ToastKind kind = ToastKind::Info;
  std::string text;       // raw copy; truncated at render time to the budget.
  bool persistent = false;
  std::string topic;      // persistent singleton key ("" for non-persistent).
  TickCount born = 0;     // tick when pushed (auto-dismiss references this).
};

// The 4-column glyph prefix for a kind (DESIGN §4.7). "[✓]" is multi-byte but 4
// display columns (the check is width-1 + "[","]"," ").
[[nodiscard]] std::string_view toast_glyph(ToastKind k);

// Foreground color for a kind (DESIGN §4.7 table).
[[nodiscard]] Rgb toast_fg(ToastKind k);

// Whether a kind renders bold (Success / Error).
[[nodiscard]] bool toast_bold(ToastKind k);

class ToastQueue {
 public:
  // Push a transient toast (auto-dismisses after kToastLifetimeTicks). If at
  // cap, the oldest non-persistent toast is evicted first (04 §8).
  void push(ToastKind kind, std::string text, TickCount now);

  // Push / refresh a persistent per-topic singleton. If a toast with the same
  // topic exists it is updated in place (born reset); otherwise inserted. Never
  // auto-dismisses; call clear_topic() on recovery (§8.5).
  void push_persistent(ToastKind kind, std::string topic, std::string text,
                       TickCount now);

  // Clear a persistent toast by topic (source recovered). No-op if absent.
  void clear_topic(std::string_view topic);

  // Expire non-persistent toasts whose age >= lifetime, called on each Tick.
  void tick(TickCount now);

  // The visible toasts, newest last (render stacks upward: newest nearest the
  // bottom bar). At most kToastMaxVisible.
  [[nodiscard]] std::vector<Toast> visible() const;

  [[nodiscard]] std::size_t size() const { return items_.size(); }
  [[nodiscard]] bool empty() const { return items_.empty(); }

 private:
  void enforce_cap();
  std::deque<Toast> items_;  // insertion order; front = oldest.
};

}  // namespace shigoku::tui
