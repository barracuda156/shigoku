// workers.hpp — worker spawn + staleness generations (P6, A1 / 04 §6).
//
// Workers are DETACHED std::threads that own their inputs (moved in), post
// OWNED events via EventQueue::try_post, and never touch App. Supersede =
// drop-by-generation, NEVER join on the hot path (04 §6 / §13 "Join on
// supersede: never").
//
// Staleness: each subsystem holds a Generation counter on App; the worker
// captures its value at spawn and stamps it into the result event; tick() drops
// events whose generation != the current one. A monotonically-bumped counter is
// the "cheap" mechanism the bible prefers for episode/search over string
// compare (04 §6 Rust lean).

#pragma once

#include <atomic>
#include <thread>
#include <utility>

#include "../event.hpp"

namespace shigoku::tui {

// A per-subsystem generation token. bump() invalidates all in-flight workers of
// that subsystem; current() is captured at spawn and compared in tick().
class GenCounter {
 public:
  Generation bump() { return value_.fetch_add(1, std::memory_order_relaxed) + 1; }
  [[nodiscard]] Generation current() const {
    return value_.load(std::memory_order_relaxed);
  }
  // True if `stamped` is still the live generation (event is not stale).
  [[nodiscard]] bool is_current(Generation stamped) const {
    return stamped == current();
  }

 private:
  std::atomic<Generation> value_{0};
};

// Spawn a detached worker. `fn` is any callable taking no args (capture the
// moved-in inputs). It runs off the UI thread and posts its result via the
// queue itself; this helper only owns the detach. Never joined (A1).
template <class Fn>
void spawn_detached(Fn&& fn) {
  std::thread(std::forward<Fn>(fn)).detach();
}

}  // namespace shigoku::tui
