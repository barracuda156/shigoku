// app.cpp — App state machine + pure render + event loop (P6/P7, A1/A2/A3).

#include "app.hpp"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>

// Real deps (app.hpp only forward-declares these — see its file comment on
// why views/app.hpp must not reach http/provider). app.cpp is the one P7 TU
// allowed to include them: it implements the worker spawns, not the render.
#include "../anilist.hpp"
#include "../aniskip.hpp"
#include "../debug_log.hpp"
#include "../download.hpp"
#include "../login.hpp"
#include "../loopback.hpp"
#include "../mal_loopback.hpp"
#include "../mal_login.hpp"
#include "../mal_mirror.hpp"
#include "../player.hpp"
#include "../provider.hpp"
#include "../store.hpp"
#include "../sync.hpp"
#include "../updatecheck.hpp"
#include "cover_probe.hpp"
#include "covers.hpp"
#include "input.hpp"
#include "iterm.hpp"
#include "kitty.hpp"
#include "prewarm.hpp"
#include "resolve_transport.hpp"
#include "sixel.hpp"
#include "views.hpp"

#include <ctime>

namespace shigoku::tui {

namespace {

// Forward decls (definitions later in this anonymous namespace).
void recompute_loading(App& app);
void load_history(App& app);
void load_schedule(App& app);  // P37.
void flush_sync(App& app, bool pull_only);  // P20; on_connect_result fires it post-bootstrap.

// Wall-clock seconds for store timestamps (updated_at / last_watched_at). The
// store only compares these for ordering (latest_resume, History sort), so a
// coarse epoch second is enough; keep it in one place so every writer agrees.
std::int64_t now_epoch_secs() { return static_cast<std::int64_t>(::time(nullptr)); }

// Persistent-toast topic for AniList reachability (app.rs ANILIST_TOPIC, §8.5):
// an enrich transport failure raises it; any AniList success (enrich hit or
// confirmed-null) clears it. One singleton across the session.
inline constexpr std::string_view kAnilistTopic = "anilist";

// The provider name to stamp on the in-flight play's store rows (P15): the
// serving provider the grid was fetched under. Falls back to primary()'s name
// only on the degraded P7 path (play.provider unset), where primary() IS the
// one provider that served the grid. Returns a view into play.provider / the
// registry (both outlive the write). "" when there is no provider at all.
std::string_view play_provider_name(const App& app) {
  if (!app.play.provider.empty()) return app.play.provider;
  if (app.deps != nullptr && app.deps->registry != nullptr &&
      app.deps->registry->primary() != nullptr) {
    return app.deps->registry->primary()->name();
  }
  return {};
}

// Save on leave/quit only, never per-keystroke (P18, settings.rs
// persist_settings): a no-op unless a Settings key actually mutated config
// this session. A missing config dir warns rather than erroring — the parent
// dir is ensure_dirs()'s job at startup, not this call's.
void persist_settings(App& app) {
  if (!app.settings.dirty()) return;
  app.settings.clear_dirty();
  const std::size_t slash = app.config_file.find_last_of('/');
  const std::string parent = slash == std::string::npos ? "" : app.config_file.substr(0, slash);
  struct stat st{};
  if (parent.empty() || ::stat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    app.toasts.push(ToastKind::Warning, "no config dir \xC2\xB7 not saved", app.tick_count);
    return;
  }
  auto r = app.config.save(app.config_file);
  if (r.has_value()) {
    app.toasts.push(ToastKind::Success, "settings saved", app.tick_count);
  } else {
    app.toasts.push(ToastKind::Error, "settings save failed", app.tick_count);
  }
}

// Settings "account" row display (P19, DESIGN §5.5): the signed-in name, or
// the fixed "not connected" copy. bearer() (not a raw access_token check) so
// a control-byte-poisoned token still reads as signed-out here too.
std::string account_display(const Auth& auth) {
  if (!auth.anilist.bearer().has_value()) return "not connected";
  return auth.anilist.user_name.empty() ? "connected" : auth.anilist.user_name;
}

// Refresh EpisodeState::watched from the store for the currently-open grid
// (05 §10.5). No-op (leaves marks empty) when no store is wired or the grid is
// empty — the render then draws every cell unmarked, the P7 behavior.
void refresh_watched_marks(App& app) {
  app.episode.watched.clear();
  if (app.deps == nullptr || app.deps->store == nullptr) return;
  if (app.episode.episodes.empty()) return;
  // v0 plays Sub only (spawn_play hardcodes it); mark against that track.
  auto marks = app.deps->store->watched_marks(app.episode.for_id, Translation::Sub,
                                              app.episode.episodes);
  if (marks.has_value()) {
    app.episode.watched.assign(marks->begin(), marks->end());
  }
}

// History load (P16, 05 §2): a SYNCHRONOUS local read, not a worker —
// deliberate deviation from the async-event pattern (History never touches
// the network, only the local store). A failure keeps the previous rows (a
// reload must never wipe the list on a soft fail). Anchors the cursor on the
// OUTGOING rows before replacing them (`order` indexes the old set, and the
// incoming one may be shorter — a delete).
void load_history(App& app) {
  if (app.deps == nullptr || app.deps->store == nullptr) return;
  auto rows = app.deps->store->list_history();
  if (!rows.has_value()) {
    app.history.load_failed = true;
    return;
  }
  const std::optional<std::int64_t> anchor = [&]() -> std::optional<std::int64_t> {
    if (app.history.cursor >= app.history.order.size()) return std::nullopt;
    return app.history.rows[app.history.order[app.history.cursor]].enrichment.anilist_id;
  }();
  app.history.load_failed = false;
  app.history.resume.clear();
  app.history.resume.reserve(rows->size());
  for (const Show& s : *rows) {
    std::optional<std::uint32_t> ep;
    auto r = app.deps->store->latest_resume(s.enrichment.anilist_id, Translation::Sub);
    if (r.has_value() && r->has_value()) {
      // The label is the raw episode string; only a numeric label carries the
      // ◐ marker (non-numeric "SP1"-shaped labels carry none — history.rs).
      try {
        ep = static_cast<std::uint32_t>(std::stoul((*r)->first));
      } catch (...) {
        ep = std::nullopt;
      }
    }
    app.history.resume.push_back(ep);
  }
  app.history.rows = std::move(*rows);
  app.history.rebuild(anchor);
}

// Schedule load (P37): a SYNCHRONOUS local read, same store call as
// load_history — Schedule keeps its own copy of the library rows rather than
// sharing History's (the two views' cursors/filters are independent). No
// resume-marker lookup (Schedule doesn't render one). rebuild() consumes
// app.clock_epoch_secs, the boot/tick stamp — never a fresh clock read here.
void load_schedule(App& app) {
  if (app.deps == nullptr || app.deps->store == nullptr) return;
  auto rows = app.deps->store->list_history();
  if (!rows.has_value()) {
    app.schedule.load_failed = true;
    return;
  }
  app.schedule.load_failed = false;
  app.schedule.rows = std::move(*rows);
  app.schedule.rebuild(app.clock_epoch_secs);
}

// P37 slice 3: clear the History NEW marker for one show, both in the
// persisted store and in whichever in-memory row copies are currently
// loaded (History and Schedule each keep their own `vector<Show>`, per
// load_history/load_schedule above) — a store-only clear would leave a
// stale marker on screen until the next full reload. Store-less callers
// (demo mode, store-less tests) only touch the in-memory copies.
void clear_notice_pending(App& app, std::int64_t anilist_id) {
  if (app.deps != nullptr && app.deps->store != nullptr) {
    (void)app.deps->store->clear_notice_pending(anilist_id);
  }
  for (Show& row : app.history.rows) {
    if (row.enrichment.anilist_id == anilist_id) row.notice_pending = false;
  }
  for (Show& row : app.schedule.rows) {
    if (row.enrichment.anilist_id == anilist_id) row.notice_pending = false;
  }
}

// P37 slice 3: scan the watchlist for episodes that aired since the last
// check, raise ONE aggregate toast, and persist both the dedup high-water
// mark and the History NEW marker per affected show. Called at boot and
// after every completed sync pull (on_sync_flushed below) — a plain
// synchronous store read + writes, same shape as load_history (no worker:
// this is local-only, no network). A store-less app (demo/tests) is a
// silent no-op, matching load_history's own degrade.
void check_schedule_notices(App& app) {
  if (app.deps == nullptr || app.deps->store == nullptr) return;
  auto rows = app.deps->store->list_history();
  if (!rows.has_value()) return;
  const std::int64_t now = now_epoch_secs();
  const std::vector<ScheduleNotice> notices = detect_schedule_notices(*rows, now);
  if (notices.empty()) return;
  std::size_t raised = 0;
  for (const ScheduleNotice& n : notices) {
    const Show& row = (*rows)[n.show_index];
    const std::int64_t aid = row.enrichment.anilist_id;
    if (!row.notice_last_episode.has_value()) {
      // First-ever look at this row (fresh v6 migration or a new library
      // add): seed the mark QUIETLY — the plan's "last-run stamp" duty. A
      // mature library is full of long-past next_airing_* rows (ROD-261
      // keeps them when a refresh omits the field), and raising for those
      // would storm the first boot with months-old notices (P37 review).
      (void)app.deps->store->seed_schedule_notice(aid, n.episode);
      continue;
    }
    (void)app.deps->store->set_schedule_notice(aid, n.episode);
    ++raised;
  }
  if (raised > 0) {
    app.toasts.push(ToastKind::Info,
                    std::to_string(raised) + " new episode" + (raised == 1 ? "" : "s") + " aired",
                    app.tick_count);
  }
  // Re-sync whichever in-memory row copies are already loaded, same
  // reasoning as clear_notice_pending — a toast with no visible marker (or a
  // seeded mark the next check would re-derive) until the next manual reload
  // would be a confusing half-update.
  if (!app.history.rows.empty()) load_history(app);
  if (!app.schedule.rows.empty()) load_schedule(app);
}

// --- P15 resolve-walk transport bridge -------------------------------------

// Build the per-call borrows the EpisodeSession / PrewarmState need.
// global_pref/translation are config-live (P18): "" preferred_provider means
// follow the registry's construction leader (03 §3.2), same as an unset
// Settings row.
EpisodeDeps build_episode_deps(App& app) {
  EpisodeDeps d;
  if (app.deps != nullptr) {
    d.store = app.deps->store;
    d.registry = app.deps->registry;
  }
  d.queue = app.queue;
  d.global_pref = app.config.preferred_provider;
  d.translation = parse_translation(app.config.translation).value_or(Translation::Sub);
  d.unix_now = now_epoch_secs();
  return d;
}

// Mirror the transport's grid state into App.episode so the pure grid render
// (views.cpp) reads only App, never the session. Called after every transport
// step. Repaints the watched marks from the store for the landed grid.
void mirror_session_to_episode(App& app) {
  if (!app.episode_session) return;
  const EpisodeSession& s = *app.episode_session;
  app.episode.for_id = s.for_id();
  app.episode.episodes = s.episodes();
  app.episode.provider_id = s.serving_id();
  app.episode.cursor = s.cursor();
  app.episode.loading = s.loading();
  app.episode.no_source = s.no_source();
  app.episode.fetched = !s.loading() && (!s.episodes().empty() || s.no_source());
  // Provider caption (05 §15): serving provider + pin + availability markers.
  app.episode.serving = s.serving().has_value() ? *s.serving() : std::string{};
  app.episode.pinned = s.pin().has_value() ? *s.pin() : std::string{};
  app.episode.avail.clear();
  for (const auto& [name, a] : s.avail()) {
    AvailMark m = AvailMark::Unchecked;
    switch (a) {
      case ProviderAvailability::Bound:     m = AvailMark::Bound; break;
      case ProviderAvailability::Absent:    m = AvailMark::Absent; break;
      case ProviderAvailability::Unchecked: m = AvailMark::Unchecked; break;
    }
    app.episode.avail.emplace_back(name, m);
  }
  refresh_watched_marks(app);
  recompute_loading(app);
  app.dirty = true;
}

// Turn one transport Feedback into a toast (04 §6: the session never toasts).
// The provider display name is looked up for the copy; a retired name falls
// back to the stored key.
void toast_feedback(App& app, const Feedback& fb) {
  auto disp = [&](const std::string& key) -> std::string {
    if (app.deps != nullptr && app.deps->registry != nullptr) {
      if (const StreamProvider* p = app.deps->registry->by_name(key); p != nullptr) {
        return std::string(p->display_name());
      }
    }
    return key;
  };
  switch (fb.kind) {
    case Feedback::Kind::Hop:
      app.toasts.push(ToastKind::Info, "trying " + disp(fb.provider), app.tick_count);
      return;
    case Feedback::Kind::NoMatch:
      app.toasts.push(ToastKind::Warning, "no match on " + disp(fb.provider) + " · trying others",
                      app.tick_count);
      return;
    case Feedback::Kind::PinKept:
      app.toasts.push(ToastKind::Warning, "no match on " + disp(fb.provider) + " · pin kept",
                      app.tick_count);
      return;
    case Feedback::Kind::PinUnreachable:
      app.toasts.push(ToastKind::Error, disp(fb.provider) + " unreachable · pin kept",
                      app.tick_count);
      return;
    case Feedback::Kind::DeadEnd:
      app.toasts.push(ToastKind::Warning, "no source for this show", app.tick_count);
      return;
    case Feedback::Kind::Fail: {
      const auto [kind, copy] = detail::provider_error_toast(fb.cause, disp(fb.provider));
      app.toasts.push(kind, copy, app.tick_count);
      return;
    }
    case Feedback::Kind::PinSet:
      app.toasts.push(ToastKind::Success, "pinned " + disp(fb.provider), app.tick_count);
      return;
    case Feedback::Kind::PinCleared:
      app.toasts.push(ToastKind::Info, "pin cleared", app.tick_count);
      return;
    case Feedback::Kind::PinNothing:
      app.toasts.push(ToastKind::Info, "nothing to pin", app.tick_count);
      return;
    case Feedback::Kind::PinPending:
      app.toasts.push(ToastKind::Info, "resolving · try again", app.tick_count);
      return;
    case Feedback::Kind::PinSaveFailed:
      app.toasts.push(ToastKind::Error,
                      fb.clearing ? "couldn't clear pin" : "couldn't save pin", app.tick_count);
      return;
  }
}

// Apply a batch of transport feedback + mirror the grid state.
void apply_feedback(App& app, const std::vector<Feedback>& fbs) {
  for (const Feedback& fb : fbs) toast_feedback(app, fb);
  mirror_session_to_episode(app);
}

}  // namespace

// ===========================================================================
// P7 pure helpers: A8 tier classifier + error->toast mapping (DESIGN §4.10).
// Exposed via detail:: (app.hpp) so tests can exercise them without a worker.
// ===========================================================================

namespace detail {

Tier classify_tier(const Enrichment& show, const StreamProvider& provider) {
  return provider.canonical_key(show).has_value() ? Tier::AKey : Tier::CSearch;
}

std::pair<ToastKind, std::string> provider_error_toast(const ProviderError& e,
                                                        std::string_view provider) {
  switch (e.kind) {
    case ProviderError::Kind::Network:
      return {ToastKind::Error, "network unreachable"};
    case ProviderError::Kind::Forbidden:
      return {ToastKind::Error, std::string(provider) + " blocked us"};
    case ProviderError::Kind::Server:
      return {ToastKind::Error, std::string(provider) + " is down"};
    case ProviderError::Kind::Http:
      return {ToastKind::Error, std::string(provider) + " returned an error"};
    case ProviderError::Kind::Decode:
      return {ToastKind::Error, "couldn't load episodes"};
    case ProviderError::Kind::Unsupported:
      return {ToastKind::Error, std::string(provider) + " returned an error"};
    case ProviderError::Kind::RateLimited:
      return {ToastKind::Error, std::string(provider) + " is rate-limiting us"};
  }
  return {ToastKind::Error, "playback failed"};  // unreachable.
}

std::pair<ToastKind, std::string> play_error_toast(const PlayError& e,
                                                    std::string_view provider) {
  (void)provider;  // no PlayError kind splices a provider name (DESIGN §4.10).
  switch (e.kind) {
    case PlayError::Kind::MpvNotFound:
      return {ToastKind::Error, "mpv not found · install mpv"};
    case PlayError::Kind::Spawn:
      return {ToastKind::Error, "mpv exited with error"};
    case PlayError::Kind::Wait:
      return {ToastKind::Error, "mpv exited with error"};
    case PlayError::Kind::Exit:
      return {ToastKind::Error, "mpv exited with error"};
    case PlayError::Kind::OpenFailed:
      return {ToastKind::Error, "stream didn't open · try again"};
    case PlayError::Kind::Resolve:
      return {ToastKind::Error, "playback failed"};
    case PlayError::Kind::UnsafeUrl:
    case PlayError::Kind::UnsafeArg:
      return {ToastKind::Error, "playback failed"};
  }
  return {ToastKind::Error, "playback failed"};  // unreachable.
}

// --- P8 cover helpers (DESIGN §3.3, A4) ------------------------------------
//
// cover_target()/cover_rect() are PURE geometry over App + cover_caps and live
// in views.cpp (the render target) so views.cpp's own references resolve within
// shigoku_tui_render — app.hpp declares them in detail::. compose_cover_apc()
// stays here: it builds APC bytes via kitty:: (render target) and is only ever
// called from run() (shigoku_tui).

// One cover transmit, whichever protocol the probe selected — the single
// place the three emitters are chosen between, so no call site can drift.
// The switch is deliberate (A2): a fourth backend fails to compile here until
// it is handled. Note what each protocol needs to size the image: kitty takes
// a cell box plus the source pixel dims, OSC 1337 takes a cell box, and SIXEL
// alone is drawn at literal pixel size — hence the per-cell geometry, which
// backend_from guarantees is known whenever it picked Sixel.
std::string cover_transmit(const App& app, const CoverPixels& px,
                           std::uint32_t id, const Rect& r) {
  switch (app.cover_caps.backend) {
    case CoverBackend::Kitty: {
      kitty::Image img;
      img.rgba = px.rgba;
      img.w = px.w;
      img.h = px.h;
      return kitty::transmit(img, id, r.w, r.h);
    }
    case CoverBackend::Iterm:
      return iterm::transmit(px.rgba, px.w, px.h, r.w, r.h);
    case CoverBackend::Sixel:
      return sixel::transmit(px.rgba, px.w, px.h, r.w, r.h,
                             app.cover_caps.cell.width,
                             app.cover_caps.cell.height);
    case CoverBackend::None:
      break;  // placeholder mode; every caller gates on images() first.
  }
  return {};
}

bool purge_placements(CoverBackend backend, PlacedCover& placed_cover,
                      std::map<std::string, Placed>& placed_grid,
                      std::string& purge) {
  purge.clear();
  bool held = false;
  if (placed_cover.id != 0) {
    if (backend == CoverBackend::Kitty)
      purge += kitty::delete_image(placed_cover.id);
    placed_cover = PlacedCover{};
    held = true;
  }
  for (const auto& [url, p] : placed_grid) {
    if (backend == CoverBackend::Kitty) purge += kitty::delete_image(p.id);
    held = true;
  }
  placed_grid.clear();
  return held;
}

bool compose_cover_apc(const App& app, PlacedCover& placed,
                       std::string& pre_diff, std::string& post_diff) {
  pre_diff.clear();
  post_diff.clear();
  const CoverBackend backend = app.cover_caps.backend;
  if (!app.cover_caps.images()) {
    // Placeholder mode: never emit; a stale placement record is still cleared
    // (defensive — the backend can't change mid-session, but keep the
    // invariant, and the returned true forces the repaint that erases).
    if (placed.id != 0) {
      placed = PlacedCover{};
      return true;
    }
    return false;
  }

  const Rect r = cover_rect(app);
  // Transmit ONLY when the held pixels belong to the currently selected show —
  // the same predicate draw() uses to mark the rect image-owned, so run() and
  // draw() never diverge (a stale cover from a browsed-away show is deleted,
  // not painted over its placeholder). cover_target is the single source of the
  // "which show is selected" truth.
  const std::optional<CoverTarget> target = cover_target(app);
  const bool want =
      app.cover_render.have_pixels && !r.empty() &&
      app.cover_render.pixels.w > 0 && app.cover_render.pixels.h > 0 &&
      target.has_value() && target->id == app.cover_render.for_id;

  if (!want) {
    // No incoming cover: delete whatever the terminal still holds. Kitty gets
    // the delete APC; under iterm no delete exists — the returned true forces
    // the repaint that overwrites the vacated cells, which is how an OSC 1337
    // image dies.
    if (placed.id != 0) {
      if (backend == CoverBackend::Kitty)
        pre_diff += kitty::delete_image(placed.id);
      placed = PlacedCover{};
      return true;
    }
    return false;
  }

  // Steady state: the terminal already holds THIS show's pixels at THIS rect —
  // emit nothing. (Same-show pixel replacement always passes through a !want
  // frame first: begin_fetch clears have_pixels, so the placement is deleted
  // before fresh pixels can land.) Without this short-circuit the full ~180KB
  // transmit went out EVERY frame and flooded slow terminals.
  if (placed.id != 0 && placed.for_id == target->id && placed.rect == r) {
    return false;
  }

  // A4 composition: delete the outgoing placement FIRST (prevents one frame of
  // old+new overlap under contour's fragment model), then transmit the incoming
  // one AFTER the diff (post_diff), CUP'd to the cover origin, C=1 so the image
  // path never moves the cursor the CellBuffer owns. Under iterm there is no
  // delete: the forced repaint overwrites the old rect's cells, and the cursor
  // the OSC moves is hidden and re-CUP'd by the next diff anyway.
  if (placed.id != 0 && backend == CoverBackend::Kitty) {
    pre_diff += kitty::delete_image(placed.id);
  }
  // CUP to the cover origin cell (1-based), then transmit.
  post_diff += "\x1b[";
  post_diff += std::to_string(r.y + 1);
  post_diff += ';';
  post_diff += std::to_string(r.x + 1);
  post_diff += 'H';
  post_diff += cover_transmit(app, app.cover_render.pixels, kDetailCoverImageId, r);
  debug_log("cover transmit: detail id=" + std::to_string(kDetailCoverImageId) +
            " px=" + std::to_string(app.cover_render.pixels.w) + "x" +
            std::to_string(app.cover_render.pixels.h) +
            " cells=" + std::to_string(r.w) + "x" + std::to_string(r.h) +
            " bytes=" + std::to_string(post_diff.size()));
  placed = PlacedCover{kDetailCoverImageId, target->id, r};
  return true;
}

// Generalizes compose_cover_apc for the P17 grid (§6, R1/R3). run() owns
// `placed` (url -> Placed) across frames; draw() stays pure. Per frame, when
// view == Discover && cover_caps.images(): the DESIRED set is exactly the urls
// draw_discover marked as exclusions this frame (same card_cover_rect call,
// same Ready-in-the-render-store predicate — R3, they must never disagree).
// pre_diff deletes every placed url that is gone or moved; post_diff CUP+
// transmits every desired url not already placed at that rect. Returns
// whether ANY placement changed (caller forces mark_all_dirty, same reasoning
// as the single-cover rule: the exclusion made prev skip those cells).
bool compose_grid_apc(const App& app, std::map<std::string, Placed>& placed,
                      std::string& pre_diff, std::string& post_diff) {
  pre_diff.clear();
  post_diff.clear();
  bool changed = false;

  const CoverBackend backend = app.cover_caps.backend;
  std::map<std::string, Rect> desired;
  if (app.view == View::Discover && app.cover_caps.images()) {
    const DiscoverAxis axis = app.discover.axis();
    const AxisSlot& slot = app.discover.slot_for(axis);
    const int w = app.win.cols;
    const int y0 = kContentY0, y1 = static_cast<int>(app.win.rows) - 1;
    if (y1 > y0) {
      const int gy = y0 + 2;
      if (gy < y1) {
        const int content_h = y1 - y0;
        const GridGeo geo = grid_geo(w, content_h, app.cover_caps.cell);
        for (std::size_t i = 0; i < slot.entries.size(); ++i) {
          const Rect r = card_cover_rect(geo, i, slot.scroll_row, /*grid_x=*/2, gy, w, y1);
          if (r.empty()) continue;
          const Enrichment& e = slot.entries[i].meta;
          if (!e.cover_url.has_value()) continue;
          if (app.cover_render_store.get(*e.cover_url) == nullptr) continue;
          desired[*e.cover_url] = r;  // last-wins is fine: a url appears once per axis slot.
        }
      }
    }
  }

  // pre_diff: delete every placed url that's gone from `desired` OR moved
  // (kitty only — under iterm the forced repaint overwrites the vacated
  // cells, which is the erase).
  for (auto it = placed.begin(); it != placed.end();) {
    const auto want = desired.find(it->first);
    if (want == desired.end() || !(want->second == it->second.rect)) {
      if (backend == CoverBackend::Kitty)
        pre_diff += kitty::delete_image(it->second.id);
      changed = true;
      it = placed.erase(it);
    } else {
      ++it;
    }
  }

  // post_diff: transmit every desired url not already placed at that rect.
  for (const auto& [url, rect] : desired) {
    if (placed.find(url) != placed.end()) continue;  // unchanged, still placed.
    const CoverRenderEntry* entry = app.cover_render_store.get(url);
    if (entry == nullptr) continue;  // adopted-then-evicted race; skip this frame.
    post_diff += "\x1b[";
    post_diff += std::to_string(rect.y + 1);
    post_diff += ';';
    post_diff += std::to_string(rect.x + 1);
    post_diff += 'H';
    post_diff += cover_transmit(app, entry->pixels, entry->id, rect);
    debug_log("cover transmit: grid id=" + std::to_string(entry->id) + " px=" +
              std::to_string(entry->pixels.w) + "x" +
              std::to_string(entry->pixels.h) + " url=" + url);
    placed[url] = Placed{entry->id, rect};
    changed = true;
  }

  return changed;
}

}  // namespace detail

// ===========================================================================
// tick(App, Event) — mutate state. std::visit + overloaded{}, NO catch-all: a
// new Event alternative must fail to compile until handled here (A2).
// ===========================================================================

namespace {

// Bump the spinner phase and slow-color derivation is a draw concern; here we
// only advance the animation frame while something is loading.
void advance_spinner(App& app) {
  if (app.any_loading) {
    app.spinner_phase = (app.spinner_phase + 1) % kSpinnerFrameCount;
    app.dirty = true;
  }
}

// Re-derive any_loading from the per-subsystem in-flight state. Called from
// every handler that changes that state — a stale (gen-dropped or query-
// mismatched) worker result must never kill the spinner a DIFFERENT in-flight
// worker still owns, so no handler writes any_loading directly. Stamps
// loading_since_tick on the false->true edge (the slow-spinner clock).
// demo_mode bypasses this (sets any_loading directly; no handler below ever
// fires there, so the demo spinner survives).
void recompute_loading(App& app) {
  // The active Discover axis slot's loading arm feeds the bottom-bar spinner
  // while a feed page is in flight (P17, 04 §8).
  const bool discover_loading = app.view == View::Discover && app.discover.slot().loading;
  const bool now =
      !app.search_inflight_query.empty() || app.episode.loading || discover_loading;
  if (now && !app.any_loading) app.loading_since_tick = app.tick_count;
  app.any_loading = now;
}

// ---------------------------------------------------------------------------
// P7 worker spawns. Each captures its inputs by value (query strings, ids,
// generation tokens) and posts an owned Event back via `queue` — never touches
// `app` from the worker thread (A1). Detached, never joined (04 §6/§13).
// ---------------------------------------------------------------------------

// `/` search: goes through AppDeps::search, which main.cpp wires to the real
// anilist::search (AniList is the catalog client, never a StreamProvider, per
// anilist.hpp's header comment) — the indirection exists solely so an offline
// test can inject canned rows (app.hpp's SearchFn doc comment). Staleness is
// by query-string compare (04 §6 table), so SearchDone carries the query and
// no generation.
void spawn_search(EventQueue& queue, const SearchFn& search, std::string query,
                  std::uint32_t page) {
  spawn_detached([&queue, &search, query = std::move(query), page]() mutable {
    auto p = search(query, page);
    if (!p.has_value()) {
      queue.try_post(Event{SearchError{p.error(), query}});
      return;
    }
    queue.try_post(
        Event{SearchDone{std::move(p->first), std::move(query), page, p->second}});
  });
}

// login::ConnectResult -> the event-safe ConnectOutcome (event.hpp stays off
// login.hpp's link graph; see event.hpp's ConnectDone comment).
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

// AniList connect (P19, 06 §4): owns the Loopback (moved in) and runs the
// blocking accept/serve loop entirely off the UI thread (A1) — start()'s fast
// local bind already happened on the UI thread in open_connect(). Builds its
// own login::Verifier over AppDeps::http (the worker never touches App).
// Posts exactly one ConnectDone — UNLESS the result is Canceled, matching
// login.rs's worker: the app may already be moving on from the session.
void spawn_connect(EventQueue& queue, const http::Client& http, loopback::Loopback loopback,
                   std::string auth_path, std::int64_t now) {
  spawn_detached([&queue, &http, loopback = std::move(loopback), auth_path = std::move(auth_path),
                  now]() mutable {
    const login::Verifier verifier = login::make_anilist_verifier(http);
    bool warned = false;
    const login::ConnectResult result = loopback.serve(verifier, auth_path, now, [&warned] {
      if (!warned) {
        debug_log("connect: ignoring callback(s) with a bad state");
        warned = true;
      }
    });
    if (result.kind != login::ConnectResult::Kind::Canceled) {
      queue.try_post(Event{to_connect_done(result)});
    }
  });
}

// Settings' Connect row (SettingsOutcome::ConnectRequested): bind the
// loopback synchronously so a bind failure is a toast, not a half-open modal
// (fast, local — never blocks the UI thread); open the browser; then run the
// accept loop off the render path (spawn_connect, A1). No AppDeps::http / no
// auth_file wired (demo_mode, most tests) degrades to a toast, same shape as
// a bind failure — there is nowhere to save a token to.
void open_connect(App& app) {
  if (app.queue == nullptr || app.deps == nullptr || app.deps->http == nullptr ||
      app.auth_file.empty()) {
    app.toasts.push(ToastKind::Error, "connect unavailable", app.tick_count);
    return;
  }
  auto started = loopback::Loopback::start();
  if (!started.has_value()) {
    app.toasts.push(ToastKind::Error, "could not start login server", app.tick_count);
    return;
  }
  loopback::Loopback lp = std::move(*started);
  const std::string url = lp.authorize_url();
  login::open_browser(url);
  loopback::Canceler canceler = lp.canceler();
  spawn_connect(*app.queue, *app.deps->http, std::move(lp), app.auth_file, now_epoch_secs());

  App::ConnectSession session;
  session.url = url;
  session.started = app.tick_count;
  session.copied = false;
  session.cancel = [canceler]() mutable { canceler.cancel(); };
  app.connect = std::move(session);
}

// MAL connect (P31 §9.1 slice 2). Structurally like spawn_connect but for
// Authorization Code + PKCE: no verifier callback, the Exchanger POSTs the
// token endpoint itself; the loopback binds /mal/callback instead of
// /callback and carries its own minted PKCE pair.
std::string account_display_mal(const Auth& auth) {
  if (!auth.mal.bearer().has_value()) return "not connected";
  // MAL's token exchange carries no display name (unlike AniList's Viewer
  // query) — slice 3's REST client fills mal.user_name-equivalent via a
  // follow-up /users/@me call; until then "connected" is what disk truth
  // actually holds.
  return "connected";
}

MalConnectDone to_mal_connect_done(const mal_login::MalConnectResult& r) {
  using K = mal_login::MalConnectResult::Kind;
  switch (r.kind) {
    case K::Ok: return MalConnectDone{MalConnectOutcome::Ok};
    case K::NoClientId: return MalConnectDone{MalConnectOutcome::NoClientId};
    case K::NoCode: return MalConnectDone{MalConnectOutcome::NoCode};
    case K::Rejected: return MalConnectDone{MalConnectOutcome::Rejected};
    case K::NetworkError: return MalConnectDone{MalConnectOutcome::NetworkError};
    case K::SaveFailed: return MalConnectDone{MalConnectOutcome::SaveFailed};
    case K::BadState: return MalConnectDone{MalConnectOutcome::BadState};
    case K::Canceled: return MalConnectDone{MalConnectOutcome::Canceled};
  }
  return MalConnectDone{MalConnectOutcome::NoCode};  // unreachable (closed enum).
}

void spawn_mal_connect(EventQueue& queue, const http::Client& http,
                       mal_loopback::Loopback loopback, std::string auth_path, std::int64_t now) {
  spawn_detached([&queue, &http, loopback = std::move(loopback), auth_path = std::move(auth_path),
                  now]() mutable {
    const mal_login::Exchanger exchanger = mal_login::make_http_exchanger(http);
    bool warned = false;
    const mal_login::MalConnectResult result =
        loopback.serve(exchanger, auth_path, now, [&warned] {
          if (!warned) {
            debug_log("mal connect: ignoring callback(s) with a bad state");
            warned = true;
          }
        });
    if (result.kind != mal_login::MalConnectResult::Kind::Canceled) {
      queue.try_post(Event{to_mal_connect_done(result)});
    }
  });
}

// Settings' MyAnimeList "connect" row (SettingsOutcome::MalConnectRequested).
// Same bind-synchronously-then-worker shape as open_connect; additionally
// refuses up front (a toast, not a half-open modal) when Config::
// mal_client_id is empty — the user has not registered a MAL app yet, so
// there is nothing to authorize against.
void open_mal_connect(App& app) {
  if (app.queue == nullptr || app.deps == nullptr || app.deps->http == nullptr ||
      app.auth_file.empty()) {
    app.toasts.push(ToastKind::Error, "connect unavailable", app.tick_count);
    return;
  }
  if (app.config.mal_client_id.empty()) {
    app.toasts.push(ToastKind::Error, "set a mal client id first", app.tick_count);
    return;
  }
  auto started = mal_loopback::Loopback::start(app.config.mal_client_id);
  if (!started.has_value()) {
    app.toasts.push(ToastKind::Error, "could not start login server", app.tick_count);
    return;
  }
  mal_loopback::Loopback lp = std::move(*started);
  const std::string url = lp.authorize_url();
  login::open_browser(url);
  mal_loopback::Canceler canceler = lp.canceler();
  spawn_mal_connect(*app.queue, *app.deps->http, std::move(lp), app.auth_file, now_epoch_secs());

  App::ConnectSession session;
  session.url = url;
  session.started = app.tick_count;
  session.copied = false;
  session.cancel = [canceler]() mutable { canceler.cancel(); };
  app.connect = std::move(session);
}

// Terminal MAL connect outcome -> UI. Same reload-then-toast shape as
// on_connect_result; no flush_sync call (P31's mirror push is slice 4 and
// rides the AniList sync flush's cadence, not its own worker).
void on_mal_connect_result(App& app, const MalConnectDone& done) {
  app.connect = std::nullopt;
  if (!app.auth_file.empty()) app.auth = Auth::load(app.auth_file);
  app.settings_mal_account = account_display_mal(app.auth);

  switch (done.outcome) {
    case MalConnectOutcome::Ok:
      app.toasts.push(ToastKind::Success, "signed in to MyAnimeList", app.tick_count);
      return;
    case MalConnectOutcome::NoClientId:
      app.toasts.push(ToastKind::Error, "set a mal client id first", app.tick_count);
      return;
    case MalConnectOutcome::NoCode:
      app.toasts.push(ToastKind::Error, "no code received", app.tick_count);
      return;
    case MalConnectOutcome::Rejected:
      app.toasts.push(ToastKind::Error, "MyAnimeList rejected the token", app.tick_count);
      return;
    case MalConnectOutcome::NetworkError:
      app.toasts.push(ToastKind::Error, "could not reach MyAnimeList", app.tick_count);
      return;
    case MalConnectOutcome::SaveFailed:
      app.toasts.push(ToastKind::Error, "signed in, but saving the token failed", app.tick_count);
      return;
    case MalConnectOutcome::BadState:
      app.toasts.push(ToastKind::Error, "login state mismatch", app.tick_count);
      return;
    case MalConnectOutcome::Canceled:
      return;  // never posted by the worker; unreachable in practice.
  }
}

// Terminal connect outcome -> UI (mirrors login.rs's on_connect_result):
// close the modal unconditionally, reload auth so the account row reflects
// disk truth after ANY outcome (06 §9 "token survives reloadAuth"), then
// toast. Canceled never reaches here (the worker doesn't post it).
void on_connect_result(App& app, const ConnectDone& done) {
  app.connect = std::nullopt;
  if (!app.auth_file.empty()) app.auth = Auth::load(app.auth_file);
  app.settings_account = account_display(app.auth);

  switch (done.outcome) {
    case ConnectOutcome::Ok:
      flush_sync(app, /*pull_only=*/false);
      app.toasts.push(ToastKind::Success, "signed in as " + done.user_name, app.tick_count);
      return;
    case ConnectOutcome::NoToken:
      app.toasts.push(ToastKind::Error, "no token received", app.tick_count);
      return;
    case ConnectOutcome::Rejected:
      app.toasts.push(ToastKind::Error, "AniList rejected the token", app.tick_count);
      return;
    case ConnectOutcome::NetworkError:
      app.toasts.push(ToastKind::Error, "could not reach AniList", app.tick_count);
      return;
    case ConnectOutcome::SaveFailed:
      app.toasts.push(ToastKind::Error, "signed in, but saving the token failed", app.tick_count);
      return;
    case ConnectOutcome::BadState:
      app.toasts.push(ToastKind::Error, "login state mismatch", app.tick_count);
      return;
    case ConnectOutcome::Canceled:
      return;  // never posted by the worker; unreachable in practice.
  }
}

// --- AniList sync (P20, 06 §5) ----------------------------------------------

// A live, unexpired token (06 §5.5 anilist_connected).
bool anilist_connected(const App& app) {
  return app.auth.anilist.bearer().has_value() && !app.auth.anilist.is_expired(now_epoch_secs());
}

// The master switch ANDed with the connection (06 §5.5 sync_enabled): the
// gate on whether to spawn any sync at all.
bool sync_enabled(const App& app) {
  return app.config.anilist_sync_enabled && anilist_connected(app);
}

// Arm the action-flush debounce after a status/play edit (ROD-291). A no-op
// when sync is off or disconnected.
void arm_sync(App& app) {
  if (sync_enabled(app)) app.sync_flush_deadline = app.tick_count + kSyncFlushTicks;
}

// sync::SyncSummary -> the event-safe SyncFlushed (event.hpp stays off
// sync.hpp's link graph; see event.hpp's SyncFlushed comment). Distinct enums
// with identical enumerator names, bridged by an explicit switch — same shape
// as to_connect_done. `mal_pushed` rides along flattened the same way (P31
// §9.1 slice 4) — the MAL mirror's own outcome is deliberately not surfaced
// here (A1: it gets no drain rights and no toast of its own; a push_failed/
// push_skipped row simply stays dirty for next run, silently, same as the
// AniList mirror's own quieter outcomes).
SyncFlushed to_sync_flushed(const sync::SyncSummary& s, std::uint32_t mal_pushed) {
  using K = sync::SyncOutcome;
  SyncOutcome outcome;
  switch (s.outcome) {
    case K::Completed:         outcome = SyncOutcome::Completed; break;
    case K::Disabled:          outcome = SyncOutcome::Disabled; break;
    case K::NoToken:           outcome = SyncOutcome::NoToken; break;
    case K::Expired:           outcome = SyncOutcome::Expired; break;
    case K::NoUserId:          outcome = SyncOutcome::NoUserId; break;
    case K::PullUnauthorized:  outcome = SyncOutcome::PullUnauthorized; break;
    case K::PullRateLimited:   outcome = SyncOutcome::PullRateLimited; break;
    case K::Unauthorized:      outcome = SyncOutcome::Unauthorized; break;
    case K::RateLimited:       outcome = SyncOutcome::RateLimited; break;
    case K::Failed:            outcome = SyncOutcome::Failed; break;
    default:                   outcome = SyncOutcome::Failed;  // unreachable (closed enum).
  }
  return SyncFlushed{outcome, s.pulled.imported, s.pulled.reconciled, s.pushed, mal_pushed};
}

// Sync worker (P20, 04 §4.6): opens its OWN store connection (store.hpp's
// documented exception) — the CAS-guarded reconcile (06 §5.4) is built for
// exactly this cross-connection concurrency with the UI thread. Reuses
// AppDeps::http (like spawn_connect — that client is process-lifetime, safe
// to reference from a detached worker), but never AppDeps::store: the sync
// worker's whole reason to exist is running off a connection the UI thread
// isn't touching.
void spawn_sync(EventQueue& queue, const http::Client& http, std::string db_path, Auth auth,
                bool enabled, bool pull_only, std::int64_t now) {
  spawn_detached([&queue, &http, db_path = std::move(db_path), auth = std::move(auth), enabled,
                  pull_only, now]() mutable {
    sync::SyncSummary summary = sync::SyncSummary::terminal(sync::SyncOutcome::Failed);
    std::uint32_t mal_pushed = 0;
    auto store = Store::open(db_path);
    if (store.has_value()) {
      const sync::HttpAniListSync ani(http);
      auto r = sync::run_sync(ani, auth, *store, now, enabled, pull_only, sync::thread_sleep);
      if (r.has_value()) summary = *r;
      // P31 §9.1 slice 4: the MAL mirror push rides this same worker/store
      // connection, no drain rights of its own (A1) — never a separate
      // worker, never its own spawn. Runs regardless of pull_only (MAL is
      // push-only to begin with, so there is no "pull" half to skip).
      const mal_mirror::HttpMalMirrorClient mal_client(http);
      auto m = mal_mirror::push_mirror(mal_client, auth.mal, *store, auth.mal.bearer().has_value());
      if (m.has_value()) mal_pushed = m->pushed;
    }
    queue.try_post(Event{to_sync_flushed(summary, mal_pushed)});
  });
}

// Spawn a sync run, unless one is already inflight (re-arm to retry) or the
// gate is closed. `pull_only` is the launch-refresh path (06 §5.2).
void flush_sync(App& app, bool pull_only) {
  if (!sync_enabled(app)) return;
  if (app.syncing) {
    // A run is going; retry after another period rather than overlap.
    app.sync_flush_deadline = app.tick_count + kSyncFlushTicks;
    return;
  }
  if (app.queue == nullptr || app.deps == nullptr || app.deps->http == nullptr ||
      app.db_file.empty()) {
    return;  // nothing to Store::open / no client (demo_mode, no-persist tests).
  }
  spawn_sync(*app.queue, *app.deps->http, app.db_file, app.auth,
             app.config.anilist_sync_enabled, pull_only, now_epoch_secs());
  app.syncing = true;
}

// A sync run finished: clear the inflight flag and toast what moved (DESIGN
// 4.10 up/down rows). Failures and no-ops are silent by design.
void on_sync_flushed(App& app, const SyncFlushed& s) {
  app.syncing = false;
  if (s.imported > 0) {
    app.toasts.push(ToastKind::Info, "+ " + std::to_string(s.imported) + " added from AniList",
                     app.tick_count);
  }
  if (s.reconciled > 0) {
    app.toasts.push(ToastKind::Info, "↓ " + std::to_string(s.reconciled) + " from AniList",
                     app.tick_count);
  }
  if (s.pushed > 0) {
    app.toasts.push(ToastKind::Info, "↑ " + std::to_string(s.pushed) + " to AniList",
                     app.tick_count);
  }
  if (s.mal_pushed > 0) {
    app.toasts.push(ToastKind::Info, "↑ " + std::to_string(s.mal_pushed) + " to MyAnimeList",
                     app.tick_count);
  }
  // P37 slice 3: "after each sync pull" re-check. NOTE the honest premise
  // (P37 review): the pull reconciles status/progress/score only — the ONLY
  // writer of next_airing_* is P21's refresh-on-view of the selected show.
  // This hook therefore catches (a) clock passage since boot over rows whose
  // stored airing time just elapsed, and (b) marks against rows P21 healed
  // earlier in the session — not pull-delivered airing data. A bulk airing
  // refresh (the plan's optional season-wide AiringSchedule arm) is the
  // recorded gap for shows the user never opens.
  check_schedule_notices(app);
}

// --- Update check (P28, 06 §6.1) --------------------------------------------

// The boot-time worker (spawn_update_check below) found a strictly-newer
// release: a low-key Info whisper (05 §14 update_available), fired at most
// once since the worker itself runs at most once per session.
void on_update_available(App& app, const UpdateAvailable& u) {
  app.toasts.push(ToastKind::Info, "update available: v" + u.latest, app.tick_count);
}

// One-shot boot-time check (04 §4.6-shaped spawn, matching spawn_sync):
// resolves the network off the UI thread and posts UpdateAvailable only on a
// confirmed-newer tag. Every other outcome (disabled, offline, rate-limited,
// no cache dir) posts nothing — silent by design (06 §6.1).
void spawn_update_check(EventQueue& queue, const http::Client& http, std::string cache_dir,
                        std::string current_version, std::int64_t now) {
  spawn_detached([&queue, &http, cache_dir = std::move(cache_dir),
                  current_version = std::move(current_version), now]() {
    auto latest = updatecheck::check(http, cache_dir, current_version, now);
    if (latest.has_value()) queue.try_post(Event{UpdateAvailable{*latest}});
  });
}

// Quit flush worker (P20, 04 §11): push only, no pull, NoSleep (teardown's own
// deadline is the only budget it gets). Signals completion via a heap-shared
// atomic, NOT the event queue — the queue is about to be shut down and has no
// reader left to act on a summary at quit anyway. Deliberately takes no
// EventQueue& (unlike every other spawn_* here): do not "fix" this to match
// the pattern.
void spawn_quit_flush_worker(const http::Client& http, std::string db_path, Auth auth,
                             bool enabled, std::int64_t now,
                             std::shared_ptr<std::atomic<bool>> done) {
  spawn_detached([&http, db_path = std::move(db_path), auth = std::move(auth), enabled, now,
                  done = std::move(done)]() mutable {
    auto store = Store::open(db_path);
    if (store.has_value()) {
      const sync::HttpAniListSync ani(http);
      (void)sync::flush_push(ani, auth, *store, now, enabled, sync::no_sleep);
      // P31 §9.1 slice 4: rides the SAME teardown budget, no extra drain
      // rights (A1) — this is the whole reason the plan puts the mirror
      // behind the AniList flush's cadence rather than a worker of its own.
      const mal_mirror::HttpMalMirrorClient mal_client(http);
      (void)mal_mirror::push_mirror(mal_client, auth.mal, *store, auth.mal.bearer().has_value());
    }
    done->store(true, std::memory_order_release);
  });
}

// Discover feed fetch (P17): goes through AppDeps::discover (main.cpp wires the
// real anilist::discover; offline tests inject canned pages). Staleness is
// POSITIONAL within one filter set — the axis + page carry identity, on_feed
// discards a page that is not slot.page+1 — plus the filter generation `gen`
// (P38 review): a page spawned under the old filters answers with its own
// gen and self-discards after a set_filters bump. A spawn failure resets the
// slot's loading arm (the caller passed the arm already set); this helper
// only owns the detach and posts DiscoverFeedDone/Error.
void spawn_discover_feed(EventQueue& queue, const DiscoverFn& discover, DiscoverAxis axis,
                         std::uint32_t page, const DiscoverFilters& filters,
                         std::uint32_t gen) {
  spawn_detached([&queue, &discover, axis, page, filters, gen]() {
    auto res = discover(axis, page, filters);
    if (!res.has_value()) {
      queue.try_post(Event{DiscoverFeedError{
          axis, std::string(provider_error_copy(res.error().kind)), gen}});
      return;
    }
    auto& [rows, has_next] = *res;
    queue.try_post(Event{DiscoverFeedDone{axis, page, std::move(rows), has_next, gen}});
  });
}

// Enter on a focused catalog row: A8 degenerate walk (classify_tier) then
// episodes(). Tier-C "best hit" scoring is deferred (A8 explicitly licenses
// first-hit + toast for v0); the toast fires from the UI thread when
// EpisodesDone/Error lands, so the worker itself stays silent on the tier-C
// fallback (04 §6: workers post only Event, never toast directly).
void spawn_resolve_episodes(EventQueue& queue, const ProviderRegistry& registry,
                            Enrichment show, Generation gen) {
  const std::int64_t for_id = show.anilist_id;
  spawn_detached([&queue, &registry, show = std::move(show), for_id, gen]() {
    const StreamProvider* provider = registry.primary();
    if (provider == nullptr) {
      queue.try_post(Event{EpisodesError{ProviderError::unsupported(), for_id, gen, {}}});
      return;
    }
    const std::string pname(provider->name());
    std::string provider_id;
    if (auto key = provider->canonical_key(show); key.has_value()) {
      provider_id = std::move(*key);  // Tier::AKey.
    } else {
      // Tier::CSearch — search by title, take the first hit (A8 v0 license).
      SearchOptions opts;
      auto hits = provider->search(show.title_romaji, opts);
      if (!hits.has_value()) {
        queue.try_post(Event{EpisodesError{hits.error(), for_id, gen, pname}});
        return;
      }
      if (hits->empty()) {
        // Authoritative not-stocked (03 §4.3): Ok(empty) from episodes() is
        // how the grid renders "no source"; mirror that via an empty list.
        queue.try_post(Event{EpisodesDone{{}, for_id, gen, pname, {}}});
        return;
      }
      provider_id = hits->front().provider_id;
    }
    auto eps = provider->episodes(provider_id, Translation::Sub, std::nullopt);
    if (!eps.has_value()) {
      queue.try_post(Event{EpisodesError{eps.error(), for_id, gen, pname}});
      return;
    }
    // Carry the id the grid was fetched under: play reuses it verbatim (a
    // Tier-C provider_id is unrecoverable at play time — see EpisodesDone).
    queue.try_post(
        Event{EpisodesDone{std::move(*eps), for_id, gen, pname, std::move(provider_id)}});
  });
}

// Enter on a focused episode grid cell: resolve + play via P5's player::play —
// unless a completed local download for the exact (show, track, ep) exists, in
// which case player::play_local skips the resolve pipeline entirely (P35
// slice 4). player::play() is blocking and calls `resolve` once per retry
// attempt itself; the inner lambda wraps provider->resolve into ResolveFn.
void spawn_play(EventQueue& queue, const ProviderRegistry& registry,
                const AppDeps& deps, std::string provider_name, std::string provider_id,
                std::string episode, std::int64_t for_id, double start_secs,
                Translation translation, Quality quality,
                std::optional<std::int64_t> mal_id, std::string show_title,
                std::uint32_t episode_ordinal, aniskip::SkipMode skip_mode,
                player::BackendKind backend, std::string player_path) {
  spawn_detached([&queue, &registry, &deps, provider_name = std::move(provider_name),
                  provider_id = std::move(provider_id),
                  episode = std::move(episode), for_id, start_secs, translation, quality,
                  mal_id, show_title = std::move(show_title), episode_ordinal, skip_mode,
                  backend, player_path = std::move(player_path)]() {
    player::EventSink on_event = [&queue, for_id, &episode](const player::PlayerEvent& pe) {
      using K = player::PlayerEvent::Kind;
      switch (pe.kind) {
        case K::Position:
          queue.try_post(Event{PositionUpdate{
              pe.pos.secs, pe.pos.duration.value_or(0.0), for_id, episode}});
          return;
        case K::Retry:
          queue.try_post(Event{PlayRetry{pe.attempt, player::kMaxPlayAttempts}});
          return;
      }
    };
    player::PlayOpts opts;
    opts.mpv_path = deps.mpv_path;
    // P39: backend + alternate path ride the call (read from app.config at
    // the call site, like translation/quality) so a Settings wheel flip
    // applies to the very next play — no restart, unlike the deps snapshot.
    opts.backend = backend;
    opts.player_path = player_path;
    opts.socket_dir = deps.socket_dir;
    opts.title = episode;
    opts.start_secs = start_secs;
    // AniSkip prepare (P22, 03 §9): best-effort, runs here (the playback
    // worker) rather than the UI thread. Any failure (no MAL id incl. Jikan
    // miss, network down, unwritable cache) collapses to plain play.
    if (!deps.aniskip_cache_dir.empty()) {
      const std::uint32_t ep_num = aniskip::episode_number(episode, episode_ordinal);
      opts.skip = aniskip::prepare(mal_id, show_title, ep_num, skip_mode,
                                   deps.aniskip_cache_dir);
    }
    auto outcome = [&]() -> Result<player::PlayOutcome, PlayError> {
      // Play-prefers-local (P35 slice 4): a completed download for this exact
      // (show, track, ep) short-circuits the whole resolve pipeline — no
      // provider, no network (downloads keep working when every provider is
      // down). This preference IS the PPC story: download once, play a file.
      if (!deps.download_dir.empty()) {
        if (auto local = download::find_local_episode(deps.download_dir, for_id,
                                                      translation, episode)) {
          debug_log("worker play: local file " + *local);
          return player::play_local(opts, *local, on_event);
        }
      }
      // Resolve on the SERVING provider (P15), never primary(): a hop can land
      // the grid on a non-primary provider and its provider_id only resolves
      // there. Empty name = the degraded P7 first-hit path, where primary() is
      // the one provider the grid was fetched under.
      const StreamProvider* provider =
          provider_name.empty() ? registry.primary() : registry.by_name(provider_name);
      if (provider == nullptr) return err(PlayError::resolve("no provider"));
      player::ResolveFn resolve = [provider, &provider_id, &episode, translation,
                                   quality]() -> Result<StreamLink, std::string> {
        auto link = provider->resolve(provider_id, episode, translation, quality);
        if (!link.has_value()) {
          return err(std::string(provider_error_copy(link.error().kind)));
        }
        return *link;
      };
      debug_log("worker play: calling player::play ep=" + episode);
      return player::play(opts, resolve, on_event);
    }();
    if (!outcome.has_value()) {
      const bool posted =
          queue.try_post(Event{PlayErrorEvent{outcome.error(), true, for_id, episode}});
      debug_log("worker play: error posted=" + std::to_string(posted));
      return;
    }
    const double final_pos =
        outcome->position.has_value() ? outcome->position->secs : 0.0;
    const bool posted = queue.try_post(Event{PlayDone{final_pos, for_id, episode}});
    debug_log("worker play: PlayDone pos=" + std::to_string(final_pos) +
              " posted=" + std::to_string(posted));
  });
}

// download::DownloadError -> the dependency-free event payload (the
// ConnectOutcome boundary-conversion pattern). The detail is the short human
// fragment the toast appends.
std::pair<DownloadOutcome, std::string> download_failed_payload(
    const download::DownloadError& e) {
  using K = download::DownloadError::Kind;
  switch (e.kind) {
    case K::Fetch:
      return {DownloadOutcome::Fetch, std::string(provider_error_copy(e.fetch.kind))};
    case K::Io:
      return {DownloadOutcome::Io, e.detail};
    case K::Cancelled:
      return {DownloadOutcome::Cancelled, {}};
    case K::UnsafeUrl:
      return {DownloadOutcome::UnsafeUrl, {}};
    case K::UnsafeArg:
      return {DownloadOutcome::UnsafeArg, e.detail};
    case K::FfmpegNotFound:
      return {DownloadOutcome::FfmpegNotFound, {}};
    case K::Ffmpeg:
      return {DownloadOutcome::Ffmpeg, e.detail};
  }
  return {DownloadOutcome::Fetch, {}};  // unreachable (closed enum).
}

// `d` on a focused episode grid cell (P35 slice 3): resolve once on the
// serving provider, then the blocking download_link (curl or ffmpeg arm) off
// the UI thread. One transfer at a time is the CALLER's gate
// (App::download.active); this worker just runs one. Progress is throttled
// worker-side (≥500ms between posts, so a slow pty never backs up) with the final
// tick always posted. No retry/hop law: a failed download toasts and rests
// (the user re-fires; a .part resumes free of charge).
void spawn_download(EventQueue& queue, const ProviderRegistry& registry,
                    const http::Client* http, std::string provider_name,
                    std::string provider_id, std::string episode,
                    std::int64_t for_id, Translation translation, Quality quality,
                    std::string download_dir, std::string ffmpeg_path,
                    std::shared_ptr<std::atomic<bool>> cancel,
                    std::shared_ptr<std::atomic<long>> child_pid) {
  spawn_detached([&queue, &registry, http, provider_name = std::move(provider_name),
                  provider_id = std::move(provider_id), episode = std::move(episode),
                  for_id, translation, quality, download_dir = std::move(download_dir),
                  ffmpeg_path = std::move(ffmpeg_path), cancel = std::move(cancel),
                  child_pid = std::move(child_pid)]() {
    // Resolve on the SERVING provider (P15) — same rule as spawn_play.
    const StreamProvider* provider =
        provider_name.empty() ? registry.primary() : registry.by_name(provider_name);
    if (provider == nullptr || http == nullptr) {
      queue.try_post(Event{DownloadFailed{DownloadOutcome::Fetch, "no provider",
                                          for_id, episode}});
      return;
    }
    auto link = provider->resolve(provider_id, episode, translation, quality);
    if (!link.has_value()) {
      queue.try_post(Event{DownloadFailed{
          DownloadOutcome::Fetch, std::string(provider_error_copy(link.error().kind)),
          for_id, episode}});
      return;
    }
    const std::string dest = download::episode_dest(download_dir, for_id, translation,
                                                    episode, link->url);
    if (auto md = download::ensure_parent_dirs(dest); !md.has_value()) {
      auto [outcome, detail] = download_failed_payload(md.error());
      queue.try_post(Event{DownloadFailed{outcome, std::move(detail), for_id, episode}});
      return;
    }
    auto last_post = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    download::ProgressFn progress = [&queue, &last_post, for_id, &episode](
                                        std::uint64_t bytes,
                                        std::optional<std::uint64_t> total) {
      const auto now = std::chrono::steady_clock::now();
      const bool final_tick = total.has_value() && bytes >= *total;
      if (!final_tick && now - last_post < std::chrono::milliseconds(500)) return;
      last_post = now;
      queue.try_post(Event{DownloadProgress{bytes, total.value_or(0), for_id, episode}});
    };
    debug_log("worker download: ep=" + episode + " dest=" + dest);
    auto r = download::download_link(*http, *link, dest, quality, ffmpeg_path,
                                     *cancel, progress, child_pid.get());
    if (!r.has_value()) {
      auto [outcome, detail] = download_failed_payload(r.error());
      debug_log("worker download: failed kind=" +
                std::to_string(static_cast<int>(outcome)));
      queue.try_post(Event{DownloadFailed{outcome, std::move(detail), for_id, episode}});
      return;
    }
    debug_log("worker download: done " + dest);
    queue.try_post(Event{DownloadDone{for_id, episode, dest}});
  });
}

// Default cell pixel geometry when the terminal reports none but still speaks
// kitty (A4 fallback): a typical 8×16 monospace cell keeps the transmitted box
// well-proportioned. Real geometry (cover_caps.cell) overrides this.
inline constexpr std::uint32_t kFallbackCellW = 8;
inline constexpr std::uint32_t kFallbackCellH = 16;

// Cover fetch worker: covers->load() off the UI thread, posting CoverDone (the
// cropped/resized RGBA) or CoverError (cooldown path). Staleness is the
// anilist_id `for_id` + the cover generation, both stamped into the event and
// checked in tick() (04 §6). `url` is the AniList cover url; `provider` is null
// for the AniList/absolute-url path (the v0 detail cover).
void spawn_cover(EventQueue& queue, Covers& covers, std::string url,
                 std::int64_t for_id, std::uint32_t box_w, std::uint32_t box_h,
                 Generation gen) {
  spawn_detached([&queue, &covers, url = std::move(url), for_id, box_w, box_h, gen]() {
    // AniList covers are absolute urls (provider == nullptr path); senshi/M2
    // provider-relative refs would pass the provider here (deferred to M2).
    auto pixels = covers.load(/*provider=*/nullptr, url, box_w, box_h);
    if (!pixels.has_value()) {
      const bool posted = queue.try_post(Event{CoverError{for_id, gen}});
      debug_log("worker detail-cover: err posted=" + std::to_string(posted));
      return;
    }
    CoverDone done;
    done.pixels = std::move(*pixels);
    done.for_id = for_id;
    done.gen = gen;
    const bool posted = queue.try_post(Event{std::move(done)});
    debug_log("worker detail-cover: ok posted=" + std::to_string(posted));
  });
}

// Grid cover fetch worker (P17b): same pipeline as spawn_cover, but keyed on
// url (not anilist_id+generation — the 04 §4.4 adopt-by-url rule: a result
// for a scrolled-away or evicted url is still adopted).
void spawn_grid_cover(EventQueue& queue, Covers& covers, std::string url,
                      std::uint32_t box_w, std::uint32_t box_h) {
  spawn_detached([&queue, &covers, url = std::move(url), box_w, box_h]() {
    auto pixels = covers.load(/*provider=*/nullptr, url, box_w, box_h);
    if (!pixels.has_value()) {
      const bool posted = queue.try_post(Event{GridCoverError{url}});
      debug_log("worker grid-cover: err posted=" + std::to_string(posted));
      return;
    }
    GridCoverDone done;
    done.url = url;
    done.pixels = std::move(*pixels);
    const bool posted = queue.try_post(Event{std::move(done)});
    debug_log("worker grid-cover: ok posted=" + std::to_string(posted));
  });
}

// Refresh-on-view enrichment worker (P21, workers.rs spawn_enrich, 04 §5.1):
// by-id fetch off the UI thread, posting the three-state answer keyed by
// `for_id`. The EnrichFn is copied into the thread (a bound http::Client + free
// function; it outlives run()). Ok(some) -> EnrichmentRefreshed, Ok(nullopt) ->
// EnrichmentNull (a confirmed no-match is still an answer), Err ->
// EnrichmentFailed. Staleness is the `for_id` guard on the UI thread (a
// selection that moved on no longer matches the shown show) — no generation is
// needed because the answer only ever mutates the row it names.
void spawn_enrich(EventQueue& queue, EnrichFn enrich, std::int64_t for_id) {
  spawn_detached([&queue, enrich = std::move(enrich), for_id]() {
    auto ans = enrich(for_id);
    Event ev = [&]() -> Event {
      if (!ans.has_value()) return EnrichmentFailed{for_id};
      if (!ans->has_value()) return EnrichmentNull{for_id};
      EnrichmentRefreshed r;
      r.for_id = for_id;
      r.enrichment = std::move(**ans);
      return r;
    }();
    const bool posted = queue.try_post(Event{std::move(ev)});
    debug_log("worker enrich: for_id=" + std::to_string(for_id) +
              " posted=" + std::to_string(posted));
  });
}

// Detail zoom `c`-section worker (P36): same three-state shape as
// spawn_enrich, its own event trio so the two fetches never contend for the
// same in-flight flag. The `c` toggle is the only spawn site — no
// refresh-on-view analogue (05 §8 is enrichment-specific).
void spawn_char_recs(EventQueue& queue, CharRecsFn fetch, std::int64_t for_id) {
  spawn_detached([&queue, fetch = std::move(fetch), for_id]() {
    auto ans = fetch(for_id);
    Event ev = [&]() -> Event {
      if (!ans.has_value()) return CharactersRecsFailed{for_id};
      if (!ans->has_value()) return CharactersRecsNull{for_id};
      CharactersRecsDone d;
      d.for_id = for_id;
      d.data = std::move(**ans);
      return d;
    }();
    const bool posted = queue.try_post(Event{std::move(ev)});
    debug_log("worker char_recs: for_id=" + std::to_string(for_id) +
              " posted=" + std::to_string(posted));
  });
}

// Discover filter overlay genre picker (P38, §9): a single-flight, session-
// cached fetch (no for_id — the vocabulary is global). Two-state, mirroring
// genre_collection's own contract (no confirmed-absent arm).
void spawn_genre_collection(EventQueue& queue, GenreCollectionFn fetch) {
  spawn_detached([&queue, fetch = std::move(fetch)]() {
    auto ans = fetch();
    Event ev = [&]() -> Event {
      if (!ans.has_value()) return GenreCollectionFailed{};
      GenreCollectionDone d;
      d.genres = std::move(*ans);
      return d;
    }();
    const bool posted = queue.try_post(Event{std::move(ev)});
    debug_log("worker genre_collection: posted=" + std::to_string(posted));
  });
}

// Reconcile the held CoverState against the current selection (05 §12 decision
// table) and act: Fetch spawns the worker; Clear/Suppress drop or hold the
// render store; UpToDate/None do nothing. Called from every transition that can
// change which show's cover should be on screen (selection move, detail entry,
// results landing, view change). Pure-decision + at-most-one spawn.
void reconcile_cover(App& app) {
  if (app.deps == nullptr || app.deps->covers == nullptr || app.queue == nullptr) {
    return;  // no cover pipeline wired (demo / fixture test without covers).
  }
  if (!app.cover_caps.images()) {
    // Placeholder mode: no fetch (nothing would be transmitted). The view draws
    // a dim box; keep the state machine idle so no spinner strands.
    return;
  }
  const std::optional<detail::CoverTarget> target = detail::cover_target(app);
  const std::optional<std::int64_t> tid =
      target.has_value() ? std::optional<std::int64_t>(target->id) : std::nullopt;
  const std::optional<std::string_view> turl =
      target.has_value() ? std::optional<std::string_view>(target->url) : std::nullopt;

  switch (app.cover.decide(tid, turl, app.tick_count)) {
    case CoverAction::None:
      return;
    case CoverAction::UpToDate:
      return;
    case CoverAction::Suppress:
      return;  // inside the 10s cooldown; the placeholder box stands.
    case CoverAction::Clear:
      app.cover.clear();
      app.cover_render.have_pixels = false;
      app.cover_render.pixels = CoverPixels{};
      app.dirty = true;
      return;
    case CoverAction::Fetch: {
      const Rect r = detail::cover_rect(app);
      if (r.empty()) return;  // no room to place a cover this frame.
      const std::uint32_t cw =
          app.cover_caps.cell.known() ? app.cover_caps.cell.width : kFallbackCellW;
      const std::uint32_t ch =
          app.cover_caps.cell.known() ? app.cover_caps.cell.height : kFallbackCellH;
      const std::uint32_t box_w = static_cast<std::uint32_t>(r.w) * cw;
      const std::uint32_t box_h = static_cast<std::uint32_t>(r.h) * ch;
      app.cover.begin_fetch(target->id, target->url);
      // A fresh cover supersedes any held pixels for a different show.
      app.cover_render.have_pixels = false;
      const Generation gen = app.cover_gen.bump();
      spawn_cover(*app.queue, *app.deps->covers, target->url, target->id, box_w,
                  box_h, gen);
      app.dirty = true;
      return;
    }
  }
}

// Refresh-on-view (P21, detail.rs maybe_enrich / 04 §10): freshness is judged
// locally, the network is spent only on miss/stale. Rides the same post-event
// reconcile as the cover, but independent of cover_art — metadata heals with
// covers off. Single-flight; a spawn refusal (no store / no seam / inflight)
// retries on the next reconcile. Keyed off selected_enrichment (the show the
// shared detail pane renders, 05 §15), NOT cover_target — a coverless import
// seed is exactly the row that most needs enriching.
void reconcile_enrich(App& app) {
  if (app.deps == nullptr || app.deps->store == nullptr || !app.deps->enrich ||
      app.queue == nullptr) {
    return;  // no enrich pipeline wired (demo / fixture test).
  }
  const Enrichment* shown = detail::selected_enrichment(app);
  if (shown == nullptr) {
    // No detail surface: the next selection re-arms the single-flight.
    app.enrich_checked = std::nullopt;
    return;
  }
  const std::int64_t id = shown->anilist_id;
  // Selection moved to a different show: re-arm (a stale answer for the old id
  // simply no longer matches in the handler).
  if (app.enrich_checked.has_value() && *app.enrich_checked != id) {
    app.enrich_checked = std::nullopt;
  }
  if (app.enrich_checked == id || app.enrich_inflight) return;

  const std::int64_t now = now_epoch_secs();
  auto stale = app.deps->store->enrichment_stale(id, now);
  // A store read error counts as fresh: no basis to spend network (detail.rs).
  if (!stale.has_value() || !*stale) {
    app.enrich_checked = id;
    return;
  }
  spawn_enrich(*app.queue, app.deps->enrich, id);
  app.enrich_checked = id;
  app.enrich_inflight = true;
}

// Reconcile the detail zoom's `c` section against the current selection
// (P36). Unlike reconcile_enrich this never fetches on its own — the section
// is on-demand, so the network is only spent while `visible` is true (armed
// by the `c` keypress). A selection move while visible re-arms: for_id/
// fetched/failed reset so the next reconcile spawns fresh, but `visible`
// itself survives (a standing per-session preference, matching app.pane).
void reconcile_char_recs(App& app) {
  if (app.deps == nullptr || !app.deps->char_recs || app.queue == nullptr) {
    return;  // no char_recs pipeline wired (demo / fixture test).
  }
  // Zoom-only (the state struct's own doc): `visible` survives demote as a
  // standing preference, but the section neither renders nor spends network
  // outside the Detail zoom — without this gate every Browse/History/Discover
  // cursor move under a surviving `visible` fired one GraphQL POST per
  // hovered show (P36 review).
  if (app.view != View::Detail) return;
  if (!app.char_recs.visible) return;
  const Enrichment* shown = detail::selected_enrichment(app);
  if (shown == nullptr) return;
  const std::int64_t id = shown->anilist_id;
  if (app.char_recs.for_id != id) {
    app.char_recs = CharactersRecsState{};
    app.char_recs.visible = true;
    app.char_recs.for_id = id;
  }
  // `failed` is terminal here (P36 review): draw renders "can't reach
  // AniList" and retry is MANUAL — a selection move or a `c` close/re-open
  // re-arms — the same never-refire shape as reconcile_enrich's
  // enrich_checked stamp. Without it a transport error respawned the fetch
  // every tick: a detached-thread + network busy-loop while offline.
  if (app.char_recs.fetched || app.char_recs.loading || app.char_recs.failed) return;
  app.char_recs.loading = true;
  spawn_char_recs(*app.queue, app.deps->char_recs, id);
}

// Whether the `c` section actually RENDERS at the current geometry (P36
// review): draw_detail_zoom skips the split on a cramped frame
// (char_recs_section_height == 0), and key routing/Tab-focus must never
// target a section that isn't on screen.
bool char_recs_section_shown(const App& app) {
  return app.char_recs.visible &&
         detail::char_recs_section_height(content_height(app.win.rows)) > 0;
}

// A healed answer replaces the shown snapshot in place (detail.rs on_enrichment):
// shigoku's detail pane renders LIVE from selected_enrichment (no held `shown`
// copy), so "replace the snapshot" means patch the row the active source list
// serves — but only while the selection still names `for_id`. History reloads
// from the store (its merged row lands via load_history); Browse/Discover carry
// transient result rows the store never re-serves, so their selected row is
// patched directly with the merged enrichment.
void apply_enrichment_to_selection(App& app, std::int64_t for_id, const Enrichment& merged) {
  const Enrichment* shown = detail::selected_enrichment(app);
  if (shown == nullptr || shown->anilist_id != for_id) return;  // stale/scrolled away.
  const View source = (app.view == View::Detail) ? app.detail_origin : app.view;
  if (source == View::History) return;  // load_history already refreshed the row.
  // Schedule is store-backed like History (P37; the caller reloads its rows
  // on a patch) — falling through to the Browse arm here wrote the schedule
  // show's enrichment into an unrelated catalog row (P36/P37 review).
  if (source == View::Schedule) return;
  if (source == View::Discover) {
    if (Enrichment* e = app.discover.selected_entry_mut(); e != nullptr) *e = merged;
    return;
  }
  // Browse (and the Detail zoom's Browse origin): the catalog cursor row.
  if (app.list_cursor >= 0 &&
      static_cast<std::size_t>(app.list_cursor) < app.catalog.size()) {
    app.catalog[static_cast<std::size_t>(app.list_cursor)].meta = merged;
  }
}

// --- Enrichment* handlers (P21, 05 §8 three-state law / app.rs) ------------

// A metadata answer overwrites drift fields but preserves user state (the store
// merge never clobbers list_status/progress), catalog_cache always refreshes,
// and a success doubles as the AniList recovery signal (§8.5). Renders from the
// canonical MERGED row, never the raw answer: the store merge can keep fields
// the fetch dropped (the read-back IS the merge).
void on_enrichment_refreshed(App& app, const EnrichmentRefreshed& ev) {
  app.enrich_inflight = false;
  if (app.deps == nullptr || app.deps->store == nullptr) return;
  Store& store = *app.deps->store;
  const std::int64_t now = now_epoch_secs();
  const std::int64_t ttl = enrichment_ttl_secs(
      ev.enrichment.status.has_value()
          ? std::optional<std::string_view>(*ev.enrichment.status)
          : std::nullopt);
  auto patched_res = store.patch_show_enrichment(ev.enrichment, /*stamp_fresh=*/true, now);
  const bool patched = patched_res.has_value() && *patched_res;
  (void)store.upsert_catalog_cache(ev.enrichment, now, now + ttl);
  if (patched) {
    load_history(app);
    // Schedule holds its own store-row copies (P37): reload them too, or a
    // heal landed while browsing Schedule (fresh next_airing_*) stays
    // invisible until the next manual `C` (P37 review).
    if (!app.schedule.rows.empty()) load_schedule(app);
  }

  // Read the canonical merged row back (02 §3.5 forbids a read-time merge, so
  // the read-back IS the merge). Fall back to the raw answer if the read fails.
  std::optional<Enrichment> merged;
  if (patched) {
    if (auto sh = store.get_show(ev.for_id); sh.has_value() && sh->has_value()) {
      merged = (*sh)->enrichment;
    }
  } else if (auto hit = store.get_catalog(ev.for_id); hit.has_value() && hit->has_value()) {
    merged = (*hit)->enrichment;
  }
  apply_enrichment_to_selection(app, ev.for_id, merged.value_or(ev.enrichment));

  app.toasts.clear_topic(kAnilistTopic);
  app.dirty = true;
}

void on_enrichment_null(App& app, const EnrichmentNull& ev) {
  app.enrich_inflight = false;
  if (app.deps != nullptr && app.deps->store != nullptr) {
    (void)app.deps->store->stamp_enrichment_checked(ev.for_id, now_epoch_secs());
  }
  app.toasts.clear_topic(kAnilistTopic);
  app.dirty = true;
}

void on_enrichment_failed(App& app) {
  app.enrich_inflight = false;
  app.toasts.push_persistent(ToastKind::Error, std::string(kAnilistTopic),
                             "can't reach AniList", app.tick_count);
  app.dirty = true;
}

// --- CharactersRecs* handlers (P36, 05 §8-style three-state) ----------------
//
// Session-cached only (no store write): the section exists to browse, not to
// persist, so unlike Enrichment* there is no catalog_cache/patch_show step
// here. A stale answer (the zoom moved to a different show while the fetch
// was out) is dropped by the for_id guard, same staleness idiom as apply_
// enrichment_to_selection.

void on_char_recs_done(App& app, const CharactersRecsDone& ev) {
  // The stale guard comes FIRST (P36 review): a stale answer must not clear
  // `loading` — when the selection moved on, `loading` belongs to the NEW
  // show's in-flight fetch, and clearing it here let the reconciler spawn a
  // duplicate.
  if (app.char_recs.for_id != ev.for_id) return;  // stale: zoom moved on.
  app.char_recs.loading = false;
  app.char_recs.data = ev.data;
  app.char_recs.fetched = true;
  app.char_recs.failed = false;
  app.char_recs.scroll = 0;
  app.toasts.clear_topic(kAnilistTopic);
  app.dirty = true;
}

void on_char_recs_null(App& app, const CharactersRecsNull& ev) {
  if (app.char_recs.for_id != ev.for_id) return;  // stale first, as above.
  app.char_recs.loading = false;
  app.char_recs.data = CharactersAndRecommendations{};
  app.char_recs.fetched = true;
  app.char_recs.failed = false;
  app.char_recs.scroll = 0;
  app.toasts.clear_topic(kAnilistTopic);
  app.dirty = true;
}

void on_char_recs_failed(App& app, const CharactersRecsFailed& ev) {
  if (app.char_recs.for_id != ev.for_id) return;  // stale first, as above.
  app.char_recs.loading = false;
  app.char_recs.failed = true;
  app.toasts.push_persistent(ToastKind::Error, std::string(kAnilistTopic),
                             "can't reach AniList", app.tick_count);
  app.dirty = true;
}

// Discover filter overlay genre picker (P38, §9): no for_id staleness guard
// needed — the fetch is global/session-scoped, and it's only ever in flight
// while the overlay itself is open (spawned from the `f` open handler).
void on_genre_collection_done(App& app, const GenreCollectionDone& ev) {
  app.discover_filters.genres_loading = false;
  app.discover_filters.genres = ev.genres;
  app.discover_filters.genres_fetched = true;
  app.discover_filters.genres_failed = false;
  app.dirty = true;
}

void on_genre_collection_failed(App& app) {
  app.discover_filters.genres_loading = false;
  app.discover_filters.genres_failed = true;
  app.dirty = true;
}

std::string describe_key(const KeyEvent& k) {
  using S = KeyEvent::Special;
  if (k.ctrl && k.codepoint) {
    std::string s = "Ctrl-";
    s.push_back(static_cast<char>(k.codepoint));
    return s;
  }
  switch (k.special) {
    case S::None: break;
    case S::Enter: return "Enter";
    case S::Escape: return "Esc";
    case S::Backspace: return "Backspace";
    case S::Tab: return "Tab";
    case S::Up: return "Up";
    case S::Down: return "Down";
    case S::Left: return "Left";
    case S::Right: return "Right";
    case S::Home: return "Home";
    case S::End: return "End";
    case S::PageUp: return "PageUp";
    case S::PageDown: return "PageDown";
    case S::Delete: return "Delete";
  }
  if (k.codepoint) {
    // Encode the printable codepoint back to UTF-8 for the debug echo.
    std::string s;
    const char32_t c = k.codepoint;
    if (c < 0x80) {
      s.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      s.push_back(static_cast<char>(0xC0 | (c >> 6)));
      s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      s.push_back(static_cast<char>(0xE0 | (c >> 12)));
      s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    return s;
  }
  return "?";
}

// Registry display name for a provider key; a retired key falls back to
// itself (same rule as toast_feedback's local lambda).
std::string display_provider_name(const App& app, const std::string& key) {
  if (app.deps != nullptr && app.deps->registry != nullptr) {
    if (const StreamProvider* p = app.deps->registry->by_name(key); p != nullptr) {
      return std::string(p->display_name());
    }
  }
  return key;
}

// The play-continuation relaunch (03 §6.4, app.rs fire_play_at relaunch arm):
// launch the session's grid cell `idx` from the CONTINUATION's captured meta,
// keeping the continuation armed so a further failure grows `tried`.
void fire_play_at(App& app, std::size_t idx) {
  if (app.play.active || !app.play_continuation.has_value()) return;
  if (!app.episode_session || app.deps == nullptr || app.deps->registry == nullptr ||
      app.queue == nullptr)
    return;
  const App::PlayContinuation& cont = *app.play_continuation;
  const EpisodeSession& s = *app.episode_session;
  if (idx >= s.episodes().size() || !s.serving().has_value()) return;
  const std::string episode = s.episodes()[idx];
  double start_secs = 0.0;
  if (app.deps->store != nullptr) {
    auto r = app.deps->store->get_resume(cont.for_id, cont.translation, episode);
    if (r.has_value() && r->has_value()) {
      start_secs = (*r)->start_secs(app.config.resume_offset_sec);
    }
  }
  app.play.active = true;
  app.play.for_id = cont.for_id;
  app.play.episode = episode;
  app.play.translation = cont.translation;
  app.play.episode_index = static_cast<std::uint32_t>(idx) + 1;
  app.play.started_tick = app.tick_count;
  app.play.last_checkpoint_tick = app.tick_count;
  app.play.position = 0.0;
  app.play.duration = 0.0;
  app.play.provider = *s.serving();
  app.play.mal_id = cont.mal_id;
  app.play.title_romaji = cont.title_romaji;
  spawn_play(*app.queue, *app.deps->registry, *app.deps, app.play.provider,
             s.serving_id(), episode, cont.for_id, start_secs, cont.translation,
             parse_quality(app.config.default_quality), cont.mal_id,
             cont.title_romaji, app.play.episode_index,
             aniskip::parse_skip_mode(app.config.skip_mode),
             player::parse_backend(app.config.player), app.config.player_path);
}

// The play-continuation consumer (03 §6.4, app.rs maybe_continue_play): once
// the walk lands a sibling grid, remap the in-progress episode (exact raw
// label, else 1-based ordinal) and relaunch. Runs after every episode event
// and after the synchronous hop path; a walk still in flight just waits.
void maybe_continue_play(App& app) {
  if (app.play.active || !app.play_continuation.has_value()) return;
  if (!app.episode_session) {
    app.play_continuation.reset();
    return;
  }
  const App::PlayContinuation& cont = *app.play_continuation;
  const EpisodeSession& s = *app.episode_session;
  const Translation translation =
      parse_translation(app.config.translation).value_or(Translation::Sub);
  if (!s.has_for_id() || s.for_id() != cont.for_id ||
      cont.translation != translation || s.no_source()) {
    app.play_continuation.reset();
    return;
  }
  if (s.loading()) return;  // walk still in flight: wait for its landing.
  if (!s.serving().has_value()) return;
  const std::string& serving = *s.serving();
  if (std::find(cont.tried.begin(), cont.tried.end(), serving) != cont.tried.end()) {
    // Nothing in flight and the grid still belongs to a burned provider: the
    // walk could not move, retire the continuation.
    app.play_continuation.reset();
    return;
  }
  const auto ix = map_episode_index(s.episodes(), cont.label, cont.ordinal);
  if (!ix.has_value()) {
    // Remap miss stops the continuation (03 §7); the label is provider text,
    // stripped before it touches a toast (ROD-435).
    const std::string raw = strip_controls(cont.label);
    app.toasts.push(ToastKind::Error,
                    "episode " + raw + " not found on " +
                        display_provider_name(app, serving),
                    app.tick_count);
    app.play_continuation.reset();
    return;
  }
  fire_play_at(app, *ix);
}

// The failed auto-open demotes to the History list (05 §10.6); the walk
// already toasted its failure classes on the way down (app.rs
// demote_resume_landing).
void demote_resume_landing(App& app) {
  app.resume_demote.reset();
  if (app.view == View::Detail) app.view = View::History;
  app.pane = Pane::List;
}

// Post-dispatch ride on the four episode events (app.rs tick's episode arm):
// the armed resume landing demotes on ITS show's dead end and clears on a
// successful load; the play continuation gets its consume chance.
void ride_episode_feedback(App& app, std::int64_t event_aid,
                           const std::vector<Feedback>& fbs) {
  bool dead_end = false;
  for (const Feedback& fb : fbs) {
    if (fb.kind == Feedback::Kind::DeadEnd) dead_end = true;
  }
  if (app.resume_demote.has_value() && *app.resume_demote == event_aid) {
    if (dead_end) {
      demote_resume_landing(app);
    } else if (app.episode.fetched && !app.episode.episodes.empty()) {
      app.resume_demote.reset();  // successful load clears the arm (05 §10.6).
    }
  }
  maybe_continue_play(app);
}

// Auto-open the most-recently-watched show parked on its resume episode
// (05 §10.6, app.rs open_resume_landing). Arms the demote: only THIS
// session's walk exhaust falls back to the list; user-driven opens never arm.
void open_resume_landing(App& app) {
  const auto aid = app.history.first_played();
  if (!aid.has_value()) return;
  const int visible = content_height(app.win.rows);
  app.history.select_aid(*aid, visible);
  const Show* sel = app.history.selected();
  if (sel == nullptr) return;
  if (app.win.cols >= kPaneSplitMin) {
    app.pane = Pane::Detail;
  } else {
    // Narrow History (DESIGN §5.4a): no pane exists, drill into the zoom.
    app.detail_origin = View::History;
    app.view = View::Detail;
  }
  if (app.episode_session && app.deps != nullptr && app.deps->registry != nullptr &&
      app.queue != nullptr) {
    app.resume_demote = *aid;
    if (app.prewarm) app.prewarm->cancel();
    EpisodeDeps ed = build_episode_deps(app);
    auto fb = app.episode_session->engage(sel->enrichment, ed);
    apply_feedback(app, fb);
    // A cached listing lands synchronously with no episode event to clear the
    // arm; a stranded arm would let a LATER same-show walk exhaust spuriously
    // demote (05 §10.6: only the auto-open's own walk may).
    if (app.episode.fetched && !app.episode.episodes.empty()) {
      app.resume_demote.reset();
    }
  }
}

// Handle a key. P6 wiring: q / Ctrl-C quit; view switch letters; `/` enters
// search; typing edits the query and (re)arms the debounce; Esc leaves search.
// The full bind set + worker fires are P7; here we only exercise the skeleton
// so the DoD demo can echo keys, animate, and quit.
// Keep the Browse cursor inside the visible band (browse.rs scroll rule; the
// History list has its own scroll_into_view — same contract, per-view state).
// File-scope, not an on_key local: P32's click-to-select shares it.
void browse_scroll_into_view(App& app) {
  const int list_visible = content_height(app.win.rows);
  if (list_visible <= 0) {
    app.browse_scroll = 0;
    return;
  }
  if (app.list_cursor < app.browse_scroll) {
    app.browse_scroll = app.list_cursor;
  } else if (app.list_cursor >= app.browse_scroll + list_visible) {
    app.browse_scroll = app.list_cursor - list_visible + 1;
  }
}

// Keep the `c` section's recommendation cursor inside its visible line band
// (P36 slice 3), same contract as browse_scroll_into_view. The section's
// height is content-dependent (views.hpp's char_recs_section_height), so this
// replays that geometry rather than assume a fixed viewport.
void char_recs_scroll_into_view(App& app) {
  const int content_rows = content_height(app.win.rows);
  const int section_h = detail::char_recs_section_height(content_rows);
  if (section_h <= 0) return;
  const std::size_t line =
      detail::char_recs_rec_line_index(app.char_recs.data, app.char_recs.cursor);
  if (line < app.char_recs.scroll) {
    app.char_recs.scroll = line;
  } else if (line >= app.char_recs.scroll + static_cast<std::size_t>(section_h)) {
    app.char_recs.scroll = line - static_cast<std::size_t>(section_h) + 1;
  }
}

// Downward motion onto the last result fires the next page (05 §16,
// ROD-156 parity; browse.rs maybe_load_more). Gated on hasNextPage, the
// in-flight guard, and the results actually answering the live buffer.
// File-scope for the same P32 sharing reason.
void maybe_load_more(App& app) {
  if (app.catalog.empty()) return;
  if (static_cast<std::size_t>(app.list_cursor) + 1 != app.catalog.size()) return;
  if (!app.search_has_next) return;
  if (!app.search_inflight_query.empty()) return;
  if (app.search_answered != app.search_query) return;
  if (app.deps == nullptr || !app.deps->search || app.queue == nullptr) return;
  app.search_inflight_query = app.search_query;
  spawn_search(*app.queue, app.deps->search, app.search_query, app.search_page + 1);
  recompute_loading(app);
}

// What Esc should return to if the user switches views right now: the view
// itself, or — from inside the Detail zoom — the view the zoom sits over
// (Detail has no view letter to synthesize back through).
View back_target(const App& app) {
  return app.view == View::Detail ? app.detail_origin : app.view;
}

void on_key(App& app, const KeyEvent& k) {
  app.last_key_debug = describe_key(k);
  debug_log("ui key: " + app.last_key_debug);
  app.dirty = true;

  if (k.ctrl && k.codepoint == U'c') {
    app.quit = true;
    return;
  }

  if (app.bar == BarMode::Search) {
    using S = KeyEvent::Special;
    // History's `/` is a LOCAL filter over the already-loaded rows (05 §2):
    // synchronous, no debounce, no network — Browse's own query buffer and
    // async search worker are untouched. `on_filter_edited`/`on_filter_cleared`
    // rebuild the nav order + reset scroll every edit (cheap: an in-memory
    // watchlist, never AniList-sized).
    const bool in_history = app.view == View::History;
    std::string& query = in_history ? app.history.filter : app.search_query;
    if (k.special == S::Escape) {
      app.bar = BarMode::Idle;
      if (in_history) {
        app.history.on_filter_cleared();
      } else {
        app.search_query.clear();
        app.search_debounce_deadline = 0;
      }
      return;
    }
    if (k.special == S::Enter) {
      app.bar = BarMode::Idle;  // lock the search; focus moves to list (P7).
      app.pane = Pane::List;
      return;
    }
    if (k.special == S::Backspace) {
      if (!query.empty()) {
        // Drop one UTF-8 codepoint (trailing continuation bytes then the lead).
        while (!query.empty() && (static_cast<unsigned char>(query.back()) & 0xC0) == 0x80) {
          query.pop_back();
        }
        if (!query.empty()) query.pop_back();
      }
      if (in_history) {
        app.history.on_filter_edited();
      } else {
        app.search_debounce_deadline = app.tick_count + kSearchDebounceTicks;
      }
      return;
    }
    if (k.special == S::None && k.codepoint >= 0x20) {
      // Append the codepoint as UTF-8.
      const char32_t c = k.codepoint;
      if (c < 0x80) {
        query.push_back(static_cast<char>(c));
      } else if (c < 0x800) {
        query.push_back(static_cast<char>(0xC0 | (c >> 6)));
        query.push_back(static_cast<char>(0x80 | (c & 0x3F)));
      } else {
        query.push_back(static_cast<char>(0xE0 | (c >> 12)));
        query.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        query.push_back(static_cast<char>(0x80 | (c & 0x3F)));
      }
      if (in_history) {
        app.history.on_filter_edited();
      } else {
        app.search_debounce_deadline = app.tick_count + kSearchDebounceTicks;
      }
      return;
    }
    return;  // other keys inert in search mode.
  }

  if (app.bar == BarMode::Score) {
    using S = KeyEvent::Special;
    // P34: a single-commit numeric prompt (unlike Search, nothing re-arms per
    // keystroke) over History's selected row. The prompt only opens with a
    // valid selection (the `s` keybind below checks), so app.history.selected()
    // returning nullptr here would mean the row vanished mid-edit (e.g. a
    // filter race) — treat that as a silent cancel, matching the store
    // writers' own unknown-show no-op law.
    if (k.special == S::Escape) {
      app.bar = BarMode::Idle;
      app.score_input.clear();
      return;
    }
    if (k.special == S::Enter) {
      app.bar = BarMode::Idle;
      const std::string input = std::move(app.score_input);
      app.score_input.clear();
      const Show* show = app.history.selected();
      if (show == nullptr || app.deps == nullptr || app.deps->store == nullptr) return;
      const ParsedScore parsed = parse_user_score(input);
      if (parsed.kind == ScoreParse::Invalid) return;  // junk: inert, no write.
      const std::int64_t aid = show->enrichment.anilist_id;
      const std::optional<std::uint32_t> score =
          parsed.kind == ScoreParse::Clear ? std::nullopt
                                            : std::optional<std::uint32_t>(parsed.value);
      if (!app.deps->store->set_user_score(aid, score).has_value()) {
        app.toasts.push(ToastKind::Error, "couldn't update score", app.tick_count);
        return;
      }
      load_history(app);
      arm_sync(app);
      return;
    }
    if (k.special == S::Backspace) {
      if (!app.score_input.empty()) app.score_input.pop_back();
      return;
    }
    // Only digits and one '.' are meaningful to parse_user_score; reject
    // anything else per keystroke so the buffer never needs its own
    // full-string validation while the user is still typing.
    if (k.special == S::None && k.codepoint < 0x80) {
      const char c = static_cast<char>(k.codepoint);
      const bool is_digit = c >= '0' && c <= '9';
      const bool is_dot = c == '.' && app.score_input.find('.') == std::string::npos;
      if (is_digit || is_dot) app.score_input.push_back(c);
    }
    return;  // other keys inert in score mode.
  }

  // --- P7 helpers, local to on_key (need app + the worker spawns above) ----

  // The currently selected show's Enrichment, or nullptr (empty list / OOB).
  // History (P16) shares this seam with Browse: both list views feed the same
  // detail-open/play machinery below (05 §15 "one detail renderer" pattern).
  auto selected_show = [&]() -> const Enrichment* {
    if (app.view == View::History) {
      const Show* s = app.history.selected();
      return s == nullptr ? nullptr : &s->enrichment;
    }
    if (app.view == View::Schedule) {
      const Show* s = app.schedule.selected();
      return s == nullptr ? nullptr : &s->enrichment;
    }
    if (app.view == View::Discover) {
      return app.discover.selected_entry();
    }
    if (app.list_cursor < 0 ||
        static_cast<std::size_t>(app.list_cursor) >= app.catalog.size())
      return nullptr;
    return &app.catalog[app.list_cursor].meta;
  };

  // The Discover grid geometry for the current window (the same helper draw
  // uses — one geometry source for nav and render, discover.rs's GridGeo rule).
  auto discover_geo = [&]() {
    return grid_geo(app.win.cols, content_height(app.win.rows), app.cover_caps.cell);
  };

  // `P` in Discover: add the selected show to the watchlist (mints/merges the
  // show row, 02 §3.7) and fire the eager sibling-provider prewarm walk so a
  // later play is tier-0 (05 §10.4). A no-store build (demo / no-persist) just
  // toasts the add optimistically. The prewarm fire mirrors the PositionUpdate
  // site's gates/deps construction.
  auto add_selected_to_library = [&]() {
    const Enrichment* show = selected_show();
    if (show == nullptr) return;
    if (app.deps != nullptr && app.deps->store != nullptr) {
      const auto res = app.deps->store->add_to_library(*show, now_epoch_secs());
      if (!res.has_value()) {
        app.toasts.push(ToastKind::Error, "couldn't add to your list", app.tick_count);
        return;
      }
      if (app.prewarm != nullptr) {
        // Live gates (app.rs on_plan): a user-facing resolve/launch or an
        // advancing fallback walk owns the CDN budget — the warm yields.
        PrewarmGates gates;
        gates.play_resolving =
            (app.play.active && app.play.duration == 0.0) ||
            (app.episode_session != nullptr && app.episode_session->loading());
        gates.fallback_active =
            app.episode_session != nullptr && app.episode_session->walk_active();
        PrewarmDeps pd;
        pd.store = app.deps->store;
        pd.registry = app.deps->registry;
        pd.queue = app.queue;
        pd.translation = Translation::Sub;
        pd.unix_now = now_epoch_secs();
        pd.now_tick = app.tick_count;
        app.prewarm->fire(*show, gates, pd);
      }
      arm_sync(app);
    }
    app.toasts.push(ToastKind::Success, "added to your list", app.tick_count);
  };

  // Enter the detail pane for the selected row: fires the episode fetch if
  // this show isn't the one already loaded/loading (DESIGN §7.1/§7.2 — fetch
  // on detail entry, never on list hover; 03 §6.1 step 1: clear stale
  // in-flight want first — a fresh id bump always supersedes any prior worker
  // via the generation, even if the old one hasn't posted yet).
  auto open_detail = [&]() {
    const Enrichment* show = selected_show();
    if (show == nullptr) return;
    // P37 slice 3: clear-on-open, regardless of which view opened it (05 §2-
    // style "cleared when the show is opened" — no view-specific branch, the
    // funnel here covers History/Schedule/Discover/Browse uniformly). Both
    // the in-memory copies (so the marker vanishes on the very next draw
    // without a reload) and the persisted row are cleared; a stale marker
    // reappearing after restart would violate clear-on-open.
    clear_notice_pending(app, show->anilist_id);
    app.pane = Pane::Detail;
    // P15: the resolve walk transport owns the open (tiers B/C, pins, absences,
    // routes, scoring). Its held-guard no-ops a re-entry on the same show+track,
    // so we no longer branch on episode.fetched here.
    if (app.episode_session && app.deps != nullptr && app.deps->registry != nullptr &&
        app.queue != nullptr) {
      // A user-facing resolve owns the CDN budget (03 §6.4): yield any prewarm
      // warm-walk so a background probe never races the user's resolve.
      if (app.prewarm) app.prewarm->cancel();
      EpisodeDeps ed = build_episode_deps(app);
      auto fb = app.episode_session->engage(*show, ed);
      apply_feedback(app, fb);
      return;
    }
    // Degraded path (no session/store — demo or a store-less test): keep the P7
    // first-hit resolve so the pty/demo harness still walks search→grid→play.
    if (app.episode.for_id == show->anilist_id && app.episode.fetched) return;
    app.episode = EpisodeState{};
    app.episode.for_id = show->anilist_id;
    if (app.deps != nullptr && app.deps->registry != nullptr && app.queue != nullptr) {
      app.episode.loading = true;
      recompute_loading(app);
      const Generation gen = app.episode_gen.bump();
      spawn_resolve_episodes(*app.queue, *app.deps->registry, *show, gen);
    }
  };

  // Enter on the char_recs section's focused recommendation row (P36 slice
  // 3): upsert catalog_cache (02 L2) then open the target's detail via the
  // Browse serving path — the row joins app.catalog under the cursor so
  // selected_enrichment/open_detail/the cover pipeline all serve it with zero
  // new identity machinery. Append-or-select, never replace (P36 review): a
  // live Browse search page must survive the promote — Escape from the
  // promoted zoom lands back in that list, promoted row selected at its tail.
  // The char_recs section itself resets (it belongs to the show that WAS
  // open), so the new zoom opens clean and a `c` there fetches its own answer.
  auto promote_recommendation = [&]() {
    if (app.char_recs.cursor >= app.char_recs.data.recommendations.size()) return;
    const Enrichment target = app.char_recs.data.recommendations[app.char_recs.cursor];
    if (app.deps != nullptr && app.deps->store != nullptr) {
      const std::int64_t now = now_epoch_secs();
      const std::int64_t ttl = enrichment_ttl_secs(
          target.status.has_value() ? std::optional<std::string_view>(*target.status)
                                    : std::nullopt);
      (void)app.deps->store->upsert_catalog_cache(target, now, now + ttl);
    }
    std::size_t idx = app.catalog.size();
    for (std::size_t i = 0; i < app.catalog.size(); ++i) {
      if (app.catalog[i].meta.anilist_id == target.anilist_id) {
        idx = i;
        break;
      }
    }
    if (idx == app.catalog.size()) app.catalog.push_back(CatalogRow{target});
    app.list_cursor = static_cast<int>(idx);
    app.detail_origin = View::Browse;
    app.char_recs = CharactersRecsState{};
    app.view = View::Detail;
    open_detail();
  };

  // Play the grid cell at app.episode.cursor: resolve+play via P5.
  auto play_focused_episode = [&]() {
    if (app.play.active) return;  // ONE play in flight at a time (04 §7.7).
    if (app.episode.cursor < 0 ||
        static_cast<std::size_t>(app.episode.cursor) >= app.episode.episodes.size())
      return;
    const Enrichment* show = selected_show();
    if (show == nullptr || app.deps == nullptr || app.deps->registry == nullptr ||
        app.queue == nullptr)
      return;
    // The grid must belong to the selected row, and it must have landed with
    // the provider-side id it was fetched under (Tier-A canonical key or the
    // Tier-C search hit). NEVER re-derive the id here: a Tier-C show has no
    // canonical_key, and its anilist_id is not a provider key — resolving by
    // it would 404 or, on an id collision, play the wrong show.
    if (app.episode.for_id != show->anilist_id) return;
    if (app.episode.provider_id.empty()) return;
    const std::size_t idx = static_cast<std::size_t>(app.episode.cursor);
    const std::string episode = app.episode.episodes[idx];
    const Translation translation =
        parse_translation(app.config.translation).value_or(Translation::Sub);
    // Resume start (03 §6.3.1): read the saved point for this (show, track,
    // ep) and apply the start rule (restart-if-watched / natural-end /
    // unusable; else back up config.resume_offset_sec). No store or no row →
    // start at 0.
    double start_secs = 0.0;
    if (app.deps->store != nullptr) {
      auto r = app.deps->store->get_resume(show->anilist_id, translation, episode);
      if (r.has_value() && r->has_value()) {
        start_secs = (*r)->start_secs(app.config.resume_offset_sec);
      }
    }
    // A user-initiated fire outranks any armed walk continuation (03 §6.4,
    // playback.rs fire): drop it rather than let a stale relaunch race this.
    app.play_continuation.reset();
    app.play.active = true;
    app.play.for_id = show->anilist_id;
    app.play.episode = episode;
    app.play.translation = translation;
    app.play.episode_index = static_cast<std::uint32_t>(idx) + 1;  // 1-based ordinal.
    app.play.started_tick = app.tick_count;
    app.play.last_checkpoint_tick = app.tick_count;
    app.play.position = 0.0;
    app.play.duration = 0.0;
    app.play.mal_id = show->mal_id;          // captured for the continuation
    app.play.title_romaji = show->title_romaji;  // relaunch (fire_play_at).
    // The grid's serving provider (empty on the degraded P7 path → primary()).
    app.play.provider = app.episode.serving;
    spawn_play(*app.queue, *app.deps->registry, *app.deps, app.play.provider,
              app.episode.provider_id, episode, show->anilist_id, start_secs,
              translation, parse_quality(app.config.default_quality),
              show->mal_id, show->title_romaji, app.play.episode_index,
              aniskip::parse_skip_mode(app.config.skip_mode),
              player::parse_backend(app.config.player), app.config.player_path);
  };

  // Download the grid cell at app.episode.cursor (P35 slice 3): resolve +
  // download_link via the detached worker. Mirrors play_focused_episode's
  // gates (grid ownership, provider_id trust) with its own one-at-a-time law.
  auto download_focused_episode = [&]() {
    if (app.download.active) {
      app.toasts.push(ToastKind::Info, "a download is already running", app.tick_count);
      return;
    }
    if (app.episode.cursor < 0 ||
        static_cast<std::size_t>(app.episode.cursor) >= app.episode.episodes.size())
      return;
    const Enrichment* show = selected_show();
    if (show == nullptr || app.deps == nullptr || app.deps->registry == nullptr ||
        app.queue == nullptr)
      return;
    // Same grid-trust gates as play (ROD-222/329): the grid must belong to
    // the selection and carry the provider-side id it was fetched under.
    if (app.episode.for_id != show->anilist_id) return;
    if (app.episode.provider_id.empty()) return;
    if (app.deps->download_dir.empty()) {
      app.toasts.push(ToastKind::Warning, "downloads need a data dir", app.tick_count);
      return;
    }
    const std::size_t idx = static_cast<std::size_t>(app.episode.cursor);
    const std::string episode = app.episode.episodes[idx];
    const Translation translation =
        parse_translation(app.config.translation).value_or(Translation::Sub);
    app.download.active = true;
    app.download.for_id = show->anilist_id;
    app.download.episode = episode;
    app.download.bytes = 0;
    app.download.total = 0;
    app.download.started_tick = app.tick_count;
    app.download.cancel = std::make_shared<std::atomic<bool>>(false);
    app.download.child_pid = std::make_shared<std::atomic<long>>(0);
    app.toasts.push(ToastKind::Info, "downloading ep " + episode, app.tick_count);
    spawn_download(*app.queue, *app.deps->registry, app.deps->http,
                   app.episode.serving, app.episode.provider_id, episode,
                   show->anilist_id, translation,
                   parse_quality(app.config.default_quality),
                   app.deps->download_dir, app.deps->ffmpeg_path,
                   app.download.cancel, app.download.child_pid);
  };

  // Demote out of the Detail zoom (DESIGN §7.4: Space/h/Esc all do this).
  // A Discover or Schedule origin lands on a list-only view with no detail
  // pane — pane resets to List (app.rs demote's Discover arm; Schedule joins
  // it at P37, same reasoning: no Pane::Detail mode to return into).
  auto demote_zoom = [&]() {
    app.view = app.detail_origin;
    app.pane = (app.detail_origin != View::Discover && app.detail_origin != View::Schedule &&
               app.win.cols >= kPaneSplitMin)
                   ? Pane::Detail
                   : Pane::List;
    // The section's Tab-focus is a zoom-local sub-focus: leaving the zoom
    // drops it so a re-zoom always lands on the grid (the documented
    // CharactersRecsState::focused invariant — P36 review).
    app.char_recs.focused = false;
  };

  // Content-region row count (draw()'s y0=kContentY0..y1=height-1
  // convention): the History list's scroll_into_view budget.
  const int list_visible = content_height(app.win.rows);

  // Reload History from the store (05 §2): re-read, cancel any armed delete
  // confirm (DESIGN §6.5 — its row identity can no longer be trusted), refresh
  // the shared detail pane's selection.
  auto reload_history = [&]() {
    load_history(app);
    app.confirm_delete = std::nullopt;
  };

  // Reload Schedule from the store (P37): same local-read discipline as
  // History, re-grouped at the current clock stamp.
  auto reload_schedule = [&]() { load_schedule(app); };

  // History status transitions (05 §4): store + memory move together (the
  // reload re-groups and the cursor follows the show's identity). Captures
  // the pre-mutation (status, progress) into the single-level undo. Shared by
  // p/x/c/w AND History's `P` (re-plan), which is just the 5th status value.
  auto on_status_key = [&](ListStatus status) {
    if (app.view != View::History || app.pane != Pane::List) return;
    const Show* show = app.history.selected();
    if (show == nullptr || app.deps == nullptr || app.deps->store == nullptr) return;
    const std::int64_t aid = show->enrichment.anilist_id;
    const auto before = std::make_tuple(aid, show->list_status, show->progress);
    if (!app.deps->store->set_list_status(aid, status, now_epoch_secs()).has_value()) {
      app.toasts.push(ToastKind::Error, "couldn't update status", app.tick_count);
      return;
    }
    app.undo = before;
    reload_history();
    arm_sync(app);
  };

  // `u` undoes the last status mutation (05 §4), keyed off the captured row
  // id, never the cursor. Single-level: consuming via reset enforces a second
  // `u` press is a no-op.
  auto on_undo = [&]() {
    if (app.view != View::History || app.pane != Pane::List) return;
    if (!app.undo.has_value() || app.deps == nullptr || app.deps->store == nullptr) return;
    const auto [aid, status, progress] = *app.undo;
    app.undo = std::nullopt;
    if (app.deps->store->restore_list_status(aid, status, progress, now_epoch_secs())
            .has_value()) {
      app.toasts.push(ToastKind::Info, "undone", app.tick_count);
    }
    reload_history();
    arm_sync(app);
  };

  // `r` recomputes progress from episode_progress (05 §4). Survives a pending
  // undo by CLEARING it (recompute itself is not undoable via `u`): the
  // c-then-r-then-u law.
  auto on_recompute = [&]() {
    if (app.view != View::History || app.pane != Pane::List) return;
    const Show* show = app.history.selected();
    if (show == nullptr || app.deps == nullptr || app.deps->store == nullptr) return;
    const std::int64_t aid = show->enrichment.anilist_id;
    if (app.deps->store->recompute_progress(aid, Translation::Sub, now_epoch_secs())
            .has_value()) {
      app.undo = std::nullopt;
      reload_history();
      app.toasts.push(ToastKind::Success, "progress reset", app.tick_count);
      arm_sync(app);
    } else {
      app.toasts.push(ToastKind::Error, "couldn't reset progress", app.tick_count);
    }
  };

  // `X` arms the hard-delete confirm (DESIGN §6.5); a no-op off the History
  // list or with no entry under the cursor.
  auto on_arm_delete = [&]() {
    if (app.view != View::History || app.pane != Pane::List) return;
    const Show* show = app.history.selected();
    if (show != nullptr) app.confirm_delete = show->enrichment.anilist_id;
  };

  // Armed-confirm key table (DESIGN §6.5): only y/Y fires; a repeat X stays
  // armed (defeats a key-repeat self-confirm); anything else cancels.
  auto on_confirm_key = [&](const KeyEvent& ck) {
    if (ck.special == KeyEvent::Special::None && !ck.ctrl &&
        (ck.codepoint == U'y' || ck.codepoint == U'Y')) {
      const std::int64_t aid = *app.confirm_delete;
      app.confirm_delete = std::nullopt;
      // ROD-220: the currently-playing show refuses the cascade; the confirm
      // is already disarmed above (freeze parity — a repeat X is needed to
      // try again).
      if (app.play.active && app.play.for_id == aid) {
        app.toasts.push(ToastKind::Warning, "can't delete, currently playing", app.tick_count);
        return;
      }
      if (app.deps != nullptr && app.deps->store != nullptr) {
        (void)app.deps->store->delete_show(aid);
      }
      if (app.undo.has_value() && std::get<0>(*app.undo) == aid) app.undo = std::nullopt;
      reload_history();
      return;
    }
    if (ck.special == KeyEvent::Special::None && !ck.ctrl && ck.codepoint == U'X') {
      return;  // repeat X: no-op, stays armed.
    }
    app.confirm_delete = std::nullopt;  // anything else cancels.
  };

  // The connect modal captures every key except Ctrl-C (handled above): esc
  // cancels the wait, c copies the url, anything else is swallowed (06 §9 /
  // DESIGN 5.5a — checked before every other mode, including the armed
  // confirm below).
  if (app.connect.has_value()) {
    using S = KeyEvent::Special;
    if (k.special == S::Escape) {
      // "stop waiting", not "cancel": a callback already landing still
      // completes (login.rs's worker only skips posting on Canceled); esc
      // only stops waiting for the RESULT (ROD-448 review).
      app.connect->cancel();
      app.connect = std::nullopt;
      return;
    }
    if (k.special == S::None && !k.ctrl && k.codepoint == U'c') {
      app.connect->copied = true;
      return;
    }
    return;  // everything else inert while connecting.
  }

  // The Discover filter overlay (P38, §9) captures every key, same tier as
  // the connect modal above (checked before the armed confirm / normal-mode
  // switch): jk move the row cursor, hl cycle the row's value (Genre's hl
  // instead walks the fetched vocabulary), space toggles genre membership,
  // c clears every filter, enter commits the draft, esc discards it.
  if (app.discover_filters.visible) {
    using S = KeyEvent::Special;
    DiscoverFilterOverlay& ov = app.discover_filters;
    if (k.special == S::Escape) {
      ov.visible = false;
      return;
    }
    if (k.special == S::Enter) {
      app.discover.set_filters(ov.draft);
      ov.visible = false;
      return;
    }
    if (k.special != S::None || k.ctrl) return;  // everything else inert.
    switch (k.codepoint) {
      case U'j':
        ov.row_cursor = (ov.row_cursor + 1) % kFilterRows.size();
        return;
      case U'k':
        ov.row_cursor = (ov.row_cursor + kFilterRows.size() - 1) % kFilterRows.size();
        return;
      case U'h':
      case U'l': {
        const std::int64_t delta = k.codepoint == U'l' ? 1 : -1;
        switch (kFilterRows[ov.row_cursor]) {
          case FilterRow::Genre:
            if (!ov.genres.empty()) {
              const std::int64_t n = static_cast<std::int64_t>(ov.genres.size());
              std::int64_t idx = static_cast<std::int64_t>(ov.genre_cursor) + delta;
              idx = ((idx % n) + n) % n;
              ov.genre_cursor = static_cast<std::size_t>(idx);
            }
            return;
          case FilterRow::Year:
            cycle_year(ov.draft.year, delta,
                      std::max<std::uint32_t>(app.discover_cour.year, 1960));
            return;
          case FilterRow::Status:
            cycle_status(ov.draft.status, delta);
            return;
          case FilterRow::MinScore:
            cycle_min_score(ov.draft.min_score, delta);
            return;
        }
        return;
      }
      case U' ':
        if (kFilterRows[ov.row_cursor] == FilterRow::Genre && !ov.genres.empty()) {
          toggle_genre(ov.draft.genres, ov.genres[ov.genre_cursor]);
        }
        return;
      case U'c':
        ov.draft = DiscoverFilters{};
        return;
      default:
        return;
    }
  }

  // The armed confirm freezes EVERY other key (DESIGN §6.5): q does not quit,
  // cursor keys do not move, view switches do not fire. Only Ctrl-C (handled
  // above, unconditionally) escapes this gate.
  if (app.confirm_delete.has_value()) {
    on_confirm_key(k);
    return;
  }

  // Settings claims its own keys first, like the armed-confirm gate above,
  // but only Ignored falls through (q/B/H/D/etc. still work when the cursor
  // isn't on an editable row) — settings.rs's on_settings_key ordering (05
  // §13: "q/view letters fall through when not editing").
  if (app.view == View::Settings) {
    std::vector<std::string> names;
    if (app.deps != nullptr && app.deps->registry != nullptr) {
      for (std::size_t i = 0; i < app.deps->registry->size(); ++i) {
        names.emplace_back(app.deps->registry->at(i)->name());
      }
    }
    const std::string translation_before = app.config.translation;
    const SettingsOutcome outcome = app.settings.on_key(k, app.config, names);
    switch (outcome) {
      case SettingsOutcome::Ignored:
        break;  // fall through to the global chain below.
      case SettingsOutcome::Consumed:
        return;
      case SettingsOutcome::ConfigChanged: {
        // translation is the one row with a live re-key effect (mirrors the
        // :dub command): re-engage an already-engaged grid so it reflects the
        // new track. title_language/palette/etc. need no callback — draw()
        // reads app.config fresh every frame (§1 scope table, P18_PLAN.md).
        if (app.config.translation != translation_before && app.episode_session &&
            app.episode_session->has_for_id()) {
          const Enrichment* show = selected_show();
          if (show != nullptr && show->anilist_id == app.episode_session->for_id()) {
            app.episode_session->reset();
            EpisodeDeps ed = build_episode_deps(app);
            auto fb = app.episode_session->engage(*show, ed);
            apply_feedback(app, fb);
          }
        }
        return;
      }
      case SettingsOutcome::ConnectRequested:
        open_connect(app);
        return;
      case SettingsOutcome::MalConnectRequested:
        open_mal_connect(app);
        return;
    }
  }

  // Arrow keys alias hjkl in normal mode (DESIGN §6.1 pairs them; the P6
  // decoder has emitted them all along — they were inert until this point).
  // Search mode already returned above, so query editing is unaffected.
  KeyEvent key = k;
  if (!key.ctrl) {
    using S = KeyEvent::Special;
    if (key.special == S::Up) { key.special = S::None; key.codepoint = U'k'; }
    else if (key.special == S::Down) { key.special = S::None; key.codepoint = U'j'; }
    else if (key.special == S::Left) { key.special = S::None; key.codepoint = U'h'; }
    else if (key.special == S::Right) { key.special = S::None; key.codepoint = U'l'; }
  }

  // Normal mode.
  if (key.special == KeyEvent::Special::None && !key.ctrl) {
    switch (key.codepoint) {
      case U'q':
        if (app.view == View::Settings) persist_settings(app);
        app.quit = true;
        return;
      case U'/':
        // `/` is "search AniList" from ANYWHERE except History, whose `/` is
        // the local filter (KEYS_AUDIT F1, generalizing the old Discover-only
        // redirect; Discover's OWN narrowing stays `f`, P38). Before the
        // redirect covered every view, Detail/Schedule/Settings armed the bar
        // over a surface that never shows results — and the debounced search
        // kept firing, replacing app.catalog under a live Browse-origin zoom.
        // Settings persists on the way out, same as the view-letter arms.
        if (app.view != View::Browse && app.view != View::History) {
          if (app.view == View::Settings) persist_settings(app);
          app.view_back = back_target(app);
          app.view = View::Browse;
          app.pane = Pane::List;
        }
        app.bar = BarMode::Search;
        if (app.view == View::History) {
          app.history.filter.clear();  // re-opening always starts fresh.
        } else {
          app.search_query.clear();
        }
        return;
      // Same-view switches are no-ops that preserve the pane (05 §6, app.rs
      // switch_view's early return); Settings persists on the way OUT only.
      case U'B':
        if (app.view == View::Browse) return;
        if (app.view == View::Settings) persist_settings(app);
        app.view_back = back_target(app);
        app.view = View::Browse;
        app.pane = Pane::List;
        return;
      case U'H':
        if (app.view == View::History) return;  // goto, not toggle — but inert in place.
        if (app.view == View::Settings) persist_settings(app);
        app.view_back = back_target(app);
        app.view = View::History;
        app.pane = Pane::List;
        reload_history();
        return;
      case U'D':
        if (app.view == View::Discover) return;
        if (app.view == View::Settings) persist_settings(app);
        app.view_back = back_target(app);
        app.view = View::Discover;
        return;
      case U'C':
        if (app.view == View::Schedule) return;
        if (app.view == View::Settings) persist_settings(app);
        app.view_back = back_target(app);
        app.view = View::Schedule;
        reload_schedule();
        return;
      case U'S':
        if (app.view == View::Settings) return;
        app.view_back = back_target(app);
        app.view = View::Settings;
        return;
      // Discover axis controls (04 §7.5): `[`/`]` cycle with wraparound (both
      // re-arm a failed slot); `1`-`4` direct select. Inert off Discover.
      case U'[': if (app.view == View::Discover) app.discover.cycle_axis(-1); return;
      case U']': if (app.view == View::Discover) app.discover.cycle_axis(1); return;
      case U'1': if (app.view == View::Discover) app.discover.select_axis(0); return;
      case U'2': if (app.view == View::Discover) app.discover.select_axis(1); return;
      case U'3': if (app.view == View::Discover) app.discover.select_axis(2); return;
      case U'4': if (app.view == View::Discover) app.discover.select_axis(3); return;
      case U'f':
        // Discover filter overlay (P38, §9): opens with the current
        // committed filters as the starting draft (re-editing, not always a
        // fresh blank), and lazily fires the genre-vocabulary fetch on
        // first open per session (genres_fetched gates a re-fetch storm).
        if (app.view == View::Discover) {
          app.discover_filters.visible = true;
          app.discover_filters.draft = app.discover.filters();
          app.discover_filters.row_cursor = 0;
          app.discover_filters.genre_cursor = 0;
          if (!app.discover_filters.genres_fetched && !app.discover_filters.genres_loading &&
              app.deps != nullptr && app.deps->genre_collection && app.queue != nullptr) {
            app.discover_filters.genres_loading = true;
            spawn_genre_collection(*app.queue, app.deps->genre_collection);
          }
        }
        return;
      case U'j':
        if (app.view == View::Discover) {
          app.discover.nav(0, 1, discover_geo());
        } else if (app.view == View::Browse && app.pane == Pane::List) {
          if (!app.catalog.empty() &&
              static_cast<std::size_t>(app.list_cursor) + 1 < app.catalog.size())
            ++app.list_cursor;
          browse_scroll_into_view(app);
          maybe_load_more(app);  // Down at the last result fires the next page.
        } else if (app.view == View::History && app.pane == Pane::List) {
          app.history.nav(1, list_visible);
        } else if (app.view == View::Schedule) {
          app.schedule.nav(1, list_visible);
        } else if (app.view == View::Detail && char_recs_section_shown(app) &&
                   app.char_recs.focused) {
          // P36 slice 3: the section owns j/k while Tab-focused AND actually
          // rendered (a cramped frame skips the split — keys must not move an
          // invisible cursor); the recommendation cursor is the Enter target,
          // never the grid's.
          const std::size_t n = app.char_recs.data.recommendations.size();
          if (n > 0 && app.char_recs.cursor + 1 < n) ++app.char_recs.cursor;
          char_recs_scroll_into_view(app);
        } else if ((app.view == View::Browse && app.pane == Pane::Detail) ||
                  (app.view == View::History && app.pane == Pane::Detail) ||
                  app.view == View::Detail) {
          // P33: one visual row down (grid geometry, not flat +1).
          if (!app.episode.episodes.empty()) {
            const int per_row = static_cast<int>(detail::episode_grid_per_row(app));
            const int last = static_cast<int>(app.episode.episodes.size()) - 1;
            app.episode.cursor = std::min(last, app.episode.cursor + per_row);
          }
        }
        return;
      case U'k':
        if (app.view == View::Discover) {
          app.discover.nav(0, -1, discover_geo());
        } else if (app.view == View::Browse && app.pane == Pane::List) {
          if (app.list_cursor > 0) --app.list_cursor;
          browse_scroll_into_view(app);
        } else if (app.view == View::History && app.pane == Pane::List) {
          app.history.nav(-1, list_visible);
        } else if (app.view == View::Schedule) {
          app.schedule.nav(-1, list_visible);
        } else if (app.view == View::Detail && char_recs_section_shown(app) &&
                   app.char_recs.focused) {
          if (app.char_recs.cursor > 0) --app.char_recs.cursor;
          char_recs_scroll_into_view(app);
        } else if ((app.view == View::Browse && app.pane == Pane::Detail) ||
                  (app.view == View::History && app.pane == Pane::Detail) ||
                  app.view == View::Detail) {
          // P33: one visual row up (grid geometry, not flat -1).
          if (!app.episode.episodes.empty()) {
            const int per_row = static_cast<int>(detail::episode_grid_per_row(app));
            app.episode.cursor = std::max(0, app.episode.cursor - per_row);
          }
        }
        return;
      // g/G jump (05 §1.1, app.rs on_jump): a focused detail surface consumes
      // it for the grid; else the focused list jumps. No load-more on a jump.
      case U'g':
      case U'G': {
        const bool top = (key.codepoint == U'g');
        if (app.view == View::Discover) {
          // KEYS_AUDIT F3: first/last card of the active axis slot — every
          // other list surface jumped on g/G, the Discover grid was the gap.
          const auto& entries = app.discover.slot().entries;
          if (!entries.empty()) {
            app.discover.select(top ? 0 : entries.size() - 1, discover_geo());
          }
          return;
        }
        const bool on_detail_surface =
            app.view == View::Detail ||
            ((app.view == View::Browse || app.view == View::History) &&
             app.pane == Pane::Detail);
        if (on_detail_surface) {
          // KEYS_AUDIT F4: while the `c` section holds Tab focus, g/G jump
          // its recommendation cursor — the same ownership rule j/k follow.
          if (app.view == View::Detail && char_recs_section_shown(app) &&
              app.char_recs.focused) {
            const std::size_t n = app.char_recs.data.recommendations.size();
            if (n > 0) {
              app.char_recs.cursor = top ? 0 : n - 1;
              char_recs_scroll_into_view(app);
            }
            return;
          }
          if (!app.episode.episodes.empty()) {
            app.episode.cursor =
                top ? 0 : static_cast<int>(app.episode.episodes.size()) - 1;
          }
          return;
        }
        if (app.view == View::Browse && app.pane == Pane::List) {
          if (!app.catalog.empty()) {
            app.list_cursor = top ? 0 : static_cast<int>(app.catalog.size()) - 1;
            browse_scroll_into_view(app);
          }
          return;
        }
        if (app.view == View::History && app.pane == Pane::List) {
          app.history.jump(top, list_visible);
        }
        if (app.view == View::Schedule) {
          app.schedule.jump(top, list_visible);
        }
        return;
      }
      case U'h':
        if (app.view == View::Discover) { app.discover.nav(-1, 0, discover_geo()); return; }
        // P33: inside the focused grid, h/Left steps the episode cursor back
        // one cell — it loses its "back to list"/demote meaning there (Esc/
        // space already cover back; DoD table: grid focused vs list pane).
        if ((app.view == View::Browse && app.pane == Pane::Detail) ||
            (app.view == View::History && app.pane == Pane::Detail) ||
            app.view == View::Detail) {
          if (app.episode.cursor > 0) --app.episode.cursor;
          return;
        }
        app.pane = Pane::List;
        return;
      case U'l':
        if (app.view == View::Discover) { app.discover.nav(1, 0, discover_geo()); return; }
        // P33: inside the focused grid, l/Right steps the episode cursor
        // forward one cell.
        if ((app.view == View::Browse && app.pane == Pane::Detail) ||
            (app.view == View::History && app.pane == Pane::Detail) ||
            app.view == View::Detail) {
          if (!app.episode.episodes.empty() &&
              static_cast<std::size_t>(app.episode.cursor) + 1 < app.episode.episodes.size())
            ++app.episode.cursor;
          return;
        }
        if (((app.view == View::Browse) || (app.view == View::History)) &&
            app.pane == Pane::List && app.win.cols >= kPaneSplitMin) {
          open_detail();
        }
        return;
      case U' ':
        // §9 consistency (user-flagged): Space is ONE thing on every surface —
        // the zoom toggle over the current selection. In the zoom it demotes
        // (DESIGN §7.4); anywhere a selection shows it promotes into the zoom.
        // (Previously the promote needed a focused pane, narrow History
        // drilled, and everything else no-op'd — three behaviors for one key.)
        if (app.view == View::Detail) { demote_zoom(); return; }
        if ((app.view == View::Browse || app.view == View::History) &&
            app.pane == Pane::Detail) {
          app.detail_origin = app.view;
          app.view = View::Detail;
          return;
        }
        // From a list, wide or narrow: drill straight into the zoom (the
        // narrow-History §5.4a landing, now the rule for every list).
        if ((app.view == View::Browse || app.view == View::History) &&
            app.pane == Pane::List) {
          if (detail::selected_enrichment(app) == nullptr) return;
          app.detail_origin = app.view;
          app.view = View::Detail;
          open_detail();
          return;
        }
        // Discover / Schedule: the same landing as their Enter arms (P17 zoom
        // over the card; Schedule's open_detail-BEFORE-flip ordering — its
        // selected_show keys off app.view, see the Enter arm).
        if (app.view == View::Discover) {
          if (app.discover.selected_entry() != nullptr) {
            app.detail_origin = View::Discover;
            app.view = View::Detail;
            open_detail();
          }
          return;
        }
        if (app.view == View::Schedule) {
          if (app.schedule.selected() != nullptr) {
            open_detail();
            app.detail_origin = View::Schedule;
            app.view = View::Detail;
          }
          return;
        }
        return;
      case U'v':
        // P15: cycle the per-show provider pin (unpinned → each provider →
        // unpinned), arming a pin_flip walk when the new pin isn't serving.
        // Only meaningful in a detail context with the transport wired.
        if (app.episode_session &&
            ((app.view == View::Browse && app.pane == Pane::Detail) ||
             (app.view == View::History && app.pane == Pane::Detail) ||
             app.view == View::Detail)) {
          if (app.prewarm) app.prewarm->cancel();  // a pin-flip owns the budget.
          EpisodeDeps ed = build_episode_deps(app);
          auto fb = app.episode_session->cycle_pin(ed);
          apply_feedback(app, fb);
        }
        return;
      case U'd':
        // P35 slice 3: download the focused grid cell. Same detail contexts
        // as Enter's play arm; inert anywhere else ('D' is the Discover tab).
        if ((app.view == View::Browse && app.pane == Pane::Detail) ||
            (app.view == View::History && app.pane == Pane::Detail) ||
            app.view == View::Detail) {
          if (app.episode.fetched && !app.episode.episodes.empty()) {
            download_focused_episode();
          }
        }
        return;
      // --- History status/undo/recompute/delete (P16, 05 §3-§4) ------------
      case U'p': on_status_key(ListStatus::Paused); return;
      case U'x': on_status_key(ListStatus::Dropped); return;
      case U'c':
        // P36: in the Detail zoom, `c` toggles the Characters/Recommendations
        // section (the fetch itself is armed by reconcile_char_recs, not
        // here — this key only flips visibility + resets scroll on open).
        // Disjoint from the History-list `c` below: the zoom and the History
        // list are never the same app.view.
        if (app.view == View::Detail) {
          app.char_recs.visible = !app.char_recs.visible;
          app.char_recs.scroll = 0;
          if (app.char_recs.visible) {
            // Re-opening is the MANUAL retry for a terminal `failed`
            // (reconcile_char_recs never refires on its own), and the
            // reopened section starts at its top — cursor re-homes with the
            // scroll (P36 review).
            app.char_recs.failed = false;
            app.char_recs.cursor = 0;
          } else {
            // Hiding always drops focus back to the grid (app.hpp's
            // CharactersRecsState::focused doc) so a later re-open never
            // starts Tab-focused on a section the user hasn't seen yet.
            app.char_recs.focused = false;
          }
          return;
        }
        on_status_key(ListStatus::Completed);
        return;
      case U'w': on_status_key(ListStatus::Watching); return;
      case U'P':
        // History-list: the 5th manual transition (re-plan). In Discover and
        // on the Browse list (05 §4, app.rs on_plan): add the selected show to
        // the watchlist (Store::add_to_library) + fire the sibling-provider
        // prewarm walk + a toast.
        if (app.view == View::History) {
          on_status_key(ListStatus::Planning);
        } else if (app.view == View::Discover ||
                   (app.view == View::Browse && app.pane == Pane::List)) {
          add_selected_to_library();
        }
        return;
      case U'u': on_undo(); return;
      case U'r': on_recompute(); return;
      case U'X': on_arm_delete(); return;
      case U's':
        // P34: opens the score prompt over the selected row. Pre-fills from
        // the show's current score (÷10, trailing .0 dropped) so re-editing
        // doesn't force retyping an unchanged value; unset starts blank.
        if (app.view == View::History && app.pane == Pane::List) {
          const Show* show = app.history.selected();
          if (show != nullptr) {
            app.bar = BarMode::Score;
            app.score_input.clear();
            if (show->user_score.has_value() && *show->user_score > 0) {
              const double v = static_cast<double>(*show->user_score) / 10.0;
              std::string s = std::to_string(v);
              // Trim trailing zeros then a trailing '.', std::to_string pads
              // to 6 decimals ("7.500000") which parse_user_score would also
              // accept, but the prefill should read like what the user typed.
              while (!s.empty() && s.back() == '0') s.pop_back();
              if (!s.empty() && s.back() == '.') s.pop_back();
              app.score_input = s;
            }
          }
        }
        return;
      default: break;
    }
  }

  if (key.special == KeyEvent::Special::Enter) {
    if (app.view == View::Discover) {
      // Enter → the full-canvas Detail zoom over the selected card (P17). The
      // zoom reuses the whole single-cover detail pane via detail_origin.
      if (app.discover.selected_entry() != nullptr) {
        app.detail_origin = View::Discover;
        app.view = View::Detail;
        open_detail();
      }
      return;
    }
    if (app.view == View::Browse && app.pane == Pane::List) {
      // Wide: into the pane. Narrow (KEYS_AUDIT F2 — deliberately diverges
      // from app.rs's no-op): drill into the zoom exactly like narrow
      // History below — after the Space rework made both narrow lists
      // zoomable, Enter matching was the remaining split. Guarded on a live
      // selection so an empty list can't open a blank zoom.
      if (app.win.cols >= kPaneSplitMin) {
        open_detail();
      } else if (detail::selected_enrichment(app) != nullptr) {
        app.detail_origin = View::Browse;
        app.view = View::Detail;
        open_detail();
      }
      return;
    }
    if (app.view == View::History && app.pane == Pane::List) {
      // Narrow History (DESIGN §5.4a): no pane exists below kPaneSplitMin, so
      // Enter drills straight into the full-screen zoom; at/above the split
      // it moves to the in-pane detail (matches Browse's `l`).
      if (app.win.cols < kPaneSplitMin) {
        app.detail_origin = View::History;
        app.view = View::Detail;
      }
      open_detail();
      return;
    }
    if (app.view == View::Schedule) {
      // Schedule is list-only (no two-pane mode, P37) — Enter always drills
      // straight into the full-screen zoom, matching Discover's Enter above.
      // open_detail() fires BEFORE the view flips to Detail: its local
      // selected_show() lambda keys off app.view directly (no detail_origin
      // indirection), so the fetch must land while app.view is still
      // Schedule — flipping first would silently fall through to the
      // Browse-catalog branch instead.
      if (app.schedule.selected() != nullptr) {
        open_detail();
        app.detail_origin = View::Schedule;
        app.view = View::Detail;
      }
      return;
    }
    if (app.view == View::Detail && char_recs_section_shown(app) && app.char_recs.focused) {
      // P36 slice 3: Enter on the section (not the grid) promotes the
      // focused recommendation instead of playing. Gated on the section
      // actually rendering — a cramped frame must not Enter-promote a show
      // the user cannot see (P36 review).
      promote_recommendation();
      return;
    }
    if ((app.view == View::Browse && app.pane == Pane::Detail) ||
        (app.view == View::History && app.pane == Pane::Detail) || app.view == View::Detail) {
      if (app.episode.fetched && !app.episode.episodes.empty()) {
        play_focused_episode();
      }
      return;
    }
    return;
  }

  if (key.special == KeyEvent::Special::Tab) {
    // P36 slice 3: while the `c` section is visible, Tab swaps focus between
    // it and the episode grid — the zoom's only sub-focus, so there is no
    // Pane-style enum for it (app.hpp's CharactersRecsState::focused doc).
    // Only while the section actually renders: on a cramped frame Tab must
    // not focus (or strand focus on) an invisible section (P36 review).
    if (app.view == View::Detail) {
      if (char_recs_section_shown(app)) {
        app.char_recs.focused = !app.char_recs.focused;
      } else {
        app.char_recs.focused = false;
      }
    }
    return;
  }

  if (key.special == KeyEvent::Special::Escape) {
    if (app.view == View::Detail) { demote_zoom(); return; }
    if ((app.view == View::Browse || app.view == View::History) && app.pane == Pane::Detail) {
      app.pane = Pane::List;
      return;
    }
    // Esc = the previous page, once every inner
    // layer (modal/bar/zoom/pane, all handled above) has declined the key.
    // Ride the view's own letter through on_key so persist-on-leave and
    // reload-on-enter fire exactly as if the user had typed it, then clear
    // the slot the letter case re-armed: Esc walks back, never forward.
    if (app.view_back.has_value() && *app.view_back != app.view) {
      KeyEvent back;
      switch (*app.view_back) {
        case View::Browse: back.codepoint = U'B'; break;
        case View::History: back.codepoint = U'H'; break;
        case View::Discover: back.codepoint = U'D'; break;
        case View::Schedule: back.codepoint = U'C'; break;
        case View::Settings: back.codepoint = U'S'; break;
        case View::Detail: return;  // never stored (back_target unwraps it).
      }
      on_key(app, back);
      app.view_back = std::nullopt;
      return;
    }
    return;  // nothing to go back to: Esc no-op (q handles quit).
  }
}

// ===========================================================================
// on_mouse (P32, §9) — translate a decoded left press into the SAME state
// moves the keyboard makes: hit-test via the draw-side geometry helpers
// (views.hpp detail::), select directly (the cursor-move mirror), and
// synthesize the view's own key through on_key for anything richer (a top-bar
// tab is its view letter; double-click is Enter) so keyboard and mouse can
// never disagree on behavior. A wheel notch is the same aliasing one step
// further: it IS the view's Up/Down arrow (the P32 ledger's deferred wheel
// alias), synthesized through on_key wherever the pointer sits — so it moves
// whatever the arrows move, Settings rows included. Motion never reaches
// here — the decoder drops it.
// ===========================================================================

void on_mouse(App& app, const MouseEvent& m) {
  const bool wheel = m.kind == MouseEvent::Kind::WheelUp ||
                     m.kind == MouseEvent::Kind::WheelDown;
  // Beyond wheel, only left presses act; releases and other buttons are inert.
  if (!wheel && (m.kind != MouseEvent::Kind::Press ||
                 m.button != MouseEvent::Button::Left)) {
    return;
  }
  app.dirty = true;
  app.last_key_debug = (wheel ? "wheel " : "click ") + std::to_string(m.x) +
                       "," + std::to_string(m.y);
  debug_log("ui mouse: " + app.last_key_debug);

  // Gate order mirrors on_key. The connect modal swallows everything (esc/c
  // are its only controls); an armed delete-confirm reads a click as the
  // "anything else cancels" arm (DESIGN §6.5); the armed search bar owns
  // input (P32 ledger: mouse inert while `/` is live); the degraded frame
  // (04 §8) renders no content to hit.
  if (app.connect.has_value()) return;
  if (app.discover_filters.visible) return;  // P38: same swallow-everything tier as connect.
  if (app.confirm_delete.has_value()) {
    app.confirm_delete = std::nullopt;
    return;
  }
  if (app.bar == BarMode::Search) return;
  // The score prompt owns focus the same way (P34): a mid-prompt click could
  // silently retarget the commit — Enter re-reads the selected row — so the
  // mouse is inert until the prompt resolves.
  if (app.bar == BarMode::Score) return;
  if (app.win.rows < 4 || app.win.cols < 16) return;

  // Wheel = the view's Up/Down (position-blind, like the keys it aliases):
  // one notch, one arrow, through on_key so wheel and keyboard can never
  // disagree on what a step means. Dispatched before the double-click logic
  // so a notch between two clicks neither disarms nor doubles.
  if (wheel) {
    KeyEvent k;
    k.special = m.kind == MouseEvent::Kind::WheelUp ? KeyEvent::Special::Up
                                                    : KeyEvent::Special::Down;
    on_key(app, k);
    return;
  }

  // Double-click = the view's Enter (P32): a second press on the SAME cell
  // within the window, where the first press already selected the target.
  // last_click_tick stores tick+1 so 0 always means disarmed; it re-arms only
  // on a selectable hit, so a tab or dead-space click never doubles.
  const bool dbl = app.last_click_tick != 0 &&
                   app.tick_count + 1 - app.last_click_tick <= kDoubleClickTicks &&
                   m.x == app.last_click_x && m.y == app.last_click_y;
  app.last_click_tick = 0;  // consumed; re-armed below.
  auto arm_double = [&]() {
    app.last_click_tick = app.tick_count + 1;
    app.last_click_x = m.x;
    app.last_click_y = m.y;
  };
  auto synth_char = [&](char32_t c) {
    KeyEvent k;
    k.codepoint = c;
    on_key(app, k);
  };
  auto synth_enter = [&]() {
    KeyEvent k;
    k.special = KeyEvent::Special::Enter;
    on_key(app, k);
  };

  // Top bar: a tab click IS its view letter (persist-on-leave, the History
  // reload — everything rides the one keyboard path).
  if (m.y == 0) {
    if (const auto v = detail::top_bar_tab_at(m.x); v.has_value()) {
      switch (*v) {
        case View::Browse: synth_char(U'B'); break;
        case View::History: synth_char(U'H'); break;
        case View::Discover: synth_char(U'D'); break;
        case View::Schedule: synth_char(U'C'); break;
        case View::Settings: synth_char(U'S'); break;
        case View::Detail: break;  // never in the tab strip.
      }
    }
    return;
  }
  if (m.y >= static_cast<int>(app.win.rows) - 1) return;  // bottom bar: inert in v1.

  if (app.view == View::Settings) {
    if (app.settings.editing()) return;  // the edit buffer owns input.
    if (const auto row = detail::settings_row_at(app, m.x, m.y); row.has_value()) {
      const bool already = app.settings.cursor() == *row;
      app.settings.select(*row);
      if (dbl && already) {
        synth_enter();  // Enter: edit a Text row / fire Connect (05 §13 table).
        return;
      }
      arm_double();
    }
    return;
  }

  if (app.view == View::Discover) {
    if (const auto ax = detail::discover_axis_at(m.x, m.y); ax.has_value()) {
      synth_char(static_cast<char32_t>(U'1' + *ax));  // the 1-4 direct binds.
      return;
    }
    if (const auto card = detail::discover_card_at(app, m.x, m.y); card.has_value()) {
      const bool already = app.discover.slot().cursor == *card;
      app.discover.select(
          *card,
          grid_geo(app.win.cols, content_height(app.win.rows), app.cover_caps.cell));
      if (dbl && already) {
        synth_enter();  // Enter: the Detail zoom over the card.
        return;
      }
      arm_double();
    }
    return;
  }

  // Browse / History list columns: click selects (focus follows the click),
  // double-click is Enter (open the detail pane; drill into the zoom when
  // History is narrow — on_key's own Enter table decides).
  if (app.view == View::Browse) {
    if (const auto row = detail::browse_row_at(app, m.x, m.y); row.has_value()) {
      const bool already = app.list_cursor == *row && app.pane == Pane::List;
      app.list_cursor = *row;
      app.pane = Pane::List;
      browse_scroll_into_view(app);
      maybe_load_more(app);  // a click on the last result prefetches like j.
      if (dbl && already) {
        synth_enter();
        return;
      }
      arm_double();
      return;
    }
  }
  if (app.view == View::History) {
    if (const auto ord = detail::history_ord_at(app, m.x, m.y); ord.has_value()) {
      const bool already = app.history.cursor == *ord && app.pane == Pane::List;
      app.history.cursor = *ord;
      app.pane = Pane::List;
      if (dbl && already) {
        synth_enter();
        return;
      }
      arm_double();
      return;
    }
  }

  // The detail surface: an episode-cell click selects it, with focus
  // following into the pane — the grid is only hit-testable when it belongs
  // to the current selection (episode_cell_at's stale-grid gate), so no
  // engage is owed. Double-click plays it (Enter). A click elsewhere in a
  // two-pane column focuses the pane the way `l` does (which fires the
  // engage when needed).
  if (detail::detail_column_contains(app, m.x, m.y)) {
    if (const auto cell = detail::episode_cell_at(app, m.x, m.y); cell.has_value()) {
      const bool focused = app.view == View::Detail || app.pane == Pane::Detail;
      const bool already = focused && app.episode.cursor == static_cast<int>(*cell);
      if (app.view != View::Detail) app.pane = Pane::Detail;
      // Focus follows the click (the P32 rule everywhere): a grid click takes
      // the zoom's sub-focus back from the `c` section, or the double-click's
      // synthesized Enter would promote a recommendation instead of playing
      // the clicked cell (P36 review).
      app.char_recs.focused = false;
      app.episode.cursor = static_cast<int>(*cell);
      if (dbl && already) {
        synth_enter();
        return;
      }
      arm_double();
      return;
    }
    if (app.view != View::Detail && app.pane == Pane::List) synth_char(U'l');
    return;
  }
}

}  // namespace

void tick(App& app, const Event& ev) {
  std::visit(
      overloaded{
          [&](const KeyEvent& k) { on_key(app, k); },
          [&](const MouseEvent& m) { on_mouse(app, m); },
          [&](const Resize& r) {
            app.win.cols = r.cols;
            app.win.rows = r.rows;
            app.win.xpixel = r.xpixel;
            app.win.ypixel = r.ypixel;
            // Below the split there is only one column (DESIGN §7.3, app.rs
            // on_resize): a Detail focus would be invisible. The zoom keeps
            // its own full-screen surface.
            if (app.win.cols < kPaneSplitMin && app.view != View::Detail) {
              app.pane = Pane::List;
            }
            // The last_watched landing waits for the first real geometry:
            // only then is pane-vs-zoom decidable (05 §10.6, DESIGN §8.3).
            if (app.resume_pending && app.win.cols > 0) {
              app.resume_pending = false;
              open_resume_landing(app);
            }
            app.dirty = true;
          },
          [&](const Tick&) {
            ++app.tick_count;
            advance_spinner(app);
            app.toasts.tick(app.tick_count);
            // Schedule's countdown clock (P37): re-stamped every Tick (unlike
            // discover_cour's boot-only stamp) so the countdown visibly
            // counts down. Rebuild the grouped view only while it's on
            // screen and only on an actual second boundary — schedule()
            // re-sorts the whole watchlist, wasted work at 10 ticks/sec.
            const std::int64_t fresh_now = now_epoch_secs();
            if (app.view == View::Schedule && fresh_now != app.clock_epoch_secs) {
              app.clock_epoch_secs = fresh_now;
              app.schedule.rebuild(app.clock_epoch_secs);
              app.dirty = true;
            } else {
              app.clock_epoch_secs = fresh_now;
            }
            // The connect modal runs its own spinner/paste-hint clock off
            // elapsed ticks (draw_connect is pure, no clock of its own) — every
            // tick must repaint while the session is live, same as the
            // top-level spinner_phase above.
            if (app.connect.has_value()) app.dirty = true;
            // P15: pace the prewarm warm-walk (spawns the next candidate probe
            // once HOP_GAP elapses). Silent — no toast, no spinner.
            if (app.prewarm && app.prewarm->active()) {
              PrewarmDeps pd;
              if (app.deps != nullptr) {
                pd.store = app.deps->store;
                pd.registry = app.deps->registry;
              }
              pd.queue = app.queue;
              pd.translation = Translation::Sub;
              pd.unix_now = now_epoch_secs();
              pd.now_tick = app.tick_count;
              app.prewarm->tick(pd);
            }
            // Search debounce fire point (04 §8: 300ms after the last edited
            // keystroke). Fire only while still in search mode with deps to
            // fetch with; an empty query clears the list without a fetch.
            if (app.search_debounce_deadline != 0 &&
                app.tick_count >= app.search_debounce_deadline) {
              app.search_debounce_deadline = 0;
              if (app.search_query.empty()) {
                app.catalog.clear();
                app.list_cursor = 0;
                app.browse_scroll = 0;
                app.search_answered.clear();
                app.search_has_next = false;
                app.search_page = 1;
              } else if (app.deps != nullptr && app.deps->search &&
                        app.queue != nullptr) {
                app.search_inflight_query = app.search_query;
                spawn_search(*app.queue, app.deps->search, app.search_query,
                             /*page=*/1);
                recompute_loading(app);
              }
              app.dirty = true;
            }
            // Sync action-flush debounce fire point (ROD-291: 3s after the
            // last status/play edit). pull_only=false: this path always
            // pulls then pushes (06 §5.2).
            if (app.sync_flush_deadline != 0 && app.tick_count >= app.sync_flush_deadline) {
              app.sync_flush_deadline = 0;
              flush_sync(app, /*pull_only=*/false);
            }
            // Discover feed pump (P17, the tick_discover analog): while the
            // grid is up, ask the active slot what page it wants (empty →
            // page 1, near-end cursor → prefetch), arm loading, and spawn the
            // feed worker. A spawn failure resets the arm so the spinner never
            // strands. No cover pump this phase (P17a is placeholder-only).
            if (app.view == View::Discover && app.deps != nullptr &&
                app.deps->discover && app.queue != nullptr) {
              const GridGeo geo =
                  grid_geo(app.win.cols, content_height(app.win.rows),
                           app.cover_caps.cell);
              if (const auto page = app.discover.wanted_fetch(geo); page.has_value()) {
                const DiscoverAxis axis = app.discover.axis();
                app.discover.arm_loading(app.tick_count);
                spawn_discover_feed(*app.queue, app.deps->discover, axis, *page,
                                    app.discover.filters(), app.discover.filter_gen());
                recompute_loading(app);
                app.dirty = true;
              }
            }
            // Grid cover pump (P17b, §8): window = visible rows + 1 prefetch
            // row, in card order (row-major, matching the desired-set order
            // compose_grid_apc/draw_discover derive independently). Runs even
            // when placeholder mode (no image backend) is harmless — pump()
            // just never gets called there per the plan, so gate on it.
            if (app.view == View::Discover && app.cover_caps.images() &&
                app.deps != nullptr && app.deps->covers != nullptr &&
                app.queue != nullptr) {
              const GridGeo geo =
                  grid_geo(app.win.cols, content_height(app.win.rows),
                           app.cover_caps.cell);
              const AxisSlot& gslot = app.discover.slot();
              const std::size_t window_rows = geo.rows_visible + 1;
              const std::size_t window_end =
                  std::min(gslot.entries.size(),
                          (gslot.scroll_row + window_rows) * geo.cols);
              const std::size_t window_start =
                  std::min(gslot.scroll_row * geo.cols, window_end);
              std::vector<std::string> window;
              for (std::size_t i = window_start; i < window_end; ++i) {
                const Enrichment& e = gslot.entries[i].meta;
                if (e.cover_url.has_value() && !e.cover_url->empty()) {
                  window.push_back(*e.cover_url);
                }
              }
              auto urls = app.cover_pool.pump(
                  window, app.tick_count,
                  static_cast<std::size_t>(app.config.effective_cover_concurrency()),
                  static_cast<std::size_t>(app.grid_covers_inflight));
              app.cover_render_store.retain(app.cover_pool);
              const std::uint32_t cw =
                  app.cover_caps.cell.known() ? app.cover_caps.cell.width : kFallbackCellW;
              const std::uint32_t ch =
                  app.cover_caps.cell.known() ? app.cover_caps.cell.height : kFallbackCellH;
              const std::uint32_t box_w = static_cast<std::uint32_t>(geo.tier.cover_w) * cw;
              const std::uint32_t box_h = static_cast<std::uint32_t>(geo.cover_h) * ch;
              for (std::string& url : urls) {
                ++app.grid_covers_inflight;
                spawn_grid_cover(*app.queue, *app.deps->covers, url, box_w, box_h);
              }
              if (!urls.empty()) app.dirty = true;
            }
          },
          [&](const SearchDone& s) {
            // A result only "finishes" the in-flight worker whose query it
            // carries; a stale result must not mark a fresher worker as done.
            if (s.query == app.search_inflight_query) app.search_inflight_query.clear();
            // Staleness by query-string compare (04 §6 table: search keys on
            // the query, not a generation) — drop a result for an edited-away
            // query rather than repaint over what the user is now typing.
            if (s.query == app.search_query) {
              bool applied = false;
              if (s.page <= 1) {
                app.catalog = s.results;
                app.list_cursor = 0;
                app.browse_scroll = 0;
                app.search_answered = s.query;
                app.search_page = 1;
                app.search_has_next = s.has_next;
                applied = true;
              } else if (app.search_answered == s.query &&
                         s.page == app.search_page + 1) {
                // A later page appends and keeps the cursor (05 §16);
                // out-of-order pages drop.
                app.catalog.insert(app.catalog.end(), s.results.begin(),
                                   s.results.end());
                app.search_page = s.page;
                app.search_has_next = s.has_next;
                applied = true;
              }
              if (applied) {
                // Applied rows upsert catalog_cache best-effort (04 §10,
                // browse.rs on_done): the null-never-wipes merge and
                // detail-open-without-refetch ride this write.
                if (app.deps != nullptr && app.deps->store != nullptr) {
                  const std::int64_t now = now_epoch_secs();
                  for (const CatalogRow& row : s.results) {
                    const std::int64_t ttl = enrichment_ttl_secs(
                        row.meta.status.has_value()
                            ? std::optional<std::string_view>(*row.meta.status)
                            : std::nullopt);
                    (void)app.deps->store->upsert_catalog_cache(row.meta, now,
                                                                now + ttl);
                  }
                }
                // An applied answer is the AniList recovery signal (05 §14).
                app.toasts.clear_topic(kAnilistTopic);
              }
            }
            recompute_loading(app);
            app.dirty = true;
          },
          [&](const SearchError& e) {
            if (e.query == app.search_inflight_query) app.search_inflight_query.clear();
            if (e.query == app.search_query) {
              // Persistent under the AniList topic (05 §14, app.rs
              // on_search_failed): the singleton survives whispers and only
              // an applied answer (search or enrich) clears it.
              app.toasts.push_persistent(ToastKind::Error, std::string(kAnilistTopic),
                                         "can't reach AniList", app.tick_count);
            }
            recompute_loading(app);
            app.dirty = true;
          },
          [&](const DiscoverFeedDone& d) {
            // File the page into its axis slot (out-of-order pages self-discard
            // in on_feed) + best-effort catalog_cache upsert. The store is
            // nullable (demo / no-persist) — the feed still renders.
            Store* store = app.deps != nullptr ? app.deps->store : nullptr;
            app.discover.on_feed(d.axis, d.page, d.rows, d.has_next, store, now_epoch_secs(),
                                 d.gen);
            recompute_loading(app);
            app.dirty = true;
          },
          [&](const DiscoverFeedError& d) {
            // Sticky failed state (discover.rs on_feed_error): cleared only by
            // an axis-key retry, never an auto-refetch (that would storm the
            // API). Silent — the grid's empty-state renders the error copy.
            app.discover.on_feed_error(d.axis, d.cause, d.gen);
            recompute_loading(app);
            app.dirty = true;
          },
          [&](const GridCoverDone& g) {
            // Adopt by url (04 §4.4): a result for a scrolled-away or even
            // evicted url is still adopted, recreating its slot. install()
            // assigns/reuses the id; compose_grid_apc picks it up next paint.
            if (app.grid_covers_inflight > 0) --app.grid_covers_inflight;
            app.cover_render_store.install(g.url, g.pixels);
            app.cover_pool.adopt(g.url);
            debug_log("ui GridCoverDone: adopted url=" + g.url);
            app.dirty = true;
          },
          [&](const GridCoverError& g) {
            if (app.grid_covers_inflight > 0) --app.grid_covers_inflight;
            app.cover_pool.note_failure(g.url, app.tick_count);
            debug_log("ui GridCoverError: cooldown url=" + g.url);
            app.dirty = true;
          },
          [&](const EpisodesDone& e) {
            // P15: the transport owns the walk (staleness, store writes, walk
            // advance). It mirrors its grid into App.episode afterward.
            if (app.episode_session) {
              EpisodeDeps ed = build_episode_deps(app);
              auto fb = app.episode_session->on_episodes_done(e, ed);
              apply_feedback(app, fb);
              ride_episode_feedback(app, e.for_id, fb);
              return;
            }
            // Degraded P7 path (no session): mirror the first-hit result.
            if (app.episode_gen.is_current(e.gen) && e.for_id == app.episode.for_id) {
              app.episode.loading = false;
              app.episode.fetched = true;
              app.episode.episodes = e.episodes;
              app.episode.provider_id = e.provider_id;
              app.episode.no_source = e.episodes.empty();
              app.episode.cursor = 0;
              if (app.deps != nullptr && app.deps->store != nullptr) {
                for (const CatalogRow& r : app.catalog) {
                  if (r.meta.anilist_id != e.for_id) continue;
                  const Enrichment& m = r.meta;
                  (void)app.deps->store->ensure_show(
                      m.anilist_id, m.title_romaji, m.mal_id, m.total_episodes,
                      m.status.has_value()
                          ? std::optional<std::string_view>(*m.status)
                          : std::nullopt);
                  break;
                }
                refresh_watched_marks(app);
              }
            }
            recompute_loading(app);
            app.dirty = true;
          },
          [&](const EpisodesError& e) {
            if (app.episode_session) {
              EpisodeDeps ed = build_episode_deps(app);
              auto fb = app.episode_session->on_episodes_error(e, ed);
              apply_feedback(app, fb);
              ride_episode_feedback(app, e.for_id, fb);
              return;
            }
            if (app.episode_gen.is_current(e.gen) && e.for_id == app.episode.for_id) {
              app.episode.loading = false;
              app.episode.fetched = true;
              app.episode.no_source = true;
              const std::string_view provider =
                  (app.deps != nullptr && app.deps->registry != nullptr &&
                  app.deps->registry->primary() != nullptr)
                      ? app.deps->registry->primary()->display_name()
                      : std::string_view{"provider"};
              const auto [kind, copy] = detail::provider_error_toast(e.cause, provider);
              app.toasts.push(kind, copy, app.tick_count);
            }
            recompute_loading(app);
            app.dirty = true;
          },
          [&](const ProviderSearchDone& e) {
            // Tier-C search hits → score → fetch (or advance the walk on miss).
            // Only the transport produces these; with no session, drop.
            if (app.episode_session) {
              EpisodeDeps ed = build_episode_deps(app);
              auto fb = app.episode_session->on_search_done(e, ed);
              apply_feedback(app, fb);
              ride_episode_feedback(app, e.for_id, fb);
            }
            app.dirty = true;
          },
          [&](const ProviderSearchError& e) {
            if (app.episode_session) {
              EpisodeDeps ed = build_episode_deps(app);
              auto fb = app.episode_session->on_search_error(e, ed);
              apply_feedback(app, fb);
              ride_episode_feedback(app, e.for_id, fb);
            }
            app.dirty = true;
          },
          [&](const PrewarmResult& r) {
            // A background sibling-warm probe landed (05 §10.4). Mint into the
            // store; if a row changed and it's the open show, refresh the rail.
            if (app.prewarm) {
              PrewarmDeps pd;
              if (app.deps != nullptr) {
                pd.store = app.deps->store;
                pd.registry = app.deps->registry;
              }
              pd.queue = app.queue;
              pd.translation = Translation::Sub;
              pd.unix_now = now_epoch_secs();
              pd.now_tick = app.tick_count;
              const bool changed = app.prewarm->on_result(r, pd);
              if (changed && app.episode_session) {
                EpisodeDeps ed = build_episode_deps(app);
                app.episode_session->on_availability_write(r.anilist_id, ed);
                mirror_session_to_episode(app);
              }
            }
            app.dirty = true;
          },
          [&](const CoverDone& c) {
            // Staleness (04 §6): drop a result whose generation was superseded
            // (a newer selection bumped cover_gen) — CoverState::on_done also
            // guards by for_id, so a late result for a browsed-away show leaves
            // state untouched and the pixels are dropped.
            const bool fresh =
                app.cover_gen.is_current(c.gen) && app.cover.on_done(c.for_id);
            debug_log("ui CoverDone: fresh=" + std::to_string(fresh));
            if (fresh) {
              app.cover_render.pixels = c.pixels;
              app.cover_render.for_id = c.for_id;
              app.cover_render.have_pixels = true;
            }
            // else: stale/wrong-id — the decoded pixels fall out of scope here.
            app.dirty = true;
          },
          [&](const CoverError& c) {
            // Records the id+url cooldown inside CoverState (10s per id+url);
            // a stale generation or wrong id is dropped untouched. The failed
            // fetch is silent (a placeholder box renders — no user toast, A4).
            debug_log("ui CoverError: gen_fresh=" +
                      std::to_string(app.cover_gen.is_current(c.gen)));
            if (app.cover_gen.is_current(c.gen)) {
              app.cover.on_error(c.for_id, app.tick_count);
            }
            app.dirty = true;
          },
          [&](const PositionUpdate& p) {
            // Match against the app-global PlayState (04 §7.7), not the open
            // grid — the user may have browsed to another show while mpv is
            // still up; positions keep landing on the play they belong to.
            if (app.play.active && p.for_id == app.play.for_id &&
                p.episode == app.play.episode) {
              app.play.position = p.position;
              app.play.duration = p.duration;
              // Checkpoint every 30s of playback (04 §7.7), UI-thread (A7). Only
              // a meaningful position persists (finite > 0, A6) — otherwise a
              // pre-roll 0 would overwrite a good resume point. save_progress is
              // the plain partial write: no play_count/ratchet (that is the
              // final record_finish). The store rejects non-finite loud, so
              // gate here rather than hand it a NaN.
              if (app.deps != nullptr && app.deps->store != nullptr &&
                  std::isfinite(p.position) && p.position > 0.0 &&
                  (app.tick_count - app.play.last_checkpoint_tick) >= kCheckpointTicks) {
                app.play.last_checkpoint_tick = app.tick_count;
                // Stamp the SERVING provider (P15), not primary() — the resume/
                // History row's owning provider must match the grid the play
                // came from (03 §3.2), so P16's open-on-owner opens the right one.
                const std::string_view provider = play_provider_name(app);
                (void)app.deps->store->save_progress(
                    app.play.for_id, app.play.translation, app.play.episode,
                    p.position, p.duration,
                    provider.empty() ? std::nullopt
                                     : std::optional<std::string_view>(provider),
                    now_epoch_secs());
              }
              // P15: mpv is up (a real position landed) — fire the sibling-warm
              // walk (05 §10.4). Deliberately here, not at play-fire, to keep the
              // warm off the CDN during play's own resolve. The prewarm gates and
              // 30s spacing make repeat positions a silent no-op.
              if (app.prewarm && app.episode_session &&
                  app.episode_session->has_for_id() &&
                  app.episode_session->for_id() == p.for_id &&
                  std::isfinite(p.position) && p.position > 0.0) {
                if (const Enrichment* meta = [&]() -> const Enrichment* {
                      for (const CatalogRow& r : app.catalog) {
                        if (r.meta.anilist_id == p.for_id) return &r.meta;
                      }
                      return nullptr;
                    }()) {
                  PrewarmGates gates;
                  gates.play_resolving = app.episode_session->loading();
                  gates.fallback_active = false;  // no live walk once playing.
                  PrewarmDeps pd;
                  if (app.deps != nullptr) {
                    pd.store = app.deps->store;
                    pd.registry = app.deps->registry;
                  }
                  pd.queue = app.queue;
                  pd.translation = Translation::Sub;
                  pd.unix_now = now_epoch_secs();
                  pd.now_tick = app.tick_count;
                  app.prewarm->fire(*meta, gates, pd);
                }
              }
            }
            app.dirty = true;
          },
          [&](const PlayDone& p) {
            debug_log("ui PlayDone: pos=" + std::to_string(p.final_position) +
                      " match=" +
                      std::to_string(app.play.active && p.for_id == app.play.for_id &&
                                     p.episode == app.play.episode));
            if (app.play.active && p.for_id == app.play.for_id &&
                p.episode == app.play.episode) {
              app.play.active = false;
              // A clean finish retires any armed walk continuation (03 §6.4,
              // playback.rs on_finished's None-failure arm).
              app.play_continuation.reset();
              // Persist the finish (02 §4b): record_finish saves the resume row
              // AND ratchets progress / marks fully_watched atomically, deriving
              // the 0.80/0.95 thresholds internally from position/duration (the
              // single authority). Gate on a meaningful position (finite > 0,
              // A6) so a failed-before-playback finish writes nothing. The
              // duration comes from the last PositionUpdate we saw (PlayDone
              // carries only the position). UI-thread (A7).
              if (app.deps != nullptr && app.deps->store != nullptr &&
                  std::isfinite(p.final_position) && p.final_position > 0.0 &&
                  app.play.episode_index != 0) {
                // The serving provider (P15), not primary() — see save_progress.
                const std::string_view provider = play_provider_name(app);
                (void)app.deps->store->record_finish(
                    p.for_id, app.play.translation, p.episode,
                    app.play.episode_index, p.final_position, app.play.duration,
                    provider.empty() ? std::nullopt
                                     : std::optional<std::string_view>(provider),
                    now_epoch_secs());
                // The grid's watched marks may have moved; repaint them if the
                // finished show is still the one open.
                if (app.episode.for_id == p.for_id) refresh_watched_marks(app);
                // P15: let the transport re-derive watched/resume and advance
                // the cursor past a completed cell (on_play_recorded). Then
                // mirror the (possibly moved) cursor back into App.episode.
                if (app.episode_session) {
                  const bool completed = natural_end(p.final_position, app.play.duration);
                  EpisodeDeps ed = build_episode_deps(app);
                  app.episode_session->on_play_recorded(
                      p.for_id, app.play.episode_index, completed, ed);
                  if (app.episode_session->has_for_id() &&
                      app.episode_session->for_id() == p.for_id) {
                    app.episode.cursor = app.episode_session->cursor();
                  }
                }
                arm_sync(app);
              }
              // Toast (05 §14): natural end (>= 0.80) counts as a full watch;
              // "all caught up" only when the finished ep is the grid's last and
              // that show is still open (the grid is otherwise untrustworthy).
              const bool same_show_open =
                  app.episode.fetched && app.episode.for_id == p.for_id;
              const bool finale = same_show_open &&
                                  !app.episode.episodes.empty() &&
                                  app.episode.episodes.back() == p.episode;
              const bool completed = natural_end(p.final_position, app.play.duration);
              if (completed) {
                app.toasts.push(ToastKind::Success,
                                finale ? "all caught up" : "episode done",
                                app.tick_count);
              }
            }
            app.dirty = true;
          },
          [&](const PlayErrorEvent& e) {
            debug_log("ui PlayError: kind=" +
                      std::to_string(static_cast<int>(e.cause.kind)));
            if (app.play.active && e.for_id == app.play.for_id &&
                e.episode == app.play.episode) {
              app.play.active = false;
              const std::string_view provider =
                  (app.deps != nullptr && app.deps->registry != nullptr &&
                  app.deps->registry->primary() != nullptr)
                      ? app.deps->registry->primary()->display_name()
                      : std::string_view{"provider"};
              const auto [kind, copy] = detail::play_error_toast(e.cause, provider);
              app.toasts.push(kind, copy, app.tick_count);
              // Play-fallback hop (03 §6.4, playback.rs hop_eligible +
              // app.rs on_play_hop): a source-side failure (resolve fail /
              // stream never opened past the retry budget) asks the episode
              // session to fail over past every provider this continuation
              // already burned, then relaunches when a sibling grid lands.
              // Player-side failures (mpv missing/crashed) stop it: no
              // sibling fixes a broken player.
              const bool hop_eligible =
                  e.cause.kind == PlayError::Kind::Resolve ||
                  e.cause.kind == PlayError::Kind::OpenFailed;
              if (hop_eligible && app.episode_session &&
                  app.episode_session->has_for_id() &&
                  app.episode_session->for_id() == e.for_id &&
                  !app.play.provider.empty()) {
                // Grow (or start) the continuation with the failed provider.
                if (!app.play_continuation.has_value() ||
                    app.play_continuation->for_id != e.for_id) {
                  App::PlayContinuation c;
                  c.for_id = e.for_id;
                  c.label = app.play.episode;
                  c.ordinal = app.play.episode_index;
                  c.translation = app.play.translation;
                  c.mal_id = app.play.mal_id;
                  c.title_romaji = app.play.title_romaji;
                  app.play_continuation = std::move(c);
                }
                auto& tried = app.play_continuation->tried;
                if (std::find(tried.begin(), tried.end(), app.play.provider) ==
                    tried.end()) {
                  tried.push_back(app.play.provider);
                }
                // The session must still serve the provider that failed (the
                // user may have re-routed mid-play) — else nothing to rescue.
                if (app.episode_session->serving().has_value() &&
                    *app.episode_session->serving() == app.play.provider) {
                  EpisodeDeps ed = build_episode_deps(app);
                  auto fb = app.episode_session->play_fail_over(
                      tried, app.play_continuation->label,
                      app.play_continuation->ordinal, ed);
                  apply_feedback(app, fb);
                  // A cache-hit hop lands synchronously with no episode event
                  // to ride: give the continuation its consume chance now.
                  maybe_continue_play(app);
                } else {
                  app.play_continuation.reset();
                }
              } else {
                app.play_continuation.reset();
              }
            }
            app.dirty = true;
          },
          [&](const PlayRetry& r) {
            std::string copy = "stream didn't open · retrying ";
            copy += std::to_string(r.attempt);
            copy += "/";
            copy += std::to_string(r.max);
            app.toasts.push(ToastKind::Warning, copy, app.tick_count);
            app.dirty = true;
          },
          [&](const ConnectDone& d) {
            on_connect_result(app, d);
            app.dirty = true;
          },
          [&](const MalConnectDone& d) {
            on_mal_connect_result(app, d);
            app.dirty = true;
          },
          [&](const SyncFlushed& s) {
            on_sync_flushed(app, s);
            app.dirty = true;
          },
          [&](const EnrichmentRefreshed& e) { on_enrichment_refreshed(app, e); },
          [&](const EnrichmentNull& e) { on_enrichment_null(app, e); },
          [&](const EnrichmentFailed&) { on_enrichment_failed(app); },
          [&](const CharactersRecsDone& e) { on_char_recs_done(app, e); },
          [&](const CharactersRecsNull& e) { on_char_recs_null(app, e); },
          [&](const CharactersRecsFailed& e) { on_char_recs_failed(app, e); },
          [&](const GenreCollectionDone& e) { on_genre_collection_done(app, e); },
          [&](const GenreCollectionFailed&) { on_genre_collection_failed(app); },
          [&](const UpdateAvailable& u) {
            on_update_available(app, u);
            app.dirty = true;
          },
          // --- Downloads (P35 slice 3) ---------------------------------------
          [&](const DownloadProgress& p) {
            if (app.download.active && p.for_id == app.download.for_id &&
                p.episode == app.download.episode) {
              app.download.bytes = p.bytes;
              app.download.total = p.total;
              app.dirty = true;
            }
          },
          [&](const DownloadDone& d) {
            if (app.download.active && d.for_id == app.download.for_id &&
                d.episode == app.download.episode) {
              app.download.active = false;
              app.toasts.push(ToastKind::Success, "downloaded ep " + d.episode,
                              app.tick_count);
              app.dirty = true;
            }
          },
          [&](const DownloadFailed& f) {
            if (!app.download.active || f.for_id != app.download.for_id ||
                f.episode != app.download.episode) {
              return;
            }
            app.download.active = false;
            // The toast matrix over the closed outcome set (A2: exhaustive, a
            // new kind forces a copy decision here).
            switch (f.outcome) {
              case DownloadOutcome::Fetch:
                app.toasts.push(ToastKind::Error, "download failed · " + f.detail,
                                app.tick_count);
                break;
              case DownloadOutcome::Io:
                app.toasts.push(ToastKind::Error, "download failed · disk error",
                                app.tick_count);
                break;
              case DownloadOutcome::Cancelled:
                app.toasts.push(ToastKind::Info, "download cancelled", app.tick_count);
                break;
              case DownloadOutcome::UnsafeUrl:
                app.toasts.push(ToastKind::Error, "download blocked · unsafe url",
                                app.tick_count);
                break;
              case DownloadOutcome::UnsafeArg:
                app.toasts.push(ToastKind::Error,
                                "download blocked · unsafe stream field",
                                app.tick_count);
                break;
              case DownloadOutcome::FfmpegNotFound:
                app.toasts.push(ToastKind::Warning,
                                "install ffmpeg to download this stream",
                                app.tick_count);
                break;
              case DownloadOutcome::Ffmpeg:
                app.toasts.push(ToastKind::Error, "download failed · ffmpeg " + f.detail,
                                app.tick_count);
                break;
            }
            app.dirty = true;
          },
      },
      ev);

  // After every event, reconcile the detail cover against the (possibly new)
  // selection (05 §12). One place, so no keybind/handler has to remember to do
  // it — a selection move, a results landing, a view switch, or a cooldown
  // expiry (via Tick) all route through here. The decision is pure and spawns
  // at most one worker; UpToDate/None are the steady-state no-ops.
  reconcile_cover(app);
  // Same one-place discipline for refresh-on-view enrichment (P21, 04 §10):
  // spends the network only on a stale/missing show, single-flighted, and
  // independent of covers.
  reconcile_enrich(app);
  // Same one-place discipline for the detail zoom's `c` section (P36): a
  // no-op unless the section is toggled visible.
  reconcile_char_recs(app);
}

// ===========================================================================
// draw(App) — PURE. Chrome + empty Browse + spinner + toasts, or degraded.
// ===========================================================================

namespace {

const char* view_label(View v) {
  switch (v) {
    case View::Browse: return "Browse";
    case View::History: return "History";
    case View::Discover: return "Discover";
    case View::Settings: return "Settings";
    case View::Schedule: return "Calendar";  // P37; label starts with its key letter, 'C'.
    case View::Detail: return "Detail";  // never in the tab strip (views[]).
  }
  return "";  // unreachable.
}

char view_key(View v) {
  switch (v) {
    case View::Browse: return 'B';
    case View::History: return 'H';
    case View::Discover: return 'D';
    case View::Settings: return 'S';
    case View::Schedule: return 'C';
    case View::Detail: return '?';  // never in the tab strip (views[]).
  }
  return '?';  // unreachable.
}

// Top bar (DESIGN §3.4): "SABIGOKU ░ [B]rowse · [H]istory · …  <chip>" plus the
// right-aligned focus `·`. shigoku renders its own name.
void draw_top_bar(const App& app, CellBuffer& buf) {
  const int w = buf.width();
  buf.fill(Rect{0, 0, w, 1}, theme::bg);
  int x = 1;  // 1-cell left padding within the bar.
  x = buf.put_str(x, 0, "SHIGOKU", theme::fg, theme::bg, Style::Bold);
  x = buf.put_str(x + 1, 0, "\xE2\x96\x91", theme::chrome, theme::bg);  // ░
  x += 1;

  const View views[] = {View::Browse, View::History, View::Discover,
                        View::Schedule, View::Settings};
  bool first = true;
  for (View v : views) {
    if (!first) x = buf.put_str(x, 0, " \xC2\xB7 ", theme::fg3, theme::bg);  // ·
    first = false;
    const bool active = (v == app.view);
    const Rgb col = active ? theme::focus : theme::fg2;
    const Style st = active ? Style::Bold : Style::None;
    // "[B]rowse": the bracket wraps the key letter, the label's remaining
    // letters follow (DESIGN §3.4). view_label starts with the key letter.
    std::string tab = "[";
    tab.push_back(view_key(v));
    tab += "]";
    tab += (view_label(v) + 1);  // label without its leading (key) letter.
    x = buf.put_str(x, 0, tab, col, theme::bg, st);
  }

  // Season chip (05 §7 topBarSeasonChip, app.rs top_bar): the selected card's
  // own season/year; Browse/History fall back to the ambient cour, Discover
  // and the zoom stay absent-not-empty, Settings shows none. Drawn only when
  // it clears the fixed right-edge dot. discover_cour is run()'s boot stamp
  // (draw is pure, no clock; the cour moves once a season).
  if (app.view != View::Settings) {
    const bool kanji = app.config.kanji_chips;
    const Enrichment* sel = detail::selected_enrichment(app);
    std::optional<std::string> chip =
        sel != nullptr ? season_chip(sel->season, sel->year, kanji) : std::nullopt;
    if (!chip.has_value() && (app.view == View::Browse || app.view == View::History)) {
      chip = cour_chip(app.discover_cour, kanji);
    }
    if (chip.has_value() && x + 2 + str_width(*chip) <= w - 2) {
      buf.put_str(x + 2, 0, *chip, theme::fg2, theme::bg);
    }
  }

  // Right-aligned active-pane focus indicator (·) in focus color.
  buf.put_str(w - 2, 0, "\xC2\xB7", theme::focus, theme::bg);
}

// Bottom bar (DESIGN §3.5 / §4.9 / §7.5): magenta ▌ + idle help, or the `/`
// search prompt + live query + result count.
void draw_bottom_bar(const App& app, CellBuffer& buf) {
  const int w = buf.width();
  const int y = buf.height() - 1;
  buf.fill(Rect{0, y, w, 1}, theme::bg);

  if (app.confirm_delete.has_value()) {
    // Hard-delete confirm prompt (DESIGN §6.5/§4.2): "[!]" replaces the idle
    // "▌", title tail-truncated so "y confirm · esc cancel" never scrolls off.
    int x = 2;
    x = buf.put_str(x, y, "[!] delete \"", theme::warn, theme::bg, Style::Bold);
    const std::string title = [&]() -> std::string {
      const Show* s = app.history.selected();
      if (s == nullptr) return {};
      const Enrichment& e = s->enrichment;
      return std::string(preferred_title(
          e.title_romaji,
          e.title_english ? std::optional<std::string_view>(*e.title_english) : std::nullopt,
          e.title_native ? std::optional<std::string_view>(*e.title_native) : std::nullopt,
          parse_title_language(app.config.title_language)));
    }();
    x = buf.put_str(x, y, truncate_to_cols(title, 48), theme::warn, theme::bg, Style::Bold);
    x = buf.put_str(x, y, "\"? episode history gone \xC2\xB7 ", theme::fg3, theme::bg);
    x = buf.put_str(x, y, "y", theme::focus, theme::bg, Style::Bold);
    x = buf.put_str(x, y, " confirm \xC2\xB7 ", theme::fg3, theme::bg);
    x = buf.put_str(x, y, "esc", theme::focus, theme::bg, Style::Bold);
    buf.put_str(x, y, " cancel", theme::fg3, theme::bg);
    return;
  }

  if (app.bar == BarMode::Search) {
    // "  /  query_                              [n results]"
    const bool in_history = app.view == View::History;
    const std::string& query = in_history ? app.history.filter : app.search_query;
    int x = 2;
    x = buf.put_str(x, y, "/", theme::focus, theme::bg, Style::Bold);
    x += 2;
    x = buf.put_str(x, y, query, theme::fg, theme::bg, Style::Bold);
    x = buf.put_str(x, y, "_", theme::focus, theme::bg);
    if (in_history) {
      const std::string count = "[" + std::to_string(app.history.count()) + "]";
      buf.put_str(w - 1 - str_width(count), y, count, theme::fg2, theme::bg);
    }
    return;
  }

  if (app.bar == BarMode::Score) {
    // "  s  7.5_                      0-10, decimals · enter save · esc cancel"
    int x = 2;
    x = buf.put_str(x, y, "s", theme::focus, theme::bg, Style::Bold);
    x += 2;
    x = buf.put_str(x, y, app.score_input, theme::fg, theme::bg, Style::Bold);
    x = buf.put_str(x, y, "_", theme::focus, theme::bg);
    const std::string hint = "0-10, decimals \xC2\xB7 enter save \xC2\xB7 esc cancel";
    buf.put_str(w - 1 - str_width(hint), y, hint, theme::fg3, theme::bg);
    return;
  }

  // Idle help line: ▌ (hot) + spaced help text; keybinds fg2+bold, words fg3.
  int x = 2;
  x = buf.put_str(x, y, "\xE2\x96\x8C", theme::hot, theme::bg);  // ▌
  x += 2;
  if (app.any_loading) {
    // Bottom-bar spinner prefix during any in-flight async (DESIGN §4.8).
    // "Slow" is measured from when THIS loading burst started, not app
    // uptime (loading_since_tick is stamped on the false->true edge).
    const Rgb sp = (app.tick_count - app.loading_since_tick) > kSlowSpinnerTicks
                       ? theme::hot
                       : theme::focus;
    x = buf.put_str(x, y, kSpinnerFrames[app.spinner_phase], sp, theme::bg);
    x += 1;
  }
  // Plain help text (segment styling is a P7 nicety; P6 renders it readable).
  // Settings edit mode (P18, DESIGN §7.5) swaps to its own two-state line —
  // idle_help(View, Pane) has no editing flag, so it's special-cased here
  // rather than growing that helper's signature for one view.
  const std::string help = (app.view == View::Settings && app.settings.editing())
                                ? "type to edit \xC2\xB7 enter confirm \xC2\xB7 esc cancel"
                                : detail::idle_help(app.view, app.pane);
  buf.put_str(x, y, help, theme::fg3, theme::bg);
}

// Toasts (DESIGN §4.7): right-aligned, stacked upward from row height-2.
void draw_toasts(const App& app, CellBuffer& buf) {
  const std::vector<Toast> vis = app.toasts.visible();
  if (vis.empty()) return;
  const int w = buf.width();
  int row = buf.height() - 2;  // one above the bottom bar.
  // Render newest nearest the bar: vis is oldest-first, so iterate reversed.
  for (auto it = vis.rbegin(); it != vis.rend() && row >= 1; ++it, --row) {
    const Toast& t = *it;
    const std::string glyph(toast_glyph(t.kind));
    const std::string copy =
        truncate_to_cols(t.text, kToastMaxCopyCols);
    const std::string box = glyph + copy;
    const int bw = str_width(box);
    const int x = (w - 1 - bw > 0) ? (w - 1 - bw) : 0;
    // Fill the box background (elevated) then the text.
    buf.fill(Rect{x, row, bw, 1}, theme::elevated);
    const Style st = toast_bold(t.kind) ? Style::Bold : Style::None;
    buf.put_str(x, row, box, toast_fg(t.kind), theme::elevated, st);
  }
}

// The degraded frame (04 §8): h<4 || w<16 -> a single centered message, never
// an exit.
void draw_degraded(CellBuffer& buf) {
  buf.clear(theme::bg);
  const int w = buf.width();
  const int h = buf.height();
  if (h <= 0 || w <= 0) return;
  // Pick the longest message that fits; "too small" fits the w<16 floor, the
  // full copy shows once there's room (a resize back up re-widens it).
  const char* full = "terminal too small";
  const char* msg = (str_width(full) <= w) ? full : "too small";
  const int mw = str_width(msg);
  const int x = (w - mw) / 2 > 0 ? (w - mw) / 2 : 0;
  const int y = h / 2;
  buf.put_str(x, y, msg, theme::warn, theme::bg);
}

}  // namespace

namespace detail {

std::optional<View> top_bar_tab_at(int x) {
  // The same span walk draw_top_bar runs — name, glyph, spacing, then the
  // four tabs separated by " · " — built from the same view_label/view_key
  // sources, so a renamed tab moves the render and the hit-test together.
  // draw: x=1 + "SHIGOKU" → 8; ░ put at x+1 (9, 1 col) → 10; x += 1 → 11.
  int cx = 1 + str_width("SHIGOKU") + 3;
  const View views[] = {View::Browse, View::History, View::Discover,
                        View::Schedule, View::Settings};
  bool first = true;
  for (View v : views) {
    if (!first) cx += 3;  // " · " separator.
    first = false;
    std::string tab = "[";
    tab.push_back(view_key(v));
    tab += "]";
    tab += (view_label(v) + 1);
    const int tw = str_width(tab);
    if (x >= cx && x < cx + tw) return v;
    cx += tw;
  }
  return std::nullopt;
}

std::string idle_help(View v, Pane p) {
  // DESIGN §7.5 subset for the views v0 implements: Browse/History (both
  // panes) and the Detail zoom are exact copy; Discover/Settings stay minimal
  // stand-ins (their full two-pane/grid render is not built until later).
  switch (v) {
    case View::Browse:
      // "hjkl grid", not "scroll" (KEYS_AUDIT F7): since P33 hjkl navigates
      // the episode grid on the detail surfaces — j/k hop rows, h/l step.
      if (p == Pane::Detail)
        return "hjkl grid · esc back · enter play · v provider · space zoom · q quit";
      return "hjkl · / find anime · space zoom · P save · q quit";
    case View::History:
      if (p == Pane::Detail)
        return "hjkl grid · esc back · enter play · v provider · space zoom · q quit";
      return "jk move · / filter · l/enter detail · p/x/c/w/P status · s score · X delete · r/u reset/undo · q quit";
    case View::Discover:
      // "P save" (KEYS_AUDIT F8): add-to-watchlist worked here all along but
      // was advertised only on Browse's line.
      return "hjkl · enter open · space zoom · f filter · P save · q quit";
    case View::Settings:
      return "jk move · hl cycle · space toggle · enter edit · q save+quit";
    case View::Schedule:
      return "jk move · enter detail · q quit";
    case View::Detail:
      // §7.5 base copy + the §9 keys that only exist here: `c` characters/
      // recommendations (P36), `d` download (P35). A key the bar never names
      // may as well not exist (the P36 section went unnoticed on hardware).
      return "hjkl grid · enter play · v provider · c cast · d download · space/esc back";
  }
  return "q quit";  // unreachable.
}

// Poll `done` at a 20ms granularity until it flips true or `timeout` elapses.
// Returns whether it finished (false = the deadline hit first). Extracted so
// the quit-under-load contract (04 §11: "a hung endpoint cannot wedge quit")
// is directly unit-testable without driving the whole terminal lifecycle.
bool bounded_wait(std::atomic<bool>& done, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

}  // namespace detail

void draw(const App& app, CellBuffer& buf) {
  buf.resize(app.win.cols, app.win.rows);

  if (buf.height() < 4 || buf.width() < 16) {
    draw_degraded(buf);
    return;
  }

  buf.clear(theme::bg);
  draw_top_bar(app, buf);

  // Content region (rows kContentY0..height-2, below the top bar + spacer
  // row): Browse/History's two-pane list/detail, the Discover card grid, the
  // Settings row list, or the Detail zoom's full-canvas render (views.cpp,
  // pure).
  const int y0 = kContentY0;
  const int y1 = buf.height() - 1;
  if (app.view == View::Browse) {
    draw_browse(app, buf, y0, y1);
  } else if (app.view == View::History) {
    draw_history(app, buf, y0, y1);
  } else if (app.view == View::Discover) {
    draw_discover(app, buf, y0, y1);
  } else if (app.view == View::Settings) {
    draw_settings(app, buf, y0, y1);
  } else if (app.view == View::Detail) {
    draw_detail_zoom(app, buf, y0, y1);
  } else if (app.view == View::Schedule) {
    draw_schedule(app, buf, y0, y1);
  }

  // Cover placement (P8, A4). The rect is derived from the live geometry so
  // draw() and run() agree. When the backend holds live pixels for the open show, the
  // rect is IMAGE-OWNED: mark the exclusion so the diff never paints into the
  // live fragments (A4 cell-ownership rule), and run() transmits after the diff.
  // Otherwise (placeholder mode, loading, suppressed, or a foreign/empty cover)
  // draw the dim box in-band and DO NOT exclude — normal cells render there.
  //
  // Discover already set its own exclusions (draw_discover, P17b, one rect per
  // placed grid cover) — cover_rect(app) is always empty there (views.cpp: "no
  // detail column in Discover"), so this block must not touch the exclusion set
  // and clobber the grid's.
  if (app.view != View::Discover) {
    const Rect crect = detail::cover_rect(app);
    const std::optional<detail::CoverTarget> ctarget = detail::cover_target(app);
    const bool image_owned =
        app.cover_caps.images() && app.cover_render.have_pixels && !crect.empty() &&
        ctarget.has_value() && ctarget->id == app.cover_render.for_id;
    if (image_owned) {
      buf.set_exclusion(crect);
    } else {
      buf.set_exclusion(Rect{});
      if (!crect.empty()) {
        // Placeholder / loading box (DESIGN §3.3): a surface-fill block, a
        // centered spinner while loading, else a dim frame holding the slot.
        buf.fill(crect, theme::surface);
        const bool loading = app.cover.is_loading();
        if (loading) {
          const char* sp = kSpinnerFrames[app.spinner_phase];
          buf.put_str(crect.x + crect.w / 2, crect.y + crect.h / 2, sp,
                      theme::focus, theme::surface);
        }
      }
    }
  }

  // Demo-only: echo the last key + terminal geometry into a debug line so the
  // drive-tui DoD ("keys echo into a debug line", "resize reflows live") is
  // visibly satisfied. Never rendered in the real app.
  if (app.demo) {
    std::string dbg = "demo · key: ";
    dbg += app.last_key_debug.empty() ? "(none)" : app.last_key_debug;
    dbg += "  ·  ";
    dbg += std::to_string(app.win.cols);
    dbg += "x";
    dbg += std::to_string(app.win.rows);
    buf.put_str(2, 2, dbg, theme::fg2, theme::bg);
  }

  // Connect modal (P19, DESIGN 5.5a): a captured overlay drawn LAST, over
  // whatever content/cover drew above (typically Settings) — never a
  // full-pane fill (draw_connect itself falls back to a bare strip on a
  // cramped pane; it never occupies the whole screen).
  if (app.connect.has_value()) {
    const App::ConnectSession& s = *app.connect;
    const TickCount elapsed_ticks = app.tick_count - s.started;
    ConnectView view{s.url, elapsed_ticks / 10, elapsed_ticks * 100, s.copied};
    draw_connect(view, Rect{0, 0, buf.width(), buf.height()}, buf);
  }

  // Discover filter overlay (P38, §9): same "captured, drawn last" shape as
  // the connect modal above; the two never coincide (connect only opens from
  // Settings).
  if (app.discover_filters.visible) {
    const DiscoverFilterOverlay& ov = app.discover_filters;
    DiscoverFilterView view{&ov.draft,        ov.row_cursor,     ov.genre_cursor,
                            &ov.genres,       ov.genres_loading, ov.genres_failed};
    draw_discover_filters(view, Rect{0, 0, buf.width(), buf.height()}, buf);
  }

  draw_toasts(app, buf);
  draw_bottom_bar(app, buf);
}

// ===========================================================================
// run() — the event loop. Terminal + stdin reader thread + queue tick.
// ===========================================================================

namespace {

// Reader thread: blocking read(0), decode to key/mouse events, post them.
// Handles the Esc-vs-CSI timeout by waiting with a short deadline when an Esc
// is held. `stop` is atomic: the UI thread writes it at quit while this thread
// reads it (a plain bool would be a data race, even if a benign-looking one).
void reader_loop(EventQueue& q, const std::atomic<bool>& stop) {
  InputDecoder dec;
  char buf[512];
  std::vector<Event> evs;
  while (!stop) {
    // If an Esc is pending, wait with a 40ms deadline so a lone Esc resolves;
    // otherwise a 200ms bounded wait so `stop` is noticed. wait_readable is the
    // poll/select platform seam (select on macOS, poll elsewhere — §3).
    const int timeout = dec.esc_pending() ? 40 : 200;
    const int pr = wait_readable(STDIN_FILENO, timeout);
    if (stop) break;
    if (pr < 0) {
      if (errno == EINTR) continue;
      debug_log("reader: wait_readable failed errno=" + std::to_string(errno));
      break;
    }
    if (pr == 0) {
      // Timeout: resolve a pending lone Esc.
      evs.clear();
      if (dec.flush_pending(evs)) {
        for (auto& e : evs) {
          if (!q.try_post(std::move(e))) break;
        }
      }
      continue;
    }
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;  // EOF / error on stdin.
    }
    evs.clear();
    dec.feed(std::string_view(buf, static_cast<std::size_t>(n)), evs);
    for (auto& e : evs) {
      if (!q.try_post(std::move(e))) break;  // queue full/shutdown: drop (A1).
    }
  }
}

}  // namespace

int run(bool demo_mode, const AppDeps* deps, const Config* config, std::string config_file,
        std::string auth_file, std::string db_file, std::string current_version) {
  Terminal term;
  if (!term.ok()) {
    // Not a tty: nothing to drive. main() picks a headless path.
    return 1;
  }

  App app;
  app.win = query_winsize();
  if (app.win.cols == 0) app.win = WinSize{80, 24, 0, 0};  // sane default.
  // Stamp the broadcast cour once for the Discover NEW-badge test (draw() is
  // pure and has no clock; the cour changes at most once per season, P17).
  app.discover_cour = current_cour(now_epoch_secs());
  // Schedule's clock stamp (P37): unlike discover_cour, re-stamped every Tick
  // too (the countdown column visibly counts down) — see tick()'s Tick arm.
  app.clock_epoch_secs = now_epoch_secs();

  // P18: config lands before anything reads it (landing-view decision below,
  // the title chain, resume offset, ...). null (demo_mode / most tests) keeps
  // Config{} defaults.
  if (config != nullptr) app.config = *config;
  app.config_file = std::move(config_file);
  app.settings_version = "v0.1.0";

  // P19: total load (auth.hpp) — missing/corrupt/no path all yield the
  // signed-out default, same "never wedges startup" contract as config.
  app.auth_file = std::move(auth_file);
  if (!app.auth_file.empty()) app.auth = Auth::load(app.auth_file);
  app.settings_account = account_display(app.auth);
  app.db_file = std::move(db_file);

  if (demo_mode) {
    app.demo = true;
    app.any_loading = true;  // keep the spinner animating (no live worker yet).
    app.toasts.push(ToastKind::Info, "drive-tui demo — press q to quit",
                    app.tick_count);
  }

  // Cover-support probe (P8, A4). ONCE, after the alternate screen is entered
  // (Terminal ctor) and BEFORE the reader thread exists — it reads stdio
  // directly. Skipped in demo_mode (drive-tui ships no covers) and when there
  // is no cover pipeline (a FixtureProvider test). A terminal speaking neither
  // protocol — or a mute one — yields {backend=None, cell={0,0}} → placeholder
  // mode, nothing image-shaped ever emitted.
  if (!demo_mode && deps != nullptr && deps->covers != nullptr) {
    const CoverProbe probe = probe_cover_support(app.win.cols, app.win.rows);
    app.cover_caps.backend = backend_from(probe);
    app.cover_caps.cell = probe.cell;
    debug_log("probe: kitty=" + std::to_string(probe.kitty) +
              " iterm=" + std::to_string(probe.iterm) +
              " iterm2=" + std::to_string(probe.iterm2) +
              " sixel=" + std::to_string(probe.sixel) + " cell=" +
              std::to_string(probe.cell.width) + "x" +
              std::to_string(probe.cell.height) + " win=" +
              std::to_string(app.win.cols) + "x" + std::to_string(app.win.rows) +
              " px=" + std::to_string(app.win.xpixel) + "x" +
              std::to_string(app.win.ypixel));
  }

  // Mouse reporting (P32) goes on only after the probe above — the probe
  // reads stdin raw, and a click mid-probe must not interleave report bytes
  // into its answer. Every restore path writes the matching disable.
  term.enable_mouse();

  EventQueue queue;
  app.queue = &queue;
  app.deps = deps;
  if (deps != nullptr) app.settings_covers_dir = deps->covers_dir_display;
  // Launch pull-refresh (04 §3, 06 §5.2): pull only, so first contact never
  // blind-pushes a session-old snapshot over whatever changed on AniList
  // since the last run.
  flush_sync(app, /*pull_only=*/true);
  // Boot-time update check (P28, 06 §6.1): gated on the config switch, a real
  // http client, a cache dir to persist the TTL into, and a known running
  // version to compare against. Any missing piece (demo_mode, a store-less
  // test, no cache dir resolved) silently skips it — same degrade shape as the
  // sync/prewarm gates just above and below.
  if (app.config.check_for_updates && deps != nullptr && deps->http != nullptr &&
      !deps->update_cache_dir.empty() && !current_version.empty()) {
    spawn_update_check(queue, *deps->http, deps->update_cache_dir, current_version,
                       now_epoch_secs());
  }
  // P15: wire the resolve walk transport + prewarm when a store is present
  // (they persist bindings/absences/routes/pins). Absent them, the app degrades
  // to the P7 first-hit resolve (demo mode / store-less tests keep working).
  if (deps != nullptr && deps->store != nullptr && deps->registry != nullptr) {
    app.episode_session = std::make_unique<EpisodeSession>();
    app.prewarm = std::make_unique<PrewarmState>();
  }
  // P37 slice 3: boot-time notice check, using next_airing_* as of process
  // start (the pull-refresh above completes asynchronously and re-checks
  // again itself, via on_sync_flushed, once fresher data lands).
  check_schedule_notices(app);

  // P18 landing (DESIGN §8.3): "last_watched" lands on History with the
  // most-recently-watched row selected, when one exists; "history" lands on
  // History empty-cursor; anything else (incl. "browse", the default view)
  // is a no-op. A synchronous local read, same as History's own `H` handler
  // — never touches the network. An explicit History landing holds even with
  // an empty watchlist (app.rs landing_view; P30 audit closed the !empty gate
  // narrowing). The last_watched auto-open (05 §10.6) arms here and fires on
  // the first real geometry — only then is pane-vs-zoom decidable.
  if (app.config.landing == "last_watched" || app.config.landing == "history") {
    load_history(app);
    app.view = View::History;
    app.pane = Pane::List;
    if (app.config.landing == "last_watched") {
      if (const auto aid = app.history.first_played(); aid.has_value()) {
        const int visible = content_height(app.win.rows);
        app.history.select_aid(*aid, visible);
        app.resume_pending = true;
      }
    }
  }

  std::atomic<bool> reader_stop{false};
  std::thread reader(reader_loop, std::ref(queue), std::cref(reader_stop));

  CellBuffer front(app.win.cols, app.win.rows);  // what's on screen (prev).
  CellBuffer back;                               // drawn each frame.
  std::string out, pre_diff, post_diff, grid_pre_diff, grid_post_diff;
  // The detail-cover placement the terminal currently holds (id 0 = none).
  // run() owns this — draw() is pure and cannot know the terminal's placement
  // history. Carrying show+rect (not just the id) lets compose_cover_apc skip
  // the steady state instead of re-transmitting every frame.
  detail::PlacedCover placed_cover;
  // P17b: the grid cover placements the terminal currently holds (url ->
  // {id, rect}), run()-owned for the same reason (draw() is pure).
  std::map<std::string, detail::Placed> placed_grid_covers;

  // Set when a SIGWINCH landed; consumed by the next paint. A resize breaks
  // the placement model's premise (the terminal reflows / re-anchors / drops
  // images), so that paint purges every held placement and re-transmits.
  bool placements_stale = false;
  std::string resize_purge;

  // One frame: draw (pure) → compose the A4 cover APC deltas (single detail
  // cover + the P17 grid) → flush with the deletes spliced pre-diff and the
  // transmits post-diff, one synchronized write.
  auto paint = [&]() {
    draw(app, back);
    // Purge BEFORE compose: with the records emptied, compose sees id 0 and
    // re-transmits whatever the new layout wants (returning true → the full
    // repaint below). `purged` covers the other arm — placements held but the
    // new layout wants none — where compose alone would report no change.
    bool purged = false;
    if (placements_stale) {
      placements_stale = false;
      purged = detail::purge_placements(app.cover_caps.backend, placed_cover,
                                        placed_grid_covers, resize_purge);
    } else {
      resize_purge.clear();
    }
    const bool cover_changed =
        detail::compose_cover_apc(app, placed_cover, pre_diff, post_diff);
    const bool grid_changed =
        detail::compose_grid_apc(app, placed_grid_covers, grid_pre_diff, grid_post_diff);
    // A placement change (appear / disappear / replace / move) means cells the
    // old image covered may now be text: force a full repaint so the diff
    // repaints that region (the exclusion made the previous frame skip it, so
    // `prev` still holds the pre-image cells there).
    if (cover_changed || grid_changed || purged) back.mark_all_dirty();
    out.clear();
    // Grid deletes/transmits splice alongside the single-cover ones, same
    // pre-diff-before-diff / post-diff-after-diff composition order (A4).
    back.flush(front, out, resize_purge + pre_diff + grid_pre_diff,
               post_diff + grid_post_diff);
    term.write(out);
  };

  paint();  // first paint.

  while (!app.quit) {
    // Poll the SIGWINCH flag each loop turn (A1: handler only set a flag).
    if (take_resize_flag()) {
      const WinSize ws = query_winsize();
      placements_stale = true;  // the next paint purges + re-transmits.
      queue.try_post(Event{Resize{ws.cols, ws.rows, ws.xpixel, ws.ypixel}});
    }

    std::optional<Event> ev = queue.wait_next();
    if (!ev) {
      // Timeout == Tick (A1 / 04 §8).
      tick(app, Event{Tick{}});
    } else {
      tick(app, *ev);
    }

    if (app.quit) break;

    if (app.dirty) {
      paint();
      app.dirty = false;
    }
  }

  // Downloads at quit (P35 slice 3): NO drain rights (A1) — set the cancel
  // flag (the curl arm's abort poll notices within ~1s if the process lives
  // that long) and SIGTERM a published ffmpeg child directly, since _exit(0)
  // below would otherwise orphan it mid-transfer. Never waited on: the .part
  // convention makes any interruption safe (a rerun resumes or restarts).
  if (app.download.active) {
    if (app.download.cancel) {
      app.download.cancel->store(true, std::memory_order_release);
    }
    if (app.download.child_pid) {
      const long pid = app.download.child_pid->load(std::memory_order_acquire);
      if (pid > 0) ::kill(static_cast<pid_t>(pid), SIGTERM);
    }
  }

  // Quit flush (P20, 04 §11 / A1 §3 amendment): push what's dirty, bounded
  // best-effort. Gated the same as the periodic flush (never overlaps a
  // pull), spawns with NoSleep (teardown's own deadline is the only budget it
  // gets), and signals completion via a heap-shared atomic — NOT the event
  // queue, which is about to be shut down and has no reader left to act on a
  // summary at quit anyway. Runs BEFORE the queue/reader teardown below since
  // it never touches either.
  if (!app.syncing && sync_enabled(app) && deps != nullptr && deps->http != nullptr &&
      !app.db_file.empty()) {
    auto flushed = std::make_shared<std::atomic<bool>>(false);
    spawn_quit_flush_worker(*deps->http, app.db_file, app.auth, app.config.anilist_sync_enabled,
                            now_epoch_secs(), flushed);
    (void)detail::bounded_wait(*flushed, std::chrono::seconds(1));
    // Deadline hit or not, teardown proceeds: _exit(0) below tears down the
    // whole address space atomically, so an abandoned worker never gets the
    // chance to touch freed memory.
  }

  // Quit: A1's production path (04 §3.4) — terminal restore, then _exit(0),
  // never a clean return. P7 workers are detached threads holding references
  // to `queue`/`app.deps` that live on THIS stack frame (spawn_search et al.);
  // there is no join/drain machinery (A1 explicitly: "no drain machinery"), so
  // returning normally would unwind this frame out from under any worker still
  // in flight (a slow HTTP fetch, mpv still exiting). _exit(0) tears down the
  // whole address space atomically instead — the in-flight workers never get
  // the chance to touch freed memory because nothing after this point runs.
  reader_stop = true;
  queue.shutdown();
  reader.detach();  // do not join a thread parked in read(); A1 avoids the hang.

  // Clear the cover image(s) on the quit path (A4 / 04 §11): a live kitty
  // placement left behind would have the terminal ack deletes onto the shell
  // after exit, or paint a stray image over the restored screen. Delete every
  // held image (detail cover + every placed grid cover) BEFORE restoring.
  //
  // Kitty ONLY. The cell-inline backends (OSC 1337, SIXEL) store no image to
  // free — their pixels live in the alternate screen's cells and die with
  // term.restore() — so a delete APC here would be a stray kitty command
  // written at a terminal that never spoke the protocol.
  if (app.cover_caps.backend == CoverBackend::Kitty) {
    if (placed_cover.id != 0) {
      const std::string clear = kitty::delete_image(placed_cover.id);
      term.write(clear);
    }
    for (const auto& [url, p] : placed_grid_covers) {
      const std::string clear = kitty::delete_image(p.id);
      term.write(clear);
    }
  }

  term.restore();
  std::fflush(nullptr);
  _exit(0);
}

}  // namespace shigoku::tui
