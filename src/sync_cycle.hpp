// sync_cycle.hpp — one whole tracker run, in the one order both entry points
// keep: the MAL mirror pulls, the AniList run pulls and pushes, the mirror
// pushes, and the Calendar's airing stamps refresh. Header-only glue over
// sync, mal_mirror and the refresh seam (mal_refresh.hpp's own shape); the
// interface's worker and `shigoku sync` are its only users, and both already
// link everything it names.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anilist.hpp"
#include "auth.hpp"
#include "http.hpp"
#include "mal_mirror.hpp"
#include "mal_refresh.hpp"
#include "store.hpp"
#include "sync.hpp"

namespace shigoku::sync {

struct CycleOptions {
  bool enabled = true;     // run_sync's master switch.
  bool pull_only = false;  // the launch refresh: no AniList push.
  std::int64_t now = 0;
  // Where a renewed MAL pair is saved. Empty: nowhere, and a renewal counts
  // as unsaved.
  std::string auth_path;
};

struct CycleResult {
  // nullopt: the AniList run could not update the local library.
  std::optional<SyncSummary> anilist;
  // nullopt: the mirror's pull could not update the local library.
  std::optional<mal_mirror::MirrorPullSummary> mal_pull;
  // The pull renewed the MAL pair and it could not be written to the auth
  // file: the next run will find the old refresh_token, which MAL has by
  // then retired.
  bool mal_token_unsaved = false;
  // nullopt: the mirror's push could not read the local library.
  std::optional<mal_mirror::MirrorSummary> mal_push;
  std::uint32_t airing_refreshed = 0;
};

// The run over its seams (tests script them). The mirror rides the caller's
// store connection with no drain rights of its own. Order: the mirror PULLS
// first, so a change made on MAL's side is in the library before the AniList
// run pushes; then the AniList pull/push; then the mirror push carries
// anything AniList brought in back to MAL; then the airing stamps, public
// data wanted with or without an account. Each half runs whatever the others
// did — they are separate client relationships — and both mirror halves run
// regardless of pull_only. `auth` is the caller's copy: a renewed MAL pair
// replaces its .mal block, so the push below carries it.
inline CycleResult run_cycle(const AniListSync& ani, const mal_mirror::MalMirrorClient& mal,
                             const mal_mirror::Refresher& refresh, const AiringFetch& airing,
                             Store& store, Auth& auth, const CycleOptions& opt,
                             const Sleeper& sleeper) {
  CycleResult out;
  const bool mal_on = auth.mal.bearer().has_value();
  auto pulled = mal_mirror::pull_mirror(mal, auth.mal, store, mal_on, opt.now, refresh);
  if (pulled.has_value()) {
    out.mal_pull = std::move(*pulled);
    if (out.mal_pull->rolled.has_value()) {
      auth.mal = *out.mal_pull->rolled;
      out.mal_token_unsaved =
          opt.auth_path.empty() || !mal_mirror::save_rolled(opt.auth_path, auth.mal);
    }
  }
  auto ran = run_sync(ani, auth, store, opt.now, opt.enabled, opt.pull_only, sleeper);
  if (ran.has_value()) out.anilist = std::move(*ran);
  // A sign-in the pull found refused has nothing to push with.
  const bool mal_refused = out.mal_pull.has_value() &&
                           out.mal_pull->outcome == mal_mirror::MirrorOutcome::Unauthorized;
  auto mirrored =
      mal_mirror::push_mirror(mal, auth.mal, store, mal_on && !mal_refused, opt.now);
  if (mirrored.has_value()) out.mal_push = std::move(*mirrored);
  auto refreshed = refresh_airing(airing, store, opt.now);
  if (refreshed.has_value()) out.airing_refreshed = *refreshed;
  return out;
}

// The production wiring: every seam over one http client. `mal_client_id` is
// the registration the stored MAL pair was issued under.
inline CycleResult run_cycle(const http::Client& http, std::string mal_client_id, Store& store,
                             Auth& auth, const CycleOptions& opt) {
  const HttpAniListSync ani(http);
  const mal_mirror::HttpMalMirrorClient mal(http);
  const mal_mirror::Refresher refresh =
      mal_mirror::make_http_refresher(http, std::move(mal_client_id));
  const AiringFetch airing = [&http](const std::vector<std::int64_t>& ids,
                                     const std::vector<std::int64_t>& mal_ids) {
    return anilist::fetch_airing(http, ids, mal_ids);
  };
  return run_cycle(ani, mal, refresh, airing, store, auth, opt, thread_sleep);
}

}  // namespace shigoku::sync
