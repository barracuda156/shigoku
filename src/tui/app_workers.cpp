// Detached worker threads: each posts one owned Event back to the queue.

#include "app_workers.hpp"

#include <sys/stat.h>

#include <chrono>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include "../anilist.hpp"
#include "../aniskip.hpp"
#include "../debug_log.hpp"
#include "../download.hpp"
#include "../http.hpp"
#include "../login.hpp"
#include "../loopback.hpp"
#include "../mal_catalog.hpp"
#include "../mal_login.hpp"
#include "../mal_loopback.hpp"
#include "../mal_mirror.hpp"
#include "../mal_refresh.hpp"
#include "../player.hpp"
#include "../provider.hpp"
#include "../store.hpp"
#include "../sync.hpp"
#include "../sync_cycle.hpp"
#include "../updatecheck.hpp"
#include "covers.hpp"
#include "resolve_transport.hpp"

namespace shigoku::tui {

// ---------------------------------------------------------------------------
// P7 worker spawns. Each captures its inputs by value (query strings, ids,
// generation tokens) and posts an owned Event back via `queue` — never touches
// `app` from the worker thread (A1). Detached, never joined (04 §6/§13).
// ---------------------------------------------------------------------------

// `/` search: goes through AppDeps::search, which main.cpp wires to the real
// anilist::search (AniList is the catalog client, never a StreamProvider, per
// anilist.hpp's header comment) — the indirection exists solely so an offline
// test can inject canned rows (app.hpp's SearchFn doc comment). Staleness is
// by query-string compare (04 §6 table), so SearchDone carries the query and
// no generation.
// The copy for a catalog failure: the dispatcher's composed detail when it
// set one ("AniList blocked us (403); MAL: no client id"), else the bare
// kind copy.
std::string catalog_cause(const ProviderError& e) {
  if (!e.detail.empty()) return e.detail;
  return std::string(provider_error_copy(e.kind));
}

void spawn_search(EventQueue& queue, const SearchFn& search, std::string query,
                  std::uint32_t page) {
  spawn_detached([&queue, &search, query = std::move(query), page]() mutable {
    auto p = search(query, page);
    if (!p.has_value()) {
      queue.try_post(Event{SearchError{p.error(), query}});
      return;
    }
    queue.try_post(
        Event{SearchDone{std::move(p->first), std::move(query), page, p->second}});
  });
}

namespace {

// login::ConnectResult -> the event-safe ConnectOutcome (event.hpp stays off
// login.hpp's link graph; see event.hpp's ConnectDone comment).
ConnectDone to_connect_done(const login::ConnectResult& r) {
  using K = login::ConnectResult::Kind;
  switch (r.kind) {
    case K::Ok: return ConnectDone{ConnectOutcome::Ok, r.user_name};
    case K::NoToken: return ConnectDone{ConnectOutcome::NoToken, {}};
    case K::Rejected: return ConnectDone{ConnectOutcome::Rejected, {}};
    case K::NetworkError: return ConnectDone{ConnectOutcome::NetworkError, {}};
    case K::SaveFailed: return ConnectDone{ConnectOutcome::SaveFailed, {}};
    case K::BadState: return ConnectDone{ConnectOutcome::BadState, {}};
    case K::Canceled: return ConnectDone{ConnectOutcome::Canceled, {}};
  }
  return ConnectDone{ConnectOutcome::NoToken, {}};  // unreachable (closed enum).
}

}  // namespace

// AniList connect (P19, 06 §4): owns the Loopback (moved in) and runs the
// blocking accept/serve loop entirely off the UI thread (A1) — start()'s fast
// local bind already happened on the UI thread in open_connect(). Builds its
// own login::Verifier over AppDeps::http (the worker never touches App).
// Posts exactly one ConnectDone — UNLESS the result is Canceled, matching
// login.rs's worker: the app may already be moving on from the session.
void spawn_connect(EventQueue& queue, const http::Client& http, loopback::Loopback loopback,
                   std::string auth_path, std::int64_t now) {
  spawn_detached([&queue, &http, loopback = std::move(loopback), auth_path = std::move(auth_path),
                  now]() mutable {
    const login::Verifier verifier = login::make_anilist_verifier(http);
    bool warned = false;
    const login::ConnectResult result = loopback.serve(verifier, auth_path, now, [&warned] {
      if (!warned) {
        debug_log("connect: ignoring callback(s) with a bad state");
        warned = true;
      }
    });
    if (result.kind != login::ConnectResult::Kind::Canceled) {
      queue.try_post(Event{to_connect_done(result)});
    }
  });
}

namespace {

MalConnectDone to_mal_connect_done(const mal_login::MalConnectResult& r) {
  using K = mal_login::MalConnectResult::Kind;
  switch (r.kind) {
    case K::Ok: return MalConnectDone{MalConnectOutcome::Ok};
    case K::NoClientId: return MalConnectDone{MalConnectOutcome::NoClientId};
    case K::NoCode: return MalConnectDone{MalConnectOutcome::NoCode};
    case K::Rejected: return MalConnectDone{MalConnectOutcome::Rejected};
    case K::NetworkError: return MalConnectDone{MalConnectOutcome::NetworkError};
    case K::SaveFailed: return MalConnectDone{MalConnectOutcome::SaveFailed};
    case K::BadState: return MalConnectDone{MalConnectOutcome::BadState};
    case K::Canceled: return MalConnectDone{MalConnectOutcome::Canceled};
  }
  return MalConnectDone{MalConnectOutcome::NoCode};  // unreachable (closed enum).
}

}  // namespace

void spawn_mal_connect(EventQueue& queue, const http::Client& http,
                       mal_loopback::Loopback loopback, std::string auth_path, std::int64_t now) {
  spawn_detached([&queue, &http, loopback = std::move(loopback), auth_path = std::move(auth_path),
                  now]() mutable {
    const mal_login::Exchanger exchanger = mal_login::make_http_exchanger(http);
    bool warned = false;
    const mal_login::MalConnectResult result =
        loopback.serve(exchanger, auth_path, now, [&warned] {
          if (!warned) {
            debug_log("mal connect: ignoring callback(s) with a bad state");
            warned = true;
          }
        });
    if (result.kind != mal_login::MalConnectResult::Kind::Canceled) {
      queue.try_post(Event{to_mal_connect_done(result)});
    }
  });
}

namespace {

// sync::SyncSummary -> the event-safe SyncFlushed (event.hpp stays off
// sync.hpp's link graph; see event.hpp's SyncFlushed comment). Distinct enums
// with identical enumerator names, bridged by an explicit switch — same shape
// as to_connect_done. `mal_pushed` rides along flattened the same way (P31
// §9.1 slice 4) — the MAL mirror's own outcome is deliberately not surfaced
// here (A1: it gets no drain rights and no toast of its own; a push_failed/
// push_skipped row simply stays dirty for next run, silently, same as the
// AniList mirror's own quieter outcomes).
SyncFlushed to_sync_flushed(const sync::SyncSummary& s, std::uint32_t mal_pushed,
                            const mal_mirror::MirrorPullSummary& mal_pull,
                            std::uint32_t airing_refreshed, bool mal_token_unsaved) {
  using K = sync::SyncOutcome;
  SyncOutcome outcome;
  switch (s.outcome) {
    case K::Completed:         outcome = SyncOutcome::Completed; break;
    case K::Disabled:          outcome = SyncOutcome::Disabled; break;
    case K::NoToken:           outcome = SyncOutcome::NoToken; break;
    case K::Expired:           outcome = SyncOutcome::Expired; break;
    case K::NoUserId:          outcome = SyncOutcome::NoUserId; break;
    case K::PullUnauthorized:  outcome = SyncOutcome::PullUnauthorized; break;
    case K::PullRateLimited:   outcome = SyncOutcome::PullRateLimited; break;
    case K::Unauthorized:      outcome = SyncOutcome::Unauthorized; break;
    case K::RateLimited:       outcome = SyncOutcome::RateLimited; break;
    case K::Failed:            outcome = SyncOutcome::Failed; break;
    default:                   outcome = SyncOutcome::Failed;  // unreachable (closed enum).
  }
  SyncFlushed out{outcome, s.pulled.imported, s.pulled.reconciled, s.pushed, mal_pushed};
  using M = mal_mirror::MirrorOutcome;
  if (mal_pull.outcome == M::Completed) {
    out.mal_pull_failed = mal_pull.pull_failed;
    out.mal_pulled = mal_pull.pulled.reconciled;
    out.mal_imported = mal_pull.pulled.imported;
  } else {
    // Disabled (no MAL account) is silence; a rate-limited pull is a failed
    // one for the toast's purposes; a token MAL would not take, with no
    // refresh to mend it, is its own word.
    out.mal_unauthorized = mal_pull.outcome == M::Unauthorized;
    out.mal_pull_failed = mal_pull.outcome == M::RateLimited;
  }
  out.mal_token_rolled = mal_pull.rolled.has_value();
  out.mal_token_unsaved = out.mal_token_rolled && mal_token_unsaved;
  out.airing_refreshed = airing_refreshed;
  return out;
}

}  // namespace

// Sync worker (P20, 04 §4.6): opens its OWN store connection (store.hpp's
// documented exception) — the CAS-guarded reconcile (06 §5.4) is built for
// exactly this cross-connection concurrency with the UI thread. Reuses
// AppDeps::http (like spawn_connect — that client is process-lifetime, safe
// to reference from a detached worker), but never AppDeps::store: the sync
// worker's whole reason to exist is running off a connection the UI thread
// isn't touching.
void spawn_sync(EventQueue& queue, const http::Client& http, std::string db_path,
                std::string auth_path, std::string mal_client_id, Auth auth, bool enabled,
                bool pull_only, std::int64_t now) {
  spawn_detached([&queue, &http, db_path = std::move(db_path), auth_path = std::move(auth_path),
                  mal_client_id = std::move(mal_client_id), auth = std::move(auth), enabled,
                  pull_only, now]() mutable {
    // The whole run (mirror pull, AniList, mirror push, airing stamps) on this
    // worker's own store connection. A renewed MAL pair is saved to the auth
    // file inside it; the UI thread's own Auth copy follows via the event
    // (on_sync_flushed). A store that will not open posts the Failed /
    // Disabled defaults.
    sync::CycleResult ran;
    auto store = Store::open(db_path);
    if (store.has_value()) {
      sync::CycleOptions opt;
      opt.enabled = enabled;
      opt.pull_only = pull_only;
      opt.now = now;
      opt.auth_path = auth_path;
      ran = sync::run_cycle(http, std::move(mal_client_id), *store, auth, opt);
      if (ran.mal_token_unsaved) debug_log("mal refresh: renewed the token but couldn't save it");
    }
    queue.try_post(Event{to_sync_flushed(
        ran.anilist.value_or(sync::SyncSummary::terminal(sync::SyncOutcome::Failed)),
        ran.mal_pushed,
        ran.mal_pull.value_or(
            mal_mirror::MirrorPullSummary::terminal(mal_mirror::MirrorOutcome::Disabled)),
        ran.airing_refreshed, ran.mal_token_unsaved)});
  });
}

// One-shot boot-time check (04 §4.6-shaped spawn, matching spawn_sync):
// resolves the network off the UI thread and posts UpdateAvailable only on a
// confirmed-newer tag. Every other outcome (disabled, offline, rate-limited,
// no cache dir) posts nothing — silent by design (06 §6.1).
void spawn_update_check(EventQueue& queue, const http::Client& http, std::string cache_dir,
                        std::string current_version, std::int64_t now) {
  spawn_detached([&queue, &http, cache_dir = std::move(cache_dir),
                  current_version = std::move(current_version), now]() {
    auto latest = updatecheck::check(http, cache_dir, current_version, now);
    if (latest.has_value()) queue.try_post(Event{UpdateAvailable{*latest}});
  });
}

// Quit flush worker (P20, 04 §11): push only, no pull, NoSleep (teardown's own
// deadline is the only budget it gets). Signals completion via a heap-shared
// atomic, NOT the event queue — the queue is about to be shut down and has no
// reader left to act on a summary at quit anyway. Deliberately takes no
// EventQueue& (unlike every other spawn_* here): do not "fix" this to match
// the pattern.
void spawn_quit_flush_worker(const http::Client& http, std::string db_path, Auth auth,
                             bool enabled, std::int64_t now,
                             std::shared_ptr<std::atomic<bool>> done) {
  spawn_detached([&http, db_path = std::move(db_path), auth = std::move(auth), enabled, now,
                  done = std::move(done)]() mutable {
    auto store = Store::open(db_path);
    if (store.has_value()) {
      const sync::HttpAniListSync ani(http);
      (void)sync::flush_push(ani, auth, *store, now, enabled, sync::no_sleep);
      // P31 §9.1 slice 4: rides the SAME teardown budget, no extra drain
      // rights (A1) — this is the whole reason the plan puts the mirror
      // behind the AniList flush's cadence rather than a worker of its own.
      const mal_mirror::HttpMalMirrorClient mal_client(http);
      (void)mal_mirror::push_mirror(mal_client, auth.mal, *store, auth.mal.bearer().has_value());
    }
    done->store(true, std::memory_order_release);
  });
}

// Discover feed fetch (P17): goes through AppDeps::discover (main.cpp wires the
// real anilist::discover; offline tests inject canned pages). Staleness is
// POSITIONAL within one filter set — the axis + page carry identity, on_feed
// discards a page that is not slot.page+1 — plus the filter generation `gen`
// (P38 review): a page spawned under the old filters answers with its own
// gen and self-discards after a set_filters bump. A spawn failure resets the
// slot's loading arm (the caller passed the arm already set); this helper
// only owns the detach and posts DiscoverFeedDone/Error.
void spawn_discover_feed(EventQueue& queue, const DiscoverFn& discover, DiscoverAxis axis,
                         std::uint32_t page, const DiscoverFilters& filters,
                         std::uint32_t gen) {
  spawn_detached([&queue, &discover, axis, page, filters, gen]() {
    auto res = discover(axis, page, filters);
    if (!res.has_value()) {
      queue.try_post(
          Event{DiscoverFeedError{axis, catalog_cause(res.error()), gen, std::nullopt}});
      return;
    }
    auto& [rows, has_next] = *res;
    queue.try_post(
        Event{DiscoverFeedDone{axis, page, std::move(rows), has_next, gen, std::nullopt}});
  });
}

// The Similar feed's fetch: spawn_discover_feed's twin, keyed by the seed show
// and the Similar feed's own generation.
void spawn_similar_feed(EventQueue& queue, const SimilarFn& similar, SimilarSeed seed,
                        std::uint32_t page, const DiscoverFilters& filters, std::uint32_t gen) {
  spawn_detached([&queue, &similar, seed = std::move(seed), page, filters, gen]() {
    auto res = similar(seed.anilist_id, seed.mal_id, page, filters);
    if (!res.has_value()) {
      DiscoverFeedError e;
      e.cause = catalog_cause(res.error());
      e.gen = gen;
      e.similar_to = seed.anilist_id;
      queue.try_post(Event{std::move(e)});
      return;
    }
    auto& [rows, has_next] = *res;
    DiscoverFeedDone d;
    d.page = page;
    d.rows = std::move(rows);
    d.has_next = has_next;
    d.gen = gen;
    d.similar_to = seed.anilist_id;
    queue.try_post(Event{std::move(d)});
  });
}

// Enter on a focused catalog row: A8 degenerate walk (classify_tier) then
// episodes(). Tier-C "best hit" scoring is deferred (A8 explicitly licenses
// first-hit + toast for v0); the toast fires from the UI thread when
// EpisodesDone/Error lands, so the worker itself stays silent on the tier-C
// fallback (04 §6: workers post only Event, never toast directly).
void spawn_resolve_episodes(EventQueue& queue, const ProviderRegistry& registry,
                            Enrichment show, Generation gen) {
  const std::int64_t for_id = show.anilist_id;
  spawn_detached([&queue, &registry, show = std::move(show), for_id, gen]() {
    const StreamProvider* provider = registry.primary();
    if (provider == nullptr) {
      queue.try_post(Event{EpisodesError{ProviderError::unsupported(), for_id, gen, {}}});
      return;
    }
    const std::string pname(provider->name());
    std::string provider_id;
    if (auto key = provider->canonical_key(show); key.has_value()) {
      provider_id = std::move(*key);  // Tier::AKey.
    } else {
      // Tier::CSearch — search by title, take the first hit (A8 v0 license).
      SearchOptions opts;
      auto hits = provider->search(show.title_romaji, opts);
      if (!hits.has_value()) {
        queue.try_post(Event{EpisodesError{hits.error(), for_id, gen, pname}});
        return;
      }
      if (hits->empty()) {
        // Authoritative not-stocked (03 §4.3): Ok(empty) from episodes() is
        // how the grid renders "no source"; mirror that via an empty list.
        queue.try_post(Event{EpisodesDone{{}, for_id, gen, pname, {}}});
        return;
      }
      provider_id = hits->front().provider_id;
    }
    auto eps = provider->episodes(provider_id, Translation::Sub, std::nullopt);
    if (!eps.has_value()) {
      queue.try_post(Event{EpisodesError{eps.error(), for_id, gen, pname}});
      return;
    }
    // Carry the id the grid was fetched under: play reuses it verbatim (a
    // Tier-C provider_id is unrecoverable at play time — see EpisodesDone).
    queue.try_post(
        Event{EpisodesDone{std::move(*eps), for_id, gen, pname, std::move(provider_id)}});
  });
}

// Enter on a focused episode grid cell: resolve + play via P5's player::play —
// unless a completed local download for the exact (show, track, ep) exists, in
// which case player::play_local skips the resolve pipeline entirely (P35
// slice 4). A non-empty `local_file` is played as it is, nothing else tried. player::play() is blocking and calls `resolve` once per retry
// attempt itself; the inner lambda wraps provider->resolve into ResolveFn.
void spawn_play(EventQueue& queue, const ProviderRegistry& registry,
                const AppDeps& deps, std::string provider_name, std::string provider_id,
                std::string episode, std::int64_t for_id, double start_secs,
                Translation translation, Quality quality,
                std::optional<std::int64_t> mal_id, std::string show_title,
                std::uint32_t episode_ordinal, aniskip::SkipMode skip_mode,
                player::BackendKind backend, std::string player_path,
                std::string local_file) {
  spawn_detached([&queue, &registry, &deps, provider_name = std::move(provider_name),
                  provider_id = std::move(provider_id),
                  episode = std::move(episode), for_id, start_secs, translation, quality,
                  mal_id, show_title = std::move(show_title), episode_ordinal, skip_mode,
                  backend, player_path = std::move(player_path),
                  local_file = std::move(local_file)]() {
    player::EventSink on_event = [&queue, for_id, &episode](const player::PlayerEvent& pe) {
      using K = player::PlayerEvent::Kind;
      switch (pe.kind) {
        case K::Position:
          queue.try_post(Event{PositionUpdate{
              pe.pos.secs, pe.pos.duration.value_or(0.0), for_id, episode}});
          return;
        case K::Retry:
          queue.try_post(Event{PlayRetry{pe.attempt, player::kMaxPlayAttempts}});
          return;
      }
    };
    player::PlayOpts opts;
    opts.mpv_path = deps.mpv_path;
    // P39: backend + alternate path ride the call (read from app.config at
    // the call site, like translation/quality) so a Settings wheel flip
    // applies to the very next play — no restart, unlike the deps snapshot.
    opts.backend = backend;
    opts.player_path = player_path;
    opts.socket_dir = deps.socket_dir;
    opts.title = episode;
    opts.start_secs = start_secs;
    opts.quality = quality;
    // AniSkip prepare (P22, 03 §9): best-effort, runs here (the playback
    // worker) rather than the UI thread. Any failure (no MAL id incl. Jikan
    // miss, network down, unwritable cache) collapses to plain play.
    if (!deps.aniskip_cache_dir.empty()) {
      const std::uint32_t ep_num = aniskip::episode_number(episode, episode_ordinal);
      auto adjuncts =
          aniskip::prepare_all(mal_id, show_title, ep_num, skip_mode, deps.aniskip_cache_dir);
      opts.skip = std::move(adjuncts.skip);
      opts.chapters_file = std::move(adjuncts.chapters_file);
    }
    auto outcome = [&]() -> Result<player::PlayOutcome, PlayError> {
      // A file named outright (the Transfers view's Enter) is what plays: no
      // scan for it, no source asked.
      if (!local_file.empty()) {
        debug_log("worker play: named file " + local_file);
        return player::play_local(opts, local_file, on_event);
      }
      // Play-prefers-local (P35 slice 4): a completed download for this exact
      // (show, track, ep) short-circuits the whole resolve pipeline — no
      // provider, no network (downloads keep working when every provider is
      // down). This preference IS the PPC story: download once, play a file.
      if (!deps.download_dir.empty()) {
        if (auto local = download::find_local_episode(deps.download_dir, for_id,
                                                      translation, episode)) {
          debug_log("worker play: local file " + *local);
          return player::play_local(opts, *local, on_event);
        }
      }
      // Resolve on the SERVING provider (P15), never primary(): a hop can land
      // the grid on a non-primary provider and its provider_id only resolves
      // there. Empty name = the degraded P7 first-hit path, where primary() is
      // the one provider the grid was fetched under.
      const StreamProvider* provider =
          provider_name.empty() ? registry.primary() : registry.by_name(provider_name);
      if (provider == nullptr) return err(PlayError::resolve("no provider"));
      player::ResolveFn resolve = [provider, &provider_id, &episode, translation,
                                   quality]() -> Result<StreamLink, std::string> {
        auto link = provider->resolve(provider_id, episode, translation, quality);
        if (!link.has_value()) {
          return err(std::string(provider_error_copy(link.error().kind)));
        }
        return *link;
      };
      debug_log("worker play: calling player::play ep=" + episode);
      return player::play(opts, resolve, on_event);
    }();
    if (!outcome.has_value()) {
      const bool posted =
          queue.try_post(Event{PlayErrorEvent{outcome.error(), true, for_id, episode}});
      debug_log("worker play: error posted=" + std::to_string(posted));
      return;
    }
    const double final_pos =
        outcome->position.has_value() ? outcome->position->secs : 0.0;
    const bool posted = queue.try_post(Event{PlayDone{final_pos, for_id, episode}});
    debug_log("worker play: PlayDone pos=" + std::to_string(final_pos) +
              " posted=" + std::to_string(posted));
  });
}

namespace {

// download::DownloadError -> the dependency-free event payload (the
// ConnectOutcome boundary-conversion pattern). The detail is the short human
// fragment the toast appends.
std::pair<DownloadOutcome, std::string> download_failed_payload(
    const download::DownloadError& e) {
  using K = download::DownloadError::Kind;
  switch (e.kind) {
    case K::Fetch:
      return {DownloadOutcome::Fetch, std::string(provider_error_copy(e.fetch.kind))};
    case K::Io:
      return {DownloadOutcome::Io, e.detail};
    case K::Cancelled:
      return {DownloadOutcome::Cancelled, {}};
    case K::UnsafeUrl:
      return {DownloadOutcome::UnsafeUrl, {}};
    case K::UnsafeArg:
      return {DownloadOutcome::UnsafeArg, e.detail};
    case K::FfmpegNotFound:
      return {DownloadOutcome::FfmpegNotFound, {}};
    case K::Ffmpeg:
      return {DownloadOutcome::Ffmpeg, e.detail};
  }
  return {DownloadOutcome::Fetch, {}};  // unreachable (closed enum).
}

}  // namespace

// One download job, off the UI thread. The stream is resolved here, when the
// job starts, never when it was queued: stream links expire. Progress is
// throttled worker-side (>=500ms between posts, so a slow pty never backs
// up) with the final tick always posted. No retry or hop law: a failed job
// rests in the queue until the user retries it (a .part resumes for free).
void spawn_download(EventQueue& queue, const ProviderRegistry& registry,
                    const http::Client* http, DownloadJob job, std::string download_dir,
                    std::string ffmpeg_path) {
  spawn_detached([&queue, &registry, http, job = std::move(job),
                  download_dir = std::move(download_dir),
                  ffmpeg_path = std::move(ffmpeg_path)]() {
    static const std::atomic<bool> kNeverCancelled{false};
    const std::atomic<bool>& cancel = job.cancel ? *job.cancel : kNeverCancelled;
    auto fail = [&](DownloadOutcome outcome, std::string detail) {
      queue.try_post(Event{DownloadFailed{job.id, outcome, std::move(detail)}});
    };
    auto fail_with = [&](const download::DownloadError& e) {
      auto [outcome, detail] = download_failed_payload(e);
      debug_log("worker download: failed kind=" + std::to_string(static_cast<int>(outcome)));
      fail(outcome, std::move(detail));
    };

    // Resolve on the job's own source (the one its grid was served by) — the
    // same rule as spawn_play.
    const StreamProvider* provider =
        job.provider.empty() ? registry.primary() : registry.by_name(job.provider);
    if (provider == nullptr || http == nullptr) {
      fail(DownloadOutcome::Fetch, "no provider");
      return;
    }
    auto link = provider->resolve(job.provider_id, job.episode, job.translation, job.quality);
    if (cancel.load(std::memory_order_acquire)) {
      fail(DownloadOutcome::Cancelled, {});
      return;
    }
    if (!link.has_value()) {
      fail(DownloadOutcome::Fetch, std::string(provider_error_copy(link.error().kind)));
      return;
    }
    if (!http::guard_fetch_url(link->url).has_value()) {
      fail(DownloadOutcome::UnsafeUrl, {});
      return;
    }
    auto planned = download::plan(*http, *link, job.rung);
    if (!planned.has_value()) {
      fail_with(planned.error());
      return;
    }
    const std::string dest = download::episode_dest(download_dir, job.show_id, job.translation,
                                                    job.episode, *link);
    if (auto md = download::ensure_parent_dirs(dest); !md.has_value()) {
      fail_with(md.error());
      return;
    }

    // What the transfer turned out to be: the rung the ladder really had,
    // and the size its bitrate suggests.
    std::optional<std::uint32_t> height = link->resolution;
    std::uint64_t estimate = 0;
    if (planned->variant.has_value()) {
      height = planned->variant->resolution;
      OfferRow row;
      row.bandwidth = planned->variant->bandwidth;
      estimate = offer_row_bytes(row, job.duration_secs).value_or(0);
    }
    queue.try_post(Event{DownloadStarted{job.id, dest, height, estimate}});

    auto last_post = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    download::ProgressFn progress = [&queue, &last_post, id = job.id](
                                        std::uint64_t bytes,
                                        std::optional<std::uint64_t> total) {
      const auto now = std::chrono::steady_clock::now();
      const bool final_tick = total.has_value() && bytes >= *total;
      if (!final_tick && now - last_post < std::chrono::milliseconds(500)) return;
      last_post = now;
      queue.try_post(Event{DownloadProgress{id, bytes, total.value_or(0)}});
    };
    debug_log("worker download: ep=" + job.episode + " dest=" + dest);
    auto r = download::run(*http, *link, *planned, dest, ffmpeg_path, cancel, progress,
                           job.child_pid.get());
    if (!r.has_value()) {
      fail_with(r.error());
      return;
    }
    debug_log("worker download: done " + dest);
    std::uint64_t size = 0;
    struct stat st{};
    if (::stat(dest.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      size = static_cast<std::uint64_t>(st.st_size);
    }
    const bool subs_missed = !download::save_subtitle(*http, *link, dest, cancel).has_value();
    if (subs_missed) debug_log("worker download: softsub not saved for " + dest);
    queue.try_post(Event{DownloadDone{job.id, dest, size, subs_missed}});
  });
}

void spawn_download_probe(EventQueue& queue, const ProviderRegistry& registry,
                          const http::Client* http, std::uint64_t token,
                          std::string provider_name, std::string provider_id,
                          std::string episode, Translation translation, Quality quality) {
  spawn_detached([&queue, &registry, http, token, provider_name = std::move(provider_name),
                  provider_id = std::move(provider_id), episode = std::move(episode),
                  translation, quality]() {
    const StreamProvider* provider =
        provider_name.empty() ? registry.primary() : registry.by_name(provider_name);
    if (provider == nullptr || http == nullptr) {
      queue.try_post(Event{DownloadOfferFailed{token, "no provider"}});
      return;
    }
    auto link = provider->resolve(provider_id, episode, translation, quality);
    if (!link.has_value()) {
      queue.try_post(Event{DownloadOfferFailed{
          token, std::string(provider_error_copy(link.error().kind))}});
      return;
    }
    if (!http::guard_fetch_url(link->url).has_value()) {
      queue.try_post(Event{DownloadOfferFailed{token, "unsafe stream url"}});
      return;
    }
    queue.try_post(Event{DownloadOfferDone{token, download::probe(*http, *link)}});
  });
}

// Cover fetch worker: covers->load() off the UI thread, posting CoverDone (the
// cropped/resized RGBA) or CoverError (cooldown path). Staleness is the
// anilist_id `for_id` + the cover generation, both stamped into the event and
// checked in tick() (04 §6). `url` is the AniList cover url; `provider` is null
// for the AniList/absolute-url path (the v0 detail cover).
void spawn_cover(EventQueue& queue, Covers& covers, std::string url,
                 std::int64_t for_id, std::uint32_t box_w, std::uint32_t box_h,
                 Generation gen) {
  spawn_detached([&queue, &covers, url = std::move(url), for_id, box_w, box_h, gen]() {
    // AniList covers are absolute urls (provider == nullptr path); senshi/M2
    // provider-relative refs would pass the provider here (deferred to M2).
    auto pixels = covers.load(/*provider=*/nullptr, url, box_w, box_h);
    if (!pixels.has_value()) {
      const bool posted = queue.try_post(Event{CoverError{for_id, gen}});
      debug_log("worker detail-cover: err posted=" + std::to_string(posted));
      return;
    }
    CoverDone done;
    done.pixels = std::move(*pixels);
    done.for_id = for_id;
    done.gen = gen;
    const bool posted = queue.try_post(Event{std::move(done)});
    debug_log("worker detail-cover: ok posted=" + std::to_string(posted));
  });
}

// Grid cover fetch worker (P17b): same pipeline as spawn_cover, but keyed on
// url (not anilist_id+generation — the 04 §4.4 adopt-by-url rule: a result
// for a scrolled-away or evicted url is still adopted).
void spawn_grid_cover(EventQueue& queue, Covers& covers, std::string url,
                      std::uint32_t box_w, std::uint32_t box_h) {
  spawn_detached([&queue, &covers, url = std::move(url), box_w, box_h]() {
    auto pixels = covers.load(/*provider=*/nullptr, url, box_w, box_h);
    if (!pixels.has_value()) {
      const bool posted = queue.try_post(Event{GridCoverError{url}});
      debug_log("worker grid-cover: err posted=" + std::to_string(posted));
      return;
    }
    GridCoverDone done;
    done.url = url;
    done.pixels = std::move(*pixels);
    const bool posted = queue.try_post(Event{std::move(done)});
    debug_log("worker grid-cover: ok posted=" + std::to_string(posted));
  });
}

// Refresh-on-view enrichment worker (P21, workers.rs spawn_enrich, 04 §5.1):
// by-id fetch off the UI thread, posting the three-state answer keyed by
// `for_id`. The EnrichFn is copied into the thread (a bound http::Client + free
// function; it outlives run()). Ok(some) -> EnrichmentRefreshed, Ok(nullopt) ->
// EnrichmentNull (a confirmed no-match is still an answer), Err ->
// EnrichmentFailed. Staleness is the `for_id` guard on the UI thread (a
// selection that moved on no longer matches the shown show) — no generation is
// needed because the answer only ever mutates the row it names.
void spawn_enrich(EventQueue& queue, EnrichFn enrich, std::int64_t for_id,
                  std::optional<std::int64_t> mal_id) {
  spawn_detached([&queue, enrich = std::move(enrich), for_id, mal_id]() {
    auto ans = enrich(for_id, mal_id);
    Event ev = [&]() -> Event {
      if (!ans.has_value()) return EnrichmentFailed{for_id};
      if (!ans->has_value()) return EnrichmentNull{for_id};
      EnrichmentRefreshed r;
      r.for_id = for_id;
      r.enrichment = std::move(**ans);
      return r;
    }();
    const bool posted = queue.try_post(Event{std::move(ev)});
    debug_log("worker enrich: for_id=" + std::to_string(for_id) +
              " posted=" + std::to_string(posted));
  });
}

// Heal worker: which AniList entry a synthetic row's MAL id belongs to. A find
// posts EnrichmentRefreshed under the row's own (synthetic) for_id carrying
// the real id, which is what tells the handler to re-key; anything else is
// EnrichmentHealMissed.
void spawn_heal(EventQueue& queue, HealFn heal, std::int64_t for_id, std::int64_t mal_id) {
  spawn_detached([&queue, heal = std::move(heal), for_id, mal_id]() {
    auto ans = heal(mal_id);
    Event ev = [&]() -> Event {
      if (!ans.has_value() || !ans->has_value() || (*ans)->anilist_id <= 0) {
        return EnrichmentHealMissed{for_id};
      }
      EnrichmentRefreshed r;
      r.for_id = for_id;
      r.enrichment = std::move(**ans);
      return r;
    }();
    const bool posted = queue.try_post(Event{std::move(ev)});
    debug_log("worker heal: for_id=" + std::to_string(for_id) +
              " posted=" + std::to_string(posted));
  });
}

// Detail zoom `c`-section worker (P36): same three-state shape as
// spawn_enrich, its own event trio so the two fetches never contend for the
// same in-flight flag. The `c` toggle is the only spawn site — no
// refresh-on-view analogue (05 §8 is enrichment-specific).
void spawn_char_recs(EventQueue& queue, CharRecsFn fetch, std::int64_t for_id,
                     std::optional<std::int64_t> mal_id) {
  spawn_detached([&queue, fetch = std::move(fetch), for_id, mal_id]() {
    auto ans = fetch(for_id, mal_id);
    Event ev = [&]() -> Event {
      if (!ans.has_value()) return CharactersRecsFailed{for_id};
      if (!ans->has_value()) return CharactersRecsNull{for_id};
      CharactersRecsDone d;
      d.for_id = for_id;
      d.data = std::move(**ans);
      return d;
    }();
    const bool posted = queue.try_post(Event{std::move(ev)});
    debug_log("worker char_recs: for_id=" + std::to_string(for_id) +
              " posted=" + std::to_string(posted));
  });
}

// Character-page worker: the same three-state shape as spawn_char_recs, keyed
// by the character id.
void spawn_character(EventQueue& queue, CharacterFn fetch, std::int64_t character_id) {
  spawn_detached([&queue, fetch = std::move(fetch), character_id]() {
    auto ans = fetch(character_id);
    Event ev = [&]() -> Event {
      if (!ans.has_value()) return CharacterFailed{character_id};
      if (!ans->has_value()) return CharacterNull{character_id};
      CharacterDone d;
      d.for_id = character_id;
      d.data = std::move(**ans);
      return d;
    }();
    const bool posted = queue.try_post(Event{std::move(ev)});
    debug_log("worker character: id=" + std::to_string(character_id) +
              " posted=" + std::to_string(posted));
  });
}

// Discover filter overlay genre picker (P38, §9): a single-flight, session-
// cached fetch (no for_id — the vocabulary is global). Two-state, mirroring
// genre_collection's own contract (no confirmed-absent arm).
void spawn_genre_collection(EventQueue& queue, GenreCollectionFn fetch) {
  spawn_detached([&queue, fetch = std::move(fetch)]() {
    auto ans = fetch();
    Event ev = [&]() -> Event {
      if (!ans.has_value()) return GenreCollectionFailed{};
      GenreCollectionDone d;
      d.genres = std::move(*ans);
      return d;
    }();
    const bool posted = queue.try_post(Event{std::move(ev)});
    debug_log("worker genre_collection: posted=" + std::to_string(posted));
  });
}

}  // namespace shigoku::tui
