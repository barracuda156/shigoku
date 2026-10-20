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

// Title -> catalogue entries: the first page of catalog.hpp's search
// (AniList, MAL when AniList can't answer). Production wires the dispatcher;
// tests script it.
using CatalogSearch =
    std::function<Result<std::vector<Enrichment>, ProviderError>(std::string_view title)>;

// What a run can reach beyond its search walk. A default Reach is the bare
// walk: a hit with no id stays play-only, and an episode that fails to play
// ends the run.
struct Reach {
  // A hit that carries no id is looked up here by its title, and the entry
  // resolver::best_catalog_match names files the show — its library row,
  // resume, history and downloads key on that id like any other. Unset, or
  // no clear match: the hit stays play-only.
  CatalogSearch catalog;
  // Every source, in the order an episode that failed at its source tries
  // them (ProviderRegistry::ordered): the stream never opened past the
  // retries, or no stream resolved. Each other source is asked for the show
  // — by its key off the show's ids when it keys that way, else by a title
  // search judged as the interface's walk judges it — and for the same
  // episode (label first, then position); the first that plays it serves
  // the rest of the run. Empty = no such move.
  Sources fallbacks;
};

// The search -> pick -> episodes -> pick -> play flow. Returns the raw exit
// code (0/1); the caller wraps it. `cache_dir`/`runtime_dir` are the resolved
// paths.cache / paths.runtime; `store` may be null (play-only). Exit law
// (06 §7.4): the play path is the one nonzero exit (1); every early return
// (no results, quit, no episodes, an `-S`/`-e`/`-r` that names nothing in
// the list) is a clean 0. A search that fails on EVERY source is the
// search-stage 1; a source that answered "nothing" makes the whole walk a
// clean no-results 0 even if others failed. `download_dir` is the RESOLVED
// download root ("" = downloads disabled): a completed local file for the
// picked (show, track, ep) plays WITHOUT resolving.
//
// After an episode plays, the pick seam offers what comes next (next /
// replay / previous / quit; cli::post_play_menu) and the run loops until
// quit, EOF, or a play that fails (exit 1, as for the first episode) — on
// every source `reach.fallbacks` could find it on.
// `-r` plays its span in order with no menu; `-e` starts at that episode
// and then asks like a prompted pick would.
[[nodiscard]] int play_flow(const Sources& sources, const PickFn& pick,
                            Translation translation, const Config& config,
                            const std::string& cache_dir, const std::string& runtime_dir,
                            const std::string& download_dir, Store* store,
                            const cli::PlayArgs& args, const Reach& reach = {});

// `shigoku download <query> [<ep>]` (P35 slice 3): the same search/pick/
// episodes spine as play_flow, then download_link instead of mpv. An explicit
// <ep> maps by exact raw label first, then 1-based ordinal (the 03 §6.6 hop
// rule); absent, the episode is picked interactively. `download_dir` is the
// RESOLVED root (main.cpp turns a blank config value into <data>/downloads).
// Same exit law: transfer failure is the nonzero exit (1); every early
// return (no results, quit, no episodes, unknown <ep>) is a clean 0. A show
// with an id downloads under it (download::episode_dest); a play-only one
// under its source's own folder (download::source_episode_dest).
[[nodiscard]] int download_flow(const Sources& sources, const PickFn& pick,
                                Translation translation, const Config& config,
                                const std::string& download_dir, Store* store,
                                const cli::DownloadArgs& args, const Reach& reach = {});

// `shigoku continue [<query>]`: a library show picked back up. The history
// (Store::list_history, most recently watched first), narrowed by the query
// when given, is the list — `-S` or a lone match skips the prompt. The show's
// stored provider binding is asked for its episodes (the `-p` source when it
// is bound; else the config's preference, then registry order); a show with
// no live binding is searched for again by title on `sources`, the hit that
// carries its AniList or MAL id taken without a prompt. The episode is the
// freshest partial watch when there is one, else the one after the last
// finished (Show::progress as a 1-based ordinal into the list); all watched
// = a clean "caught up" 0. Then the same play loop as play_flow, keyed to
// the library row whatever the hit claimed. The store is required — there
// is no history without one — so it is a reference here.
[[nodiscard]] int continue_flow(const ProviderRegistry& registry, const Sources& sources,
                                const PickFn& pick, Translation translation,
                                const Config& config, const std::string& cache_dir,
                                const std::string& runtime_dir, const std::string& download_dir,
                                Store& store, const cli::ContinueArgs& args,
                                const Reach& reach = {});

}  // namespace shigoku::cli_play
