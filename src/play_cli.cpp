// play_cli.cpp — see play_cli.hpp. Ported from sabigoku main.rs play_flow /
// load_episodes.

#include "play_cli.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <vector>

#include "aniskip.hpp"
#include "domain.hpp"
#include "download.hpp"
#include "player.hpp"

namespace shigoku::cli_play {

namespace {

std::int64_t unix_now() { return static_cast<std::int64_t>(::time(nullptr)); }
void flush_stdout() { std::fflush(stdout); }

// Cache-first episodes: an unexpired store hit wins; a miss fetches and warms
// the cache. Both gated on a store AND an anilist key. Cache read/warm failures
// degrade to a plain fetch, never an error.
Result<std::vector<std::string>, ProviderError> load_episodes(
    Store* store, std::optional<std::int64_t> anilist_id, const StreamProvider& provider,
    const SearchHit& hit, Translation translation, std::int64_t now) {
  const std::string title = strip_controls(hit.title);
  if (store != nullptr && anilist_id.has_value()) {
    auto cached = store->get_cached_episodes(*anilist_id, provider.name(), translation, now);
    if (cached.has_value() && cached->has_value()) {
      std::printf("\n  episodes for \"%s\" (cached)\n", title.c_str());
      return **cached;
    }
  }
  std::printf("\n  fetching episodes for \"%s\"…\n", title.c_str());
  flush_stdout();
  auto episodes = provider.episodes(hit.provider_id, translation, hit.total_episodes);
  if (!episodes.has_value()) return err(episodes.error());
  if (store != nullptr && anilist_id.has_value()) {
    // Airing status unknown on the query path; the cache lands with default
    // TTL. A warm miss is inert.
    (void)store->set_episode_cache(*anilist_id, provider.name(), translation, *episodes,
                                   std::nullopt, now);
  }
  return *episodes;
}

}  // namespace

int play_flow(const StreamProvider& provider, const PickFn& pick, Translation translation,
              const Config& config, const std::string& cache_dir, const std::string& runtime_dir,
              const std::string& download_dir, Store* store, const cli::PlayArgs& args) {
  const std::int64_t now = unix_now();

  SearchOptions sopts;
  sopts.translation = translation;
  sopts.limit = 20;
  sopts.page = 1;
  auto hits_r = provider.search(args.query, sopts);
  if (!hits_r.has_value()) {
    std::printf("%s", cli::fetch_error_line(cli::FetchStage::Search, hits_r.error().kind,
                                            provider.display_name())
                          .c_str());
    return 1;
  }
  const std::vector<SearchHit>& hits = *hits_r;
  if (hits.empty()) {
    std::printf("\n  no results for \"%s\". try a different spelling or romaji.\n",
                strip_controls(args.query).c_str());
    return 0;
  }

  std::printf("%s", cli::render_search_hits(hits, translation).c_str());
  auto idx = pick("\n  pick a show # (q to quit): ", hits.size());
  if (!idx.has_value()) {
    std::printf("  bye.\n");
    return 0;
  }
  const SearchHit& hit = hits[*idx];

  // Persistence keys on the hit's anilist_id. Absent (senshi never carries one;
  // anidbapp/allanime sometimes) => play-only: episodes fetch fresh, no bind,
  // no cache, and later no resume or history.
  const std::optional<std::int64_t> anilist_id = hit.anilist_id;
  if (store != nullptr && anilist_id.has_value()) {
    Enrichment enrichment;
    enrichment.anilist_id = *anilist_id;
    enrichment.mal_id = hit.mal_id;
    // Provider-supplied title persists into a row every render trusts as
    // pre-scrubbed; this path must strip too, or it plants a replay-on-render
    // injection.
    enrichment.title_romaji = strip_controls(hit.title);
    enrichment.total_episodes = hit.total_episodes;
    // Best-effort warm; a bind miss must not sink the play.
    (void)store->bind_provider(enrichment, provider.name(), hit.provider_id, now);
  }

  auto episodes_r = load_episodes(store, anilist_id, provider, hit, translation, now);
  if (!episodes_r.has_value()) {
    std::printf("%s", cli::fetch_error_line(cli::FetchStage::Episodes, episodes_r.error().kind,
                                            provider.display_name())
                          .c_str());
    return 1;
  }
  const std::vector<std::string>& episodes = *episodes_r;
  if (episodes.empty()) {
    std::printf("\n  no %s episodes listed for this show.\n",
                std::string(to_string(translation)).c_str());
    return 0;
  }

  std::printf("%s", cli::render_episode_list(episodes).c_str());
  auto ep_idx = pick("\n  pick an episode # (q to quit): ", episodes.size());
  if (!ep_idx.has_value()) {
    std::printf("  bye.\n");
    return 0;
  }
  const std::string& episode = episodes[*ep_idx];
  const std::uint32_t episode_index = static_cast<std::uint32_t>(*ep_idx + 1);

  // Resume start, gated on store+anilist_id. Computed per the 03 §6.3.1 rewind
  // rule; the player emits --start only when > 0.
  double start_secs = 0.0;
  if (store != nullptr && anilist_id.has_value()) {
    auto resume = store->get_resume(*anilist_id, translation, episode);
    if (resume.has_value() && resume->has_value()) {
      start_secs = (*resume)->start_secs(config.resume_offset_sec);
    }
  }
  if (start_secs > 0.0) {
    std::printf("  ↺ resuming at %.0fs\n", start_secs);
  }

  // AniSkip keys on mal_id, independent of the store gate (03 §9). A None mal
  // plays plain.
  player::PlayOpts opts;
  opts.mpv_path = config.mpv_path;
  opts.backend = player::parse_backend(config.player);  // P39 slice 2.
  opts.player_path = config.player_path;
  opts.socket_dir = runtime_dir;
  opts.title = strip_controls(hit.title + " · ep " + episode);
  opts.start_secs = start_secs;
  if (!cache_dir.empty()) {
    const std::uint32_t ep_num = aniskip::episode_number(episode, episode_index);
    opts.skip = aniskip::prepare(hit.mal_id, strip_controls(hit.title), ep_num,
                                 aniskip::parse_skip_mode(config.skip_mode), cache_dir + "/aniskip");
  }

  // Quality rides config.default_quality; --quality is parsed-but-inert.
  const Quality quality = parse_quality(config.default_quality);

  // Play-prefers-local (P35 slice 4): a completed download for this exact
  // (show, track, ep) plays as a file — no resolve, no network. Gated on a
  // real anilist_id: the value_or(0) shelf download_flow writes for play-only
  // shows can mix shows, and a wrong-show match is worse than a resolve.
  std::optional<std::string> local;
  if (!download_dir.empty() && anilist_id.has_value()) {
    local = download::find_local_episode(download_dir, *anilist_id, translation, episode);
  }

  if (local.has_value()) {
    std::printf("\n  ▶ playing the downloaded copy of ep %s (%s) in mpv…\n",
                strip_controls(episode).c_str(),
                std::string(to_string(translation)).c_str());
  } else {
    std::printf("\n  ▶ resolving ep %s (%s) and launching mpv…\n", strip_controls(episode).c_str(),
                std::string(to_string(translation)).c_str());
  }
  flush_stdout();

  // player::play collapses a resolve ProviderError into a PlayError::resolve
  // detail string, so capture the class here to render the per-class resolve
  // copy (cli::player_failure_line's resolve_class arm).
  std::optional<ProviderError::Kind> resolve_class;
  player::ResolveFn resolve = [&]() -> Result<StreamLink, std::string> {
    auto link = provider.resolve(hit.provider_id, episode, translation, quality);
    if (!link.has_value()) {
      resolve_class = link.error().kind;
      return err(std::string(provider_error_copy(link.error().kind)));
    }
    return *link;
  };
  player::EventSink on_event = [](const player::PlayerEvent& ev) {
    if (ev.kind == player::PlayerEvent::Kind::Retry) {
      std::printf("  stream didn't open, retrying (%u/%u)\n", ev.attempt, player::kMaxPlayAttempts);
      flush_stdout();
    }
  };

  auto outcome = local.has_value() ? player::play_local(opts, *local, on_event)
                                   : player::play(opts, resolve, on_event);
  if (!outcome.has_value()) {
    // The one nonzero exit (06 §7.4): a play that never yielded a meaningful
    // watch. Meaningful-watch mpv failures fold into Ok below.
    std::printf("%s", cli::player_failure_line(outcome.error().kind, provider.display_name(),
                                               resolve_class)
                          .c_str());
    return 1;
  }

  // A meaningful watch returns Ok with a position even when mpv then exits
  // badly; persist it. record_finish shuts its own gate on a None position, so
  // an empty watch writes nothing.
  if (store != nullptr && anilist_id.has_value() && outcome->position.has_value()) {
    const player::Position& pos = *outcome->position;
    (void)store->record_finish(*anilist_id, translation, episode, episode_index, pos.secs,
                               pos.duration.value_or(0.0), provider.name(), unix_now());
  }
  std::printf("\n  ✓ done.\n");
  return 0;
}

int download_flow(const StreamProvider& provider, const PickFn& pick,
                  Translation translation, const Config& config,
                  const std::string& download_dir, Store* store,
                  const cli::DownloadArgs& args) {
  const std::int64_t now = unix_now();

  SearchOptions sopts;
  sopts.translation = translation;
  sopts.limit = 20;
  sopts.page = 1;
  auto hits_r = provider.search(args.query, sopts);
  if (!hits_r.has_value()) {
    std::printf("%s", cli::fetch_error_line(cli::FetchStage::Search, hits_r.error().kind,
                                            provider.display_name())
                          .c_str());
    return 1;
  }
  const std::vector<SearchHit>& hits = *hits_r;
  if (hits.empty()) {
    std::printf("\n  no results for \"%s\". try a different spelling or romaji.\n",
                strip_controls(args.query).c_str());
    return 0;
  }

  std::printf("%s", cli::render_search_hits(hits, translation).c_str());
  auto idx = pick("\n  pick a show # (q to quit): ", hits.size());
  if (!idx.has_value()) {
    std::printf("  bye.\n");
    return 0;
  }
  const SearchHit& hit = hits[*idx];

  // Same best-effort bind as play_flow: a download is a first-class library
  // touch (the show becomes resumable/locatable later).
  const std::optional<std::int64_t> anilist_id = hit.anilist_id;
  if (store != nullptr && anilist_id.has_value()) {
    Enrichment enrichment;
    enrichment.anilist_id = *anilist_id;
    enrichment.mal_id = hit.mal_id;
    enrichment.title_romaji = strip_controls(hit.title);
    enrichment.total_episodes = hit.total_episodes;
    (void)store->bind_provider(enrichment, provider.name(), hit.provider_id, now);
  }

  auto episodes_r = load_episodes(store, anilist_id, provider, hit, translation, now);
  if (!episodes_r.has_value()) {
    std::printf("%s", cli::fetch_error_line(cli::FetchStage::Episodes, episodes_r.error().kind,
                                            provider.display_name())
                          .c_str());
    return 1;
  }
  const std::vector<std::string>& episodes = *episodes_r;
  if (episodes.empty()) {
    std::printf("\n  no %s episodes listed for this show.\n",
                std::string(to_string(translation)).c_str());
    return 0;
  }

  // Episode selection: an explicit <ep> maps exact-raw-label first, then
  // 1-based ordinal (map_episode_index, 03 §6.6); absent, prompt like play.
  std::optional<std::size_t> ep_idx;
  if (args.episode.has_value()) {
    std::uint32_t ordinal = 0;
    for (const char c : *args.episode) {
      if (c < '0' || c > '9') {
        ordinal = 0;
        break;
      }
      ordinal = ordinal * 10 + static_cast<std::uint32_t>(c - '0');
    }
    ep_idx = map_episode_index(episodes, *args.episode, ordinal);
    if (!ep_idx.has_value()) {
      std::printf("\n  episode \"%s\" isn't in this show's list (%zu episodes).\n",
                  strip_controls(*args.episode).c_str(), episodes.size());
      return 0;
    }
  } else {
    std::printf("%s", cli::render_episode_list(episodes).c_str());
    ep_idx = pick("\n  pick an episode # (q to quit): ", episodes.size());
    if (!ep_idx.has_value()) {
      std::printf("  bye.\n");
      return 0;
    }
  }
  const std::string& episode = episodes[*ep_idx];

  const Quality quality = parse_quality(config.default_quality);
  std::printf("\n  \xE2\x87\xA3 resolving ep %s (%s)…\n", strip_controls(episode).c_str(),
              std::string(to_string(translation)).c_str());
  flush_stdout();

  auto link = provider.resolve(hit.provider_id, episode, translation, quality);
  if (!link.has_value()) {
    std::printf("%s", cli::fetch_error_line(cli::FetchStage::Resolve, link.error().kind,
                                            provider.display_name())
                          .c_str());
    return 1;
  }

  // The download client rides the same A5 wrapper the provider used.
  auto client = http::Client::create();
  if (!client.has_value()) {
    std::printf("  ✗ couldn't set up the download client.\n");
    return 1;
  }

  const std::int64_t dest_id = anilist_id.value_or(0);  // 0 = play-only shows.
  const std::string dest =
      download::episode_dest(download_dir, dest_id, translation, episode, link->url);
  if (auto md = download::ensure_parent_dirs(dest); !md.has_value()) {
    std::printf("  ✗ couldn't create %s (%s).\n", download_dir.c_str(),
                md.error().detail.c_str());
    return 1;
  }

  std::printf("  \xE2\x87\xA3 downloading to %s\n", dest.c_str());
  flush_stdout();
  std::atomic<bool> cancel{false};  // Ctrl-C kills the process group instead.
  download::ProgressFn progress = [](std::uint64_t bytes,
                                     std::optional<std::uint64_t> total) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (total.has_value() && *total > 0) {
      const double tmb = static_cast<double>(*total) / (1024.0 * 1024.0);
      std::printf("\r  %.1f / %.1f MB (%u%%)   ", mb, tmb,
                  static_cast<unsigned>(bytes * 100 / *total));
    } else {
      std::printf("\r  %.1f MB   ", mb);
    }
    flush_stdout();
  };

  auto outcome = download::download_link(*client, *link, dest, quality,
                                         config.ffmpeg_path, cancel, progress);
  std::printf("\n");
  if (!outcome.has_value()) {
    using K = download::DownloadError::Kind;
    switch (outcome.error().kind) {
      case K::Fetch:
        std::printf("%s", cli::fetch_error_line(cli::FetchStage::Resolve,
                                                outcome.error().fetch.kind,
                                                provider.display_name())
                              .c_str());
        std::printf("  (a partial .part file, if any, resumes on re-run.)\n");
        break;
      case K::Io:
        std::printf("  ✗ disk error: %s\n", outcome.error().detail.c_str());
        break;
      case K::Cancelled:
        std::printf("  cancelled; the partial .part resumes on re-run.\n");
        break;
      case K::UnsafeUrl:
        std::printf("  ✗ the stream url failed the safety check; not fetched.\n");
        break;
      case K::UnsafeArg:
        std::printf("  ✗ the stream carried an unsafe %s field; not fetched.\n",
                    outcome.error().detail.c_str());
        break;
      case K::FfmpegNotFound:
        std::printf("  ✗ this stream needs ffmpeg to download (%s not found).\n"
                    "    install ffmpeg, or set ffmpeg_path in config.json.\n",
                    config.ffmpeg_path.c_str());
        break;
      case K::Ffmpeg:
        std::printf("  ✗ ffmpeg failed (%s); nothing was published.\n",
                    outcome.error().detail.c_str());
        break;
    }
    return 1;
  }

  std::printf("  ✓ saved %s\n", dest.c_str());
  return 0;
}

}  // namespace shigoku::cli_play
