// picker.cpp — see picker.hpp.

#include "picker.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
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

std::optional<std::size_t> fzf_pick(const std::string& binary, std::string_view prompt,
                                    const std::vector<std::string>& rows) {
  if (rows.empty()) return std::nullopt;

  int in[2] = {-1, -1};
  int out[2] = {-1, -1};
  if (::pipe(in) != 0 || ::pipe(out) != 0) {
    for (const int fd : {in[0], in[1], out[0], out[1]}) {
      if (fd >= 0) ::close(fd);
    }
    return std::nullopt;
  }
  for (const int fd : {in[0], in[1], out[0], out[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

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
    debug_log(std::string("picker: spawn ") + binary + " failed: " + std::strerror(rc));
    ::close(in[1]);
    ::close(out[0]);
    return std::nullopt;
  }

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
    if (errno != EINTR) return std::nullopt;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    debug_log("picker: " + binary + " exited " +
              (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : std::string("by signal")));
    return std::nullopt;
  }
  const auto nl = answer.find('\n');
  return parse_selection(nl == std::string::npos ? answer : answer.substr(0, nl), rows.size());
}

}  // namespace shigoku::picker
