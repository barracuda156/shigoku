// picker.cpp — see picker.hpp.

#include "picker.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "debug_log.hpp"

extern char** environ;

namespace shigoku::picker {

Mode parse_mode(std::string_view s) {
  if (s == "prompt") return Mode::Prompt;
  if (s == "fzf") return Mode::Fzf;
  return Mode::Auto;
}

std::vector<std::string> numbered(const std::vector<std::string>& rows) {
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    std::string line = std::to_string(i + 1) + ". ";
    // One row per line by construction: a newline inside a title would
    // split it into two picker entries the number could not tell apart.
    for (const char c : rows[i]) line.push_back((c == '\n' || c == '\r') ? ' ' : c);
    out.push_back(std::move(line));
  }
  return out;
}

std::optional<std::size_t> parse_selection(std::string_view line, std::size_t max) {
  std::size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  std::size_t n = 0;
  std::size_t digits = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
    if (n > max) return std::nullopt;  // never overflows: capped at max + 1.
    n = n * 10 + static_cast<std::size_t>(line[i] - '0');
    ++i;
    ++digits;
  }
  if (digits == 0 || i >= line.size() || line[i] != '.') return std::nullopt;
  if (n < 1 || n > max) return std::nullopt;
  return n - 1;
}

namespace {

bool executable(const std::string& path) { return ::access(path.c_str(), X_OK) == 0; }

std::optional<std::string> search_path(std::string_view name) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) return std::nullopt;
  std::string_view rest(path);
  while (!rest.empty()) {
    const auto colon = rest.find(':');
    const std::string_view dir = rest.substr(0, colon);
    rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
    if (dir.empty()) continue;
    std::string candidate(dir);
    candidate += '/';
    candidate += name;
    if (executable(candidate)) return candidate;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> find_binary(std::string_view override_path) {
  if (!override_path.empty()) {
    if (override_path.find('/') != std::string_view::npos) {
      const std::string p(override_path);
      return executable(p) ? std::optional<std::string>(p) : std::nullopt;
    }
    return search_path(override_path);
  }
  return search_path("fzf");
}

Choice choose(Mode mode, std::string_view picker_path, bool at_terminal) {
  Choice c;
  if (mode == Mode::Prompt) {
    c.why = "cli_picker = prompt";
    return c;
  }
  auto binary = find_binary(picker_path);
  if (!binary.has_value()) {
    c.binary_missing = true;
    if (picker_path.empty()) {
      c.why = "no fzf on PATH";
    } else if (picker_path.find('/') == std::string_view::npos) {
      c.why = "no " + std::string(picker_path) + " on PATH";
    } else {
      c.why = std::string(picker_path) + " isn't there or isn't executable";
    }
    return c;
  }
  // `auto` keeps the prompt for a piped or scripted run; `fzf` spawns
  // regardless (the picker draws on /dev/tty, not stdin/stdout).
  if (mode == Mode::Auto && !at_terminal) {
    c.why = "stdin/stdout not a terminal";
    return c;
  }
  c.binary = std::move(*binary);
  return c;
}

namespace {

Pick failed(std::string detail) {
  Pick p;
  p.kind = Pick::Kind::Failed;
  p.detail = std::move(detail);
  return p;
}

}  // namespace

Pick fzf_pick(const std::string& binary, std::string_view prompt,
              const std::vector<std::string>& rows) {
  if (rows.empty()) return Pick{};

  int in[2] = {-1, -1};
  int out[2] = {-1, -1};
  if (::pipe(in) != 0 || ::pipe(out) != 0) {
    const int e = errno;
    for (const int fd : {in[0], in[1], out[0], out[1]}) {
      if (fd >= 0) ::close(fd);
    }
    return failed(std::string("couldn't open a pipe to the picker: ") + std::strerror(e));
  }
  for (const int fd : {in[0], in[1], out[0], out[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

  // The picker takes the terminal (raw mode) and gives it back on its own
  // exit; a crash mid-screen leaves it raw, so the line discipline is
  // snapshotted here and put back on a failure — the prompt that follows
  // must be typeable. Absent when stdin is a pipe (a forced `fzf` run).
  struct termios tty_before{};
  const bool tty_saved = ::tcgetattr(STDIN_FILENO, &tty_before) == 0;
  const auto fail_restoring = [&](std::string detail) {
    if (tty_saved) ::tcsetattr(STDIN_FILENO, TCSANOW, &tty_before);
    return failed(std::move(detail));
  };

  // A prompt is our own literal (never provider bytes); the rows are
  // scrubbed upstream. Inline below the list rather than full-screen where
  // the terminal allows it (--height is honoured by fzf and fzf++ alike).
  std::string prompt_arg = "--prompt=";
  prompt_arg += prompt;
  prompt_arg += "> ";
  const std::string args[] = {prompt_arg, "--height=40%", "--reverse", "--no-multi", "--cycle"};
  std::vector<char*> cargv;
  cargv.push_back(const_cast<char*>(binary.c_str()));
  for (const std::string& a : args) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
  pid_t child = -1;
  const int rc = ::posix_spawnp(&child, binary.c_str(), &fa, nullptr, cargv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(in[0]);
  ::close(out[1]);
  if (rc != 0) {
    ::close(in[1]);
    ::close(out[0]);
    const std::string detail = "couldn't start " + binary + ": " + std::strerror(rc);
    debug_log("picker: " + detail);
    return failed(detail);
  }
  debug_log("picker: spawned " + binary + " for " + std::to_string(rows.size()) + " row(s)");

  // The picker may exit before it has read every row (an abort on the first
  // keystroke): a write to a closed pipe must not kill us.
  struct sigaction ignore{};
  struct sigaction saved{};
  ignore.sa_handler = SIG_IGN;
  ::sigaction(SIGPIPE, &ignore, &saved);
  std::string feed;
  for (const std::string& line : numbered(rows)) {
    feed += line;
    feed.push_back('\n');
  }
  std::size_t off = 0;
  while (off < feed.size()) {
    const ssize_t n = ::write(in[1], feed.data() + off, feed.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    off += static_cast<std::size_t>(n);
  }
  ::close(in[1]);
  ::sigaction(SIGPIPE, &saved, nullptr);

  std::string answer;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(out[0], buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    answer.append(buf, static_cast<std::size_t>(n));
  }
  ::close(out[0]);

  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return fail_restoring("couldn't wait for " + binary + ": " + std::strerror(errno));
    }
  }
  if (WIFSIGNALED(status)) {
    const std::string detail = binary + " died (signal " + std::to_string(WTERMSIG(status)) + ")";
    debug_log("picker: " + detail);
    return fail_restoring(detail);
  }
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (code == 1 || code == 130) {
    debug_log("picker: " + binary + " exited " + std::to_string(code) + " (declined)");
    return Pick{};
  }
  if (code != 0) {
    const std::string detail = binary + " failed (exit " + std::to_string(code) + ")";
    debug_log("picker: " + detail);
    return fail_restoring(detail);
  }
  const auto nl = answer.find('\n');
  const auto index =
      parse_selection(nl == std::string::npos ? answer : answer.substr(0, nl), rows.size());
  if (!index.has_value()) {
    const std::string detail = "couldn't read " + binary + "'s answer";
    debug_log("picker: " + detail + ": " + debug_escape(answer.substr(0, 80)));
    return fail_restoring(detail);
  }
  Pick p;
  p.kind = Pick::Kind::Picked;
  p.index = *index;
  return p;
}

}  // namespace shigoku::picker
