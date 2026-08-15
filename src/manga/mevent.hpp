// mevent.hpp — the manga app's own event variant.
//
// Deliberately NOT alternatives added to the shared Event variant: A2's
// no-catch-all law means any new alternative there forces arms into every
// anime visitor — an MF-1 violation by construction. The manga app reuses the
// PLAIN structs from event.hpp (KeyEvent, MouseEvent, Resize, Tick,
// CoverPixels, Generation — none carry anime semantics) inside its own
// variant, runs it through BasicEventQueue<MgEvent>, and re-wraps the input
// decoder's Key/Mouse output at the one reader-loop seam (mapp.cpp).
//
// Same laws as the anime variant: workers post OWNED payloads (moved in,
// no pointers into app state), every visitor is std::visit + overloaded{}
// with NO catch-all (adding an alternative must break every visitor loudly).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "../error.hpp"
#include "../event.hpp"
#include "mangadex.hpp"

namespace shigoku::manga {

// --- Search (worker → UI) ----------------------------------------------------

struct MgSearchDone {
  Generation gen = 0;  // stale if != search generation at receipt.
  std::vector<MdManga> results;
  friend bool operator==(const MgSearchDone&, const MgSearchDone&) = default;
};

struct MgSearchFailed {
  Generation gen = 0;
  ProviderError cause;
  friend bool operator==(const MgSearchFailed&, const MgSearchFailed&) = default;
};

// --- Chapter feed (worker → UI) ----------------------------------------------

struct MgChaptersDone {
  Generation gen = 0;
  std::string manga_id;  // native id the feed belongs to (attribution).
  // The store key ("md:{uuid}") the worker's source scoped this feed to; ""
  // when no source is wired (bare test frames), which skips the cache write.
  std::string scoped_id;
  std::vector<MdChapter> chapters;  // deduped + sorted by the transport.
  friend bool operator==(const MgChaptersDone&, const MgChaptersDone&) = default;
};

struct MgChaptersFailed {
  Generation gen = 0;
  std::string manga_id;
  ProviderError cause;
  friend bool operator==(const MgChaptersFailed&, const MgChaptersFailed&) = default;
};

// --- Detail cover (worker → UI) ----------------------------------------------
//
// `for_id` is the FNV-1a-64 of the manga uuid (CoverState keys on int64;
// MangaDex ids are uuids, and al_id is not always present).

struct MgCoverDone {
  std::int64_t for_id = 0;
  CoverPixels pixels;
};

struct MgCoverFailed {
  std::int64_t for_id = 0;
  friend bool operator==(const MgCoverFailed&, const MgCoverFailed&) = default;
};

// --- Chapter pages fetch (worker → UI) ---------------------------------------

struct MgPagesProgress {
  Generation gen = 0;
  std::string chapter_id;
  std::uint32_t done = 0;
  std::uint32_t total = 0;
  friend bool operator==(const MgPagesProgress&, const MgPagesProgress&) = default;
};

struct MgPagesDone {
  Generation gen = 0;
  std::string chapter_id;
  std::string dir;             // the fetched chapter dir (viewer input).
  std::uint32_t pages = 0;
  friend bool operator==(const MgPagesDone&, const MgPagesDone&) = default;
};

struct MgPagesFailed {
  Generation gen = 0;
  std::string chapter_id;
  ProviderError cause;
  friend bool operator==(const MgPagesFailed&, const MgPagesFailed&) = default;
};

// --- Viewer lifecycle (worker → UI) ------------------------------------------
//
// No generation: several viewers may run at once (browser-tab style), one per
// chapter — open_chapter only refuses re-opening a chapter that already has
// one, which is what keeps chapter_id a safe key for MgApp::open_viewers. A
// late exit event must never be dropped — it erases that chapter's entry.

struct MgViewerExited {
  std::string chapter_id;
  std::uint32_t total_pages = 0;      // page count the viewer was opened over.
  std::optional<int> last_page;       // parsed report (1-based); nullopt = none.
  std::string error;                  // "" = clean spawn/exit.
  friend bool operator==(const MgViewerExited&, const MgViewerExited&) = default;
};

// --- Update sweep (worker → UI) ----------------------------------------------
//
// One worker walks the whole followed list sequentially and posts a
// per-manga result, then exactly one Finished. Ids here are SCOPED (store
// keys): the sweep spans sources, so a native id would be ambiguous.

struct MgSweepChapters {
  Generation gen = 0;
  std::string manga_id;  // scoped ("md:{uuid}").
  std::vector<MdChapter> chapters;
  friend bool operator==(const MgSweepChapters&, const MgSweepChapters&) = default;
};

struct MgSweepFailed {
  Generation gen = 0;
  std::string manga_id;  // scoped.
  ProviderError cause;
  friend bool operator==(const MgSweepFailed&, const MgSweepFailed&) = default;
};

struct MgSweepFinished {
  Generation gen = 0;
  std::uint32_t checked = 0;
  std::uint32_t failed = 0;
  friend bool operator==(const MgSweepFinished&, const MgSweepFinished&) = default;
};

// --- AniList sync (worker → UI) ----------------------------------------------
//
// The connect worker reuses event.hpp's plain ConnectDone (the AniList OAuth
// outcome carries no anime semantics — the same reason the KeyEvent/Resize
// structs are shared). The push worker gets its own payload here rather than
// carrying msync.hpp's MgSyncResult, keeping this header off anilist.hpp's
// link graph (and with it the anime store's) — the event.hpp purity split.

enum class MgSyncOutcome { Pushed, UpToDate, ReadFailed, PushFailed };

struct MgSyncDone {
  std::string manga_id;  // scoped ("md:{uuid}") — whose high-water mark moves.
  MgSyncOutcome outcome = MgSyncOutcome::UpToDate;
  std::uint32_t progress = 0;  // Pushed only: what the list now shows.
  ProviderError cause;         // failures only.
  friend bool operator==(const MgSyncDone&, const MgSyncDone&) = default;
};

// --- The variant ---------------------------------------------------------------

using MgEvent = std::variant<KeyEvent, MouseEvent, Resize, Tick, MgSearchDone,
                             MgSearchFailed, MgChaptersDone, MgChaptersFailed,
                             MgCoverDone, MgCoverFailed, MgPagesProgress,
                             MgPagesDone, MgPagesFailed, MgViewerExited,
                             MgSweepChapters, MgSweepFailed, MgSweepFinished,
                             ConnectDone, MgSyncDone>;

}  // namespace shigoku::manga
