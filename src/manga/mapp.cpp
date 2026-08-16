// mapp.cpp — the manga app's tick/draw/run. The anime app.cpp's architecture
// at 1/20th the size: one Search view, the same A1-A4 laws (see mapp.hpp).
// Where a rule here mirrors an anime-side rule, the comment names it rather
// than restating it.

#include "mapp.hpp"

#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include "../debug_log.hpp"
#include "../login.hpp"
#include "../loopback.hpp"
#include "../tui/connect.hpp"
#include "../tui/cover_probe.hpp"
#include "../tui/input.hpp"
#include "../tui/iterm.hpp"
#include "../tui/kitty.hpp"
#include "../tui/sixel.hpp"
#include "../tui/views.hpp"  // pane_split, kContentY0/content_height, wrap_text.
#include "msync.hpp"
#include "pages.hpp"
#include "viewer.hpp"

namespace shigoku::manga {

namespace {

namespace th = tui::theme;
using tui::Cell;
using tui::CellBuffer;
using tui::Rect;
using tui::Rgb;
using tui::Style;

// Same fallback per-cell pixels as the anime cover spawn (app.cpp): a cover
// fetch needs a pixel box even when the terminal reports no geometry.
inline constexpr std::uint32_t kFallbackCellW = 8;
inline constexpr std::uint32_t kFallbackCellH = 16;

// Description budget in the detail pane (rows); chapters get the rest.
inline constexpr int kDescMaxRows = 6;

// Double-click window in ticks — the anime side's constant value (app.hpp
// kDoubleClickTicks = 4; not included here, the TUI App header is anime-side).
inline constexpr std::uint64_t kDoubleClickTicks = 4;

const char* spinner_frame(std::uint64_t tick) {
  return tui::kSpinnerFrames[tick % tui::kSpinnerFrameCount];
}

// UTF-8 encode one codepoint onto `out` (prompt input; decoder hands
// codepoints, the prompt buffer is UTF-8 bytes).
void utf8_append(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Pop the last UTF-8 codepoint (backspace: continuation bytes then the lead).
void utf8_pop(std::string& s) {
  while (!s.empty()) {
    const unsigned char c = static_cast<unsigned char>(s.back());
    s.pop_back();
    if ((c & 0xC0) != 0x80) break;  // removed a lead (or ASCII) byte: done.
  }
}

// One line of user copy per ProviderError kind (A2: exhaustive, no default).
// `who` names whoever answered — the active source for a search/feed/pages
// failure, "anilist" for a tracker push.
std::string error_text(const ProviderError& e, std::string_view who = "the source") {
  const std::string subject(who);
  switch (e.kind) {
    case ProviderError::Kind::Network:
      return "network unreachable";
    case ProviderError::Kind::Forbidden:
      return subject + " blocked us (" + std::to_string(e.status) + ")";
    case ProviderError::Kind::Server:
      return subject + " is down (" + std::to_string(e.status) + ")";
    case ProviderError::Kind::Http:
      return subject + " returned an error (" + std::to_string(e.status) + ")";
    case ProviderError::Kind::Decode:
      return "bad response from " + subject;
    case ProviderError::Kind::Unsupported:
      return "operation unsupported";
    case ProviderError::Kind::RateLimited:
      return "rate limited — try again shortly";
  }
  return "error";  // unreachable; the switch is exhaustive.
}

// The active source: the in-app switch target when set (`s`), else the boot
// winner main resolved. Null only in bare test frames.
const MangaSource* active_source(const MgApp& app) {
  if (app.active_source != nullptr) return app.active_source;
  return app.deps != nullptr ? app.deps->source : nullptr;
}

// The active source's display name; the literal fallback keeps bare-deps
// test frames stable (no source wired = the historical string).
std::string_view source_name(const MgApp& app) {
  const MangaSource* s = active_source(app);
  return s != nullptr ? s->name() : std::string_view("MangaDex");
}

std::int64_t now_secs() { return static_cast<std::int64_t>(std::time(nullptr)); }

// The compiled-in source owning a scoped id's prefix, or nullptr. A library
// row from a source this build no longer carries (or one the nsfw gate hides)
// simply has no source — it still renders, it just cannot be fetched.
const MangaSource* source_for_key(const MgApp& app, std::string_view key) {
  if (app.deps == nullptr) return nullptr;
  for (const MangaSource* s : app.deps->sources) {
    if (s != nullptr && s->key() == key) return s;
  }
  const MangaSource* active = active_source(app);
  if (active != nullptr && active->key() == key) return active;
  return nullptr;
}

// The source owning the SELECTION. In the library that is the row's own
// source — library rows span sources, and opening one must not depend on
// which source `s` last cycled to.
const MangaSource* selected_source(const MgApp& app) {
  if (app.view == MgView::Library) {
    if (app.library_cursor < 0 ||
        app.library_cursor >= static_cast<int>(app.library.size())) {
      return nullptr;
    }
    return source_for_key(
        app, app.library[static_cast<std::size_t>(app.library_cursor)].source_key);
  }
  return active_source(app);
}

// The selection's store key ("md:{uuid}"), or "" when there is no selection or
// no source to scope it with.
std::string selected_scoped(const MgApp& app) {
  const MdManga* sel = selected(app);
  const MangaSource* src = selected_source(app);
  if (sel == nullptr || src == nullptr) return {};
  return scoped_id(*src, sel->id);
}

MangaStore* store_of(const MgApp& app) {
  return app.deps != nullptr ? app.deps->store : nullptr;
}

// Store failures are surfaced, never swallowed — a library that silently
// stops persisting is worse than a toast.
void note_store_error(MgApp& app, const MgStoreError& e) {
  app.toasts.push(tui::ToastKind::Error, mstore_error_text(e), app.tick_count);
  app.dirty = true;
}

// Re-read the followed list (cheap: local sqlite, a handful of rows) and keep
// the cursor inside it.
void reload_library(MgApp& app) {
  MangaStore* store = store_of(app);
  if (store == nullptr) return;
  auto rows = store->library();
  if (!rows.has_value()) {
    note_store_error(app, rows.error());
    return;
  }
  app.library = std::move(*rows);
  const int last = static_cast<int>(app.library.size()) - 1;
  if (app.library_cursor > last) app.library_cursor = last < 0 ? 0 : last;
  if (app.library_cursor < 0) app.library_cursor = 0;
  if (app.library_scroll > app.library_cursor) app.library_scroll = app.library_cursor;
  if (app.library_scroll < 0) app.library_scroll = 0;
  app.dirty = true;
}

// Point the render-facing read state at one manga. A no-op without a store —
// there the two containers ARE the state and must never be cleared.
void hydrate_progress(MgApp& app, const std::string& scoped) {
  MangaStore* store = store_of(app);
  if (store == nullptr || scoped.empty() || app.progress_for == scoped) return;
  app.read_chapters.clear();
  app.resume_page.clear();
  app.undo_read.reset();  // the undo slot belongs to the manga it was armed on.
  app.progress_for = scoped;
  auto rows = store->progress(scoped);
  if (!rows.has_value()) {
    note_store_error(app, rows.error());
    return;
  }
  for (const ChapterProgress& p : *rows) {
    if (p.read) app.read_chapters.insert(p.chapter_id);
    if (p.last_page.has_value()) app.resume_page[p.chapter_id] = *p.last_page;
  }
  app.dirty = true;
}

// Mint/refresh the identity row. Every progress and chapter-cache write FKs
// it, so this runs before anything else touches the manga.
void remember_manga(MgApp& app, const MangaSource& src, const MdManga& m) {
  MangaStore* store = store_of(app);
  if (store == nullptr) return;
  if (auto r = store->upsert_manga(src.key(), m, now_secs()); !r.has_value()) {
    note_store_error(app, r.error());
  }
}

// A source status string that means the run is over ("completed" everywhere
// today; compared case-blind so a source that capitalizes it still counts).
bool is_finished_status(std::string_view status) {
  static constexpr std::string_view kDone = "completed";
  if (status.size() != kDone.size()) return false;
  for (std::size_t i = 0; i < status.size(); ++i) {
    const char c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(status[i])));
    if (c != kDone[i]) return false;
  }
  return true;
}

// The display number of a chapter id in the held list ("" when the list moved
// on — the store column is cosmetic, ordering rides the cache).
std::string chapter_number(const MgApp& app, std::string_view chapter_id) {
  for (const MdChapter& c : app.chapters) {
    if (c.id == chapter_id) return c.chapter;
  }
  return {};
}

// --- Workers (A1: detached, own their inputs, post owned events) ------------

void spawn_search(MgApp& app, const MangaSource* source, std::string query) {
  const Generation gen = app.search_gen.bump();
  auto* queue = app.queue;
  tui::spawn_detached([queue, source, gen, query = std::move(query)]() {
    auto r = source->search(query);
    if (r.has_value()) {
      queue->try_post(MgEvent{MgSearchDone{gen, std::move(*r)}});
    } else {
      queue->try_post(MgEvent{MgSearchFailed{gen, r.error()}});
    }
  });
}

void spawn_chapters(MgApp& app, const MangaSource* source, std::string manga_id) {
  const Generation gen = app.chapters_gen.bump();
  auto* queue = app.queue;
  std::string scoped = scoped_id(*source, manga_id);
  tui::spawn_detached([queue, source, gen, manga_id = std::move(manga_id),
                       scoped = std::move(scoped),
                       lang = app.deps->config.chapter_lang]() {
    auto r = source->chapters(manga_id, lang);
    if (r.has_value()) {
      queue->try_post(
          MgEvent{MgChaptersDone{gen, manga_id, scoped, std::move(*r)}});
    } else {
      queue->try_post(MgEvent{MgChaptersFailed{gen, manga_id, r.error()}});
    }
  });
}

// The update sweep: ONE worker walking the whole followed list sequentially
// (a burst of parallel feed fetches is exactly the traffic these sources
// throttle). Each answer posts its own event so tick() can write the cache
// incrementally; the Finished event closes the run.
void spawn_sweep(MgApp& app, std::vector<std::pair<std::string, const MangaSource*>> jobs) {
  const Generation gen = app.sweep_gen.bump();
  auto* queue = app.queue;
  tui::spawn_detached([queue, gen, jobs = std::move(jobs),
                       lang = app.deps->config.chapter_lang]() {
    std::uint32_t failed = 0;
    for (const auto& [scoped, source] : jobs) {
      const std::string native = split_scoped(scoped).native;
      auto r = source->chapters(native, lang);
      if (r.has_value()) {
        queue->try_post(MgEvent{MgSweepChapters{gen, scoped, std::move(*r)}});
      } else {
        ++failed;
        queue->try_post(MgEvent{MgSweepFailed{gen, scoped, r.error()}});
      }
    }
    queue->try_post(MgEvent{
        MgSweepFinished{gen, static_cast<std::uint32_t>(jobs.size()), failed}});
  });
}

// Page resolution + fetch, one worker for the whole chapter. The progress
// callback posts per-page events (a chapter is ≤ a few hundred pages, well
// under any queue pressure); the refetch callback is the rotation's one
// fresh resolution (pages.hpp).
void spawn_pages(MgApp& app, const MangaSource* source, std::string chapter_id,
                 std::string dir) {
  const Generation gen = app.pages_gen.bump();
  auto* queue = app.queue;
  const http::Client* client = app.deps->pages_client;
  const bool saver = app.deps->config.data_saver;
  tui::spawn_detached([queue, source, client, gen, saver,
                       chapter_id = std::move(chapter_id),
                       dir = std::move(dir)]() {
    // The guard lives HERE, not in the core (pages.hpp contract / the
    // download.hpp split): every unit of every set — the initial resolution
    // and each rotation refetch alike — has its provider-supplied URL
    // vetted before the core sees it.
    auto guarded_pages = [&]() -> Result<PageSet, ProviderError> {
      auto g = source->pages(chapter_id, saver);
      if (!g.has_value()) return g;
      for (const PageUnit& u : g->units) {
        if (!http::guard_fetch_url(u.url).has_value()) {
          return err(ProviderError::decode("unsafe page url refused"));
        }
      }
      return g;
    };
    auto set = guarded_pages();
    if (!set.has_value()) {
      queue->try_post(MgEvent{MgPagesFailed{gen, chapter_id, set.error()}});
      return;
    }
    auto r = fetch_chapter_pages(
        *client, *set, dir,
        [&](std::uint32_t done, std::uint32_t total) {
          queue->try_post(MgEvent{MgPagesProgress{gen, chapter_id, done, total}});
        },
        guarded_pages);
    if (r.has_value()) {
      queue->try_post(MgEvent{MgPagesDone{gen, chapter_id, dir, *r}});
    } else {
      queue->try_post(MgEvent{MgPagesFailed{gen, chapter_id, r.error()}});
    }
  });
}

// Spawn the configured viewer over a fetched dir and wait for it on a
// detached worker; the exit event carries the parsed report (mark-read /
// resume input). No generation — each open chapter gets its own worker, and
// the chapter id is the key MgApp::open_viewers is resolved by.
void spawn_viewer_wait(MgApp& app, std::vector<std::string> argv,
                       std::string chapter_id, std::string report_path,
                       std::uint32_t total_pages) {
  auto* queue = app.queue;
  tui::spawn_detached([queue, argv = std::move(argv),
                       chapter_id = std::move(chapter_id),
                       report_path = std::move(report_path), total_pages]() {
    auto r = run_viewer(argv);
    MgViewerExited ev;
    ev.chapter_id = chapter_id;
    ev.total_pages = total_pages;
    if (!r.has_value()) {
      ev.error = r.error();
    } else {
      ev.last_page = parse_report(report_path);
    }
    queue->try_post(MgEvent{ev});
  });
}

// --- AniList ----------------------------------------------------------

// The account line the status row shows: disk truth, never a cached guess.
std::string account_display(const Auth& auth) {
  if (!auth.anilist.bearer().has_value()) return "not connected";
  return auth.anilist.user_name.empty() ? "connected" : auth.anilist.user_name;
}

// Every gate a push has to clear before it is even worth spawning: the master
// switch, a wired client, and a token with a user id behind it.
bool sync_ready(const MgApp& app) {
  if (app.deps == nullptr || app.deps->sync_client == nullptr) return false;
  if (!app.deps->config.anilist_sync) return false;
  if (!app.auth.anilist.bearer().has_value()) return false;
  return app.auth.anilist.user_id > 0;
}

MgSyncOutcome to_sync_outcome(MgSyncResult::Kind k) {
  switch (k) {
    case MgSyncResult::Kind::Pushed:     return MgSyncOutcome::Pushed;
    case MgSyncResult::Kind::UpToDate:   return MgSyncOutcome::UpToDate;
    case MgSyncResult::Kind::ReadFailed: return MgSyncOutcome::ReadFailed;
    case MgSyncResult::Kind::PushFailed: return MgSyncOutcome::PushFailed;
  }
  return MgSyncOutcome::UpToDate;  // unreachable (closed enum).
}

// One read-then-write against AniList, off the UI thread. Everything it needs
// is copied in (A1): the token, the ids, the wanted progress. `flag` is the
// quit path's completion signal — set LAST, after the event is posted, so a
// quit that observes it knows the work is actually over.
void spawn_sync_push(MgApp& app, std::string manga_scoped, std::int64_t media_id,
                     std::uint32_t want_progress, bool finishes_series) {
  auto* queue = app.queue;
  const http::Client* client = app.deps->sync_client;
  const std::string token(*app.auth.anilist.bearer());
  const std::int64_t user_id = app.auth.anilist.user_id;
  const ScoreFormat format = app.auth.anilist.score_format;
  auto flag = std::make_shared<std::atomic<bool>>(false);
  app.sync_flag = flag;
  app.syncing = true;
  app.syncing_manga = manga_scoped;
  tui::spawn_detached([queue, client, token, user_id, media_id, want_progress,
                       finishes_series, format, flag,
                       manga_scoped = std::move(manga_scoped)]() {
    const HttpMangaListClient list(*client);
    const MgSyncResult r = push_chapter(list, token, user_id, media_id,
                                        want_progress, finishes_series, format);
    MgSyncDone ev;
    ev.manga_id = manga_scoped;
    ev.outcome = to_sync_outcome(r.kind);
    ev.progress = r.progress;
    ev.cause = r.cause;
    queue->try_post(MgEvent{std::move(ev)});
    flag->store(true, std::memory_order_release);
  });
}

// login::ConnectResult -> the event-safe ConnectDone (the anime app.cpp's
// to_connect_done, reused verbatim — the enum is shared, the login module is
// not).
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

// The blocking accept/serve loop, off the UI thread (A1). A Canceled result
// posts nothing: the app already moved on when it cancelled.
void spawn_connect(MgQueue& queue, const http::Client& http, loopback::Loopback lp,
                   std::string auth_path, std::int64_t now) {
  tui::spawn_detached([&queue, &http, lp = std::move(lp), auth_path = std::move(auth_path),
                       now]() mutable {
    const login::Verifier verifier = login::make_anilist_verifier(http);
    const login::ConnectResult result =
        lp.serve(verifier, auth_path, now, [] {});
    if (result.kind != login::ConnectResult::Kind::Canceled) {
      queue.try_post(MgEvent{to_connect_done(result)});
    }
  });
}

// `A`: bind the loopback on the UI thread (fast, local — a bind failure is a
// toast, not a half-open modal), open the browser, then serve off it.
//
// The port is AniList's, not ours to choose: the authorize redirect goes to
// whatever URL this build's app registration holds, so the manga app listens
// on the same one the anime app does. Only one connect can be in flight
// machine-wide, which the bind failure says plainly.
void open_connect(MgApp& app) {
  if (app.queue == nullptr || app.deps == nullptr || app.deps->sync_client == nullptr ||
      app.deps->auth_file.empty()) {
    app.toasts.push(tui::ToastKind::Error, "connect unavailable", app.tick_count);
    app.dirty = true;
    return;
  }
  if (app.connect.has_value()) return;  // already waiting on the browser.
  auto started = loopback::Loopback::start();
  if (!started.has_value()) {
    app.toasts.push(tui::ToastKind::Error,
                    "login port " + std::to_string(login::kLoopbackPort) +
                        " busy \xC2\xB7 is shigoku connecting?",
                    app.tick_count);
    app.dirty = true;
    return;
  }
  loopback::Loopback lp = std::move(*started);
  const std::string url = lp.authorize_url();
  login::open_browser(url);
  loopback::Canceler canceler = lp.canceler();
  spawn_connect(*app.queue, *app.deps->sync_client, std::move(lp), app.deps->auth_file,
                now_secs());

  MgApp::MgConnectSession session;
  session.url = url;
  session.started = app.tick_count;
  session.cancel = [canceler]() mutable { canceler.cancel(); };
  app.connect = std::move(session);
  app.dirty = true;
}

void spawn_cover(MgApp& app, std::int64_t for_id, std::string url,
                 std::uint32_t box_w, std::uint32_t box_h) {
  auto* queue = app.queue;
  tui::Covers* covers = app.deps->covers;
  tui::spawn_detached([queue, covers, for_id, url = std::move(url), box_w, box_h]() {
    auto pixels = covers->load(/*provider=*/nullptr, url, box_w, box_h);
    if (pixels.has_value()) {
      queue->try_post(MgEvent{MgCoverDone{for_id, std::move(*pixels)}});
    } else {
      queue->try_post(MgEvent{MgCoverFailed{for_id}});
    }
  });
}

// --- Selection / list motion -------------------------------------------------

int visible_rows(const MgApp& app) { return tui::content_height(app.win.rows); }

void move_cursor(MgApp& app, int delta) {
  if (app.results.empty()) return;
  const int last = static_cast<int>(app.results.size()) - 1;
  int c = app.cursor + delta;
  if (c < 0) c = 0;
  if (c > last) c = last;
  if (c == app.cursor) return;
  app.cursor = c;
  const int vis = visible_rows(app);
  if (app.cursor < app.scroll) app.scroll = app.cursor;
  if (vis > 0 && app.cursor >= app.scroll + vis) app.scroll = app.cursor - vis + 1;
  app.dirty = true;
}

// Library list motion (same keep-visible law as move_cursor; its own cursor
// so switching views returns to where each list was left).
void move_library_cursor(MgApp& app, int delta) {
  if (app.library.empty()) return;
  const int last = static_cast<int>(app.library.size()) - 1;
  int c = app.library_cursor + delta;
  if (c < 0) c = 0;
  if (c > last) c = last;
  if (c == app.library_cursor) return;
  app.library_cursor = c;
  const int vis = visible_rows(app);
  if (app.library_cursor < app.library_scroll) app.library_scroll = app.library_cursor;
  if (vis > 0 && app.library_cursor >= app.library_scroll + vis) {
    app.library_scroll = app.library_cursor - vis + 1;
  }
  app.dirty = true;
}

// Enter on a manga: paint the CACHED feed at once (offline-first — a library
// row lists and marks its chapters with no network at all), then refresh
// behind it. The refresh's Done event replaces the list and rewrites the
// cache; a failed refresh leaves the cached list standing.
void request_chapters(MgApp& app) {
  const MdManga* sel = selected(app);
  const MangaSource* src = selected_source(app);
  if (sel == nullptr || src == nullptr) return;
  if (app.chapters_for == sel->id) {
    // Already held: Enter re-engages by moving focus into the list.
    app.focus = MgFocus::Chapters;
    app.dirty = true;
    return;
  }
  if (app.chapters_pending_for == sel->id) return;  // already in flight.

  const std::string scoped = scoped_id(*src, sel->id);
  hydrate_progress(app, scoped);
  // The identity row must exist before the Done event writes the cache
  // (both FK it), and it must be minted from the selection we still hold.
  remember_manga(app, *src, *sel);
  if (MangaStore* store = store_of(app); store != nullptr) {
    auto cached = store->cached_chapters(scoped);
    if (!cached.has_value()) {
      note_store_error(app, cached.error());
    } else if (!cached->empty()) {
      app.chapters = std::move(*cached);
      app.chapters_for = sel->id;
      app.chapter_cursor = 0;
      app.chapter_scroll = 0;
      app.focus = MgFocus::Chapters;
    }
  }
  app.chapters_loading = true;
  app.chapters_pending_for = sel->id;
  spawn_chapters(app, src, sel->id);
  app.dirty = true;
}

// Chapter-list motion: same keep-visible law as move_cursor. The
// visible row count comes from the last frame's chapters_plan (draw is the
// geometry authority); before any frame showed rows, fall back to a sane
// floor so keyboard motion still works.
void move_chapter_cursor(MgApp& app, int delta) {
  if (app.chapters.empty()) return;
  const int last = static_cast<int>(app.chapters.size()) - 1;
  int c = app.chapter_cursor + delta;
  if (c < 0) c = 0;
  if (c > last) c = last;
  if (c == app.chapter_cursor) return;
  app.chapter_cursor = c;
  const int vis = app.chapters_plan.rect.h > 0 ? app.chapters_plan.rect.h : 10;
  if (c < app.chapter_scroll) app.chapter_scroll = c;
  if (c >= app.chapter_scroll + vis) app.chapter_scroll = c - vis + 1;
  app.dirty = true;
}

// `s` cycles the active source (the registry cycle list); switching sources
// re-arms search. Search/feed state is source-scoped — ids don't cross
// sources — so the whole search surface re-arms; in-flight results die by
// generation bump. A running pages fetch is left alone: its units are
// already resolved and the download is durable and source-agnostic on disk.
// The chosen key persists to config (best effort — a failed save still
// switches).
void cycle_source(MgApp& app) {
  if (app.deps == nullptr || app.deps->sources.size() < 2) return;
  // Search-view only: the library spans sources and carries each row's own
  // source, so cycling there would mean nothing but would tear down the
  // chapter list under the selection.
  if (app.view != MgView::Search) return;
  const auto& list = app.deps->sources;
  const MangaSource* cur = active_source(app);
  std::size_t idx = 0;
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (list[i] == cur) {
      idx = i;
      break;
    }
  }
  const MangaSource* next = list[(idx + 1) % list.size()];
  if (next == nullptr || next == cur) return;
  app.active_source = next;
  app.results.clear();
  app.cursor = 0;
  app.scroll = 0;
  app.searching = false;
  app.last_query.clear();
  app.search_gen.bump();
  app.chapters_for.clear();
  app.chapters_pending_for.clear();
  app.chapters.clear();
  app.chapters_loading = false;
  app.chapters_gen.bump();
  app.chapter_cursor = 0;
  app.chapter_scroll = 0;
  app.focus = MgFocus::Results;
  if (!app.deps->config_path.empty()) {
    MangaConfig c = app.deps->config;
    c.source = std::string(next->key());
    if (!save_manga_config(app.deps->config_path, c).has_value()) {
      app.toasts.push(tui::ToastKind::Error, "config save failed",
                      app.tick_count);
    }
  }
  app.toasts.push(tui::ToastKind::Info, "source: " + std::string(next->name()),
                  app.tick_count);
  app.dirty = true;
}

// The chapter list is live for the CURRENT selection (held, not stale).
bool chapters_engaged(const MgApp& app) {
  const MdManga* sel = selected(app);
  return sel != nullptr && !app.chapters_for.empty() &&
         app.chapters_for == sel->id;
}

void open_viewer(MgApp& app, const MdManga& manga, const MdChapter& ch,
                 const std::string& dir) {
  const std::vector<std::string> pages = list_pages(dir);
  if (pages.empty() || app.deps == nullptr || app.queue == nullptr) {
    app.toasts.push(tui::ToastKind::Error, "no pages on disk", app.tick_count);
    app.dirty = true;
    return;
  }
  // Whose progress this viewer's exit writes, pinned now: the selection is
  // free to move (or another viewer open) while this one reads.
  MgApp::MgOpenViewer ov;
  if (const MangaSource* src = selected_source(app); src != nullptr) {
    ov.manga_scoped = scoped_id(*src, manga.id);
    remember_manga(app, *src, manga);  // reading a manga records it, followed or not.
  }
  // The tracker's half of the same pin: the chapter's display number, the
  // manga's AniList id, and whether finishing this chapter finishes the whole
  // series (the last chapter of a source-declared "completed" run).
  ov.chapter_number = ch.chapter;
  ov.al_id = manga.al_id;
  ov.finishes_series = is_finished_status(manga.status) &&
                       !app.chapters.empty() && app.chapters.back().id == ch.id;
  ViewerOpts opts;
  opts.rtl = app.deps->config.rtl;
  if (const auto it = app.resume_page.find(ch.id); it != app.resume_page.end()) {
    opts.start_page = it->second;
  }
  const std::string report = dir + "/.report";
  std::remove(report.c_str());  // a stale report must not mark-read a chapter.
  opts.report_file = report;
  std::string title = manga.title;
  if (!ch.chapter.empty()) title += " \xE2\x80\x94 ch " + ch.chapter;  // —
  opts.title = std::move(title);
  const ViewerKind kind = parse_viewer_kind(app.deps->config.viewer);
  std::vector<std::string> argv =
      build_viewer_argv(kind, app.deps->config.viewer_path,
                        app.deps->viewer_builtin, dir, pages, opts);
  app.open_viewers[ch.id] = ov;
  spawn_viewer_wait(app, std::move(argv), ch.id, report,
                    static_cast<std::uint32_t>(pages.size()));
  app.dirty = true;
}

// Enter on a chapter: complete on disk → straight into the viewer;
// otherwise one pages fetch at a time, the viewer auto-opens on Done.
void open_chapter(MgApp& app) {
  const MdManga* sel = selected(app);
  const MangaSource* src = selected_source(app);
  if (sel == nullptr || src == nullptr) return;
  if (!chapters_engaged(app)) return;
  if (app.chapter_cursor < 0 ||
      app.chapter_cursor >= static_cast<int>(app.chapters.size())) {
    return;
  }
  if (app.deps->pages_root.empty() || app.deps->pages_client == nullptr) {
    return;  // read flow not wired (bare unit-test deps).
  }
  const MdChapter& ch =
      app.chapters[static_cast<std::size_t>(app.chapter_cursor)];
  // Browser-tab style: a chapter already open in its own viewer window is the
  // only thing Enter refuses — every OTHER chapter is free to open a second
  // (third, ...) window rather than being silently blocked by one already up.
  if (app.open_viewers.count(ch.id) != 0) {
    app.toasts.push(tui::ToastKind::Info, "already reading this chapter",
                    app.tick_count);
    app.dirty = true;
    return;
  }
  if (app.pages_fetching) {
    app.toasts.push(tui::ToastKind::Info, "already fetching a chapter",
                    app.tick_count);
    app.dirty = true;
    return;
  }
  const std::string dir = library_chapter_dir(app.deps->pages_root, *sel, ch);
  if (chapter_complete(dir)) {
    open_viewer(app, *sel, ch, dir);  // offline hit: no network at all.
    return;
  }
  // Pre-library downloads (the legacy uuid layout) stay readable in place —
  // offline-first forbids orphaning fetched pages over the layout rename.
  if (!app.deps->legacy_pages_root.empty()) {
    const std::string legacy =
        chapter_dir(app.deps->legacy_pages_root, sel->id, ch.id);
    if (chapter_complete(legacy)) {
      open_viewer(app, *sel, ch, legacy);
      return;
    }
  }
  app.pages_fetching = true;
  app.pages_chapter = ch.id;
  app.pages_done = 0;
  app.pages_total = ch.pages;
  spawn_pages(app, src, ch.id, dir);
  app.dirty = true;
}

// --- Library actions ---------------------------------------------------------

// `f`: follow / unfollow the selection. Following mints the identity row and
// seeds the chapter cache from whatever list is on screen so the library's
// unread counter is right immediately rather than after the next sweep.
void toggle_following(MgApp& app) {
  MangaStore* store = store_of(app);
  const MdManga* sel = selected(app);
  const MangaSource* src = selected_source(app);
  if (store == nullptr || sel == nullptr || src == nullptr) return;
  const std::string scoped = scoped_id(*src, sel->id);
  auto following = store->is_following(scoped);
  if (!following.has_value()) {
    note_store_error(app, following.error());
    return;
  }
  if (*following) {
    if (auto r = store->set_following(scoped, false, now_secs()); !r.has_value()) {
      note_store_error(app, r.error());
      return;
    }
    app.toasts.push(tui::ToastKind::Info, "unfollowed \xC2\xB7 " + sel->title,
                    app.tick_count);
  } else {
    remember_manga(app, *src, *sel);
    if (auto r = store->set_following(scoped, true, now_secs()); !r.has_value()) {
      note_store_error(app, r.error());
      return;
    }
    if (chapters_engaged(app) && !app.chapters.empty()) {
      if (auto r = store->put_chapters(scoped, app.chapters, now_secs());
          !r.has_value()) {
        note_store_error(app, r.error());
      }
    }
    app.toasts.push(tui::ToastKind::Success, "following \xC2\xB7 " + sel->title,
                    app.tick_count);
  }
  reload_library(app);
  app.dirty = true;
}

// The continue-reading pin: the first chapter strictly after the highest read
// one. With no feed on screen yet, load it first and continue when it lands.
void continue_reading(MgApp& app) {
  if (!chapters_engaged(app)) {
    request_chapters(app);
    if (!chapters_engaged(app)) {
      app.continue_pending = true;  // the cache was cold; the fetch will finish it.
      return;
    }
  }
  const int idx = next_unread_index(app.chapters, app.read_chapters);
  if (idx < 0) {
    app.toasts.push(tui::ToastKind::Info, "all chapters read", app.tick_count);
    app.dirty = true;
    return;
  }
  app.focus = MgFocus::Chapters;
  app.chapter_cursor = idx;
  const int vis = app.chapters_plan.rect.h > 0 ? app.chapters_plan.rect.h : 10;
  if (idx < app.chapter_scroll) app.chapter_scroll = idx;
  if (idx >= app.chapter_scroll + vis) app.chapter_scroll = idx - vis + 1;
  open_chapter(app);
}

// Pull the next queued chapter, skipping ones already complete on disk. The
// queue drains into a summary toast — download-ahead is a background errand,
// not something to watch.
void start_next_download(MgApp& app) {
  while (!app.dl_queue.empty()) {
    const MdChapter ch = app.dl_queue.front();
    app.dl_queue.erase(app.dl_queue.begin());
    const std::string dir =
        library_chapter_dir(app.deps->pages_root, app.dl_manga, ch);
    if (chapter_complete(dir)) {
      ++app.dl_done;  // already downloaded (re-entry, or read earlier).
      continue;
    }
    app.pages_fetching = true;
    app.pages_chapter = ch.id;
    app.pages_done = 0;
    app.pages_total = ch.pages;
    app.dl_active = true;
    spawn_pages(app, app.dl_source, ch.id, dir);
    app.dirty = true;
    return;
  }
  std::string line = "downloaded " + std::to_string(app.dl_done) + "/" +
                     std::to_string(app.dl_total);
  if (app.dl_failed > 0) line += " \xC2\xB7 " + std::to_string(app.dl_failed) + " failed";
  app.toasts.push(app.dl_failed > 0 ? tui::ToastKind::Error : tui::ToastKind::Success,
                  line, app.tick_count);
  app.dl_active = false;
  app.dl_source = nullptr;
  app.dl_total = 0;
  app.dl_done = 0;
  app.dl_failed = 0;
  app.dirty = true;
}

// `D`: fetch every unread chapter of the selection, one at a time, through
// the same pages machinery a read runs. Nothing new is needed — it is a loop
// over that one fetch.
void queue_downloads(MgApp& app) {
  const MdManga* sel = selected(app);
  const MangaSource* src = selected_source(app);
  if (sel == nullptr || src == nullptr || app.deps == nullptr) return;
  if (app.deps->pages_root.empty() || app.deps->pages_client == nullptr) return;
  if (!chapters_engaged(app)) {
    app.toasts.push(tui::ToastKind::Info, "load the chapter list first",
                    app.tick_count);
    app.dirty = true;
    return;
  }
  if (app.pages_fetching || app.dl_active) {
    app.toasts.push(tui::ToastKind::Info, "already fetching a chapter",
                    app.tick_count);
    app.dirty = true;
    return;
  }
  app.dl_queue.clear();
  for (const MdChapter& c : app.chapters) {
    if (app.read_chapters.count(c.id) == 0) app.dl_queue.push_back(c);
  }
  if (app.dl_queue.empty()) {
    app.toasts.push(tui::ToastKind::Info, "nothing unread to download",
                    app.tick_count);
    app.dirty = true;
    return;
  }
  app.dl_manga = *sel;
  app.dl_source = src;
  app.dl_total = static_cast<std::uint32_t>(app.dl_queue.size());
  app.dl_done = 0;
  app.dl_failed = 0;
  app.toasts.push(tui::ToastKind::Info,
                  "downloading " + std::to_string(app.dl_total) + " chapters",
                  app.tick_count);
  start_next_download(app);
}

// `U`: refresh every followed manga's feed. tick() diffs each answer against
// the chapter cache; the run ends in one aggregate toast.
void start_sweep(MgApp& app) {
  if (store_of(app) == nullptr) return;
  if (app.sweep_running) {
    app.toasts.push(tui::ToastKind::Info, "already checking", app.tick_count);
    app.dirty = true;
    return;
  }
  reload_library(app);
  std::vector<std::pair<std::string, const MangaSource*>> jobs;
  for (const LibraryEntry& e : app.library) {
    if (const MangaSource* s = source_for_key(app, e.source_key); s != nullptr) {
      jobs.emplace_back(e.scoped_id(), s);
    }
  }
  if (jobs.empty()) {
    app.toasts.push(tui::ToastKind::Info, "nothing followed to check",
                    app.tick_count);
    app.dirty = true;
    return;
  }
  if (app.queue == nullptr) return;  // no worker seam (bare test frames).
  app.sweep_running = true;
  app.sweep_new = 0;
  app.sweep_series = 0;
  app.sweep_done = 0;
  app.sweep_total = static_cast<std::uint32_t>(jobs.size());
  app.toasts.push(tui::ToastKind::Info,
                  "checking " + std::to_string(jobs.size()) + " series…",
                  app.tick_count);
  spawn_sweep(app, std::move(jobs));
  app.dirty = true;
}

// The automatic push, fired by a mark-read. Every gate here is a reason NOT
// to spend a round trip; the guard that protects the user's list lives on the
// worker (msync.hpp), not in these tests.
void maybe_push_progress(MgApp& app, const std::string& manga_scoped,
                         std::optional<std::int64_t> al_id, std::string_view number,
                         bool finishes_series) {
  // No tracker link: every source but MangaDex, every time (only MangaDex
  // publishes the AniList id).
  if (!al_id.has_value() || *al_id <= 0) return;
  if (!sync_ready(app) || app.queue == nullptr) return;
  // One at a time. A dropped push costs nothing durable: the local mark only
  // advances on a completed one, and AniList progress means "read up to N", so
  // the next chapter's push subsumes this one. `S` forces it in the meantime.
  if (app.syncing) return;
  const auto want = chapter_progress(number);
  if (!want.has_value() || *want == 0) return;
  // The local high-water mark, so re-reading an old chapter never asks AniList
  // what it already told us.
  if (MangaStore* store = store_of(app); store != nullptr && !manga_scoped.empty()) {
    auto synced = store->al_synced(manga_scoped);
    if (!synced.has_value()) {
      note_store_error(app, synced.error());
    } else if (*synced >= *want) {
      return;
    }
  }
  spawn_sync_push(app, manga_scoped, *al_id, *want, finishes_series);
  app.dirty = true;
}

// `S`: push the selection's highest read chapter now. The manual form of the
// same one-entry write, and deliberately blind to the LOCAL high-water mark —
// "sync this" means ask the server, whatever we think we already sent (a list
// reset on the web is exactly when that matters). The server-side guard still
// refuses anything that would walk the entry backwards.
void sync_selection(MgApp& app) {
  const MdManga* sel = selected(app);
  const MangaSource* src = selected_source(app);
  if (sel == nullptr || src == nullptr || app.deps == nullptr) return;
  auto refuse = [&](std::string line) {
    app.toasts.push(tui::ToastKind::Info, std::move(line), app.tick_count);
    app.dirty = true;
  };
  if (!sel->al_id.has_value() || *sel->al_id <= 0) {
    return refuse("no anilist id for this title");
  }
  if (!app.deps->config.anilist_sync) return refuse("anilist sync is off in config");
  if (app.deps->sync_client == nullptr) return refuse("anilist sync unavailable");
  if (!app.auth.anilist.bearer().has_value()) {
    return refuse("not connected \xC2\xB7 A connects anilist");
  }
  if (app.auth.anilist.user_id <= 0) return refuse("reconnect anilist \xC2\xB7 A");
  if (app.syncing) return refuse("already syncing");
  if (app.queue == nullptr) return;

  const std::string scoped = scoped_id(*src, sel->id);
  hydrate_progress(app, scoped);  // the read set must belong to THIS manga.
  std::vector<MdChapter> chapters;
  if (chapters_engaged(app)) {
    chapters = app.chapters;
  } else if (MangaStore* store = store_of(app); store != nullptr) {
    auto cached = store->cached_chapters(scoped);
    if (!cached.has_value()) {
      note_store_error(app, cached.error());
      return;
    }
    chapters = std::move(*cached);
  }
  std::uint32_t want = 0;
  bool finishes = false;
  for (std::size_t i = 0; i < chapters.size(); ++i) {
    const MdChapter& c = chapters[i];
    if (app.read_chapters.count(c.id) == 0) continue;
    const auto p = chapter_progress(c.chapter);
    if (!p.has_value() || *p <= want) continue;
    want = *p;
    finishes = is_finished_status(sel->status) && i + 1 == chapters.size();
  }
  if (want == 0) return refuse("nothing read to sync");
  spawn_sync_push(app, scoped, *sel->al_id, want, finishes);
  app.toasts.push(tui::ToastKind::Info, "anilist \xE2\x80\xA6 ch " + std::to_string(want),
                  app.tick_count);
  app.dirty = true;
}

// --- Key / mouse -------------------------------------------------------------

void on_key(MgApp& app, const KeyEvent& k) {
  using S = KeyEvent::Special;
  if (app.prompt_open) {
    if (k.special == S::Enter) {
      app.prompt_open = false;
      if (!app.prompt.empty() && active_source(app) != nullptr) {
        app.last_query = app.prompt;
        app.searching = true;
        app.view = MgView::Search;  // results have nowhere else to land.
        spawn_search(app, active_source(app), app.prompt);
      }
      app.dirty = true;
    } else if (k.special == S::Escape) {
      app.prompt_open = false;
      app.dirty = true;
    } else if (k.special == S::Backspace) {
      utf8_pop(app.prompt);
      app.dirty = true;
    } else if (k.codepoint != 0 && !k.ctrl) {
      utf8_append(app.prompt, k.codepoint);
      app.dirty = true;
    }
    return;
  }
  // The connect overlay captures the keyboard while the browser round trip is
  // out (the anime modal's law): Esc abandons it, everything else is ignored
  // rather than acting on a screen the user cannot see.
  if (app.connect.has_value()) {
    if (k.special == S::Escape) {
      if (app.connect->cancel) app.connect->cancel();
      app.connect.reset();
      app.toasts.push(tui::ToastKind::Info, "connect canceled", app.tick_count);
      app.dirty = true;
    }
    return;
  }
  if (k.codepoint == U'/') {
    app.prompt_open = true;
    app.prompt.clear();
    app.dirty = true;
    return;
  }
  if (k.codepoint == U'q') {
    app.quit = true;
    return;
  }
  // `A` connects the AniList account chapter reads push to.
  if (k.codepoint == U'A') {
    open_connect(app);
    return;
  }
  // `S` pushes the selection's read progress now.
  if (k.codepoint == U'S') {
    sync_selection(app);
    return;
  }
  // Esc = one step back: chapter focus returns to the results list; the
  // prompt case is handled above; from the results list there is nowhere
  // further back (q quits).
  if (k.special == S::Escape) {
    if (app.focus == MgFocus::Chapters) {
      app.focus = MgFocus::Results;
      app.dirty = true;
    }
    return;
  }
  // `s` cycles the active source (no-op with fewer than two wired).
  if (k.codepoint == U's') {
    cycle_source(app);
    return;
  }
  // `u` undoes the last mark-read (one-deep undo), in the store and on
  // screen. The prior row goes back exactly as it was — a chapter that had a
  // resume page before the mark gets that resume page back.
  if (k.codepoint == U'u') {
    if (app.undo_read.has_value()) {
      const MgReadUndo u = *app.undo_read;
      if (MangaStore* store = store_of(app); store != nullptr) {
        auto r = u.had_row ? store->restore_progress(u.manga_scoped, u.prior)
                           : store->clear_progress(u.manga_scoped, u.chapter_id);
        if (!r.has_value()) note_store_error(app, r.error());
      }
      // Only touch the on-screen copy when it belongs to the same manga (no
      // store = the copy IS the state, and the ids always match).
      if (store_of(app) == nullptr || app.progress_for == u.manga_scoped) {
        app.read_chapters.erase(u.chapter_id);
        app.resume_page.erase(u.chapter_id);
        if (u.had_row) {
          if (u.prior.read) app.read_chapters.insert(u.chapter_id);
          if (u.prior.last_page.has_value()) {
            app.resume_page[u.chapter_id] = *u.prior.last_page;
          }
        }
      }
      app.undo_read = std::nullopt;
      reload_library(app);
      app.toasts.push(tui::ToastKind::Info, "read mark undone", app.tick_count);
      app.dirty = true;
    }
    return;
  }
  // `L` swaps the left pane between the library and the search results. The
  // detail pane (and with it the chapter list) is shared, so the selection
  // just changes underneath it.
  if (k.codepoint == U'L') {
    if (app.view == MgView::Library) {
      app.view = MgView::Search;
    } else {
      reload_library(app);
      if (app.library.empty()) {
        app.toasts.push(tui::ToastKind::Info,
                        "library is empty \xC2\xB7 f follows a manga",
                        app.tick_count);
      } else {
        app.view = MgView::Library;
      }
    }
    app.focus = MgFocus::Results;
    app.continue_pending = false;
    app.dirty = true;
    return;
  }
  // `f` follows / unfollows the selection.
  if (k.codepoint == U'f') {
    toggle_following(app);
    return;
  }
  // `r` continues reading: the first chapter after the highest read one.
  if (k.codepoint == U'r') {
    continue_reading(app);
    return;
  }
  // `D` downloads every unread chapter of the selection.
  if (k.codepoint == U'D') {
    queue_downloads(app);
    return;
  }
  // `U` checks every followed manga for new chapters.
  if (k.codepoint == U'U') {
    start_sweep(app);
    return;
  }
  // h/l hop between the panes the way the anime's do (list <-> detail).
  if (k.codepoint == U'h' || k.special == S::Left) {
    if (app.focus == MgFocus::Chapters) {
      app.focus = MgFocus::Results;
      app.dirty = true;
    }
    return;
  }
  if (k.codepoint == U'l' || k.special == S::Right) {
    if (app.focus == MgFocus::Results && chapters_engaged(app)) {
      app.focus = MgFocus::Chapters;
      app.dirty = true;
    }
    return;
  }

  // Vertical motion routes by focus first (the chapter list owns j/k while
  // engaged), then by view (library or results in the left pane).
  const bool ch_focus = app.focus == MgFocus::Chapters && chapters_engaged(app);
  const bool lib_focus = !ch_focus && app.view == MgView::Library;
  const int page = visible_rows(app);
  const int all = ch_focus  ? static_cast<int>(app.chapters.size())
                  : lib_focus ? static_cast<int>(app.library.size())
                              : static_cast<int>(app.results.size());
  auto move = [&](int delta) {
    if (ch_focus) {
      move_chapter_cursor(app, delta);
    } else if (lib_focus) {
      move_library_cursor(app, delta);
    } else {
      move_cursor(app, delta);
    }
  };
  if (k.codepoint == U'j' || k.special == S::Down) {
    move(+1);
  } else if (k.codepoint == U'k' || k.special == S::Up) {
    move(-1);
  } else if (k.codepoint == U'g' || k.special == S::Home) {
    move(-all);
  } else if (k.codepoint == U'G' || k.special == S::End) {
    move(all);
  } else if (k.special == S::PageDown) {
    move(page);
  } else if (k.special == S::PageUp) {
    move(-page);
  } else if (k.special == S::Enter) {
    if (ch_focus) {
      open_chapter(app);
    } else {
      request_chapters(app);
    }
  }
}

void on_mouse(MgApp& app, const MouseEvent& m) {
  if (app.connect.has_value()) return;  // captured, same as the key path.
  switch (m.kind) {
    case MouseEvent::Kind::WheelUp:
    case MouseEvent::Kind::WheelDown: {
      // Wheel = the focused list's j/k (the anime side's position-blind alias).
      const int delta = m.kind == MouseEvent::Kind::WheelUp ? -1 : +1;
      if (app.focus == MgFocus::Chapters && chapters_engaged(app)) {
        move_chapter_cursor(app, delta);
      } else if (app.view == MgView::Library) {
        move_library_cursor(app, delta);
      } else {
        move_cursor(app, delta);
      }
      return;
    }
    case MouseEvent::Kind::Press: {
      if (m.button != MouseEvent::Button::Left) return;
      // Double-click = Enter on the same cell within the window
      // (last_click_tick stores tick+1 so 0 always means disarmed).
      const bool dbl =
          app.last_click_tick != 0 &&
          app.tick_count + 1 - app.last_click_tick <= kDoubleClickTicks &&
          m.x == app.last_click_x && m.y == app.last_click_y;
      app.last_click_tick = 0;
      auto arm_double = [&]() {
        app.last_click_tick = app.tick_count + 1;
        app.last_click_x = m.x;
        app.last_click_y = m.y;
      };
      if (m.y < tui::kContentY0 || m.y >= app.win.rows - 1) return;

      // Chapter rows first (draw's own geometry via chapters_plan — the
      // cover_plan idiom): click selects with focus following; a second
      // click on the selected row opens it (click-to-select).
      const tui::Rect& cr = app.chapters_plan.rect;
      if (!cr.empty() && m.x >= cr.x && m.x < cr.x + cr.w && m.y >= cr.y &&
          m.y < cr.y + cr.h && chapters_engaged(app)) {
        const int idx = app.chapters_plan.first_idx + (m.y - cr.y);
        if (idx >= 0 && idx < static_cast<int>(app.chapters.size())) {
          const bool already =
              app.focus == MgFocus::Chapters && app.chapter_cursor == idx;
          app.focus = MgFocus::Chapters;
          app.chapter_cursor = idx;
          app.dirty = true;
          if (dbl && already) {
            open_chapter(app);
            return;
          }
          arm_double();
        }
        return;
      }

      // The left list column — library or results, by view.
      const int list_w = app.win.cols >= tui::kPaneSplitMin
                             ? tui::pane_split(app.win.cols).list_w
                             : app.win.cols;
      if (m.x >= list_w) return;
      const bool lib = app.view == MgView::Library;
      const int scroll = lib ? app.library_scroll : app.scroll;
      const int rows = lib ? static_cast<int>(app.library.size())
                           : static_cast<int>(app.results.size());
      const int idx = scroll + (m.y - tui::kContentY0);
      if (idx >= 0 && idx < rows) {
        const int cursor = lib ? app.library_cursor : app.cursor;
        const bool already = app.focus == MgFocus::Results && idx == cursor;
        app.focus = MgFocus::Results;
        if (lib) {
          move_library_cursor(app, idx - cursor);
        } else {
          move_cursor(app, idx - cursor);
        }
        app.dirty = true;
        if (dbl && already) {
          request_chapters(app);  // the left list's Enter.
          return;
        }
        arm_double();
      }
      return;
    }
    case MouseEvent::Kind::Release:
      return;
  }
}

// --- Cover reconcile (the anime reconcile_cover, in miniature) --------------

void cover_tick(MgApp& app) {
  const MangaSource* source = selected_source(app);
  if (source == nullptr || app.deps == nullptr || app.deps->covers == nullptr) {
    return;
  }
  const MdManga* sel = selected(app);
  std::string url;
  if (sel != nullptr) url = source->cover_thumb_url(*sel);

  std::optional<std::int64_t> target_id;
  std::optional<std::string_view> target_url;
  // Placeholder mode never fetches: without a kitty path there is nowhere to
  // put pixels (the box renders regardless).
  if (sel != nullptr && !url.empty() &&
      app.cover_backend != tui::CoverBackend::None) {
    // Keyed on the SCOPED id: the library mixes sources, and two sites are
    // free to hand out the same native id.
    target_id = cover_key(scoped_id(*source, sel->id));
    target_url = url;
  }

  switch (app.cover.decide(target_id, target_url, app.tick_count)) {
    case tui::CoverAction::None:
    case tui::CoverAction::Suppress:
    case tui::CoverAction::UpToDate:
      return;
    case tui::CoverAction::Clear:
      app.cover.clear();
      app.cover_pixels = CoverPixels{};
      app.dirty = true;
      return;
    case tui::CoverAction::Fetch: {
      const Rect r = app.cover_plan.rect;
      if (r.empty()) return;  // no room this frame; retry next tick.
      const std::uint32_t cw = app.cell.known() ? app.cell.width : kFallbackCellW;
      const std::uint32_t ch = app.cell.known() ? app.cell.height : kFallbackCellH;
      app.cover.begin_fetch(*target_id, *target_url);
      app.cover_pixels = CoverPixels{};  // supersede held pixels (anime rule).
      spawn_cover(app, *target_id, url,
                  static_cast<std::uint32_t>(r.w) * cw,
                  static_cast<std::uint32_t>(r.h) * ch);
      app.dirty = true;
      return;
    }
  }
}

// --- Draw (A3: pure over state; cover_plan is the one draw→run channel) -----

void draw_top_bar(const MgApp& app, CellBuffer& buf) {
  buf.fill(Rect{0, 0, app.win.cols, 1}, th::surface);
  buf.put_str(1, 0, "shigoku-manga", th::fg, th::surface, Style::Bold);
  // Per-view hints, in two widths: the full set on a wide terminal, a short
  // set at 80 columns (where the full one would be dropped whole rather than
  // truncated — a half-sentence of keys is worse than fewer keys).
  const bool lib = app.view == MgView::Library;
  const std::string wide =
      lib ? "/ search  Enter open  r continue  D download  U updates  f unfollow  S sync  L search  q quit"
          : "/ search  Enter open  r continue  D download  f follow  s source  S sync  L library  q quit";
  const std::string narrow =
      lib ? "Enter open  r read  D get  U check  f unfollow  L search"
          : "/ search  Enter open  r read  f follow  L library  q quit";
  const int wide_x = app.win.cols - tui::str_width(wide) - 1;
  const std::string& hints = wide_x > 16 ? wide : narrow;
  const int hx = app.win.cols - tui::str_width(hints) - 1;
  if (hx > 16) buf.put_str(hx, 0, hints, th::fg3, th::surface);
}

// The library list: title, then the counter that decides whether it is worth
// opening tonight (unread chapters, or "not fetched" for a row the sweep has
// never seen).
void draw_library(MgApp& app, CellBuffer& buf, int x, int w, int y0, int y1) {
  if (app.library.empty()) {
    buf.put_str(x + 2, y0 + 1, "library is empty", th::fg2, th::bg);
    buf.put_str(x + 2, y0 + 2, "f follows the selected manga", th::fg3, th::bg);
    return;
  }
  const int vis = y1 - y0;
  for (int row = 0; row < vis; ++row) {
    const int idx = app.library_scroll + row;
    if (idx >= static_cast<int>(app.library.size())) break;
    const LibraryEntry& e = app.library[static_cast<std::size_t>(idx)];
    const bool sel = idx == app.library_cursor;
    const Rgb bg = sel ? th::surface : th::bg;
    const Rgb fg = sel ? th::focus : th::fg;
    if (sel) buf.fill(Rect{x, y0 + row, w, 1}, bg);
    // The counter is right-aligned so titles of any length line up against it.
    std::string tail;
    if (e.known_chapters == 0) {
      tail = "\xE2\x80\x94";  // — : never fetched, so nothing is known yet.
    } else if (e.unread() == 0) {
      tail = "\xE2\x9C\x93";  // ✓ : all read.
    } else {
      tail = std::to_string(e.unread()) + " new";
    }
    const int tw = tui::str_width(tail);
    const int title_w = w - 4 - tw;
    if (title_w > 0) {
      buf.put_str(x + 2, y0 + row, tui::truncate_to_cols(e.manga.title, title_w),
                  fg, bg, sel ? Style::Bold : Style::None);
    }
    if (tw < w - 2) {
      buf.put_str(x + w - 1 - tw, y0 + row, tail,
                  e.unread() > 0 && e.known_chapters > 0 ? th::focus : th::fg3, bg);
    }
  }
}

void draw_list(MgApp& app, CellBuffer& buf, int x, int w, int y0, int y1) {
  if (app.results.empty()) {
    const std::string copy =
        app.searching ? "searching…"
        : app.last_query.empty()
            ? "press / to search " + std::string(source_name(app))
            : "no results";
    buf.put_str(x + 2, y0 + 1, copy, th::fg2, th::bg);
    return;
  }
  const int vis = y1 - y0;
  for (int row = 0; row < vis; ++row) {
    const int idx = app.scroll + row;
    if (idx >= static_cast<int>(app.results.size())) break;
    const MdManga& m = app.results[static_cast<std::size_t>(idx)];
    const bool sel = idx == app.cursor;
    const Rgb bg = sel ? th::surface : th::bg;
    const Rgb fg = sel ? th::focus : th::fg;
    if (sel) buf.fill(Rect{x, y0 + row, w, 1}, bg);
    std::string line = m.title;
    if (m.year.has_value()) line += " (" + std::to_string(*m.year) + ")";
    buf.put_str(x + 2, y0 + row, tui::truncate_to_cols(line, w - 3), fg, bg,
                sel ? Style::Bold : Style::None);
  }
}

void draw_cover_box(MgApp& app, CellBuffer& buf, const MdManga& sel, int x,
                    int w, int y0, int y1) {
  const tui::CoverTier tier = tui::cover_tier(w);
  int rows = tui::detail_cover_rows(tier, app.cell);
  // The aesthetic caps (28/20) assume a tall canvas; on a short terminal the
  // cover must not eat the description + chapters below it. Half the pane is
  // the ceiling.
  if (rows > (y1 - y0) / 2) rows = (y1 - y0) / 2;
  if (rows <= 0 || tier.cover_w <= 0) return;
  const Rect r{x, y0, tier.cover_w, rows};

  const MangaSource* source = selected_source(app);
  const std::string url =
      source != nullptr ? source->cover_thumb_url(sel) : std::string{};
  const std::int64_t want =
      url.empty() ? 0 : cover_key(scoped_id(*source, sel.id));
  const bool have = want != 0 &&
                    app.cover_backend != tui::CoverBackend::None &&
                    app.cover.has_pixels() &&
                    app.cover.for_id().has_value() && *app.cover.for_id() == want;

  app.cover_plan.rect = r;
  app.cover_plan.want_id = want;
  app.cover_plan.have_pixels = have;

  if (!have) {
    // Placeholder box (kitty off, no cover art, loading, or failed): the
    // layout slot is preserved either way (A4's placeholder mode).
    buf.fill(r, th::surface);
    const char* glyph = url.empty() ? "×" : app.cover.is_loading()
                                                ? spinner_frame(app.tick_count)
                                                : "·";
    buf.put_str(r.x + r.w / 2, r.y + r.h / 2, glyph, th::fg3, th::surface);
  }
}

void draw_detail(MgApp& app, CellBuffer& buf, int x, int w, int y0, int y1) {
  const MdManga* sel = selected(app);
  if (sel == nullptr) {
    buf.put_str(x + 1, y0 + 1, "select a result", th::fg3, th::bg);
    return;
  }

  draw_cover_box(app, buf, *sel, x, w, y0, y1);
  const tui::CoverTier tier = tui::cover_tier(w);
  const int cover_rows = app.cover_plan.rect.empty() ? 0 : app.cover_plan.rect.h;

  // Meta column right of the cover slot.
  const int tx = x + tier.slot_w;
  const int tw = w - tier.slot_w - 1;
  int ty = y0;
  if (tw > 8) {
    const auto title_lines = tui::detail::wrap_text(sel->title, tw);
    for (std::size_t i = 0; i < title_lines.size() && i < 2 && ty < y1; ++i) {
      buf.put_str(tx, ty++, title_lines[i], th::fg, th::bg, Style::Bold);
    }
    std::string meta;
    if (sel->year.has_value()) meta += std::to_string(*sel->year);
    if (!sel->status.empty()) {
      if (!meta.empty()) meta += " · ";
      meta += sel->status;
    }
    if (!meta.empty() && ty < y1) {
      buf.put_str(tx, ty++, tui::truncate_to_cols(meta, tw), th::fg2, th::bg);
    }
    std::string ids;
    if (sel->al_id.has_value()) ids += "AL " + std::to_string(*sel->al_id);
    if (sel->mal_id.has_value()) {
      if (!ids.empty()) ids += " · ";
      ids += "MAL " + std::to_string(*sel->mal_id);
    }
    // What the tracker was last told, for the library rows that carry it.
    if (app.view == MgView::Library && app.library_cursor >= 0 &&
        app.library_cursor < static_cast<int>(app.library.size())) {
      const std::uint32_t synced =
          app.library[static_cast<std::size_t>(app.library_cursor)].al_synced_chapter;
      if (synced > 0) {
        if (!ids.empty()) ids += " · ";
        ids += "synced ch " + std::to_string(synced);
      }
    }
    if (!ids.empty() && ty < y1) {
      buf.put_str(tx, ty++, tui::truncate_to_cols(ids, tw), th::fg3, th::bg);
    }
  }

  // Description below the cover block, capped so the chapters section keeps
  // a floor of rows even on a short terminal (the pty smoke's 30-row lesson:
  // cover + 6 description lines used to push chapters past the bottom edge).
  int dy = y0 + (cover_rows > 0 ? cover_rows : ty - y0) + 1;
  if (!sel->description.empty() && dy < y1) {
    const auto lines = tui::detail::wrap_text(sel->description, w - 2);
    const int reserve = 6;  // header + ~4 chapter rows + the overflow line.
    int max_desc = y1 - dy - reserve;
    if (max_desc > kDescMaxRows) max_desc = kDescMaxRows;
    if (max_desc < 1 && y1 - dy >= 2) max_desc = 1;  // starved pane: 1 line.
    for (int i = 0; i < max_desc && i < static_cast<int>(lines.size()); ++i) {
      const bool truncated =
          i == max_desc - 1 && static_cast<int>(lines.size()) > max_desc;
      buf.put_str(x + 1, dy++,
                  truncated ? tui::truncate_to_cols(lines[static_cast<std::size_t>(i)] + " …", w - 2)
                            : lines[static_cast<std::size_t>(i)],
                  th::fg2, th::bg);
    }
    ++dy;
  }

  // Chapters section: a navigable list, not a peek — cursor, scroll window,
  // read marks, and the mouse plan all live here.
  if (dy >= y1) return;
  const bool held = app.chapters_for == sel->id;
  const bool pending = app.chapters_loading && app.chapters_pending_for == sel->id;
  // The spinner replaces the list only when there is no list yet: a cached
  // feed stays on screen while the refresh behind it runs.
  if (pending && !held) {
    buf.put_str(x + 1, dy,
                std::string(spinner_frame(app.tick_count)) + " fetching chapters…",
                th::fg2, th::bg);
    return;
  }
  if (!held) {
    buf.put_str(x + 1, dy, "Enter — load chapters", th::fg3, th::bg);
    return;
  }
  const std::string lang =
      app.deps != nullptr ? app.deps->config.chapter_lang : std::string("en");
  std::string header = "chapters — " + std::to_string(app.chapters.size()) +
                       " (" + lang + ")";
  const std::size_t read_here = [&] {
    std::size_t n = 0;
    for (const MdChapter& c : app.chapters) {
      if (app.read_chapters.count(c.id) != 0) ++n;
    }
    return n;
  }();
  if (read_here > 0) header += "  \xC2\xB7 " + std::to_string(read_here) + " read";
  if (pending) header += "  \xC2\xB7 refreshing";
  buf.put_str(x + 1, dy++, tui::truncate_to_cols(header, w - 2), th::fg2, th::bg,
              Style::Bold);
  if (app.chapters.empty()) {
    if (dy < y1) buf.put_str(x + 1, dy, "no chapters in this language", th::fg3, th::bg);
    return;
  }

  // Read-flow status line (fetch progress / open viewer) above the rows.
  if (app.pages_fetching && dy < y1) {
    std::string line = std::string(spinner_frame(app.tick_count)) + " fetching pages";
    if (app.pages_total > 0) {
      line += " " + std::to_string(app.pages_done) + "/" +
              std::to_string(app.pages_total);
    }
    if (app.dl_active) {
      line += "  \xC2\xB7 chapter " + std::to_string(app.dl_done + 1) + "/" +
              std::to_string(app.dl_total);
    }
    buf.put_str(x + 1, dy++, tui::truncate_to_cols(line, w - 2), th::focus, th::bg);
  } else if (!app.open_viewers.empty() && dy < y1) {
    const std::size_t n = app.open_viewers.size();
    const std::string line =
        n == 1 ? "viewer open — close it to resume here"
               : std::to_string(n) + " viewers open — close them to resume here";
    buf.put_str(x + 1, dy++, tui::truncate_to_cols(line, w - 2), th::fg3,
                th::bg, Style::Italic);
  }

  if (dy >= y1) return;
  const int n = static_cast<int>(app.chapters.size());
  const int room = y1 - dy;
  // The scroll window: start from the committed offset, then correct so the
  // cursor is always inside the frame (draw stays pure over state; the local
  // copy is what the plan records, so the mouse sees what the eye sees).
  int scroll = app.chapter_scroll;
  if (scroll > n - room) scroll = n - room;
  if (scroll < 0) scroll = 0;
  if (app.chapter_cursor >= 0 && app.chapter_cursor < n) {
    if (app.chapter_cursor < scroll) scroll = app.chapter_cursor;
    if (app.chapter_cursor >= scroll + room) scroll = app.chapter_cursor - room + 1;
  }
  const int shown = n - scroll > room ? room : n - scroll;
  const bool ch_focus = app.focus == MgFocus::Chapters;
  app.chapters_plan.rect = tui::Rect{x + 1, dy, w - 2, shown};
  app.chapters_plan.first_idx = scroll;
  for (int row = 0; row < shown; ++row) {
    const int idx = scroll + row;
    const MdChapter& c = app.chapters[static_cast<std::size_t>(idx)];
    const bool is_cursor = idx == app.chapter_cursor;
    const bool is_read = app.read_chapters.count(c.id) != 0;
    const Rgb bg = is_cursor && ch_focus ? th::surface : th::bg;
    if (is_cursor && ch_focus) buf.fill(tui::Rect{x + 1, dy, w - 2, 1}, bg);
    int cx = x + 1;
    if (is_cursor) {
      cx = buf.put_str(cx, dy, "\xE2\x96\xB8 ", ch_focus ? th::focus : th::fg2, bg);  // ▸
    } else {
      cx += 2;
    }
    std::string line = c.chapter.empty() ? std::string("—") : c.chapter;
    if (!c.title.empty()) line += "  " + c.title;
    if (const auto it = app.resume_page.find(c.id); it != app.resume_page.end()) {
      line += "  \xC2\xB7 p." + std::to_string(it->second);  // ·
    }
    if (is_read) line += "  \xE2\x9C\x93";  // ✓
    const Rgb fg = is_read ? th::fg3 : (is_cursor && ch_focus ? th::focus : th::fg);
    buf.put_str(cx, dy, tui::truncate_to_cols(line, w - 2 - (cx - (x + 1))), fg,
                bg, is_cursor && ch_focus ? Style::Bold : Style::None);
    ++dy;
  }
  if (scroll + shown < n && dy < y1) {
    buf.put_str(x + 1, dy, "+ " + std::to_string(n - scroll - shown) + " more",
                th::fg3, th::bg);
  }
}

void draw_bottom(const MgApp& app, CellBuffer& buf) {
  const int y = app.win.rows - 1;
  buf.fill(Rect{0, y, app.win.cols, 1}, th::surface);
  if (app.prompt_open) {
    std::string line = "/" + app.prompt;
    buf.put_str(1, y, tui::truncate_to_cols(line, app.win.cols - 4), th::fg,
                th::surface, Style::Bold);
    const int cx = 1 + tui::str_width(line);
    if (cx < app.win.cols - 1) buf.put_str(cx, y, "▌", th::focus, th::surface);
    return;
  }
  std::string status;
  if (app.sweep_running) {
    status = std::string(spinner_frame(app.tick_count)) + " checking " +
             std::to_string(app.sweep_done) + "/" + std::to_string(app.sweep_total) +
             " series…";
  } else if (app.searching) {
    status = std::string(spinner_frame(app.tick_count)) + " searching \"" +
             app.last_query + "\"…";
  } else if (app.view == MgView::Library) {
    status = "library — " + std::to_string(app.library.size()) + " followed";
  } else if (!app.results.empty()) {
    status = std::to_string(app.results.size()) + " results for \"" +
             app.last_query + "\"";
  } else {
    status = std::string(source_name(app));
  }
  // The account chip, right-aligned: a tracker that pushes silently should at
  // least say whose list it is pushing to. Only ever drawn when it fits whole.
  std::string chip;
  if (app.syncing) {
    chip = std::string(spinner_frame(app.tick_count)) + " anilist…";
  } else if (app.auth.anilist.bearer().has_value()) {
    chip = "AL \xC2\xB7 " + account_display(app.auth);
  } else if (app.deps != nullptr && !app.deps->auth_file.empty()) {
    chip = "A connects anilist";
  }
  int status_w = app.win.cols - 2;
  if (!chip.empty()) {
    const int cw = tui::str_width(chip);
    const int cx = app.win.cols - cw - 1;
    if (cx > 20) {
      buf.put_str(cx, y, chip, app.syncing ? th::focus : th::fg3, th::surface);
      status_w = cx - 2;
    }
  }
  if (status_w > 0) {
    buf.put_str(1, y, tui::truncate_to_cols(status, status_w), th::fg2, th::surface);
  }
}

void draw_toasts(const MgApp& app, CellBuffer& buf) {
  const auto toasts = app.toasts.visible();
  int y = app.win.rows - 2;
  for (auto it = toasts.rbegin(); it != toasts.rend() && y >= tui::kContentY0;
       ++it, --y) {
    const std::string text = std::string(tui::toast_glyph(it->kind)) +
                             tui::truncate_to_cols(it->text, tui::kToastMaxCopyCols);
    const int tw = tui::str_width(text) + 2;
    const int tx = app.win.cols - tw - 1;
    if (tx < 0) continue;
    buf.fill(Rect{tx, y, tw, 1}, th::elevated);
    buf.put_str(tx + 1, y, text, tui::toast_fg(it->kind), th::elevated,
                tui::toast_bold(it->kind) ? Style::Bold : Style::None);
  }
}

// Wait up to `timeout` for a worker's completion flag (the anime
// detail::bounded_wait, kept local rather than pulling the whole anime App
// header in for eight lines). Returns whether the flag arrived; the caller
// tears down either way.
bool bounded_wait(std::atomic<bool>& done, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

// --- Reader thread (anime reader_loop + the MgEvent re-wrap seam) -----------

void reader_loop(MgQueue& q, const std::atomic<bool>& stop) {
  tui::InputDecoder dec;
  char buf[512];
  std::vector<Event> evs;  // the decoder's native output type.
  // Re-wrap the two alternatives the decoder ever emits into MgEvent; every
  // other Event alternative is unreachable from the decoder by construction.
  auto post_all = [&q, &evs]() {
    for (auto& e : evs) {
      if (const auto* k = std::get_if<KeyEvent>(&e)) {
        if (!q.try_post(MgEvent{*k})) break;
      } else if (const auto* m = std::get_if<MouseEvent>(&e)) {
        if (!q.try_post(MgEvent{*m})) break;
      }
    }
  };
  while (!stop) {
    const int timeout = dec.esc_pending() ? 40 : 200;
    const int pr = tui::wait_readable(STDIN_FILENO, timeout);
    if (stop) break;
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) {
      evs.clear();
      if (dec.flush_pending(evs)) post_all();
      continue;
    }
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    evs.clear();
    dec.feed(std::string_view(buf, static_cast<std::size_t>(n)), evs);
    post_all();
  }
}

std::string cursor_to(int x, int y) {
  return "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
}

}  // namespace

std::int64_t cover_key(std::string_view uuid) {
  // FNV-1a 64. Interpreted through int64 for CoverState's key type; the
  // wrap-around cast is defined (two's complement).
  std::uint64_t h = 1469598103934665603ull;
  for (const char c : uuid) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  // 0 is the "no target" sentinel at the call sites; nudge a real hash off it.
  if (h == 0) h = 1;
  return static_cast<std::int64_t>(h);
}

const MdManga* selected(const MgApp& app) {
  if (app.view == MgView::Library) {
    if (app.library_cursor < 0 ||
        app.library_cursor >= static_cast<int>(app.library.size())) {
      return nullptr;
    }
    return &app.library[static_cast<std::size_t>(app.library_cursor)].manga;
  }
  if (app.cursor < 0 || app.cursor >= static_cast<int>(app.results.size())) {
    return nullptr;
  }
  return &app.results[static_cast<std::size_t>(app.cursor)];
}

void tick(MgApp& app, const MgEvent& ev) {
  std::visit(
      overloaded{
          [&](const KeyEvent& k) { on_key(app, k); },
          [&](const MouseEvent& m) { on_mouse(app, m); },
          [&](const Resize& r) {
            app.win = tui::WinSize{r.cols, r.rows, r.xpixel, r.ypixel};
            const tui::CellPx px =
                tui::cell_px_from_window(r.xpixel, r.ypixel, r.cols, r.rows);
            if (px.known()) app.cell = px;
            app.dirty = true;
          },
          [&](const Tick&) {
            app.tick_count++;
            app.toasts.tick(app.tick_count);
            cover_tick(app);
            // Spinners and toast expiry animate on the tick clock. The connect
            // overlay runs its own spinner/elapsed clock off the same counter
            // (draw_connect is pure), so a live session repaints every tick.
            if (app.searching || app.chapters_loading || app.sweep_running ||
                app.syncing || app.connect.has_value() || !app.toasts.empty()) {
              app.dirty = true;
            }
          },
          [&](const MgSearchDone& d) {
            if (!app.search_gen.is_current(d.gen)) return;
            app.searching = false;
            app.results = d.results;
            app.cursor = 0;
            app.scroll = 0;
            app.chapters_for.clear();
            app.chapters_pending_for.clear();
            app.chapters.clear();
            app.chapters_loading = false;
            app.focus = MgFocus::Results;
            app.chapter_cursor = 0;
            app.chapter_scroll = 0;
            if (app.results.empty()) {
              app.toasts.push(tui::ToastKind::Info, "no results", app.tick_count);
            }
            app.dirty = true;
          },
          [&](const MgSearchFailed& f) {
            if (!app.search_gen.is_current(f.gen)) return;
            app.searching = false;
            app.toasts.push(tui::ToastKind::Error,
                            error_text(f.cause, source_name(app)),
                            app.tick_count);
            app.dirty = true;
          },
          [&](const MgChaptersDone& d) {
            if (!app.chapters_gen.is_current(d.gen)) return;
            app.chapters_loading = false;
            app.chapters_pending_for.clear();
            app.chapters = d.chapters;
            app.chapters_for = d.manga_id;
            app.chapter_cursor = 0;
            app.chapter_scroll = 0;
            // The cache is what an offline run lists and what the library
            // counts unread against, so every fresh feed rewrites it.
            if (MangaStore* store = store_of(app);
                store != nullptr && !d.scoped_id.empty()) {
              if (auto r = store->put_chapters(d.scoped_id, d.chapters, now_secs());
                  !r.has_value()) {
                note_store_error(app, r.error());
              }
              hydrate_progress(app, d.scoped_id);
              reload_library(app);
            }
            // Focus follows the feed the user asked for (Enter on a result
            // must land somewhere navigable, never a dead end), but only
            // while that manga is still the selection.
            const MdManga* sel = selected(app);
            if (sel != nullptr && sel->id == d.manga_id &&
                !app.chapters.empty()) {
              app.focus = MgFocus::Chapters;
              if (app.continue_pending) {
                app.continue_pending = false;
                continue_reading(app);  // `r` pressed over a cold cache.
              }
            } else {
              app.continue_pending = false;
            }
            app.dirty = true;
          },
          [&](const MgChaptersFailed& f) {
            if (!app.chapters_gen.is_current(f.gen)) return;
            app.chapters_loading = false;
            app.chapters_pending_for.clear();
            app.continue_pending = false;  // `r` must not fire on the next feed.
            app.toasts.push(tui::ToastKind::Error,
                            "chapters: " + error_text(f.cause, source_name(app)),
                            app.tick_count);
            app.dirty = true;
          },
          [&](const MgCoverDone& c) {
            if (app.cover.on_done(c.for_id)) {
              app.cover_pixels = c.pixels;
              app.dirty = true;
            }
          },
          [&](const MgCoverFailed& c) {
            if (app.cover.on_error(c.for_id, app.tick_count)) app.dirty = true;
          },
          [&](const MgPagesProgress& p) {
            if (!app.pages_gen.is_current(p.gen)) return;
            app.pages_done = p.done;
            app.pages_total = p.total;
            app.dirty = true;
          },
          [&](const MgPagesDone& d) {
            if (!app.pages_gen.is_current(d.gen)) return;
            app.pages_fetching = false;
            app.pages_chapter.clear();
            // A queued download never opens a viewer: it walks on to the next
            // chapter and reports once at the end.
            if (app.dl_active) {
              ++app.dl_done;
              start_next_download(app);
              return;
            }
            // Auto-open over the fetched dir while the chapter is still on
            // screen; a moved-away selection just keeps the download (it IS
            // the offline copy — nothing is wasted).
            const MdManga* sel = selected(app);
            if (sel != nullptr && chapters_engaged(app)) {
              for (const MdChapter& ch : app.chapters) {
                if (ch.id == d.chapter_id) {
                  open_viewer(app, *sel, ch, d.dir);
                  return;
                }
              }
            }
            app.toasts.push(tui::ToastKind::Success, "chapter downloaded",
                            app.tick_count);
            app.dirty = true;
          },
          [&](const MgPagesFailed& f) {
            if (!app.pages_gen.is_current(f.gen)) return;
            app.pages_fetching = false;
            app.pages_chapter.clear();
            app.toasts.push(tui::ToastKind::Error,
                            "pages: " + error_text(f.cause, source_name(app)),
                            app.tick_count);
            // One bad chapter does not abandon the queue — it is counted and
            // the walk continues (the summary toast names the failures).
            if (app.dl_active) {
              ++app.dl_failed;
              start_next_download(app);
              return;
            }
            app.dirty = true;
          },
          [&](const MgViewerExited& v) {
            // Pull (and drop) this chapter's pin — whichever of possibly
            // several concurrent viewers just exited, this is the only one
            // whose progress the event is about.
            MgApp::MgOpenViewer ov;
            if (const auto it = app.open_viewers.find(v.chapter_id);
                it != app.open_viewers.end()) {
              ov = it->second;
              app.open_viewers.erase(it);
            }
            if (!v.error.empty()) {
              app.toasts.push(tui::ToastKind::Error, v.error, app.tick_count);
              app.dirty = true;
              return;
            }
            if (!v.last_page.has_value()) {
              app.dirty = true;
              return;
            }
            MangaStore* store = store_of(app);
            // The on-screen copy is only touched while it still belongs to
            // the manga this viewer was reading (the selection may have moved
            // meanwhile); the STORE write always lands, keyed on the manga
            // pinned at spawn.
            const bool on_screen =
                store == nullptr || app.progress_for == ov.manga_scoped;
            // The number pinned at spawn, falling back to the held list for a
            // viewer that predates the pin (a bare frame, or a chapter opened
            // before this field existed in state).
            const std::string number = ov.chapter_number.empty()
                                           ? chapter_number(app, v.chapter_id)
                                           : ov.chapter_number;
            if (*v.last_page >= static_cast<int>(v.total_pages)) {
              // Read to the end: mark + the one-deep undo, captured BEFORE the
              // write so the restore is exact.
              MgReadUndo undo;
              undo.manga_scoped = ov.manga_scoped;
              undo.chapter_id = v.chapter_id;
              if (store != nullptr && !ov.manga_scoped.empty()) {
                auto prior = store->progress_row(ov.manga_scoped, v.chapter_id);
                if (!prior.has_value()) {
                  note_store_error(app, prior.error());
                } else if (prior->has_value()) {
                  undo.had_row = true;
                  undo.prior = **prior;
                }
                if (auto r = store->mark_read(ov.manga_scoped, v.chapter_id,
                                              number, now_secs());
                    !r.has_value()) {
                  note_store_error(app, r.error());
                }
                reload_library(app);
              }
              if (on_screen) {
                app.read_chapters.insert(v.chapter_id);
                app.resume_page.erase(v.chapter_id);
              }
              app.undo_read = undo;
              app.toasts.push(tui::ToastKind::Success,
                              "marked read \xC2\xB7 u undoes", app.tick_count);
              // The tracker bump rides the mark, not the page count: chapter
              // ≡ episode, which is exactly AniList's own manga granularity.
              maybe_push_progress(app, ov.manga_scoped, ov.al_id,
                                  number, ov.finishes_series);
            } else {
              if (on_screen) app.resume_page[v.chapter_id] = *v.last_page;
              if (store != nullptr && !ov.manga_scoped.empty()) {
                if (auto r = store->set_last_page(ov.manga_scoped, v.chapter_id,
                                                  number, *v.last_page, now_secs());
                    !r.has_value()) {
                  note_store_error(app, r.error());
                }
                reload_library(app);
              }
            }
            app.dirty = true;
          },
          [&](const MgSweepChapters& s) {
            if (!app.sweep_gen.is_current(s.gen)) return;
            ++app.sweep_done;
            MangaStore* store = store_of(app);
            if (store == nullptr) return;
            auto cached = store->cached_chapters(s.manga_id);
            if (!cached.has_value()) {
              note_store_error(app, cached.error());
            } else {
              const std::size_t added = new_chapter_ids(*cached, s.chapters).size();
              if (added > 0) {
                app.sweep_new += static_cast<std::uint32_t>(added);
                ++app.sweep_series;
              }
            }
            if (auto r = store->put_chapters(s.manga_id, s.chapters, now_secs());
                !r.has_value()) {
              note_store_error(app, r.error());
            }
            // The swept manga may be the one on screen: keep the visible list
            // in step rather than making the user re-Enter for it.
            const MdManga* sel = selected(app);
            if (sel != nullptr && selected_scoped(app) == s.manga_id &&
                app.chapters_for == sel->id) {
              app.chapters = s.chapters;
            }
            app.dirty = true;
          },
          [&](const MgSweepFailed& f) {
            if (!app.sweep_gen.is_current(f.gen)) return;
            ++app.sweep_done;
            app.dirty = true;
          },
          [&](const MgSweepFinished& f) {
            if (!app.sweep_gen.is_current(f.gen)) return;
            app.sweep_running = false;
            app.sweep_done = f.checked;
            reload_library(app);
            std::string line;
            if (app.sweep_new > 0) {
              line = std::to_string(app.sweep_new) + " new chapters in " +
                     std::to_string(app.sweep_series) + " series";
            } else {
              line = "no new chapters";
            }
            if (f.failed > 0) {
              line += " \xC2\xB7 " + std::to_string(f.failed) + " failed";
            }
            app.toasts.push(app.sweep_new > 0 ? tui::ToastKind::Success
                                              : tui::ToastKind::Info,
                            line, app.tick_count);
            app.dirty = true;
          },
          [&](const ConnectDone& c) {
            app.connect.reset();  // terminal either way: the overlay is done.
            // Disk is the authority (06 §9): the worker wrote auth.json in the
            // Ok branch, so re-read it rather than believing the event.
            if (app.deps != nullptr && !app.deps->auth_file.empty()) {
              app.auth = Auth::load(app.deps->auth_file);
            }
            switch (c.outcome) {
              case ConnectOutcome::Ok:
                app.toasts.push(tui::ToastKind::Success,
                                "connected \xC2\xB7 " + (c.user_name.empty()
                                                             ? std::string("anilist")
                                                             : c.user_name),
                                app.tick_count);
                break;
              case ConnectOutcome::NoToken:
                app.toasts.push(tui::ToastKind::Error, "no token in the callback",
                                app.tick_count);
                break;
              case ConnectOutcome::Rejected:
                app.toasts.push(tui::ToastKind::Error, "anilist rejected the token",
                                app.tick_count);
                break;
              case ConnectOutcome::NetworkError:
                app.toasts.push(tui::ToastKind::Error, "anilist unreachable",
                                app.tick_count);
                break;
              case ConnectOutcome::SaveFailed:
                app.toasts.push(tui::ToastKind::Error, "could not save auth.json",
                                app.tick_count);
                break;
              case ConnectOutcome::BadState:
                app.toasts.push(tui::ToastKind::Error, "login state mismatch",
                                app.tick_count);
                break;
              case ConnectOutcome::Canceled:
                break;  // the cancel path already said so.
            }
            app.dirty = true;
          },
          [&](const MgSyncDone& s) {
            app.syncing = false;
            app.syncing_manga.clear();
            app.sync_flag.reset();
            // The local high-water mark moves only on a completed push — a
            // failed one must stay re-pushable.
            if (s.outcome == MgSyncOutcome::Pushed) {
              if (MangaStore* store = store_of(app);
                  store != nullptr && !s.manga_id.empty()) {
                if (auto r = store->set_al_synced(s.manga_id, s.progress);
                    !r.has_value()) {
                  note_store_error(app, r.error());
                }
              }
              reload_library(app);
              app.toasts.push(tui::ToastKind::Success,
                              "anilist \xC2\xB7 ch " + std::to_string(s.progress),
                              app.tick_count);
            } else if (s.outcome == MgSyncOutcome::UpToDate) {
              app.toasts.push(tui::ToastKind::Info, "anilist already up to date",
                              app.tick_count);
            } else {
              app.toasts.push(tui::ToastKind::Error,
                              "anilist: " + error_text(s.cause, "anilist"),
                              app.tick_count);
            }
            app.dirty = true;
          },
      },
      ev);
}

void draw(MgApp& app, CellBuffer& buf) {
  buf.resize(app.win.cols, app.win.rows);
  buf.clear();
  app.cover_plan = MgCoverPlan{};  // rebuilt below when a cover slot renders.
  app.chapters_plan = MgChaptersPlan{};  // ditto: stale rows must not hit-test.

  draw_top_bar(app, buf);
  const int y0 = tui::kContentY0;
  const int y1 = app.win.rows - 1;
  if (y1 > y0) {
    const bool lib = app.view == MgView::Library;
    if (app.win.cols >= tui::kPaneSplitMin) {
      const tui::PaneSplit ps = tui::pane_split(app.win.cols);
      if (lib) {
        draw_library(app, buf, 0, ps.list_w, y0, y1);
      } else {
        draw_list(app, buf, 0, ps.list_w, y0, y1);
      }
      draw_detail(app, buf, ps.detail_x, ps.detail_w, y0, y1);
    } else if (lib) {
      draw_library(app, buf, 0, app.win.cols, y0, y1);
    } else {
      draw_list(app, buf, 0, app.win.cols, y0, y1);
    }
  }
  draw_bottom(app, buf);
  draw_toasts(app, buf);

  // The connect overlay, drawn LAST over whatever the panes painted (the
  // anime modal's law). The cover comes down with it: a kitty placement is
  // owned by the terminal, not the cell grid, so it would otherwise sit ON
  // TOP of the float. Clearing the plan makes run()'s reconcile delete it.
  if (app.connect.has_value()) {
    const MgApp::MgConnectSession& s = *app.connect;
    const std::uint64_t elapsed = app.tick_count - s.started;
    app.cover_plan = MgCoverPlan{};
    const tui::ConnectView view{s.url, elapsed / 10, elapsed * 100, false};
    tui::draw_connect(view, Rect{0, 0, buf.width(), buf.height()}, buf);
  }

  // A4: the cover rect is image-owned only while pixels are actually placed.
  if (app.cover_plan.have_pixels && !app.cover_plan.rect.empty()) {
    buf.set_exclusion(app.cover_plan.rect);
  } else {
    buf.set_exclusion(Rect{});
  }
}

int run(const MgDeps& deps) {
  tui::Terminal term;
  if (!term.ok()) return 1;

  MgApp app;
  app.win = tui::query_winsize();
  if (app.win.cols == 0) app.win = tui::WinSize{80, 24, 0, 0};

  // Cover probe: once, after the alternate screen, before the reader thread
  // exists (it reads stdio raw — same ordering law as the anime run()).
  const tui::CoverProbe probe =
      tui::probe_cover_support(app.win.cols, app.win.rows);
  app.cover_backend = tui::backend_from(probe);
  app.cell = probe.cell;
  debug_log("probe: kitty=" + std::to_string(probe.kitty) +
            " iterm=" + std::to_string(probe.iterm) +
            " iterm2=" + std::to_string(probe.iterm2) +
            " sixel=" + std::to_string(probe.sixel) + " cell=" +
            std::to_string(probe.cell.width) + "x" +
            std::to_string(probe.cell.height) + " win=" +
            std::to_string(app.win.cols) + "x" + std::to_string(app.win.rows) +
            " px=" + std::to_string(app.win.xpixel) + "x" +
            std::to_string(app.win.ypixel));

  term.enable_mouse();

  MgQueue queue;
  app.queue = &queue;
  app.deps = &deps;
  // Disk truth for the tracker account, read once here and re-read after every
  // connect. A missing/corrupt file loads as the signed-out record (06 §3.1),
  // so this never gates startup.
  if (!deps.auth_file.empty()) app.auth = Auth::load(deps.auth_file);
  // Landing view: the library when there is one, else search — a reader with
  // a shelf should see the shelf.
  if (deps.store != nullptr) {
    reload_library(app);
    if (!app.library.empty()) app.view = MgView::Library;
  }
  app.toasts.push(tui::ToastKind::Info,
                  app.view == MgView::Library
                      ? "L for search \xC2\xB7 r continues reading"
                      : "/ to search " + std::string(source_name(app)),
                  0);

  std::atomic<bool> reader_stop{false};
  std::thread reader(reader_loop, std::ref(queue), std::cref(reader_stop));

  CellBuffer front(app.win.cols, app.win.rows);
  CellBuffer back;
  std::string out, pre_diff, post_diff, resize_purge;

  // The placement the terminal holds (run-owned; draw is pure — the anime
  // PlacedCover bridge).
  struct Placed {
    std::uint32_t id = 0;
    Rect rect;
    std::int64_t for_id = 0;
  } placed;
  bool placements_stale = false;

  auto paint = [&]() {
    draw(app, back);
    pre_diff.clear();
    post_diff.clear();
    resize_purge.clear();
    bool purged = false;
    const bool is_kitty = app.cover_backend == tui::CoverBackend::Kitty;
    if (placements_stale) {
      placements_stale = false;
      if (placed.id != 0) {
        // A resize breaks the placement model (reflow/re-anchor/drop — the
        // 2df5af5 lesson): purge and let the reconcile below re-transmit.
        // Kitty needs the delete APC; an OSC 1337 image dies with the cells
        // the resize ED clear wipes.
        if (is_kitty) resize_purge = tui::kitty::delete_image(placed.id);
        placed = Placed{};
        purged = true;
      }
    }
    const bool want = app.cover_plan.have_pixels && !app.cover_plan.rect.empty();
    bool changed = false;
    if (want && (placed.id == 0 || placed.for_id != app.cover_plan.want_id ||
                 placed.rect != app.cover_plan.rect)) {
      if (placed.id != 0 && is_kitty)
        pre_diff += tui::kitty::delete_image(placed.id);
      post_diff += cursor_to(app.cover_plan.rect.x, app.cover_plan.rect.y);
      // Same three-way choice the anime app makes (its detail::cover_transmit);
      // switched, not if/else, so a fourth backend cannot slip past this site.
      switch (app.cover_backend) {
        case tui::CoverBackend::Kitty: {
          tui::kitty::Image img;
          img.rgba = app.cover_pixels.rgba;
          img.w = app.cover_pixels.w;
          img.h = app.cover_pixels.h;
          post_diff += tui::kitty::transmit(img, kCoverImageId,
                                            app.cover_plan.rect.w,
                                            app.cover_plan.rect.h);
          break;
        }
        case tui::CoverBackend::Iterm:
          post_diff += tui::iterm::transmit(app.cover_pixels.rgba,
                                            app.cover_pixels.w, app.cover_pixels.h,
                                            app.cover_plan.rect.w,
                                            app.cover_plan.rect.h);
          break;
        case tui::CoverBackend::Sixel:
          // Pixel-sized, so it needs the per-cell geometry backend_from
          // guarantees is known whenever it picked this backend.
          post_diff += tui::sixel::transmit(
              app.cover_pixels.rgba, app.cover_pixels.w, app.cover_pixels.h,
              app.cover_plan.rect.w, app.cover_plan.rect.h, app.cell.width,
              app.cell.height);
          break;
        case tui::CoverBackend::None:
          break;  // placeholder mode; the cover plan never asks for pixels.
      }
      placed = Placed{kCoverImageId, app.cover_plan.rect, app.cover_plan.want_id};
      changed = true;
    } else if (!want && placed.id != 0) {
      // Kitty gets the delete APC; under iterm the mark_all_dirty repaint
      // below overwrites the vacated cells, which is the erase.
      if (is_kitty) pre_diff += tui::kitty::delete_image(placed.id);
      placed = Placed{};
      changed = true;
    }
    if (changed || purged) back.mark_all_dirty();
    out.clear();
    back.flush(front, out, resize_purge + pre_diff, post_diff);
    term.write(out);
  };

  paint();

  while (!app.quit) {
    if (tui::take_resize_flag()) {
      const tui::WinSize ws = tui::query_winsize();
      placements_stale = true;
      queue.try_post(MgEvent{Resize{ws.cols, ws.rows, ws.xpixel, ws.ypixel}});
    }
    std::optional<MgEvent> ev = queue.wait_next();
    if (!ev.has_value()) {
      tick(app, MgEvent{Tick{}});
    } else {
      tick(app, *ev);
    }
    if (app.quit) break;
    if (app.dirty) {
      paint();
      app.dirty = false;
    }
  }

  // Quit flush (P20's shape, one row wide): a push spawned by the last chapter
  // read gets a bounded moment to land. No drain rights (A1) — the deadline is
  // the whole budget, and _exit(0) below takes the address space with it either
  // way, so an abandoned worker never touches freed memory. The flag is the
  // signal, not the queue, which has no reader left to act on an event.
  if (app.syncing && app.sync_flag) {
    (void)bounded_wait(*app.sync_flag, std::chrono::seconds(2));
  }

  // Quit (A1's production path, the anime run() verbatim): detached workers
  // hold references to this frame's queue/deps — _exit(0) tears the address
  // space down atomically instead of unwinding under them.
  reader_stop = true;
  queue.shutdown();
  reader.detach();
  // Kitty only: free the terminal's stored image (quota). An OSC 1337 image
  // lives in the alternate screen's cells and dies with term.restore().
  if (placed.id != 0 && app.cover_backend == tui::CoverBackend::Kitty)
    term.write(tui::kitty::delete_image(placed.id));
  term.restore();
  std::fflush(nullptr);
  _exit(0);
}

}  // namespace shigoku::manga
