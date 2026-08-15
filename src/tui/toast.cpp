// toast.cpp — toast queue behavior (P6, DESIGN §4.7 / 04 §8).

#include "toast.hpp"

#include <algorithm>

namespace shigoku::tui {

std::string_view toast_glyph(ToastKind k) {
  switch (k) {
    case ToastKind::Info:    return "[~] ";
    case ToastKind::Success: return "[\xE2\x9C\x93] ";  // [✓]
    case ToastKind::Error:   return "[!] ";
    case ToastKind::Warning: return "[!] ";
  }
  return "[?] ";  // unreachable (closed enum).
}

Rgb toast_fg(ToastKind k) {
  switch (k) {
    case ToastKind::Info:    return theme::fg2;
    case ToastKind::Success: return theme::success;
    case ToastKind::Error:   return theme::error;
    case ToastKind::Warning: return theme::warn;
  }
  return theme::fg;  // unreachable.
}

bool toast_bold(ToastKind k) {
  switch (k) {
    case ToastKind::Success:
    case ToastKind::Error:
      return true;
    case ToastKind::Info:
    case ToastKind::Warning:
      return false;
  }
  return false;  // unreachable.
}

void ToastQueue::push(ToastKind kind, std::string text, TickCount now) {
  Toast t;
  t.kind = kind;
  t.text = std::move(text);
  t.persistent = false;
  t.born = now;
  items_.push_back(std::move(t));
  enforce_cap();
}

void ToastQueue::push_persistent(ToastKind kind, std::string topic,
                                 std::string text, TickCount now) {
  for (Toast& t : items_) {
    if (t.persistent && t.topic == topic) {
      t.kind = kind;
      t.text = std::move(text);
      t.born = now;  // refreshed in place.
      return;
    }
  }
  Toast t;
  t.kind = kind;
  t.text = std::move(text);
  t.persistent = true;
  t.topic = std::move(topic);
  t.born = now;
  items_.push_back(std::move(t));
  enforce_cap();
}

void ToastQueue::clear_topic(std::string_view topic) {
  items_.erase(std::remove_if(items_.begin(), items_.end(),
                              [&](const Toast& t) {
                                return t.persistent && t.topic == topic;
                              }),
               items_.end());
}

void ToastQueue::tick(TickCount now) {
  items_.erase(std::remove_if(items_.begin(), items_.end(),
                              [&](const Toast& t) {
                                if (t.persistent) return false;
                                return now - t.born >= kToastLifetimeTicks;
                              }),
               items_.end());
}

void ToastQueue::enforce_cap() {
  // Evict oldest NON-persistent first; only if none remain do persistent ones
  // yield (persistent toasts are the important source-unreachable variant).
  while (items_.size() > static_cast<std::size_t>(kToastMaxVisible)) {
    auto it = std::find_if(items_.begin(), items_.end(),
                           [](const Toast& t) { return !t.persistent; });
    if (it == items_.end()) it = items_.begin();  // all persistent: drop oldest.
    items_.erase(it);
  }
}

std::vector<Toast> ToastQueue::visible() const {
  // items_ already respects the cap; newest last is the render order.
  return std::vector<Toast>(items_.begin(), items_.end());
}

}  // namespace shigoku::tui
