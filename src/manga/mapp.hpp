// mapp.hpp — the manga app's state + tick() + draw() + run(). A
// deliberately small sibling of the anime tui/app.hpp: two views (Search —
// prompt → results list; Library — the followed list) sharing ONE detail pane
// (cover / description / navigable chapter list), because everything below
// the selection is view-agnostic.
//
// Same architecture laws as the anime side: A1 (workers are detached threads
// posting owned events; quit is _exit(0), no join/drain), A2 (tick is
// std::visit + overloaded{}, no catch-all — mevent.hpp), A3 (draw is pure
// over state into the CellBuffer; run owns the terminal), A4 (the cover is
// an image-owned exclusion rect; APC deletes splice before the diff,
// transmits after, one synchronized frame).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../auth.hpp"
#include "../http.hpp"
#include "../tui/cells.hpp"
#include "../tui/cover_geom.hpp"
#include "../tui/cover_probe.hpp"  // CoverBackend — the probe's verdict.
#include "../tui/covers.hpp"
#include "../tui/queue.hpp"
#include "../tui/term.hpp"
#include "../tui/toast.hpp"
#include "../tui/workers.hpp"
#include "mangadex.hpp"
#include "mconfig.hpp"
#include "mevent.hpp"
#include "mstore.hpp"
#include "source.hpp"

namespace shigoku::manga {

using MgQueue = tui::BasicEventQueue<MgEvent>;

// Stable image id for the single detail cover (a replace re-transmits the
// same id, reusing the terminal's slot).
inline constexpr std::uint32_t kCoverImageId = 1;

// FNV-1a 64 of the manga uuid — the int64 key CoverState wants (al_id is not
// always present; the uuid is). Exposed for tests.
[[nodiscard]] std::int64_t cover_key(std::string_view uuid);

// What draw() decided about the cover this frame; run() reconciles it against
// the placement the terminal actually holds (the anime CoverPlan bridge:
// draw is pure and cannot know terminal history).
struct MgCoverPlan {
  tui::Rect rect;              // empty = no cover slot this frame.
  std::int64_t want_id = 0;    // cover_key of the selection; 0 = none.
  bool have_pixels = false;    // pixels for want_id are in app.cover_pixels.
};

struct MgDeps {
  // The boot-time active source — main resolves it from the compiled-in
  // list via pick_source(config.source, config.nsfw_sources). Worker
  // threads only.
  const MangaSource* source = nullptr;
  // The in-app cycle list: every source the nsfw gate admits, in registry
  // order — `s` walks it. Empty = no switching (bare test frames wire only
  // `source`).
  std::vector<const MangaSource*> sources;
  // Where `s` persists the chosen source key. "" = don't persist (tests).
  std::string config_path;
  tui::Covers* covers = nullptr;     // shared cover pipeline (worker threads).
  // The read flow. pages_client is a SEPARATE http client (the Covers
  // precedent: one blocking worker per client while MangaDex's serves API
  // calls). pages_root lives under the DATA dir — chapter pages are durable
  // downloads (offline-first), never evictable cache.
  const http::Client* pages_client = nullptr;
  std::string pages_root;            // "" = read flow disabled (bare tests).
  // The legacy uuid layout root (<data>/chapters). Read-only fallback: a
  // chapter complete there opens from there; new fetches always land under
  // pages_root's title-keyed layout. "" = no legacy store to consult.
  std::string legacy_pages_root;
  std::string viewer_builtin = "shigoku-view";  // resolved argv[0]-sibling.
  // manga.db: the library, persistent read marks/resume, the chapter cache.
  // UI THREAD ONLY (A7) — workers post events, tick() writes. nullptr = no
  // persistence at all (bare test frames): every call site degrades to the
  // in-memory-only read state below.
  MangaStore* store = nullptr;
  // AniList sync: its own client (the Covers precedent — one blocking worker
  // per client) and its OWN auth.json under the shigoku-manga config dir. The
  // two apps share an account, never a file. nullptr / "" = no tracker at all
  // (bare test frames): every call site degrades to local-only reading.
  const http::Client* sync_client = nullptr;
  std::string auth_file;
  MangaConfig config;
};

// Which pane owns j/k/Enter: the left list, or the chapter list inside the
// detail pane once a feed is held for the selection.
enum class MgFocus { Results, Chapters };

// What the left pane lists. The detail pane is the same either way — the
// selection is just sourced from a different list.
enum class MgView { Search, Library };

// The one-deep mark-read undo (`u`), captured BEFORE the write so the restore
// is exact: either the row did not exist (delete it) or it did (put it back
// verbatim, resume page and all).
struct MgReadUndo {
  std::string manga_scoped;  // store key of the manga the mark belongs to.
  std::string chapter_id;
  bool had_row = false;
  ChapterProgress prior;  // meaningful only when had_row.
};

// Where draw() put the chapter rows this frame (the cover_plan idiom: a pure
// draw→input channel so the mouse hit-test replays real geometry, never a
// parallel computation). rect empty = no chapter rows on screen.
struct MgChaptersPlan {
  tui::Rect rect;       // the chapter-row block (x, y0, w, rows shown).
  int first_idx = 0;    // chapters[] index of the row at rect.y.
};

struct MgApp {
  // Terminal / frame.
  tui::WinSize win;
  bool quit = false;
  bool dirty = true;
  std::uint64_t tick_count = 0;

  // Which list the left pane shows. run() lands on Library when the store has
  // followed rows, else Search.
  MgView view = MgView::Search;

  // Search prompt (bottom row when open).
  bool prompt_open = false;
  std::string prompt;

  // Results list (left pane).
  std::vector<MdManga> results;
  int cursor = 0;   // index into results.
  int scroll = 0;   // first visible row.
  bool searching = false;
  std::string last_query;
  tui::GenCounter search_gen;

  // Library list (left pane in MgView::Library) — the store's followed rows,
  // reloaded whenever something that changes them lands.
  std::vector<LibraryEntry> library;
  int library_cursor = 0;
  int library_scroll = 0;

  // Chapters (detail pane). `chapters_for` = the uuid the HELD list belongs
  // to; `chapters_pending_for` = the uuid an in-flight fetch was spawned for
  // (the spinner shows only when it matches the selection — a moved cursor
  // must not wear another manga's spinner).
  std::string chapters_for;          // "" = nothing held.
  std::string chapters_pending_for;  // "" = nothing in flight.
  std::vector<MdChapter> chapters;
  bool chapters_loading = false;
  tui::GenCounter chapters_gen;

  // --- Read flow -------------------------------------------------------------
  MgFocus focus = MgFocus::Results;
  int chapter_cursor = 0;   // index into chapters (focus == Chapters).
  int chapter_scroll = 0;   // first visible chapter row.
  MgChaptersPlan chapters_plan;  // written by draw(), read by on_mouse.

  // Pages fetch in flight (one at a time; open_chapter refuses while set).
  bool pages_fetching = false;
  std::string pages_chapter;     // chapter uuid the fetch belongs to.
  std::uint32_t pages_done = 0;
  std::uint32_t pages_total = 0;
  tui::GenCounter pages_gen;

  // Viewers in flight — browser-tab style: opening a new chapter never
  // refuses just because another one is still being read, it spawns its own
  // shigoku-view window (open_chapter only refuses re-opening a chapter that
  // already has one). Keyed by chapter id, which is safe as a key exactly
  // because of that refusal: at most one live viewer exists per chapter, so
  // the exit event (which carries only a chapter id) always resolves to the
  // right entry.
  struct MgOpenViewer {
    // Store key of the manga this OPEN viewer belongs to, captured at spawn:
    // the selection may move (or another viewer open) while this one reads,
    // and its exit event must write progress to the manga it actually read.
    std::string manga_scoped;
    // The rest of what the exit event needs about the chapter it opened,
    // pinned at the same moment and for the same reason: the display number
    // (the tracker progress is its floor), the manga's AniList id, and
    // whether this was the last chapter of a series the source calls
    // finished.
    std::string chapter_number;
    std::optional<std::int64_t> al_id;
    bool finishes_series = false;
  };
  std::map<std::string, MgOpenViewer> open_viewers;  // key = chapter id.

  // The render-facing read state for ONE manga: chapter ids marked read,
  // per-chapter resume page (1-based, "reopen here"), and which manga
  // (store key) they were hydrated from. The store is the authority; this is
  // the copy draw() reads. Without a store they are the whole truth.
  std::set<std::string> read_chapters;
  std::map<std::string, int> resume_page;
  std::string progress_for;  // "" = never hydrated (or no store).
  std::optional<MgReadUndo> undo_read;

  // Download-ahead (`D`): the remaining chapters to fetch back-to-back through
  // the same one-at-a-time pages machinery. A SNAPSHOT — moving the cursor (or
  // switching view) must not retarget a running queue.
  MdManga dl_manga;
  std::vector<MdChapter> dl_queue;
  const MangaSource* dl_source = nullptr;
  bool dl_active = false;      // the running pages fetch is queue-driven.
  std::uint32_t dl_total = 0;  // chapters queued when it started.
  std::uint32_t dl_done = 0;
  std::uint32_t dl_failed = 0;

  // Update sweep (`U`): one worker walks the followed list; tick() diffs each
  // answer against the chapter cache and aggregates into one toast.
  bool sweep_running = false;
  tui::GenCounter sweep_gen;
  std::uint32_t sweep_new = 0;     // new chapters seen this run.
  std::uint32_t sweep_series = 0;  // series that grew.
  std::uint32_t sweep_done = 0;    // answers (ok or failed) received.
  std::uint32_t sweep_total = 0;   // series queued this run.

  // `r` pressed with a cold chapter cache: continue reading as soon as the
  // feed lands (one-shot, cleared by the arrival or by any other navigation).
  bool continue_pending = false;

  // --- AniList ---------------------------------------------------------

  // Disk truth, reloaded after every connect. Empty = signed out, which is
  // also what a bare test frame (no auth_file) always reads.
  Auth auth;

  // The connect overlay, live only while the loopback is serving. `cancel` is
  // type-erased so this header stays off loopback.hpp's link graph.
  struct MgConnectSession {
    std::string url;
    std::uint64_t started = 0;
    std::function<void()> cancel;
  };
  std::optional<MgConnectSession> connect;

  // One push in flight at a time (a chapter read is a single-row edit; there
  // is no work list to drain). `sync_flag` is the heap-shared completion the
  // quit path waits on — not the event queue, which is about to shut down.
  bool syncing = false;
  std::string syncing_manga;  // scoped key the in-flight push belongs to.
  std::shared_ptr<std::atomic<bool>> sync_flag;

  // Double-click state (the anime idiom: tick+1 so 0 = disarmed).
  std::uint64_t last_click_tick = 0;
  int last_click_x = -1;
  int last_click_y = -1;

  // Detail cover (the anime cover state machine reused verbatim; keys on
  // cover_key).
  tui::CoverBackend cover_backend =
      tui::CoverBackend::None;  // startup probe verdict (None = placeholder).
  tui::CellPx cell;             // per-cell pixels ({0,0} = floors/caps).
  tui::CoverState cover;
  CoverPixels cover_pixels;    // pixels for cover.for_id() when has_pixels.
  MgCoverPlan cover_plan;      // written by draw(), read by run().

  tui::ToastQueue toasts;

  // The in-app source switch (`s`): non-null after the first cycle,
  // overriding deps->source. Read through active_source() in mapp.cpp —
  // never directly.
  const MangaSource* active_source = nullptr;

  // Wiring (run() sets these; workers capture by reference — A1's _exit(0)
  // teardown is what makes that safe).
  MgQueue* queue = nullptr;
  const MgDeps* deps = nullptr;
};

// The selected manga — the library row in MgView::Library, the search result
// otherwise. nullptr when the active list is empty or the cursor is out of
// range. Everything downstream (detail pane, chapters, read flow) reads the
// selection through here and never learns which list it came from.
[[nodiscard]] const MdManga* selected(const MgApp& app);

// Mutate state for one event (A2: exhaustive visit, no catch-all).
void tick(MgApp& app, const MgEvent& ev);

// Pure render of the full frame into `buf` (top bar, panes, prompt/status
// row, toasts) + the cover plan/exclusion for run()'s reconcile.
void draw(MgApp& app, tui::CellBuffer& buf);

// The event loop: terminal RAII, cover probe, reader thread, paint cycle,
// _exit(0) quit path. Returns only on a pre-loop failure (not a tty).
int run(const MgDeps& deps);

}  // namespace shigoku::manga
