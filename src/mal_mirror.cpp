#include "mal_mirror.hpp"

#include <algorithm>
#include <string>

#include "debug_log.hpp"

namespace shigoku::mal_mirror {

namespace {

std::string row_tag(const MalMirrorRow& row) {
  return "mal push: mal_id " + std::to_string(row.mal_id) + " (" +
         std::string(to_string(row.list_status)) + " " + std::to_string(row.progress) + ")";
}

// Logs why a row did not go and keeps the line for the summary.
void note_failure(MirrorSummary& summary, const MalMirrorRow& row, const std::string& what) {
  debug_log(row_tag(row) + ": " + what);
  if (summary.failures.size() < kFailureNotes) {
    summary.failures.push_back("myanimelist.net/anime/" + std::to_string(row.mal_id) + ": " + what);
  }
}

std::string error_tag(const ProviderError& e) {
  if (e.kind == ProviderError::Kind::RateLimited) return "rate limited";
  if (e.status != 0) return "http " + std::to_string(e.status);
  return e.kind == ProviderError::Kind::Decode ? "unreadable answer" : "no answer";
}

}  // namespace

Result<MirrorSummary, StoreError> push_mirror(const MalMirrorClient& client, const MalAuth& auth,
                                               Store& store, bool enabled, std::int64_t now) {
  if (!enabled) return MirrorSummary::terminal(MirrorOutcome::Disabled);

  const auto token = auth.bearer();
  if (!token.has_value()) return MirrorSummary::terminal(MirrorOutcome::Disabled);

  auto dirty = store.list_dirty_for_mal_mirror();
  if (!dirty.has_value()) return err(dirty.error());

  MirrorSummary summary;
  summary.dirty = static_cast<std::uint32_t>(dirty->size());

  const auto is_401 = [](const ProviderError& e) {
    return e.kind == ProviderError::Kind::Http && e.status == 401;
  };

  for (MalMirrorRow row : *dirty) {
    auto guard = client.fetch_entry(*token, row.mal_id);
    if (!guard.has_value()) {
      const std::string why = "entry read failed, " + error_tag(guard.error());
      if (is_401(guard.error())) {
        debug_log(row_tag(row) + ": " + why);
        summary.outcome = MirrorOutcome::Unauthorized;
        return summary;
      }
      if (guard.error().kind == ProviderError::Kind::RateLimited) {
        debug_log(row_tag(row) + ": " + why);
        summary.outcome = MirrorOutcome::RateLimited;
        return summary;
      }
      // Unverified is not a licence to overwrite.
      note_failure(summary, row, why);
      summary.push_failed += 1;
      continue;
    }

    // MAL holds an entry the snapshot does not account for: it moved on
    // MAL's side, or this row was never mirrored. Merge first; what the merge
    // leaves differing is what gets sent. Status/progress and score answer to
    // their own snapshots; a score the mirror never sent (no snapshot) has
    // nothing to have moved from.
    if (guard->has_value()) {
      const auto& [live_status, live_progress, live_score] = **guard;
      const bool pair_differs =
          !row.synced.has_value() || std::make_pair(live_status, live_progress) != *row.synced;
      const bool score_moved = row.synced_score.has_value() && live_score != *row.synced_score;
      if (pair_differs || score_moved) {
        auto rebased =
            store.rebase_mal_row(row.anilist_id, live_status, live_progress, live_score, now);
        if (!rebased.has_value()) return err(rebased.error());
        summary.merged += 1;
        if (rebased->contended) {
          debug_log(row_tag(row) + ": edited during the merge, left for the next run");
          summary.push_skipped += 1;
          continue;
        }
        if (!rebased->owed.has_value()) {
          debug_log(row_tag(row) + ": merged with MAL's " +
                    std::string(to_string(live_status)) + " " + std::to_string(live_progress) +
                    ", nothing left to send");
          summary.settled += 1;
          continue;
        }
        row = *rebased->owed;
      }
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
    const std::string why = "save failed, " + error_tag(saved.error());
    if (is_401(saved.error())) {
      debug_log(row_tag(row) + ": " + why);
      summary.outcome = MirrorOutcome::Unauthorized;
      return summary;
    }
    if (saved.error().kind == ProviderError::Kind::RateLimited) {
      debug_log(row_tag(row) + ": " + why);
      summary.outcome = MirrorOutcome::RateLimited;
      return summary;
    }
    note_failure(summary, row, why);
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
