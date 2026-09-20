#include "sync.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace shigoku::sync {

namespace {

// A usable bearer + user id, or the terminal reason it is not.
Result<std::pair<std::string_view, std::int64_t>, SyncOutcome> usable_auth(const Auth& auth,
                                                                            std::int64_t now) {
  const auto token = auth.anilist.bearer();
  if (!token.has_value()) return err(SyncOutcome::NoToken);
  if (auth.anilist.is_expired(now)) return err(SyncOutcome::Expired);
  if (auth.anilist.user_id <= 0) return err(SyncOutcome::NoUserId);
  return std::pair<std::string_view, std::int64_t>{*token, auth.anilist.user_id};
}

}  // namespace

namespace detail {

Result<Unit, StoreError> push_dirty(const AniListSync& client, std::string_view token,
                                     std::int64_t user_id, Store& store, const Sleeper& sleeper,
                                     const std::vector<std::int64_t>& contended,
                                     SyncSummary& summary, ScoreFormat format) {
  auto dirty = store.list_dirty_for_sync();
  if (!dirty.has_value()) return err(dirty.error());
  summary.dirty = static_cast<std::uint32_t>(dirty->size());

  bool backed_off = false;
  bool called = false;
  for (const auto& row : *dirty) {
    if (std::find(contended.begin(), contended.end(), row.anilist_id) != contended.end()) {
      summary.push_skipped += 1;
      continue;
    }
    // Spacing is between calls, not between work-list entries: a skipped row
    // costs no request and must not buy the next one a free 2s.
    if (called) sleeper(kPushSpacing);
    called = true;

    for (;;) {
      auto guard = client.fetch_entry(token, user_id, row.anilist_id, format);
      if (guard.has_value()) {
        // No entry on the server: nothing of theirs to destroy, and a
        // deleted entry never returns in the pull to clear our snapshot, so
        // holding the row back here would strand it dirty forever.
        if (!guard->has_value()) {
          // fall through to the save.
        } else if (**guard == row.synced) {
          // fall through to the save.
        } else {
          summary.push_skipped += 1;
          break;
        }
      } else if (guard.error().kind == ProviderError::Kind::Http &&
                 guard.error().status == 401) {
        summary.outcome = SyncOutcome::Unauthorized;
        return Unit{};
      } else if (guard.error().kind == ProviderError::Kind::RateLimited) {
        if (backed_off) {
          summary.outcome = SyncOutcome::RateLimited;
          return Unit{};
        }
        backed_off = true;
        sleeper(kRateLimitBackoff);
        continue;  // retry the row from the guard.
      } else {
        // Unverified is not a licence to overwrite.
        summary.push_failed += 1;
        break;
      }

      auto saved =
          client.save_entry(token, row.anilist_id, row.list_status, row.progress, row.score, format);
      if (saved.has_value()) {
        // Snapshot what the server ACTUALLY stored: under a lossy scoreFormat
        // the wire quantizes (raw 75 -> Point5 wire 4 -> raw 80), and a raw
        // snapshot would strand the row dirty / spawn phantom conflicts on
        // the next pull. The local score adopts the same quantized value,
        // CAS-guarded against a mid-push edit (mark_synced's adopt arm).
        const std::uint32_t stored =
            from_anilist_score(to_anilist_score(row.score, format), format);
        auto marked =
            store.mark_synced(row.anilist_id, row.list_status, row.progress, stored, row.score);
        if (!marked.has_value()) return err(marked.error());
        summary.pushed += 1;
        break;
      }
      if (saved.error().kind == ProviderError::Kind::Http && saved.error().status == 401) {
        summary.outcome = SyncOutcome::Unauthorized;
        return Unit{};
      }
      if (saved.error().kind == ProviderError::Kind::RateLimited) {
        if (backed_off) {
          summary.outcome = SyncOutcome::RateLimited;
          return Unit{};
        }
        backed_off = true;
        sleeper(kRateLimitBackoff);
        continue;  // retry the same row.
      }
      summary.push_failed += 1;
      break;
    }
  }
  return Unit{};
}

}  // namespace detail

Result<SyncSummary, StoreError> run_sync(const AniListSync& client, const Auth& auth,
                                          Store& store, std::int64_t now, bool enabled,
                                          bool pull_only, const Sleeper& sleeper) {
  if (!enabled) return SyncSummary::terminal(SyncOutcome::Disabled);

  const auto usable = usable_auth(auth, now);
  if (!usable.has_value()) return SyncSummary::terminal(usable.error());
  const auto [token, user_id] = *usable;

  std::optional<std::vector<RemoteEntry>> remote;
  SyncOutcome pull_terminal = SyncOutcome::Completed;
  auto fetched = client.fetch_list(token, user_id, auth.anilist.score_format);
  if (fetched.has_value()) {
    remote = std::move(*fetched);
  } else if (fetched.error().kind == ProviderError::Kind::Http && fetched.error().status == 401) {
    pull_terminal = SyncOutcome::PullUnauthorized;
  } else if (fetched.error().kind == ProviderError::Kind::RateLimited) {
    pull_terminal = SyncOutcome::PullRateLimited;
  }
  // else: transport/decode miss does not gate the push (06 §5.2) — the push
  // has its own transport.

  if (pull_terminal != SyncOutcome::Completed) return SyncSummary::terminal(pull_terminal);

  SyncSummary summary;
  summary.outcome = SyncOutcome::Completed;
  summary.pull_failed = !remote.has_value();

  if (remote.has_value()) {
    auto reconciled = store.reconcile_pull(*remote, now);
    if (!reconciled.has_value()) return err(reconciled.error());
    summary.pulled = *reconciled;
  }

  if (pull_only) return summary;

  const std::vector<std::int64_t> contended = summary.pulled.contended;
  auto pushed = detail::push_dirty(client, token, user_id, store, sleeper, contended, summary,
                                   auth.anilist.score_format);
  if (!pushed.has_value()) return err(pushed.error());
  return summary;
}

Result<std::uint32_t, StoreError> refresh_airing(const AiringFetch& fetch, Store& store,
                                                 std::int64_t now) {
  auto candidates = store.list_airing_candidates(now);
  if (!candidates.has_value()) return err(candidates.error());
  if (candidates->empty()) return std::uint32_t{0};

  std::vector<std::int64_t> ids;
  std::vector<std::int64_t> mal_ids;
  std::unordered_map<std::int64_t, std::int64_t> row_by_mal;  // mal_id -> synthetic row.
  for (const AiringCandidate& c : *candidates) {
    if (c.anilist_id > 0) {
      ids.push_back(c.anilist_id);
    } else if (c.mal_id.has_value() && *c.mal_id > 0) {
      mal_ids.push_back(*c.mal_id);
      row_by_mal.emplace(*c.mal_id, c.anilist_id);
    }
  }
  if (ids.empty() && mal_ids.empty()) return std::uint32_t{0};

  auto rows = fetch(ids, mal_ids);
  if (!rows.has_value()) return std::uint32_t{0};  // no answer: try again next run.

  std::unordered_set<std::int64_t> wanted(ids.begin(), ids.end());
  std::uint32_t written = 0;
  for (const anilist::AiringRow& r : *rows) {
    // A real-id row answers under its own id; a MAL-only row under its
    // mal_id. A media the library never asked for is ignored.
    std::optional<std::int64_t> target;
    if (wanted.contains(r.anilist_id)) {
      target = r.anilist_id;
    } else if (r.mal_id.has_value()) {
      if (const auto it = row_by_mal.find(*r.mal_id); it != row_by_mal.end()) target = it->second;
    }
    if (!target.has_value()) continue;
    auto w = store.set_next_airing(*target, r.next_airing_at, r.next_airing_episode);
    if (!w.has_value()) return err(w.error());
    ++written;
  }
  return written;
}

Result<SyncSummary, StoreError> flush_push(const AniListSync& client, const Auth& auth,
                                            Store& store, std::int64_t now, bool enabled,
                                            const Sleeper& sleeper) {
  if (!enabled) return SyncSummary::terminal(SyncOutcome::Disabled);

  const auto usable = usable_auth(auth, now);
  if (!usable.has_value()) return SyncSummary::terminal(usable.error());
  const auto [token, user_id] = *usable;

  SyncSummary summary = SyncSummary::terminal(SyncOutcome::Completed);
  // No pull this run, so no contended set to gate on. The per-row guard in
  // push_dirty is the only thing standing between a session-old snapshot and
  // whatever the server holds now, which is why this path needs it most.
  auto pushed = detail::push_dirty(client, token, user_id, store, sleeper, {}, summary,
                                   auth.anilist.score_format);
  if (!pushed.has_value()) return err(pushed.error());
  return summary;
}

}  // namespace shigoku::sync
