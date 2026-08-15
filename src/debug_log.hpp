// debug_log.hpp — env-gated diagnostic log for the hardware checkpoints
// (HW-2). Off unless SHIGOKU_DEBUG_LOG=<path> is set at launch; then each call
// appends one timestamped line (steady-clock ms since first call + a short
// thread tag) to that file. Append-open per call: no held fd, no buffering,
// worker threads interleave whole lines via O_APPEND. Diagnostic scaffolding,
// not a product surface — call sites are the covers pipeline and the play/quit
// chain, two surfaces that once failed on real hardware with nothing observable.

#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace shigoku {

// Render bytes with non-printables escaped as \xNN — for logging terminal
// wire traffic (probe replies, swallowed sequences) legibly.
inline std::string debug_escape(std::string_view s) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const auto b = static_cast<unsigned char>(c);
    if (b >= 0x20 && b < 0x7f) {
      out += static_cast<char>(b);
    } else {
      out += "\\x";
      out += hex[b >> 4];
      out += hex[b & 0xf];
    }
  }
  return out;
}

inline const char* debug_log_path() {
  static const char* p = std::getenv("SHIGOKU_DEBUG_LOG");
  return p;
}

inline bool debug_log_enabled() { return debug_log_path() != nullptr; }

inline void debug_log(const std::string& msg) {
  const char* path = debug_log_path();
  if (path == nullptr) return;
  using clock = std::chrono::steady_clock;
  static const clock::time_point t0 = clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock::now() - t0)
                      .count();
  // Short per-thread tag so UI-thread lines separate from worker lines.
  const unsigned tid = static_cast<unsigned>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()) % 10000u);
  std::string line = std::to_string(ms);
  line += "ms t";
  line += std::to_string(tid);
  line += " ";
  line += msg;
  line += "\n";
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) return;
  (void)::write(fd, line.data(), line.size());
  ::close(fd);
}

}  // namespace shigoku
