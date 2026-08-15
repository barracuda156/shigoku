// viewer.hpp — the ViewerBackend seam: how the manga app turns a fetched
// chapter dir into an open reader window.
//
// Three kinds (the same shape as the anime player-backend config row):
//   Builtin    — shigoku-view, resolved argv[0]-sibling first then PATH; gets
//                the full flag surface (--start-page / --report-file / --rtl /
//                --title) so resume + mark-read work.
//   PreviewApp — `open -W -a Preview <files…>` (macOS; -W blocks until the
//                viewer closes). No report file: exit means "done", the app
//                falls back to page 1 semantics.
//   Custom     — the configured path verbatim + the chapter dir as the one
//                positional. No report either (a foreign viewer's exit code
//                tells us nothing about pages).
//
// The argv builder is pure (golden tests); run_viewer is the blocking
// spawn/wait half (posix_spawnp + waitpid, null stdio — the player.cpp
// shape minus IPC), called from a detached worker (A1).

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../result.hpp"

namespace shigoku::manga {

enum class ViewerKind { Builtin, PreviewApp, Custom };

// Config-string mapping ("builtin" / "preview" / "custom"); anything else —
// typo'd config included — degrades to Builtin, never to a failure.
[[nodiscard]] ViewerKind parse_viewer_kind(std::string_view s);

struct ViewerOpts {
  bool rtl = false;
  int start_page = 1;  // 1-based (the shigoku-view CLI contract).
  std::optional<std::string> report_file;
  std::optional<std::string> title;
};

// The pure argv table. `builtin_bin` is the resolved shigoku-view spelling
// (resolve_builtin_viewer below, or bare "shigoku-view" for PATH lookup);
// `pages` is the sorted page list PreviewApp needs (`open` opens files, not
// dirs); Builtin/Custom take `dir` as the positional. Custom with an empty
// custom_path degrades to Builtin (an unconfigured path row must not spawn
// an empty argv).
[[nodiscard]] std::vector<std::string> build_viewer_argv(
    ViewerKind kind, const std::string& custom_path,
    const std::string& builtin_bin, const std::string& dir,
    const std::vector<std::string>& pages, const ViewerOpts& opts);

// argv[0]-sibling resolution: "<dir of argv0>/shigoku-view" when that exists
// and is executable, else the bare name (posix_spawnp walks PATH). An argv0
// with no '/' (launched via PATH itself) goes straight to the bare name.
[[nodiscard]] std::string resolve_builtin_viewer(std::string_view argv0);

// "LAST_PAGE=<N>" (shigoku-view's report, 1-based) → N. Missing file, junk,
// N < 1 → nullopt (the caller treats it as "no report").
[[nodiscard]] std::optional<int> parse_report_line(std::string_view content);
[[nodiscard]] std::optional<int> parse_report(const std::string& path);

// Spawn + wait, blocking. Ok = the child's exit code (0 = clean); Err = the
// spawn itself failed (ENOENT → "viewer not found: <bin>", the install-it
// degrade the toast shows).
[[nodiscard]] Result<int, std::string> run_viewer(
    const std::vector<std::string>& argv);

}  // namespace shigoku::manga
