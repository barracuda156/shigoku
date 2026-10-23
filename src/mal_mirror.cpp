#include "mal_mirror.hpp"

#include <algorithm>

namespace shigoku::mal_mirror {

Result<MirrorSummary, StoreError> push_mirror(const MalMirrorClient& client, const MalAuth& auth,
                                               Store& store, bool enabled) {
  if (!enabled) return MirrorSummary::terminal(MirrorOutcome::Disabled);

  const auto token = auth.bearer();
  if (!token.has_value()) return MirrorSummary::terminal(MirrorOutcome::Disabled);

  auto dirty = store.list_dirty_for_mal_mirror();
  if (!dirty.has_value()) return err(dirty.error());

  MirrorSummary summary;
  summary.dirty = static_cast<std::uint32_t>(dirty->size());

  for (const MalMirrorRow& row : *dirty) {
    auto guard = client.fetch_entry(*token, row.mal_id);
    if (guard.has_value()) {
      // No entry on MAL: nothing of theirs to overwrite, and MAL mirrors get
      // no pull that could ever clear this row's dirty flag from our side, so
      // holding it back here would strand it dirty forever (mirrors sync.cpp's
      // own "no entry on the server" branch, header comment). A first-ever
      // push (row.synced/synced_score == nullopt) is never held back either —
      // there is no prior belief to have been invalidated. Status/progress
      // and score are checked against their OWN snapshots independently (the
      // two fields don't share a guard any more than they share a dirty bit).
      if (guard->has_value()) {
        const auto& [live_status, live_progress, live_score] = **guard;
        const bool pair_moved =
            row.synced.has_value() &&
            std::make_pair(live_status, live_progress) != *row.synced;
        const bool score_moved = row.synced_score.has_value() && live_score != *row.synced_score;
        if (pair_moved || score_moved) {
          // MAL's live value has moved since our last accepted push: someone
          // (or MAL itself) changed it out from under the mirror. Skip rather
          // than overwrite (ROD-498-equivalent guard); stays dirty, retried
          // next run.
          summary.push_skipped += 1;
          continue;
        }
      }
    } else if (guard.error().kind == ProviderError::Kind::Http && guard.error().status == 401) {
      summary.outcome = MirrorOutcome::Unauthorized;
      return summary;
    } else if (guard.error().kind == ProviderError::Kind::RateLimited) {
      summary.outcome = MirrorOutcome::RateLimited;
      return summary;
    } else {
      // Unverified is not a licence to overwrite.
      summary.push_failed += 1;
      continue;
    }

    // A locally-unscored row (converted score 0 = unset) WITHHOLDS the score
    // field: MAL reads score=0 as "remove the score", and the first-ever push
    // of a pre-P34 row would otherwise erase whatever the user rated on the
    // MAL site. The snapshot mirrors the withholding (NULL, not 0) so the
    // guard's score arm keeps waiving a belief the mirror never held.
    const std::optional<std::uint32_t> push_score =
        row.score == 0 ? std::nullopt : std::optional<std::uint32_t>(row.score);
    auto saved = client.save_entry(*token, row.mal_id, row.list_status, row.progress, push_score);
    if (saved.has_value()) {
      auto marked =
          store.mark_mal_synced(row.anilist_id, row.list_status, row.progress, push_score);
      if (!marked.has_value()) return err(marked.error());
      summary.pushed += 1;
      continue;
    }
    if (saved.error().kind == ProviderError::Kind::Http && saved.error().status == 401) {
      summary.outcome = MirrorOutcome::Unauthorized;
      return summary;
    }
    if (saved.error().kind == ProviderError::Kind::RateLimited) {
      summary.outcome = MirrorOutcome::RateLimited;
      return summary;
    }
    summary.push_failed += 1;
  }
  return summary;
}

Result<MirrorPullSummary, StoreError> pull_mirror(const MalMirrorClient& client,
                                                  const MalAuth& auth, Store& store, bool enabled,
                                                  std::int64_t now, const Refresher& refresh) {
  if (!enabled) return MirrorPullSummary::terminal(MirrorOutcome::Disabled);
  if (!auth.bearer().has_value()) return MirrorPullSummary::terminal(MirrorOutcome::Disabled);

  // The pair the run authenticates with: the stored one until a roll
  // replaces it. Rolled at most once per run — a renewed token that MAL
  // still refuses is a sign-in problem, not a stale-token one.
  MalAuth current = auth;
  bool rolled = false;
  // Rolls `current` forward through the seam; the summary to return instead
  // when the run cannot go on. Refused is Unauthorized (only a reconnect
  // mends it); Unreachable a plain missed pull (nothing adopted, next run
  // tries again). Both happen before any roll, so neither carries a pair.
  const auto roll = [&]() -> std::optional<MirrorPullSummary> {
    const Refresh r = refresh(current, now);
    if (r.outcome == Refresh::Outcome::Rolled && r.auth.bearer().has_value()) {
      current = r.auth;
      rolled = true;
      return std::nullopt;
    }
    if (r.outcome == Refresh::Outcome::Unreachable) {
      MirrorPullSummary failed;
      failed.pull_failed = true;
      return failed;
    }
    return MirrorPullSummary::terminal(MirrorOutcome::Unauthorized);
  };
  // Every return from here on carries the renewed pair when there is one.
  const auto finish = [&](MirrorPullSummary s) {
    if (rolled) s.rolled = current;
    return s;
  };
  const auto is_401 = [](const ProviderError& e) {
    return e.kind == ProviderError::Kind::Http && e.status == 401;
  };

  if (refresh && current.is_expired(now)) {
    if (auto stop = roll(); stop.has_value()) return *stop;
  }
  auto fetched = client.fetch_list(*current.bearer());
  if (!fetched.has_value() && is_401(fetched.error()) && refresh && !rolled) {
    if (auto stop = roll(); stop.has_value()) return *stop;
    fetched = client.fetch_list(*current.bearer());
  }
  if (!fetched.has_value()) {
    const ProviderError& e = fetched.error();
    if (is_401(e)) return finish(MirrorPullSummary::terminal(MirrorOutcome::Unauthorized));
    if (e.kind == ProviderError::Kind::RateLimited) {
      return finish(MirrorPullSummary::terminal(MirrorOutcome::RateLimited));
    }
    MirrorPullSummary failed;
    failed.pull_failed = true;
    return finish(failed);
  }

  std::vector<RemoteEntry> remote;
  remote.reserve(fetched->size());
  for (const mal_catalog::UserListEntry& e : *fetched) {
    RemoteEntry r;
    r.anilist_id = e.seed.anilist_id;
    r.status = e.status;
    r.progress = e.progress;
    r.score = std::min<std::uint32_t>(e.score, 10) * 10;  // MAL 0..=10 -> raw 0..=100.
    r.updated_at = e.updated_at;
    r.import_seed = e.seed;
    remote.push_back(std::move(r));
  }
  auto out = store.reconcile_mal_pull(remote, now);
  if (!out.has_value()) return err(out.error());
  MirrorPullSummary summary;
  summary.pulled = *out;
  return finish(summary);
}

}  // namespace shigoku::mal_mirror
