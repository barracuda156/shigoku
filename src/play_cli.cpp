// play_cli.cpp — see play_cli.hpp. Ported from sabigoku main.rs play_flow /
// load_episodes; the per-episode loop, the span and `continue` are shigoku's.

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
#include "idmap.hpp"
#include "player.hpp"

namespace shigoku::cli_play {

namespace {

std::int64_t unix_now() { return static_cast<std::int64_t>(::time(nullptr)); }
void flush_stdout() { std::fflush(stdout); }
const char* g(std::string_view glyph) { return glyph.data(); }  // the table's views are literals

// The show a run is bound to: the source and its id for the show, plus the
// catalogue facts the loop keys on — the ids for the store and AniSkip, the
// title for the player's window, the total for the episode fetch's hint.
// For a searched show these are the hit's claims; for a continued one the
// library row's, whatever the hit said (the row is the identity the resume
// and the play record hang off).
struct BoundShow {
  const StreamProvider* provider = nullptr;
  std::string provider_id;
  std::string title;  // control bytes already stripped
  std::optional<std::int64_t> anilist_id;
  std::optional<std::int64_t> mal_id;
  std::optional<std::uint32_t> total_episodes;
};

// The hit as the show a run is bound to. Persistence keys on an anilist_id:
// a hit that carries one is that show; a hit that carries only a MAL id
// (senshi, hianime, megaplay name shows that way) is filed as the MAL
// catalog files such a show — under the offline table's AniList id, else
// the synthetic negative one — so it joins the same library row the
// interface would make, resumes, and is there for `continue`. A hit with
// neither id is play-only: episodes fetch fresh, no bind, no cache, and
// later no resume or history.
BoundShow bind_hit(const StreamProvider& provider, const SearchHit& hit) {
  BoundShow b;
  b.provider = &provider;
  b.provider_id = hit.provider_id;
  b.title = strip_controls(hit.title);
  b.anilist_id = hit.anilist_id;
  if (!b.anilist_id.has_value() && hit.mal_id.has_value()) {
    b.anilist_id = idmap::anilist_or_synthetic(*hit.mal_id);
  }
  b.mal_id = hit.mal_id;
  b.total_episodes = hit.total_episodes;
  return b;
}

// The library touch for a keyed show (bind_hit's rule). Best-effort warm; a
// bind miss must not sink the play.
void warm_binding(Store* store, const BoundShow& show, std::int64_t now) {
  if (store == nullptr || !show.anilist_id.has_value()) return;
  Enrichment enrichment;
  enrichment.anilist_id = *show.anilist_id;
  enrichment.mal_id = show.mal_id;
  // Provider-supplied title persists into a row every render trusts as
  // pre-scrubbed; BoundShow::title is already stripped for that reason.
  enrichment.title_romaji = show.title;
  enrichment.total_episodes = show.total_episodes;
  (void)store->bind_provider(enrichment, show.provider->name(), show.provider_id, now);
}

// Cache-first episodes: an unexpired store hit wins; a miss fetches and warms
// the cache. Both gated on a store AND an anilist key. Cache read/warm failures
// degrade to a plain fetch, never an error.
Result<std::vector<std::string>, ProviderError> load_episodes(Store* store, const BoundShow& show,
                                                              Translation translation,
                                                              std::int64_t now) {
  const StreamProvider& provider = *show.provider;
  if (store != nullptr && show.anilist_id.has_value()) {
    auto cached = store->get_cached_episodes(*show.anilist_id, provider.name(), translation, now);
    if (cached.has_value() && cached->has_value()) {
      std::printf("\n  episodes for \"%s\" (cached)\n", show.title.c_str());
      return **cached;
    }
  }
  std::printf("\n  fetching episodes for \"%s\"%s\n", show.title.c_str(),
              g(cli::glyphs().ellipsis));
  flush_stdout();
  auto episodes = provider.episodes(show.provider_id, translation, show.total_episodes);
  if (!episodes.has_value()) return err(episodes.error());
  if (store != nullptr && show.anilist_id.has_value()) {
    // Airing status unknown on the query path; the cache lands with default
    // TTL. A warm miss is inert.
    (void)store->set_episode_cache(*show.anilist_id, provider.name(), translation, *episodes,
                                   std::nullopt, now);
  }
  return *episodes;
}

// The search stage over the ordered sources. Binds the first source whose
// search answers with hits; a source that fails or finds nothing is noted
// (cli::search_walk_note) and the next is tried. When none answers: exit 1
// if every source FAILED (the exit law's search-stage failure, rendered for
// the last one), else a clean no-results 0 — some source was healthy and
// simply has no such title.
struct SearchWalk {
  const StreamProvider* provider = nullptr;  // null = nothing answered.
  std::vector<SearchHit> hits;
  int exit_code = 0;  // meaningful only when provider is null.
};

SearchWalk search_walk(const Sources& sources, std::string_view query,
                       Translation translation) {
  SearchOptions sopts;
  sopts.translation = translation;
  sopts.limit = 20;
  sopts.page = 1;
  bool any_empty = false;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const StreamProvider& p = *sources[i];
    const StreamProvider* next = i + 1 < sources.size() ? sources[i + 1] : nullptr;
    auto hits_r = p.search(query, sopts);
    if (!hits_r.has_value()) {
      if (next != nullptr) {
        std::printf("%s", cli::search_walk_note(p.display_name(), hits_r.error().kind,
                                                next->display_name())
                              .c_str());
        flush_stdout();
        continue;
      }
      std::printf("%s", cli::fetch_error_line(cli::FetchStage::Search, hits_r.error().kind,
                                              p.display_name())
                            .c_str());
      break;
    }
    if (hits_r->empty()) {
      any_empty = true;
      if (next != nullptr) {
        std::printf("%s", cli::search_walk_note(p.display_name(), std::nullopt,
                                                next->display_name())
                              .c_str());
        flush_stdout();
        continue;
      }
      break;
    }
    SearchWalk bound;
    bound.provider = &p;
    bound.hits = std::move(*hits_r);
    return bound;
  }

  SearchWalk none;
  if (sources.empty()) {
    std::printf("  %s no configured source can search.\n", g(cli::glyphs().fail));
    none.exit_code = 1;
  } else if (!any_empty) {
    none.exit_code = 1;  // every source failed; the last one's line is above.
  } else {
    std::printf("\n  no results for \"%s\"%s. try a different spelling or romaji.\n",
                strip_controls(query).c_str(), sources.size() > 1 ? " on any source" : "");
    none.exit_code = 0;
  }
  return none;
}

// The show pick over the search hits: `-S` takes the nth result (a number
// past the list is said and ends the run — the caller's clean 0), else the
// seam asks. nullopt = nothing chosen; the reason is already printed.
std::optional<std::size_t> choose_show(const std::vector<SearchHit>& hits,
                                       Translation translation,
                                       std::optional<std::uint32_t> nth, const PickFn& pick) {
  const std::vector<std::string> rows = cli::search_hit_rows(hits, translation);
  if (nth.has_value()) {
    if (*nth == 0 || *nth > hits.size()) {
      std::printf("\n  %zu result(s); there is no #%u to take.\n", hits.size(),
                  static_cast<unsigned>(*nth));
      return std::nullopt;
    }
    std::printf("\n  %zu result(s); taking #%u: %s\n", hits.size(), static_cast<unsigned>(*nth),
                rows[*nth - 1].c_str());
    return *nth - 1;
  }
  std::printf("\n  %zu result(s):\n\n", hits.size());
  auto idx = pick("pick a show", rows);
  if (!idx.has_value() || *idx >= hits.size()) {
    std::printf("  bye.\n");
    return std::nullopt;
  }
  return idx;
}

// An explicit episode (`-e`, or download's <ep>) against the list: exact raw
// label first, then 1-based ordinal (map_episode_index, 03 §6.6). A miss is
// said; nullopt.
std::optional<std::size_t> named_episode(const std::vector<std::string>& episodes,
                                         const std::string& wanted) {
  auto idx = map_episode_index(episodes, wanted, cli::ordinal_of(wanted));
  if (!idx.has_value()) {
    std::printf("\n  episode \"%s\" isn't in this show's list (%zu episodes).\n",
                strip_controls(wanted).c_str(), episodes.size());
  }
  return idx;
}

// The episode prompt. nullopt = quit ("bye" printed).
std::optional<std::size_t> prompt_episode(const std::vector<std::string>& episodes,
                                          const PickFn& pick) {
  std::printf("\n  %zu episode(s):\n\n", episodes.size());
  auto idx = pick("pick an episode", cli::episode_rows(episodes));
  if (!idx.has_value() || *idx >= episodes.size()) {
    std::printf("  bye.\n");
    return std::nullopt;
  }
  return idx;
}

// The quality for the run: the flag's spelling over the config's; either
// parsed by parse_quality (an unknown spelling is best — main said so).
Quality run_quality(const std::optional<std::string>& flag, const Config& config) {
  return parse_quality(flag.has_value() ? *flag : config.default_quality);
}

// What every episode of a run plays under, fixed once the show is bound.
struct PlayEnv {
  const Config* config = nullptr;
  const std::string* cache_dir = nullptr;
  const std::string* runtime_dir = nullptr;
  const std::string* download_dir = nullptr;
  Store* store = nullptr;
  Translation translation = Translation::Sub;
  Quality quality = Quality::Best;
};

// One episode, start to finish: resume point, AniSkip adjuncts, a completed
// download or a resolve, the player, the play record. false = the play
// failed (the one nonzero exit, said here); true = it ran, a short check
// included.
bool play_one(const PlayEnv& env, const BoundShow& show, const std::vector<std::string>& episodes,
              std::size_t ep_idx) {
  const Config& config = *env.config;
  const Translation translation = env.translation;
  const StreamProvider& provider = *show.provider;
  const std::string& episode = episodes[ep_idx];
  const std::uint32_t episode_index = static_cast<std::uint32_t>(ep_idx + 1);
  const cli::Glyphs& gl = cli::glyphs();

  // Resume start, gated on store+anilist_id. Computed per the 03 §6.3.1 rewind
  // rule; the player emits --start only when > 0.
  double start_secs = 0.0;
  if (env.store != nullptr && show.anilist_id.has_value()) {
    auto resume = env.store->get_resume(*show.anilist_id, translation, episode);
    if (resume.has_value() && resume->has_value()) {
      start_secs = (*resume)->start_secs(config.resume_offset_sec);
    }
  }
  if (start_secs > 0.0) {
    std::printf("  %s resuming at %.0fs\n", g(gl.resume), start_secs);
  }

  // AniSkip keys on mal_id, independent of the store gate (03 §9). A None mal
  // plays plain.
  player::PlayOpts opts;
  opts.mpv_path = config.mpv_path;
  opts.backend = player::parse_backend(config.player);
  opts.player_path = config.player_path;
  opts.socket_dir = *env.runtime_dir;
  opts.title = strip_controls(show.title + " · ep " + episode);
  opts.start_secs = start_secs;
  if (!env.cache_dir->empty()) {
    const std::uint32_t ep_num = aniskip::episode_number(episode, episode_index);
    auto adjuncts = aniskip::prepare_all(show.mal_id, show.title, ep_num,
                                         aniskip::parse_skip_mode(config.skip_mode),
                                         *env.cache_dir + "/aniskip");
    opts.skip = std::move(adjuncts.skip);
    opts.chapters_file = std::move(adjuncts.chapters_file);
  }

  // Play-prefers-local (P35 slice 4): a completed download for this exact
  // (show, track, ep) plays as a file — no resolve, no network. Gated on a
  // real anilist_id: the value_or(0) shelf download_flow writes for play-only
  // shows can mix shows, and a wrong-show match is worse than a resolve.
  std::optional<std::string> local;
  if (!env.download_dir->empty() && show.anilist_id.has_value()) {
    local = download::find_local_episode(*env.download_dir, *show.anilist_id, translation, episode);
  }

  if (local.has_value()) {
    std::printf("\n  %s playing the downloaded copy of ep %s (%s) in mpv%s\n", g(gl.play),
                strip_controls(episode).c_str(), std::string(to_string(translation)).c_str(),
                g(gl.ellipsis));
  } else {
    std::printf("\n  %s resolving ep %s (%s) and launching mpv%s\n", g(gl.play),
                strip_controls(episode).c_str(), std::string(to_string(translation)).c_str(),
                g(gl.ellipsis));
  }
  flush_stdout();

  // player::play collapses a resolve ProviderError into a PlayError::resolve
  // detail string, so capture the class here to render the per-class resolve
  // copy (cli::player_failure_line's resolve_class arm).
  std::optional<ProviderError::Kind> resolve_class;
  player::ResolveFn resolve = [&]() -> Result<StreamLink, std::string> {
    auto link = provider.resolve(show.provider_id, episode, translation, env.quality);
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
    return false;
  }

  // A meaningful watch returns Ok with a position even when mpv then exits
  // badly; persist it. record_finish shuts its own gate on a None position, so
  // an empty watch writes nothing; and it counts the play (library, status,
  // the trackers) only past half the episode, so a short check leaves just
  // its resume point.
  if (env.store != nullptr && show.anilist_id.has_value() && outcome->position.has_value()) {
    const player::Position& pos = *outcome->position;
    (void)env.store->record_finish(*show.anilist_id, translation, episode, episode_index, pos.secs,
                                   pos.duration.value_or(0.0), provider.name(), unix_now());
  }
  std::printf("\n  %s done.\n", g(gl.ok));
  return true;
}

// The play loop from `start`. With `range_last`, the span plays in order and
// the run ends after it (exit 0), no questions asked. Without, every played
// episode ends in the post-play menu through the pick seam: next / replay /
// previous move `idx`, quit (or EOF, or a pick past the rows) ends the run.
// A play that fails ends the run with 1 wherever it happens.
int play_from(const PlayEnv& env, const BoundShow& show, const std::vector<std::string>& episodes,
              std::size_t start, std::optional<std::size_t> range_last, const PickFn& pick) {
  std::size_t idx = start;
  if (range_last.has_value()) {
    for (; idx <= *range_last; ++idx) {
      if (!play_one(env, show, episodes, idx)) return 1;
    }
    return 0;
  }
  for (;;) {
    if (!play_one(env, show, episodes, idx)) return 1;
    const cli::PostPlayMenu menu = cli::post_play_menu(idx, episodes);
    std::printf("\n");
    auto choice = pick("what next", menu.rows);
    if (!choice.has_value() || *choice >= menu.actions.size()) {
      std::printf("  bye.\n");
      return 0;
    }
    switch (menu.actions[*choice]) {
      case cli::PostPlay::Next:
        ++idx;
        break;
      case cli::PostPlay::Previous:
        --idx;
        break;
      case cli::PostPlay::Replay:
        break;
      case cli::PostPlay::Quit:
        std::printf("  bye.\n");
        return 0;
    }
  }
}

// The hit among `hits` that is this library show: the one carrying its
// AniList id, else its MAL id (when the row has one); nullopt = none says.
std::optional<std::size_t> hit_for_show(const std::vector<SearchHit>& hits, const Show& show) {
  for (std::size_t i = 0; i < hits.size(); ++i) {
    if (hits[i].anilist_id.has_value() && *hits[i].anilist_id == show.enrichment.anilist_id) {
      return i;
    }
  }
  if (show.enrichment.mal_id.has_value()) {
    for (std::size_t i = 0; i < hits.size(); ++i) {
      if (hits[i].mal_id.has_value() && *hits[i].mal_id == *show.enrichment.mal_id) return i;
    }
  }
  return std::nullopt;
}

// The stored binding to ask first for a continued show: the `-p` source when
// it is bound (and only that — the flag names the source, a stored one does
// not overrule it); else the config's preference when bound; else the first
// bound source in registry order. nullptr = no usable binding.
const StreamProvider* bound_source(const ProviderRegistry& registry,
                                   const std::vector<Binding>& bindings,
                                   const std::optional<std::string>& flag, std::string_view pref,
                                   std::string& provider_id) {
  auto bound_to = [&](const StreamProvider* p) -> const StreamProvider* {
    if (p == nullptr) return nullptr;
    for (const Binding& b : bindings) {
      if (b.provider == p->name()) {
        provider_id = b.provider_id;
        return p;
      }
    }
    return nullptr;
  };
  if (flag.has_value()) return bound_to(registry.by_name(*flag));
  if (const StreamProvider* p = bound_to(registry.by_name(pref)); p != nullptr) return p;
  for (std::size_t i = 0; i < registry.size(); ++i) {
    if (const StreamProvider* p = bound_to(registry.at(i)); p != nullptr) return p;
  }
  return nullptr;
}

}  // namespace

int play_flow(const Sources& sources, const PickFn& pick, Translation translation,
              const Config& config, const std::string& cache_dir, const std::string& runtime_dir,
              const std::string& download_dir, Store* store, const cli::PlayArgs& args) {
  const std::int64_t now = unix_now();

  const SearchWalk walked = search_walk(sources, args.query, translation);
  if (walked.provider == nullptr) return walked.exit_code;

  auto idx = choose_show(walked.hits, translation, args.show, pick);
  if (!idx.has_value()) return 0;
  const BoundShow show = bind_hit(*walked.provider, walked.hits[*idx]);
  warm_binding(store, show, now);

  auto episodes_r = load_episodes(store, show, translation, now);
  if (!episodes_r.has_value()) {
    std::printf("%s", cli::fetch_error_line(cli::FetchStage::Episodes, episodes_r.error().kind,
                                            show.provider->display_name())
                          .c_str());
    return 1;
  }
  const std::vector<std::string>& episodes = *episodes_r;
  if (episodes.empty()) {
    std::printf("\n  no %s episodes listed for this show.\n",
                std::string(to_string(translation)).c_str());
    return 0;
  }

  // Where to start, and whether it is a span: `-r`, `-e`, or the prompt.
  std::size_t start = 0;
  std::optional<std::size_t> range_last;
  if (args.range.has_value()) {
    auto span = cli::range_indices(episodes, *args.range);
    if (!span.has_value()) {
      std::printf("\n  the span %s-%s isn't in this show's list (%zu episodes).\n",
                  strip_controls(args.range->first).c_str(),
                  args.range->last.has_value() ? strip_controls(*args.range->last).c_str() : "",
                  episodes.size());
      return 0;
    }
    start = span->first;
    range_last = span->second;
  } else if (args.episode.has_value()) {
    auto ep = named_episode(episodes, *args.episode);
    if (!ep.has_value()) return 0;
    start = *ep;
  } else {
    auto ep = prompt_episode(episodes, pick);
    if (!ep.has_value()) return 0;
    start = *ep;
  }

  PlayEnv env;
  env.config = &config;
  env.cache_dir = &cache_dir;
  env.runtime_dir = &runtime_dir;
  env.download_dir = &download_dir;
  env.store = store;
  env.translation = translation;
  env.quality = run_quality(args.quality, config);
  return play_from(env, show, episodes, start, range_last, pick);
}

int download_flow(const Sources& sources, const PickFn& pick,
                  Translation translation, const Config& config,
                  const std::string& download_dir, Store* store,
                  const cli::DownloadArgs& args) {
  const std::int64_t now = unix_now();
  const cli::Glyphs& gl = cli::glyphs();

  const SearchWalk walked = search_walk(sources, args.query, translation);
  if (walked.provider == nullptr) return walked.exit_code;

  auto idx = choose_show(walked.hits, translation, args.show, pick);
  if (!idx.has_value()) return 0;
  const BoundShow show = bind_hit(*walked.provider, walked.hits[*idx]);
  const StreamProvider& provider = *show.provider;
  // Same best-effort bind as play_flow: a download is a first-class library
  // touch (the show becomes resumable/locatable later).
  warm_binding(store, show, now);

  auto episodes_r = load_episodes(store, show, translation, now);
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
  std::optional<std::size_t> ep_idx = args.episode.has_value()
                                          ? named_episode(episodes, *args.episode)
                                          : prompt_episode(episodes, pick);
  if (!ep_idx.has_value()) return 0;
  const std::string& episode = episodes[*ep_idx];

  const Quality quality = run_quality(args.quality, config);
  std::printf("\n  %s resolving ep %s (%s)%s\n", g(gl.fetch), strip_controls(episode).c_str(),
              std::string(to_string(translation)).c_str(), g(gl.ellipsis));
  flush_stdout();

  auto link = provider.resolve(show.provider_id, episode, translation, quality);
  if (!link.has_value()) {
    std::printf("%s", cli::fetch_error_line(cli::FetchStage::Resolve, link.error().kind,
                                            provider.display_name())
                          .c_str());
    return 1;
  }

  // The download client rides the same A5 wrapper the provider used.
  auto client = http::Client::create();
  if (!client.has_value()) {
    std::printf("  %s couldn't set up the download client.\n", g(gl.fail));
    return 1;
  }

  const std::int64_t dest_id = show.anilist_id.value_or(0);  // 0 = play-only shows.
  const std::string dest =
      download::episode_dest(download_dir, dest_id, translation, episode, link->url);
  if (auto md = download::ensure_parent_dirs(dest); !md.has_value()) {
    std::printf("  %s couldn't create %s (%s).\n", g(gl.fail), download_dir.c_str(),
                md.error().detail.c_str());
    return 1;
  }

  std::printf("  %s downloading to %s\n", g(gl.fetch), dest.c_str());
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
        std::printf("  %s disk error: %s\n", g(gl.fail), outcome.error().detail.c_str());
        break;
      case K::Cancelled:
        std::printf("  cancelled; the partial .part resumes on re-run.\n");
        break;
      case K::UnsafeUrl:
        std::printf("  %s the stream url failed the safety check; not fetched.\n", g(gl.fail));
        break;
      case K::UnsafeArg:
        std::printf("  %s the stream carried an unsafe %s field; not fetched.\n", g(gl.fail),
                    outcome.error().detail.c_str());
        break;
      case K::FfmpegNotFound:
        std::printf("  %s this stream needs ffmpeg to download (%s not found).\n"
                    "    install ffmpeg, or set ffmpeg_path in config.json.\n",
                    g(gl.fail), config.ffmpeg_path.c_str());
        break;
      case K::Ffmpeg:
        std::printf("  %s ffmpeg failed (%s); nothing was published.\n", g(gl.fail),
                    outcome.error().detail.c_str());
        break;
    }
    return 1;
  }

  std::printf("  %s saved %s\n", g(gl.ok), dest.c_str());
  return 0;
}

int continue_flow(const ProviderRegistry& registry, const Sources& sources, const PickFn& pick,
                  Translation translation, const Config& config, const std::string& cache_dir,
                  const std::string& runtime_dir, const std::string& download_dir, Store& store,
                  const cli::ContinueArgs& args) {
  const std::int64_t now = unix_now();
  const cli::Glyphs& gl = cli::glyphs();

  auto history_r = store.list_history();
  if (!history_r.has_value()) {
    std::printf("  %s couldn't read your library (%s).\n", g(gl.fail),
                history_r.error().detail.c_str());
    return 1;
  }
  const std::vector<Show>& history = *history_r;
  if (history.empty()) {
    std::printf("\n  your history is empty; play something first.\n");
    return 0;
  }

  // The list: the whole history, or the rows a query names.
  const std::vector<std::size_t> matches =
      cli::history_matches(history, args.query.has_value() ? std::string_view(*args.query)
                                                            : std::string_view{});
  if (matches.empty()) {
    std::printf("\n  nothing in your history matches \"%s\".\n",
                strip_controls(*args.query).c_str());
    return 0;
  }
  std::vector<Show> listed;
  listed.reserve(matches.size());
  for (std::size_t i : matches) listed.push_back(history[i]);
  const std::vector<std::string> rows = cli::history_rows(listed);

  std::size_t chosen = 0;
  if (args.show.has_value()) {
    if (*args.show == 0 || *args.show > listed.size()) {
      std::printf("\n  %zu show(s) in the list; there is no #%u to take.\n", listed.size(),
                  static_cast<unsigned>(*args.show));
      return 0;
    }
    chosen = *args.show - 1;
    std::printf("\n  taking #%u: %s\n", static_cast<unsigned>(*args.show), rows[chosen].c_str());
  } else if (listed.size() == 1) {
    std::printf("\n  %s\n", rows[0].c_str());
  } else {
    std::printf("\n  %zu show(s):\n\n", listed.size());
    auto idx = pick("continue which show", rows);
    if (!idx.has_value() || *idx >= listed.size()) {
      std::printf("  bye.\n");
      return 0;
    }
    chosen = *idx;
  }
  const Show& show = listed[chosen];
  const std::int64_t anilist_id = show.enrichment.anilist_id;

  // The source: a stored binding whose episodes still answer, else a fresh
  // search by title. Either way the run stays keyed to the library row.
  std::optional<BoundShow> bound;
  std::vector<std::string> episodes;
  {
    std::vector<Binding> bindings;
    if (auto b = store.bindings_for(anilist_id); b.has_value()) bindings = std::move(*b);
    std::string provider_id;
    const StreamProvider* p =
        bound_source(registry, bindings, args.provider, config.preferred_provider, provider_id);
    if (p != nullptr) {
      BoundShow b;
      b.provider = p;
      b.provider_id = provider_id;
      b.title = strip_controls(show.enrichment.title_romaji);
      b.anilist_id = anilist_id;
      b.mal_id = show.enrichment.mal_id;
      b.total_episodes = show.enrichment.total_episodes;
      auto eps = load_episodes(&store, b, translation, now);
      if (eps.has_value() && !eps->empty()) {
        bound = std::move(b);
        episodes = std::move(*eps);
      } else {
        std::printf("  (%s %s; searching for it again%s)\n", std::string(p->display_name()).c_str(),
                    eps.has_value() ? "lists no episodes" : "didn't answer", g(gl.ellipsis));
        flush_stdout();
      }
    }
  }
  if (!bound.has_value()) {
    const SearchWalk walked = search_walk(sources, show.enrichment.title_romaji, translation);
    if (walked.provider == nullptr) return walked.exit_code;
    std::optional<std::size_t> idx = hit_for_show(walked.hits, show);
    if (!idx.has_value() && walked.hits.size() == 1) idx = 0;
    if (!idx.has_value()) idx = choose_show(walked.hits, translation, std::nullopt, pick);
    if (!idx.has_value()) return 0;
    BoundShow b = bind_hit(*walked.provider, walked.hits[*idx]);
    // The library row is the identity: its ids and title, whatever the hit
    // claimed (a hit with no AniList id would otherwise leave the run
    // play-only, and a different one would split the history).
    b.anilist_id = anilist_id;
    if (show.enrichment.mal_id.has_value()) b.mal_id = show.enrichment.mal_id;
    b.title = strip_controls(show.enrichment.title_romaji);
    warm_binding(&store, b, now);
    auto eps = load_episodes(&store, b, translation, now);
    if (!eps.has_value()) {
      std::printf("%s", cli::fetch_error_line(cli::FetchStage::Episodes, eps.error().kind,
                                              b.provider->display_name())
                            .c_str());
      return 1;
    }
    if (eps->empty()) {
      std::printf("\n  no %s episodes listed for this show.\n",
                  std::string(to_string(translation)).c_str());
      return 0;
    }
    bound = std::move(b);
    episodes = std::move(*eps);
  }

  // Where to pick up: the freshest partial watch when there is one, else the
  // episode after the last one finished (progress is a 1-based ordinal into
  // the sorted list, so it is also the 0-based index of the next).
  std::optional<std::size_t> start;
  if (auto partial = store.latest_resume(anilist_id, translation);
      partial.has_value() && partial->has_value()) {
    const std::string& label = (*partial)->first;
    for (std::size_t i = 0; i < episodes.size(); ++i) {
      if (episodes[i] == label) {
        start = i;
        break;
      }
    }
  }
  if (!start.has_value()) {
    if (show.progress < episodes.size()) {
      start = show.progress;
    } else {
      std::printf("\n  you're caught up: all %zu episode(s) watched.\n", episodes.size());
      return 0;
    }
  }
  std::printf("\n  continuing \"%s\" at ep %s\n", bound->title.c_str(),
              strip_controls(episodes[*start]).c_str());

  PlayEnv env;
  env.config = &config;
  env.cache_dir = &cache_dir;
  env.runtime_dir = &runtime_dir;
  env.download_dir = &download_dir;
  env.store = &store;
  env.translation = translation;
  env.quality = run_quality(args.quality, config);
  return play_from(env, *bound, episodes, *start, std::nullopt, pick);
}

}  // namespace shigoku::cli_play
