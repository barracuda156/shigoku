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
#include <vector>

#include "cli.hpp"
#include "config.hpp"
#include "provider.hpp"
#include "store.hpp"

namespace shigoku::cli_play {

// The pick seam: a prompt ("pick a show") + the unnumbered rows -> a 0-based
// index, or nullopt to abort (q / EOF / overlong on the prompt; Esc, no
// match or a spawn failure on an fzf-compatible picker). Production wires
// main.cpp's numbered prompt or picker::fzf_pick; tests script it. The
// picker owns the rows' presentation — the flow prints only the count line.
using PickFn = std::function<std::optional<std::size_t>(std::string_view,
                                                        const std::vector<std::string>&)>;

// The ordered search-capable sources (ProviderRegistry::searchable): the
// preferred one first when it can search, then the rest by registry position.
// Both flows walk it at the search stage — a source that fails or finds
// nothing is noted and the next is tried — and bind the whole run (episodes,
// resolve, store keys) to the first that answers. Non-owning; the registry
// outlives the flow.
using Sources = std::vector<const StreamProvider*>;

// The search -> pick -> episodes -> pick -> play flow. Returns the raw exit
// code (0/1); the caller wraps it. `cache_dir`/`runtime_dir` are the resolved
// paths.cache / paths.runtime; `store` may be null (play-only). Exit law
// (06 §7.4): the play path is the one nonzero exit (1); every early return
// (no results, quit, no episodes) is a clean 0. A search that fails on EVERY
// source is the search-stage 1; a source that answered "nothing" makes the
// whole walk a clean no-results 0 even if others failed. `download_dir` is
// the RESOLVED download root ("" = downloads disabled): a completed local
// file for the picked (show, track, ep) plays WITHOUT resolving (P35 slice 4).
[[nodiscard]] int play_flow(const Sources& sources, const PickFn& pick,
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
[[nodiscard]] int download_flow(const Sources& sources, const PickFn& pick,
                                Translation translation, const Config& config,
                                const std::string& download_dir, Store* store,
                                const cli::DownloadArgs& args);

}  // namespace shigoku::cli_play
