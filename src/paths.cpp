// paths.cpp — platform dirs (P7). Ported from sabigoku src/paths.rs (06 §1).

#include "paths.hpp"

#include <sys/stat.h>

#include <cstdlib>

namespace shigoku {

namespace {

std::optional<std::string> env_var(const char* k) {
  const char* v = std::getenv(k);
  if (v == nullptr || v[0] == '\0') return std::nullopt;
  return std::string(v);
}

// mkdir -p: create every path component that doesn't exist yet. Best-effort
// (paths.rs: "real failure surfaces on open") — errors here are swallowed.
void mkdir_p(const std::string& dir) {
  std::string prefix;
  std::size_t pos = 0;
  while (pos < dir.size()) {
    const std::size_t next = dir.find('/', pos + 1);
    prefix = (next == std::string::npos) ? dir : dir.substr(0, next);
    if (!prefix.empty()) ::mkdir(prefix.c_str(), 0755);
    if (next == std::string::npos) break;
    pos = next;
  }
}

}  // namespace

std::optional<Paths> resolve_paths(const char* segment) {
  const std::optional<std::string> home = env_var("HOME");

  auto base = [&](const char* xdg, const char* home_suffix) -> std::optional<std::string> {
    if (auto dir = env_var(xdg); dir.has_value()) {
      return *dir + "/" + segment;
    }
    if (!home.has_value()) return std::nullopt;
    return *home + "/" + home_suffix + "/" + segment;
  };

  const auto config = base("XDG_CONFIG_HOME", ".config");
  const auto data = base("XDG_DATA_HOME", ".local/share");
  const auto cache = base("XDG_CACHE_HOME", ".cache");
  if (!config.has_value() || !data.has_value() || !cache.has_value()) {
    return std::nullopt;  // paths.rs Error::NoHome.
  }

  std::string runtime;
  if (auto dir = env_var("XDG_RUNTIME_DIR"); dir.has_value()) {
    runtime = *dir + "/" + segment;
  } else {
    runtime = std::string("/tmp/") + segment;
  }

  return Paths{*config, *data, *cache, runtime};
}

void ensure_dirs(const Paths& p) {
  for (const std::string* dir : {&p.config, &p.data, &p.cache, &p.runtime}) {
    mkdir_p(*dir);
  }
  // The mpv IPC socket lives under runtime; lock it to the owner rather than
  // trust the ambient umask (paths.rs ensure_dirs).
  ::chmod(p.runtime.c_str(), 0700);
}

std::string config_file_path(const Paths& p) { return p.config + "/config.json"; }

std::string auth_file_path(const Paths& p) { return p.config + "/auth.json"; }

}  // namespace shigoku
