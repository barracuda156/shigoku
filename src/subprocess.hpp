// subprocess.hpp — run a helper program to completion: argv in, its stdin fed
// from a buffer, its stdout kept, its stderr dropped, the whole run under a
// wall-clock bound, and the child always reaped.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "result.hpp"

namespace shigoku::subprocess {

struct Failure {
  enum class Kind {
    NotFound,  // argv[0] is not on PATH: the runtime dependency is missing.
    Spawn,     // pipes or the spawn failed for another reason.
    Timeout,   // the child outlived the bound and was killed.
    Io,        // a pipe read failed, or stdout passed the cap.
  };
  Kind kind;
  std::string detail;
};

struct Output {
  int exit_code = -1;  // the child's exit status, or -1 when a signal ended it.
  std::string out;     // everything the child wrote to stdout.
};

// PATH-search argv[0], hand `input` to its stdin (closed once written), and
// collect stdout until the child closes it and exits. `timeout_ms` bounds the
// whole run: past it the child gets SIGKILL and the call fails with Timeout.
// stdout beyond `max_out` bytes is Io. A non-zero exit is not a failure here
// (the caller reads exit_code), so a helper can report through its output.
// A child that exits without reading its input is fine: the unread rest is
// dropped, never a signal.
[[nodiscard]] Result<Output, Failure> run(const std::vector<std::string>& argv,
                                          std::string_view input, int timeout_ms,
                                          std::size_t max_out = std::size_t{4} << 20);

}  // namespace shigoku::subprocess
