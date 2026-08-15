// play_cli.hpp — the CLI play flow (06 §7.3, P27), split from main.cpp's
// environment wiring (paths, config, store, registry) so the exit table
// (06 §7.4) is unit-testable with a fake provider and a scripted picker: no
// process spawn, no live search, no mpv. Ported from sabigoku main.rs's
// play_flow, whose test mod pins the same exit contract.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "cli.hpp"
#include "config.hpp"
#include "provider.hpp"
#include "store.hpp"

namespace shigoku::cli_play {

// Numbered-pick seam: prompt + max -> a 0-based index, or nullopt to abort
// (q/EOF/overlong). Production wires prompt_pick (stdin); tests script it.
using PickFn = std::function<std::optional<std::size_t>(const char*, std::size_t)>;

// The search -> pick -> episodes -> pick -> play flow. Returns the raw exit
// code (0/1); the caller wraps it. `cache_dir`/`runtime_dir` are the resolved
// paths.cache / paths.runtime; `store` may be null (play-only). Exit law
// (06 §7.4): the play path is the one nonzero exit (1); every early return
// (no results, quit, no episodes) is a clean 0. `download_dir` is the
// RESOLVED download root ("" = downloads disabled): a completed local file
// for the picked (show, track, ep) plays WITHOUT resolving (P35 slice 4).
[[nodiscard]] int play_flow(const StreamProvider& provider, const PickFn& pick,
                            Translation translation, const Config& config,
                            const std::string& cache_dir, const std::string& runtime_dir,
                            const std::string& download_dir, Store* store,
                            const cli::PlayArgs& args);

// `shigoku download <query> [<ep>]` (P35 slice 3): the same search/pick/
// episodes spine as play_flow, then download_link instead of mpv. An explicit
// <ep> maps by exact raw label first, then 1-based ordinal (the 03 §6.6 hop
// rule); absent, the episode is picked interactively. `download_dir` is the
// RESOLVED root (main.cpp turns a blank config value into <data>/downloads).
// Same exit law: transfer failure is the nonzero exit (1); every early
// return (no results, quit, no episodes, unknown <ep>) is a clean 0.
[[nodiscard]] int download_flow(const StreamProvider& provider, const PickFn& pick,
                                Translation translation, const Config& config,
                                const std::string& download_dir, Store* store,
                                const cli::DownloadArgs& args);

}  // namespace shigoku::cli_play
