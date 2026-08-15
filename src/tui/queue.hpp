// queue.hpp — the one event queue (A1). std::deque<E> + mutex + condvar.
// The UI thread waits with wait_for(100ms); a timeout IS the Tick that
// drives spinner / search debounce / cover cooldowns. Workers post owned events
// via try_post, which NEVER blocks — a full queue at quit must not strand a
// worker forever (A1). No draining machinery in v0.
//
// Templated over the event type: the manga app runs its own MgEvent variant
// through BasicEventQueue<MgEvent> — it must NOT grow the shared Event
// variant, because A2's no-catch-all law would force manga arms into every
// anime visitor (an MF-1 violation by construction). The EventQueue alias
// keeps the anime side byte-identical.

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "../event.hpp"

namespace shigoku::tui {

template <class E>
class BasicEventQueue {
 public:
  // ~100ms tick period (A1).
  static constexpr std::chrono::milliseconds kTickPeriod{100};
  // Soft cap so a runaway worker can't grow the queue without bound; posts past
  // it are dropped (try_post returns false). Generously above any real burst.
  static constexpr std::size_t kMaxDepth = 1024;

  // Post an owned event. Returns false (dropping the event) if the queue is at
  // capacity or shut down — the caller (a worker) must tolerate the drop rather
  // than block. Wakes the UI thread.
  bool try_post(E ev) {
    {
      std::lock_guard<std::mutex> lk(m_);
      if (shutdown_ || q_.size() >= kMaxDepth) return false;
      q_.push_back(std::move(ev));
    }
    cv_.notify_one();
    return true;
  }

  // Block until an event is available or `kTickPeriod` elapses. Returns the
  // next event, or nullopt on timeout (the caller synthesizes a Tick). Also
  // returns nullopt immediately if shut down and drained.
  std::optional<E> wait_next() {
    std::unique_lock<std::mutex> lk(m_);
    if (q_.empty()) {
      cv_.wait_for(lk, kTickPeriod, [&] { return !q_.empty() || shutdown_; });
    }
    if (q_.empty()) return std::nullopt;  // timeout -> Tick, or shutdown.
    E ev = std::move(q_.front());
    q_.pop_front();
    return ev;
  }

  // Mark shut down: further try_post is refused. Existing entries stay drainable
  // (the UI thread stops looping on its own quit path; A1 does _exit anyway).
  void shutdown() {
    {
      std::lock_guard<std::mutex> lk(m_);
      shutdown_ = true;
    }
    cv_.notify_all();
  }

  [[nodiscard]] bool is_shutdown() const {
    std::lock_guard<std::mutex> lk(m_);
    return shutdown_;
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<E> q_;
  bool shutdown_ = false;
};

// The anime app's queue, exactly as before the template rename.
using EventQueue = BasicEventQueue<Event>;

}  // namespace shigoku::tui
