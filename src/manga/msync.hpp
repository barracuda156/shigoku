// msync.hpp — AniList sync for the manga app: turn "chapter N is read" into a
// tracker progress bump, and nothing more.
//
// Deliberately NOT a reuse of sync.hpp. That module is the anime side's whole
// funnel — pull the collection, reconcile it into the anime store's dirty set,
// push the set with 2s spacing and 429 backoff — and every one of those parts
// is keyed on the anime store's rows. Here there is no dirty set to drain and
// no collection to reconcile: a chapter is marked read, one entry moves. What
// IS shared is the law that matters (06 §5.3 / ROD-498): read the server's own
// value immediately before overwriting it, and never write a value that would
// walk it backwards.
//
// Only MangaDex hands out AniList link ids today, so entries from the scrape
// sources simply never reach here — the caller's "al_id present" test is the
// whole gate.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../anilist.hpp"
#include "../domain.hpp"
#include "../error.hpp"
#include "../http.hpp"
#include "../result.hpp"
#include "../store.hpp"  // ListEntry: the (status, progress, score) triple.

namespace shigoku::manga {

// The tracker progress a display chapter number is worth: the floor of the
// leading numeric run. "10.5" -> 10 (a half chapter has not finished 11),
// "7" -> 7, "0" -> 0. A number that does not start with a digit ("Extra",
// "Omake") or an empty one (a oneshot) has no tracker meaning at all ->
// nullopt, and nothing is pushed for it.
[[nodiscard]] std::optional<std::uint32_t> chapter_progress(std::string_view chapter);

// What one push should do, decided from the entry the server currently holds.
struct MgPushPlan {
  bool push = false;
  std::uint32_t progress = 0;
  ListStatus status = ListStatus::Watching;  // AniList CURRENT == "reading".
  std::uint32_t score = 0;  // the remote's own score, echoed back untouched.
  friend bool operator==(const MgPushPlan&, const MgPushPlan&) = default;
};

// The guard, as a pure function (06 §5.3 in miniature):
//
// - progress 0 is not a state worth writing (nothing has been read yet).
// - a remote already at or past the wanted chapter is left alone — re-reading
//   chapter 3 of a 40-chapter list must never rewind the list entry.
// - the score is whatever the server holds. SaveMediaListEntry takes a full
//   row, so omitting it is not an option and sending 0 would silently wipe a
//   rating the user set on the web.
// - status is CURRENT (reading) unless this read finishes the series, EXCEPT
//   over an entry the account already marked COMPLETED: someone who completed
//   a series and re-read into it keeps that status.
[[nodiscard]] MgPushPlan plan_push(const std::optional<ListEntry>& remote,
                                   std::uint32_t want_progress, bool finishes_series);

// The two AniList list calls this module needs, behind an ABC so the tests
// script both without a socket (sync.hpp's AniListSync shape, minus the
// collection pull manga has no use for).
class MangaListClient {
 public:
  virtual ~MangaListClient() = default;

  [[nodiscard]] virtual Result<std::optional<ListEntry>, ProviderError> fetch_entry(
      std::string_view token, std::int64_t user_id, std::int64_t media_id,
      ScoreFormat format) const = 0;

  [[nodiscard]] virtual Result<std::int64_t, ProviderError> save_entry(
      std::string_view token, std::int64_t media_id, ListStatus status, std::uint32_t progress,
      std::uint32_t score, ScoreFormat format) const = 0;
};

// The real one: anilist::pull_entry/push_entry with MediaKind::Manga bound.
// The client reference must outlive this wrapper.
class HttpMangaListClient final : public MangaListClient {
 public:
  explicit HttpMangaListClient(const http::Client& client) : client_(client) {}

  [[nodiscard]] Result<std::optional<ListEntry>, ProviderError> fetch_entry(
      std::string_view token, std::int64_t user_id, std::int64_t media_id,
      ScoreFormat format) const override {
    return anilist::pull_entry(client_, token, user_id, media_id, format,
                               anilist::MediaKind::Manga);
  }

  [[nodiscard]] Result<std::int64_t, ProviderError> save_entry(
      std::string_view token, std::int64_t media_id, ListStatus status, std::uint32_t progress,
      std::uint32_t score, ScoreFormat format) const override {
    // The mutation carries no MediaType: the media id alone decides which list
    // the row lands in.
    return anilist::push_entry(client_, token, media_id, status, progress, score, format);
  }

 private:
  const http::Client& client_;
};

// One push attempt's terminal state.
struct MgSyncResult {
  enum class Kind {
    Pushed,      // the entry moved; `progress` is what the list now shows.
    UpToDate,    // the server was already at or past this chapter.
    ReadFailed,  // the pre-read failed: nothing was written (the guard's point).
    PushFailed,  // the read succeeded, the write did not.
  };

  Kind kind = Kind::UpToDate;
  std::uint32_t progress = 0;
  ProviderError cause;  // ReadFailed / PushFailed only.

  static MgSyncResult pushed(std::uint32_t p) { return {Kind::Pushed, p, {}}; }
  static MgSyncResult up_to_date() { return {Kind::UpToDate, 0, {}}; }
  static MgSyncResult read_failed(ProviderError e) { return {Kind::ReadFailed, 0, std::move(e)}; }
  static MgSyncResult push_failed(ProviderError e) { return {Kind::PushFailed, 0, std::move(e)}; }
};

// Read-then-write for one media id (blocking; the caller runs it on a worker).
// A failed read is terminal by design: without knowing the server's value
// there is no way to honour the never-decrease rule, so nothing is written.
[[nodiscard]] MgSyncResult push_chapter(const MangaListClient& client, std::string_view token,
                                        std::int64_t user_id, std::int64_t media_id,
                                        std::uint32_t want_progress, bool finishes_series,
                                        ScoreFormat format);

// One line of user copy per outcome (the UI toasts these).
[[nodiscard]] std::string msync_result_text(const MgSyncResult& r);

}  // namespace shigoku::manga
