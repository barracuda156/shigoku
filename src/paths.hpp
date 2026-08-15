// paths.hpp — platform dirs. Ported from sabigoku src/paths.rs: Linux XDG
// only. Only `runtime` (mpv IPC sockets, PlayOpts::socket_dir) is used right
// now; config/data/cache are carried for the config-lite phases that will
// actually read/write them (store, config.toml) so the type doesn't need
// reshaping later — resolve() fills all four now, same as the Rust
// reference, even though only runtime is consumed in v0.

#pragma once

#include <optional>
#include <string>

namespace shigoku {

inline constexpr const char* kAppSegment = "shigoku";

struct Paths {
  std::string config;   // config.toml (unused until config-lite lands)
  std::string data;      // shigoku.db (not yet used)
  std::string cache;     // covers, update-check cache (not yet used)
  std::string runtime;   // mpv IPC sockets — the one v0 actually uses.
};

// XDG var wins when set and non-empty, else the HOME-based default. Runtime
// never needs HOME ($XDG_RUNTIME_DIR/shigoku or /tmp/shigoku). nullopt = no
// HOME and no XDG override for config/data/cache (paths.rs Error::NoHome).
// `segment` is the app dir name under each base: the manga binary passes
// "shigoku-manga" so the two apps can never touch each other's state; the
// default keeps every existing caller byte-identical (MF-1).
[[nodiscard]] std::optional<Paths> resolve_paths(const char* segment = kAppSegment);

// mkdir -p on all four dirs (best-effort; real failures surface on open) and
// 0700 the runtime dir (the mpv socket lives there; on the /tmp fallback with
// no XDG_RUNTIME_DIR that dir is otherwise world-visible).
void ensure_dirs(const Paths& p);

// {config}/config.json. Ported from sabigoku's config_file() as a free
// function, since Paths has no methods elsewhere in this codebase.
[[nodiscard]] std::string config_file_path(const Paths& p);

// {config}/auth.json. Ported from sabigoku's auth_file() the same way.
// Deliberately its own file, never folded into config.json — the bearer
// must not ride Settings round-trips.
[[nodiscard]] std::string auth_file_path(const Paths& p);

}  // namespace shigoku
