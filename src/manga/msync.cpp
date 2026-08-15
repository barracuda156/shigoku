// msync.cpp — the manga app's AniList progress push (see msync.hpp).

#include "msync.hpp"

#include <cctype>

namespace shigoku::manga {

std::optional<std::uint32_t> chapter_progress(std::string_view chapter) {
  std::size_t i = 0;
  while (i < chapter.size() && std::isspace(static_cast<unsigned char>(chapter[i]))) ++i;
  const std::size_t first = i;
  std::uint64_t n = 0;
  while (i < chapter.size() && std::isdigit(static_cast<unsigned char>(chapter[i]))) {
    // Saturate rather than wrap: a pathological id-shaped "chapter number"
    // must not fold into a small progress that then gets pushed.
    if (n < 1'000'000) n = n * 10 + static_cast<std::uint64_t>(chapter[i] - '0');
    ++i;
  }
  if (i == first) return std::nullopt;  // no leading digit run: not a number.
  // Everything after the digits is the fractional/suffix part and is dropped —
  // that IS the floor rule ("10.5" is chapter 10 finished, not 11).
  return static_cast<std::uint32_t>(n);
}

MgPushPlan plan_push(const std::optional<ListEntry>& remote, std::uint32_t want_progress,
                     bool finishes_series) {
  MgPushPlan plan;
  if (want_progress == 0) return plan;
  if (remote.has_value() && remote->progress >= want_progress) return plan;

  plan.push = true;
  plan.progress = want_progress;
  plan.score = remote.has_value() ? remote->score : 0;
  const bool keep_completed =
      remote.has_value() && remote->status == ListStatus::Completed;
  plan.status = (finishes_series || keep_completed) ? ListStatus::Completed
                                                    : ListStatus::Watching;
  return plan;
}

MgSyncResult push_chapter(const MangaListClient& client, std::string_view token,
                          std::int64_t user_id, std::int64_t media_id,
                          std::uint32_t want_progress, bool finishes_series,
                          ScoreFormat format) {
  auto remote = client.fetch_entry(token, user_id, media_id, format);
  if (!remote.has_value()) return MgSyncResult::read_failed(remote.error());

  const MgPushPlan plan = plan_push(*remote, want_progress, finishes_series);
  if (!plan.push) return MgSyncResult::up_to_date();

  auto saved = client.save_entry(token, media_id, plan.status, plan.progress, plan.score, format);
  if (!saved.has_value()) return MgSyncResult::push_failed(saved.error());
  return MgSyncResult::pushed(plan.progress);
}

std::string msync_result_text(const MgSyncResult& r) {
  switch (r.kind) {
    case MgSyncResult::Kind::Pushed:
      return "anilist \xC2\xB7 ch " + std::to_string(r.progress);
    case MgSyncResult::Kind::UpToDate:
      return "anilist already up to date";
    case MgSyncResult::Kind::ReadFailed:
      return "anilist unreachable \xC2\xB7 not synced";
    case MgSyncResult::Kind::PushFailed:
      return "anilist push failed";
  }
  return "anilist sync";  // unreachable; the switch is exhaustive.
}

}  // namespace shigoku::manga
