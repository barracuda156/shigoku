// viewer.cpp — the ViewerBackend seam. Pure argv table + the blocking
// spawn/wait half (the player.cpp posix_spawnp shape, minus the watcher
// machinery — a reader has no positions to observe).

#include "viewer.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

extern char** environ;

namespace shigoku::manga {

ViewerKind parse_viewer_kind(std::string_view s) {
  if (s == "preview") return ViewerKind::PreviewApp;
  if (s == "custom") return ViewerKind::Custom;
  return ViewerKind::Builtin;  // incl. "builtin" and any typo (unknowns degrade).
}

std::vector<std::string> build_viewer_argv(ViewerKind kind,
                                           const std::string& custom_path,
                                           const std::string& builtin_bin,
                                           const std::string& dir,
                                           const std::vector<std::string>& pages,
                                           const ViewerOpts& opts) {
  // An unconfigured Custom degrades to Builtin (never an empty argv[0]).
  if (kind == ViewerKind::Custom && custom_path.empty()) {
    kind = ViewerKind::Builtin;
  }
  std::vector<std::string> argv;
  switch (kind) {
    case ViewerKind::Builtin: {
      argv.push_back(builtin_bin);
      if (opts.rtl) argv.push_back("--rtl");
      if (opts.start_page > 1) {
        argv.push_back("--start-page");
        argv.push_back(std::to_string(opts.start_page));
      }
      if (opts.report_file.has_value()) {
        argv.push_back("--report-file");
        argv.push_back(*opts.report_file);
      }
      if (opts.title.has_value()) {
        argv.push_back("--title");
        argv.push_back(*opts.title);
      }
      argv.push_back("--");  // the dir is data, never a flag.
      argv.push_back(dir);
      return argv;
    }
    case ViewerKind::PreviewApp: {
      // -W blocks `open` until Preview closes the documents — that block IS
      // the wait the read flow keys mark-read prompts off. No report surface.
      argv = {"open", "-W", "-a", "Preview"};
      argv.insert(argv.end(), pages.begin(), pages.end());
      return argv;
    }
    case ViewerKind::Custom: {
      argv.push_back(custom_path);
      argv.push_back(dir);
      return argv;
    }
  }
  return argv;  // unreachable; the switch is exhaustive.
}

std::string resolve_builtin_viewer(std::string_view argv0) {
  const std::size_t slash = argv0.rfind('/');
  if (slash == std::string_view::npos) return "shigoku-view";
  const std::string sibling =
      std::string(argv0.substr(0, slash)) + "/shigoku-view";
  if (::access(sibling.c_str(), X_OK) == 0) return sibling;
  return "shigoku-view";
}

std::optional<int> parse_report_line(std::string_view content) {
  constexpr std::string_view kPrefix = "LAST_PAGE=";
  if (content.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
  std::string_view rest = content.substr(kPrefix.size());
  int v = 0;
  std::size_t i = 0;
  while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') {
    if (v > 1000000) return std::nullopt;  // absurd — treat as garbage.
    v = v * 10 + (rest[i] - '0');
    ++i;
  }
  if (i == 0) return std::nullopt;                       // no digits.
  if (i < rest.size() && rest[i] != '\n') return std::nullopt;  // trailing junk.
  if (v < 1) return std::nullopt;
  return v;
}

std::optional<int> parse_report(const std::string& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  std::stringstream buf;
  buf << f.rdbuf();
  return parse_report_line(buf.str());
}

Result<int, std::string> run_viewer(const std::vector<std::string>& argv) {
  if (argv.empty()) return err(std::string("empty viewer argv"));

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  // Null stdio: the viewer is a GUI window; the terminal belongs to the TUI.
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  pid_t child = -1;
  const int rc = ::posix_spawnp(&child, argv[0].c_str(), &fa, nullptr,
                                cargv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    // rc IS the errno (posix_spawnp returns it, the player.cpp debt-#5 rule).
    if (rc == ENOENT) {
      return err("viewer not found: " + argv[0]);
    }
    return err(argv[0] + ": " + std::strerror(rc));
  }

  int status = 0;
  for (;;) {
    const pid_t w = ::waitpid(child, &status, 0);
    if (w == child) break;
    if (w < 0 && errno == EINTR) continue;
    return err(std::string("waitpid: ") + std::strerror(errno));
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return err(std::string("viewer killed by signal"));
}

}  // namespace shigoku::manga
