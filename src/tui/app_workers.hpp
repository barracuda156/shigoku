// app_workers.hpp — the app's detached workers: each takes its inputs by
// value, runs off the UI thread, and posts one owned Event back to the queue.
// Private to app.cpp and app_workers.cpp.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "../aniskip.hpp"
#include "../loopback.hpp"
#include "../mal_loopback.hpp"
#include "../player.hpp"
#include "app.hpp"

namespace shigoku::tui {

void spawn_search(EventQueue& queue, const SearchFn& search, std::string query,
                  std::uint32_t page);

void spawn_connect(EventQueue& queue, const http::Client& http, loopback::Loopback loopback,
                   std::string auth_path, std::int64_t now);

void spawn_mal_connect(EventQueue& queue, const http::Client& http,
                       mal_loopback::Loopback loopback, std::string auth_path, std::int64_t now);

void spawn_sync(EventQueue& queue, const http::Client& http, std::string db_path,
                std::string auth_path, std::string mal_client_id, Auth auth, bool enabled,
                bool pull_only, std::int64_t now);

void spawn_update_check(EventQueue& queue, const http::Client& http, std::string cache_dir,
                        std::string current_version, std::int64_t now);

void spawn_quit_flush_worker(const http::Client& http, std::string db_path, Auth auth,
                             bool enabled, std::int64_t now,
                             std::shared_ptr<std::atomic<bool>> done);

void spawn_discover_feed(EventQueue& queue, const DiscoverFn& discover, DiscoverAxis axis,
                         std::uint32_t page, const DiscoverFilters& filters,
                         std::uint32_t gen);

void spawn_similar_feed(EventQueue& queue, const SimilarFn& similar, SimilarSeed seed,
                        std::uint32_t page, const DiscoverFilters& filters, std::uint32_t gen);

void spawn_resolve_episodes(EventQueue& queue, const ProviderRegistry& registry,
                            Enrichment show, Generation gen);

void spawn_play(EventQueue& queue, const ProviderRegistry& registry,
                const AppDeps& deps, std::string provider_name, std::string provider_id,
                std::string episode, std::int64_t for_id, double start_secs,
                Translation translation, Quality quality,
                std::optional<std::int64_t> mal_id, std::string show_title,
                std::uint32_t episode_ordinal, aniskip::SkipMode skip_mode,
                player::BackendKind backend, std::string player_path);

void spawn_download(EventQueue& queue, const ProviderRegistry& registry,
                    const http::Client* http, std::string provider_name,
                    std::string provider_id, std::string episode,
                    std::int64_t for_id, Translation translation, Quality quality,
                    std::string download_dir, std::string ffmpeg_path,
                    std::shared_ptr<std::atomic<bool>> cancel,
                    std::shared_ptr<std::atomic<long>> child_pid);

void spawn_cover(EventQueue& queue, Covers& covers, std::string url,
                 std::int64_t for_id, std::uint32_t box_w, std::uint32_t box_h,
                 Generation gen);

void spawn_grid_cover(EventQueue& queue, Covers& covers, std::string url,
                      std::uint32_t box_w, std::uint32_t box_h);

void spawn_enrich(EventQueue& queue, EnrichFn enrich, std::int64_t for_id,
                  std::optional<std::int64_t> mal_id);

void spawn_heal(EventQueue& queue, HealFn heal, std::int64_t for_id, std::int64_t mal_id);

void spawn_char_recs(EventQueue& queue, CharRecsFn fetch, std::int64_t for_id,
                     std::optional<std::int64_t> mal_id);

void spawn_genre_collection(EventQueue& queue, GenreCollectionFn fetch);

// The message a failed catalog fetch carries: the error's own detail, else
// the copy for its kind.
[[nodiscard]] std::string catalog_cause(const ProviderError& e);

}  // namespace shigoku::tui
