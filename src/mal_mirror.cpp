#include "mal_mirror.hpp"

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

}  // namespace shigoku::mal_mirror
