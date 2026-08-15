// app_tests.cpp — P7 pure-helper tests: pane_split, classify_tier, the
// PlayError/ProviderError -> toast mapping, and idle_help's new Detail arm.
// The end-to-end wiring (search worker -> episode grid -> play) is covered by
// pty_walk_tests.cpp; this file is the offline, no-worker unit layer (§8
// pure-function bias).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "../src/provider.hpp"
#include "../src/store.hpp"
#include "../src/tui/app.hpp"
#include "../src/tui/prewarm.hpp"
#include "../src/tui/resolve_transport.hpp"
#include "../src/tui/views.hpp"

using namespace shigoku;
using namespace shigoku::tui;

namespace {
// app.cpp's now_epoch_secs() is anonymous-namespace, not test-visible; the
// schedule-notice tests below (P37 slice 3) need their own "now" stamp for
// next_airing_at fixtures.
std::int64_t test_now_secs() { return static_cast<std::int64_t>(::time(nullptr)); }
}  // namespace

// --- pane_split (DESIGN §3.2) -----------------------------------------------

TEST_CASE("pane_split: formula matches the DESIGN §3.2 table") {
  const auto s100 = pane_split(100);
  CHECK(s100.list_w == 38);           // max(30, 100*38/100)
  CHECK(s100.detail_x == 2 + 38 + 2);  // 42
  CHECK(s100.detail_w == 100 - 42 - 1);

  // list_w floors at 30 for narrow widths.
  const auto s60 = pane_split(60);
  CHECK(s60.list_w == 30);  // max(30, 60*38/100=22) -> 30
}

// --- classify_tier (A8 degenerate walk) -------------------------------------

namespace {
class TierProbeProvider final : public StreamProvider {
 public:
  bool key_present = true;
  std::string_view name() const override { return "probe"; }
  std::string_view display_name() const override { return "Probe"; }
  std::optional<std::string> canonical_key(const Enrichment&) const override {
    return key_present ? std::optional<std::string>("1") : std::nullopt;
  }
  Result<std::vector<SearchHit>, ProviderError> search(std::string_view,
                                                        const SearchOptions&) const override {
    return std::vector<SearchHit>{};
  }
  Result<std::vector<std::string>, ProviderError> episodes(
      std::string_view, Translation, std::optional<std::uint32_t>) const override {
    return std::vector<std::string>{};
  }
  Result<StreamLink, ProviderError> resolve(std::string_view, std::string_view, Translation,
                                            Quality) const override {
    return StreamLink{};
  }
  Result<CoverRequest, ProviderError> cover_request(std::string_view) const override {
    return err(ProviderError::unsupported());
  }
};
}  // namespace

TEST_CASE("classify_tier: mal_id-shaped canonical_key -> AKey, else CSearch") {
  TierProbeProvider p;
  Enrichment show;

  p.key_present = true;
  CHECK(detail::classify_tier(show, p) == Tier::AKey);

  p.key_present = false;
  CHECK(detail::classify_tier(show, p) == Tier::CSearch);
}

// --- provider_error_toast / play_error_toast (DESIGN §4.10) ----------------

TEST_CASE("provider_error_toast: splices display_name into the DESIGN copy") {
  {
    auto [kind, copy] = detail::provider_error_toast(ProviderError::forbidden(403), "Senshi");
    CHECK(kind == ToastKind::Error);
    CHECK(copy == "Senshi blocked us");
  }
  {
    auto [kind, copy] = detail::provider_error_toast(ProviderError::server(503), "Senshi");
    CHECK(copy == "Senshi is down");
  }
  {
    auto [kind, copy] = detail::provider_error_toast(ProviderError::network(), "Senshi");
    CHECK(copy == "network unreachable");  // no provider splice for Network.
  }
  {
    auto [kind, copy] = detail::provider_error_toast(ProviderError::decode("bad json"), "Senshi");
    CHECK(copy == "couldn't load episodes");
  }
}

TEST_CASE("play_error_toast: MpvNotFound is its own kind, not a Spawn sniff (P22, debt #5)") {
  {
    auto [kind, copy] =
        detail::play_error_toast(PlayError::mpv_not_found("mpv: No such file or directory"), "Senshi");
    CHECK(kind == ToastKind::Error);
    CHECK(copy == "mpv not found \xC2\xB7 install mpv");
  }
  {
    auto [kind, copy] = detail::play_error_toast(PlayError::spawn("mpv: permission denied"), "Senshi");
    CHECK(copy == "mpv exited with error");
  }
  {
    auto [kind, copy] = detail::play_error_toast(PlayError::open_failed(3), "Senshi");
    CHECK(copy == "stream didn't open \xC2\xB7 try again");
  }
}

// --- idle_help: the Detail zoom arm exists (A2 fence coverage) -------------

TEST_CASE("idle_help: Detail zoom copy matches DESIGN §7.5") {
  const std::string help = detail::idle_help(View::Detail, Pane::List);
  CHECK(help.find("space/esc back") != std::string::npos);
  // The §9 zoom-only keys are advertised (an unnamed key may as well not
  // exist — the P36 `c` section went unnoticed on hardware).
  CHECK(help.find("c cast") != std::string::npos);
  CHECK(help.find("d download") != std::string::npos);
  // F7: hjkl navigates the grid since P33 — the copy must not say "scroll".
  CHECK(help.find("hjkl grid") != std::string::npos);
  CHECK(detail::idle_help(View::Browse, Pane::Detail).find("hjkl grid") !=
        std::string::npos);
  // F8: Discover's add-to-watchlist is advertised like Browse's.
  CHECK(detail::idle_help(View::Discover, Pane::List).find("P save") !=
        std::string::npos);
}

// P33: the two-pane Detail-pane hint no longer claims h backs out (h now
// steps the grid cursor there) — esc carries "back" instead.
TEST_CASE("idle_help: Browse/History detail-pane hint says esc back, not h back") {
  const std::string browse = detail::idle_help(View::Browse, Pane::Detail);
  CHECK(browse.find("esc back") != std::string::npos);
  CHECK(browse.find("h back") == std::string::npos);
  const std::string history = detail::idle_help(View::History, Pane::Detail);
  CHECK(history.find("esc back") != std::string::npos);
  CHECK(history.find("h back") == std::string::npos);
}

// --- provider_caption (P15, 05 §15) ----------------------------------------

TEST_CASE("provider_caption: serving leads, markers + pin follow") {
  EpisodeState es;
  CHECK(detail::provider_caption(es).empty());  // nothing serving yet.

  es.serving = "alpha";
  es.pinned = "beta";
  es.avail = {{"alpha", AvailMark::Bound}, {"beta", AvailMark::Absent},
              {"gamma", AvailMark::Unchecked}};
  const std::string cap = detail::provider_caption(es);
  // Serving provider leads.
  CHECK(cap.rfind("\xE2\x96\xB8 alpha", 0) == 0);  // "▸ alpha" at the start.
  // Other providers appear with their markers; alpha (serving) is not repeated.
  CHECK(cap.find("beta[-]") != std::string::npos);   // absent → [-].
  CHECK(cap.find("gamma[?]") != std::string::npos);  // unchecked → [?].
  CHECK(cap.find("\xC2\xB7 alpha") == std::string::npos);  // serving not re-listed.
  // The pinned provider (beta) carries the pin glyph.
  CHECK(cap.find("beta[-]\xF0\x9F\x93\x8C") != std::string::npos);
}

// --- HistoryState (P16, 05 §2, ROD-439 chunk 5a) ----------------------------

namespace {

Show history_show(std::int64_t aid, std::string_view title, ListStatus status,
                  std::uint32_t progress) {
  Show s;
  s.enrichment.anilist_id = aid;
  s.enrichment.title_romaji = std::string(title);
  s.enrichment.title_english = std::string(title) + " EN";
  s.enrichment.total_episodes = 12;
  s.list_status = status;
  s.progress = progress;
  s.library_added_at = 100;
  return s;
}

HistoryState history_state(std::vector<Show> rows) {
  HistoryState s;
  s.resume.assign(rows.size(), std::nullopt);
  s.rows = std::move(rows);
  s.rebuild(std::nullopt);
  return s;
}

std::vector<std::int64_t> nav_ids(const HistoryState& s) {
  std::vector<std::int64_t> out;
  out.reserve(s.order.size());
  for (std::size_t ix : s.order) out.push_back(s.rows[ix].enrichment.anilist_id);
  return out;
}

}  // namespace

TEST_CASE("history: order walks groups, not store order") {
  // Store order interleaves statuses; nav order must group them
  // watching -> planning -> paused -> completed -> dropped (05 §2).
  HistoryState s = history_state({
      history_show(1, "A", ListStatus::Completed, 12),
      history_show(2, "B", ListStatus::Watching, 3),
      history_show(3, "C", ListStatus::Dropped, 1),
      history_show(4, "D", ListStatus::Watching, 5),
      history_show(5, "E", ListStatus::Planning, 0),
  });
  CHECK(nav_ids(s) == std::vector<std::int64_t>{2, 4, 5, 1, 3});
}

TEST_CASE("history: cursor follows identity across reorder and clamps") {
  HistoryState s = history_state({
      history_show(1, "A", ListStatus::Watching, 1),
      history_show(2, "B", ListStatus::Watching, 2),
      history_show(3, "C", ListStatus::Watching, 3),
  });
  s.cursor = 1;  // B
  // B completes: it moves to the completed group's slot; cursor follows.
  s.rows[1].list_status = ListStatus::Completed;
  s.rebuild(s.rows[1].enrichment.anilist_id);
  CHECK(nav_ids(s) == std::vector<std::int64_t>{1, 3, 2});
  CHECK(s.cursor == 2);

  // B vanishes entirely: clamp to the last valid ordinal.
  const std::int64_t gone = s.rows[1].enrichment.anilist_id;
  s.rows.erase(s.rows.begin() + 1);
  s.resume.erase(s.resume.begin() + 1);
  s.rebuild(gone);
  CHECK(s.cursor == 1);

  // Out-of-range cursor clamps too.
  s.cursor = 99;
  s.rebuild(std::nullopt);
  CHECK(s.cursor == 1);
}

TEST_CASE("history: filter matches any title form, esc resets") {
  HistoryState s = history_state({
      history_show(1, "Sousou no Frieren", ListStatus::Watching, 1),
      history_show(2, "Vinland Saga", ListStatus::Watching, 1),
  });
  s.filter = "frieren en";  // matches the " EN" english suffix.
  s.on_filter_edited();
  CHECK(s.count() == 1);
  s.filter = "VINLAND";
  s.on_filter_edited();
  CHECK(nav_ids(s) == std::vector<std::int64_t>{2});  // case-insensitive.
  s.on_filter_cleared();
  CHECK(s.filter.empty());
  CHECK(s.count() == 2);
  CHECK(s.cursor == 0);
}

TEST_CASE("history: filtered anchor falls to clamp when excluded") {
  HistoryState s = history_state({
      history_show(1, "Alpha", ListStatus::Watching, 1),
      history_show(2, "Beta", ListStatus::Watching, 1),
  });
  s.cursor = 1;  // Beta
  s.filter = "alpha";
  s.on_filter_edited();
  CHECK(s.count() == 1);
  CHECK(s.cursor == 0);
}

TEST_CASE("history: geometry counts headers, rules, and blanks") {
  HistoryState s = history_state({
      history_show(1, "A", ListStatus::Watching, 1),
      history_show(2, "B", ListStatus::Watching, 2),
      history_show(3, "C", ListStatus::Completed, 12),
  });
  const std::vector<HistoryLine> lines = s.layout();
  using K = HistoryLine::Kind;
  REQUIRE(lines.size() == 11);
  CHECK(lines[0].kind == K::Header);
  CHECK(lines[0].status == ListStatus::Watching);
  CHECK(lines[0].count == 2);
  CHECK(lines[1].kind == K::Rule);
  CHECK(lines[2] == HistoryLine{K::Title, {}, 0, 0});
  CHECK(lines[3] == HistoryLine{K::Bar, {}, 0, 0});
  CHECK(lines[4] == HistoryLine{K::Title, {}, 0, 1});
  CHECK(lines[5] == HistoryLine{K::Bar, {}, 0, 1});
  CHECK(lines[6].kind == K::Blank);
  CHECK(lines[7].kind == K::Header);
  CHECK(lines[7].status == ListStatus::Completed);
  CHECK(lines[7].count == 1);
  CHECK(lines[8].kind == K::Rule);
  CHECK(lines[9] == HistoryLine{K::Title, {}, 0, 2});
  CHECK(lines[10] == HistoryLine{K::Bar, {}, 0, 2});
}

TEST_CASE("history: scroll keeps the cursor and its header visible") {
  std::vector<Show> rows;
  for (std::int64_t i = 1; i <= 10; ++i) {
    rows.push_back(history_show(i, "S" + std::to_string(i), ListStatus::Watching, 1));
  }
  HistoryState s = history_state(std::move(rows));
  s.nav(9, 8);
  const std::vector<HistoryLine> lines = s.layout();
  auto it = std::find_if(lines.begin(), lines.end(), [&](const HistoryLine& li) {
    return li.kind == HistoryLine::Kind::Title && li.ord == s.cursor;
  });
  REQUIRE(it != lines.end());
  const std::size_t title_ix = static_cast<std::size_t>(it - lines.begin());
  CHECK(title_ix + 2 <= s.scroll + 8);
  s.jump(true, 8);
  CHECK(s.scroll == 0);
}

TEST_CASE("history: select_aid moves the cursor by identity") {
  HistoryState s = history_state({
      history_show(1, "A", ListStatus::Watching, 1),
      history_show(2, "B", ListStatus::Watching, 1),
  });
  CHECK(s.select_aid(2, 8));
  CHECK(s.cursor == 1);
  CHECK_FALSE(s.select_aid(999, 8));  // not in nav order: false, cursor untouched.
  CHECK(s.cursor == 1);
}

// --- compose_cover_apc: transmit-once placement (terminal-flood regression) --
//
// The composer must emit the ~180KB transmit ONCE per placement change, not
// every frame: the frame-rate re-transmit is what flooded the PPC terminal
// into an mpv-quit freeze (term.write blocks once the pty backs up).

TEST_CASE("compose_cover_apc: transmit once, steady state emits nothing") {
  App app;
  app.view = View::Browse;
  app.pane = Pane::Detail;
  app.win = WinSize{100, 24, 0, 0};
  app.cover_caps.backend = CoverBackend::Kitty;
  CatalogRow row;
  row.meta.anilist_id = 42;
  row.meta.title_romaji = "Show";
  row.meta.cover_url = "https://example.com/c.jpg";
  app.catalog.push_back(row);
  app.list_cursor = 0;
  app.cover_render.have_pixels = true;
  app.cover_render.for_id = 42;
  app.cover_render.pixels.w = 2;
  app.cover_render.pixels.h = 2;
  app.cover_render.pixels.rgba.assign(16, 0xFF);

  detail::PlacedCover placed;
  std::string pre, post;

  // First frame with Ready pixels: one transmit, with the pixel dims on the
  // wire (s=/v= — mandatory for f=32; their omission was the no-covers bug).
  CHECK(detail::compose_cover_apc(app, placed, pre, post));
  CHECK(pre.empty());  // nothing placed before: no delete owed.
  CHECK(post.find("a=T") != std::string::npos);
  CHECK(post.find("s=2") != std::string::npos);
  CHECK(post.find("v=2") != std::string::npos);
  CHECK(placed.id != 0);
  CHECK(placed.for_id == 42);

  // Steady state (same show, same rect, already placed): NOTHING goes out.
  CHECK_FALSE(detail::compose_cover_apc(app, placed, pre, post));
  CHECK(pre.empty());
  CHECK(post.empty());

  // Pixels withdrawn (begin_fetch / selection away): delete once, then quiet.
  app.cover_render.have_pixels = false;
  CHECK(detail::compose_cover_apc(app, placed, pre, post));
  CHECK(pre.find("a=d,d=I") != std::string::npos);
  CHECK(post.empty());
  CHECK(placed.id == 0);
  CHECK_FALSE(detail::compose_cover_apc(app, placed, pre, post));
  CHECK(pre.empty());
}

TEST_CASE("compose_cover_apc: iterm backend — OSC 1337 out, no delete bytes") {
  App app;
  app.view = View::Browse;
  app.pane = Pane::Detail;
  app.win = WinSize{100, 24, 0, 0};
  app.cover_caps.backend = CoverBackend::Iterm;
  CatalogRow row;
  row.meta.anilist_id = 42;
  row.meta.title_romaji = "Show";
  row.meta.cover_url = "https://example.com/c.jpg";
  app.catalog.push_back(row);
  app.list_cursor = 0;
  app.cover_render.have_pixels = true;
  app.cover_render.for_id = 42;
  app.cover_render.pixels.w = 2;
  app.cover_render.pixels.h = 2;
  app.cover_render.pixels.rgba.assign(16, 0xFF);

  detail::PlacedCover placed;
  std::string pre, post;

  // First frame: one OSC 1337 transmit after a CUP; nothing before the diff
  // (no delete command exists on this wire).
  CHECK(detail::compose_cover_apc(app, placed, pre, post));
  CHECK(pre.empty());
  CHECK(post.find("\x1b]1337;File=inline=1") != std::string::npos);
  CHECK(post.find("preserveAspectRatio=0;doNotMoveCursor=1:") !=
        std::string::npos);
  CHECK(post.find("\x1b_G") == std::string::npos);  // no kitty APC leaks.
  CHECK(placed.id != 0);

  // Steady state: nothing goes out (the flood rule holds for both backends).
  CHECK_FALSE(detail::compose_cover_apc(app, placed, pre, post));
  CHECK(pre.empty());
  CHECK(post.empty());

  // Pixels withdrawn: NO bytes either way — the returned true forces the
  // repaint that overwrites the vacated cells, which is the OSC 1337 erase.
  app.cover_render.have_pixels = false;
  CHECK(detail::compose_cover_apc(app, placed, pre, post));
  CHECK(pre.empty());
  CHECK(post.empty());
  CHECK(placed.id == 0);
}

// --- App + Store: status/undo/recompute/delete (P16, 05 §3-§4) -------------

namespace {

// A minimal App wired to a real in-memory Store, mirroring the fixture shape
// resolve_transport_tests.cpp uses for App/Store integration. App holds a
// GenCounter (std::atomic members), so it is neither copyable nor movable —
// HistoryHarness is built in place (never returned by value) and tests hold
// it via std::unique_ptr.
struct HistoryHarness {
  Store store;
  AppDeps deps;
  App app;

  explicit HistoryHarness(Store s) : store(std::move(s)) {
    deps.store = &store;
    app.deps = &deps;
    app.view = View::History;
    app.pane = Pane::List;
    app.win = WinSize{100, 24, 0, 0};
  }

  static std::unique_ptr<HistoryHarness> make() {
    auto st = Store::open_memory();
    REQUIRE(st.has_value());
    return std::make_unique<HistoryHarness>(std::move(*st));
  }

  void add(std::int64_t aid, std::string_view title, std::uint32_t total) {
    Enrichment e;
    e.anilist_id = aid;
    e.title_romaji = std::string(title);
    e.total_episodes = total;
    REQUIRE(store.add_to_library(e, 50).has_value());
  }

  // Reload rides the H *entry* (05 §6: H is a goto whose entry reloads; a
  // same-view H is a no-op since P30) — hop out to Browse and back.
  void reload() {
    tick(app, Event{KeyEvent{U'B', KeyEvent::Special::None, false}});
    tick(app, Event{KeyEvent{U'H', KeyEvent::Special::None, false}});
  }
};

}  // namespace

TEST_CASE("history app: p/x/c/w transition status and capture undo") {
  auto h = HistoryHarness::make();
  h->add(1, "Show", 12);
  h->reload();
  REQUIRE(h->app.history.count() == 1);

  tick(h->app, Event{KeyEvent{U'c', KeyEvent::Special::None, false}});
  {
    auto g = h->store.get_show(1);
    REQUIRE(g->has_value());
    CHECK((*g)->list_status == ListStatus::Completed);
    CHECK((*g)->progress == 12);  // snapped to total.
  }
  REQUIRE(h->app.undo.has_value());
  CHECK(std::get<0>(*h->app.undo) == 1);
  CHECK(std::get<1>(*h->app.undo) == ListStatus::Planning);  // add_to_library default.

  // `u` restores the exact captured pair.
  tick(h->app, Event{KeyEvent{U'u', KeyEvent::Special::None, false}});
  {
    auto g = h->store.get_show(1);
    REQUIRE(g->has_value());
    CHECK((*g)->list_status == ListStatus::Planning);
    CHECK((*g)->progress == 0);
  }
  CHECK_FALSE(h->app.undo.has_value());  // single-level: consumed.

  // A second `u` with nothing pending is a no-op.
  tick(h->app, Event{KeyEvent{U'u', KeyEvent::Special::None, false}});
  CHECK_FALSE(h->app.undo.has_value());
}

TEST_CASE("history app: recompute clears a pending undo (c-then-r-then-u law)") {
  auto h = HistoryHarness::make();
  h->add(2, "Show", 12);
  h->reload();

  tick(h->app, Event{KeyEvent{U'c', KeyEvent::Special::None, false}});
  REQUIRE(h->app.undo.has_value());

  tick(h->app, Event{KeyEvent{U'r', KeyEvent::Special::None, false}});
  CHECK_FALSE(h->app.undo.has_value());  // recompute clears it, not undoable itself.
  {
    auto g = h->store.get_show(2);
    REQUIRE(g->has_value());
    CHECK((*g)->progress == 0);  // no episode_progress rows: recomputes to 0.
  }

  // `u` is now a no-op: the Completed/0 state from recompute survives.
  tick(h->app, Event{KeyEvent{U'u', KeyEvent::Special::None, false}});
  {
    auto g = h->store.get_show(2);
    REQUIRE(g->has_value());
    CHECK((*g)->list_status == ListStatus::Completed);
    CHECK((*g)->progress == 0);
  }
}

TEST_CASE("history app: X arms delete, only y fires, X/other cancels or re-arms") {
  auto h = HistoryHarness::make();
  h->add(3, "Doomed", 12);
  h->reload();

  tick(h->app, Event{KeyEvent{U'X', KeyEvent::Special::None, false}});
  REQUIRE(h->app.confirm_delete.has_value());
  CHECK(*h->app.confirm_delete == 3);

  // `q` does NOT quit while armed (DESIGN §6.5): it falls into "anything
  // else" and just cancels the confirm back to idle.
  tick(h->app, Event{KeyEvent{U'q', KeyEvent::Special::None, false}});
  CHECK_FALSE(h->app.quit);
  CHECK_FALSE(h->app.confirm_delete.has_value());

  // Re-arm, then a repeat X stays armed (no self-confirm from key-repeat).
  tick(h->app, Event{KeyEvent{U'X', KeyEvent::Special::None, false}});
  REQUIRE(h->app.confirm_delete.has_value());
  tick(h->app, Event{KeyEvent{U'X', KeyEvent::Special::None, false}});
  CHECK(h->app.confirm_delete.has_value());

  // Esc cancels.
  tick(h->app, Event{KeyEvent{0, KeyEvent::Special::Escape, false}});
  CHECK_FALSE(h->app.confirm_delete.has_value());

  // Re-arm and confirm with y: the row is gone.
  tick(h->app, Event{KeyEvent{U'X', KeyEvent::Special::None, false}});
  tick(h->app, Event{KeyEvent{U'y', KeyEvent::Special::None, false}});
  CHECK_FALSE(h->app.confirm_delete.has_value());
  {
    auto g = h->store.get_show(3);
    REQUIRE(g.has_value());
    CHECK_FALSE(g->has_value());  // deleted.
  }
  CHECK(h->app.history.empty());
}

TEST_CASE("history app: delete refuses the currently-playing show") {
  auto h = HistoryHarness::make();
  h->add(4, "Playing", 12);
  h->reload();
  h->app.play.active = true;
  h->app.play.for_id = 4;

  tick(h->app, Event{KeyEvent{U'X', KeyEvent::Special::None, false}});
  tick(h->app, Event{KeyEvent{U'y', KeyEvent::Special::None, false}});
  CHECK_FALSE(h->app.confirm_delete.has_value());  // refusal still disarms.
  {
    auto g = h->store.get_show(4);
    REQUIRE(g->has_value());  // NOT deleted.
  }
}

TEST_CASE("history app: delete nullifies a stale undo pointing at the same row") {
  auto h = HistoryHarness::make();
  h->add(5, "Target", 12);
  h->reload();
  tick(h->app, Event{KeyEvent{U'c', KeyEvent::Special::None, false}});
  REQUIRE(h->app.undo.has_value());
  CHECK(std::get<0>(*h->app.undo) == 5);

  tick(h->app, Event{KeyEvent{U'X', KeyEvent::Special::None, false}});
  tick(h->app, Event{KeyEvent{U'y', KeyEvent::Special::None, false}});
  CHECK_FALSE(h->app.undo.has_value());
}

TEST_CASE("history app: a background reload cancels an armed confirm") {
  auto h = HistoryHarness::make();
  h->add(6, "A", 12);
  h->reload();
  tick(h->app, Event{KeyEvent{U'X', KeyEvent::Special::None, false}});
  REQUIRE(h->app.confirm_delete.has_value());

  // Switching to History again re-triggers reload_history's unconditional
  // confirm_delete clear (DESIGN §6.5 frozen-list/reload-cancels rule).
  h->reload();
  CHECK_FALSE(h->app.confirm_delete.has_value());
}

// --- bounded_wait (P20, 04 §11 quitFlush DoD: "a hung endpoint cannot wedge
// quit") ----------------------------------------------------------------

TEST_CASE("bounded_wait: a hung worker cannot exceed its deadline") {
  std::atomic<bool> done{false};  // never set: models a hung endpoint.
  const auto start = std::chrono::steady_clock::now();
  const bool finished = detail::bounded_wait(done, std::chrono::milliseconds(100));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK_FALSE(finished);
  CHECK(elapsed < std::chrono::milliseconds(500));  // generous margin, proves no wedge.
}

TEST_CASE("bounded_wait: returns promptly once the worker signals") {
  std::atomic<bool> done{false};
  std::thread t([&done] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    done.store(true, std::memory_order_release);
  });
  CHECK(detail::bounded_wait(done, std::chrono::milliseconds(1000)));
  t.join();
}

// --- Enrichment refresh three-state persist law (P21, 05 §8 / ROD-278) -----
//
// Drives the Enrichment* event handlers directly (no worker/network) so the
// store side-effects are deterministic. HistoryHarness gives an in-memory store
// and a live App; a library show under History is the source the handler patches
// + reloads. The contract: a metadata answer overwrites drift but PRESERVES user
// state and stamps freshness; a confirmed-null stamps freshness with no fields;
// a transport failure stamps NOTHING (never freeze a never-answered row) and
// raises the AniList toast.

namespace {
// A healed answer for `aid`: fresh drift fields, a FINISHED status. Never
// carries list_status/progress — the store merge owns user state, not enrich.
Enrichment healed(std::int64_t aid) {
  Enrichment e;
  e.anilist_id = aid;
  e.title_romaji = "Healed Title";
  e.title_english = "Healed EN";
  e.total_episodes = 24;
  e.status = "FINISHED";
  e.score = 88;
  return e;
}
}  // namespace

TEST_CASE("enrich refreshed: overwrites drift, preserves user state, stamps fresh") {
  auto h = HistoryHarness::make();
  h->add(700, "Stale Title", 12);
  h->reload();
  // Give the show real user state the enrich answer must NOT clobber. Snapping
  // to Watching is the guarded user-state write the merge must preserve.
  REQUIRE(h->store.set_list_status(700, ListStatus::Watching, 100).has_value());
  {
    auto pre = h->store.get_show(700);
    REQUIRE(pre->has_value());
    CHECK_FALSE((*pre)->enrichment_fetched_at.has_value());  // never enriched yet.
  }

  tick(h->app, Event{EnrichmentRefreshed{700, healed(700)}});

  auto g = h->store.get_show(700);
  REQUIRE(g->has_value());
  const Show& s = **g;
  // Drift fields overwritten from the answer.
  CHECK(s.enrichment.title_romaji == "Healed Title");
  CHECK(s.enrichment.status == std::optional<std::string>("FINISHED"));
  CHECK(s.enrichment.score == std::optional<std::uint32_t>(88));
  // User state preserved (the merge never touches list_status via enrich).
  CHECK(s.list_status == ListStatus::Watching);
  // Freshness stamped.
  CHECK(s.enrichment_fetched_at.has_value());
  // A success is the AniList recovery signal: no persistent error toast stands.
  for (const Toast& t : h->app.toasts.visible()) CHECK(t.topic != "anilist");
}

TEST_CASE("enrich null: a confirmed no-match stamps freshness with no fields") {
  auto h = HistoryHarness::make();
  h->add(701, "Keep Title", 12);
  h->reload();
  {
    auto pre = h->store.get_show(701);
    REQUIRE(pre->has_value());
    CHECK_FALSE((*pre)->enrichment_fetched_at.has_value());
  }

  tick(h->app, Event{EnrichmentNull{701}});

  auto g = h->store.get_show(701);
  REQUIRE(g->has_value());
  const Show& s = **g;
  CHECK(s.enrichment_fetched_at.has_value());     // stamped: a true negative is an answer.
  CHECK(s.enrichment.title_romaji == "Keep Title");  // no field overwrite.
}

TEST_CASE("enrich failed: no stamp, no persist, raises the AniList toast") {
  auto h = HistoryHarness::make();
  h->add(702, "Untouched", 12);
  h->reload();

  tick(h->app, Event{EnrichmentFailed{702}});

  auto g = h->store.get_show(702);
  REQUIRE(g->has_value());
  // Never freeze a never-answered row: no freshness stamp on a transport fail.
  CHECK_FALSE((*g)->enrichment_fetched_at.has_value());
  // A persistent AniList error toast now stands (§8.5).
  bool has_anilist_toast = false;
  for (const Toast& t : h->app.toasts.visible()) {
    if (t.topic == "anilist" && t.persistent) has_anilist_toast = true;
  }
  CHECK(has_anilist_toast);

  // Recovery: a later success clears the topic.
  tick(h->app, Event{EnrichmentNull{702}});
  for (const Toast& t : h->app.toasts.visible()) CHECK(t.topic != "anilist");
}

// --- P30 parity-audit pins: view switching, narrow arms, P-add, pagination,
// resume landing, season chips ----------------------------------------------

namespace {
KeyEvent press(char32_t c) { return KeyEvent{c, KeyEvent::Special::None, false}; }
KeyEvent special_key(KeyEvent::Special s) { return KeyEvent{0, s, false}; }

CatalogRow row_for(std::int64_t aid, std::string_view title) {
  Enrichment e;
  e.anilist_id = aid;
  e.title_romaji = std::string(title);
  e.total_episodes = 12;
  return CatalogRow{e};
}
}  // namespace

TEST_CASE("same-view switch is a no-op that preserves the pane (05 §6)") {
  auto h = HistoryHarness::make();
  h->add(1, "Show", 12);
  h->reload();
  h->app.pane = Pane::Detail;
  tick(h->app, Event{press(U'H')});  // H while in History: inert.
  CHECK(h->app.view == View::History);
  CHECK(h->app.pane == Pane::Detail);

  h->app.view = View::Browse;
  h->app.pane = Pane::Detail;
  tick(h->app, Event{press(U'B')});  // B while in Browse: inert.
  CHECK(h->app.view == View::Browse);
  CHECK(h->app.pane == Pane::Detail);
}

TEST_CASE("narrow Browse Enter drills into the zoom (KEYS_AUDIT F2)") {
  // Deliberate divergence from app.rs's narrow no-op (was pinned here as
  // "05 §1.4"): after the Space rework made both narrow lists zoomable,
  // Enter now matches narrow History and drills straight into the zoom.
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  h->app.win = WinSize{50, 24, 0, 0};  // below kPaneSplitMin.
  h->app.catalog.push_back(row_for(9, "Narrow"));
  tick(h->app, Event{special_key(KeyEvent::Special::Enter)});
  CHECK(h->app.view == View::Detail);
  CHECK(h->app.detail_origin == View::Browse);

  // Empty list: Enter stays inert — no blank zoom.
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  h->app.catalog.clear();
  h->app.list_cursor = 0;
  tick(h->app, Event{special_key(KeyEvent::Special::Enter)});
  CHECK(h->app.view == View::Browse);
}

TEST_CASE("narrow History Space opens the zoom (05 §1.4)") {
  auto h = HistoryHarness::make();
  h->add(2, "Zoomed", 12);
  h->reload();
  h->app.win = WinSize{50, 24, 0, 0};
  tick(h->app, Event{press(U' ')});
  CHECK(h->app.view == View::Detail);
  CHECK(h->app.detail_origin == View::History);
}

TEST_CASE("Space is the zoom toggle on every selection surface (§9 consistency)") {
  auto h = HistoryHarness::make();
  // Wide Browse list: Space drills into the zoom (previously a no-op — the
  // promote needed a focused pane first).
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  h->app.catalog.push_back(row_for(4, "Zoomable"));
  tick(h->app, Event{press(U' ')});
  CHECK(h->app.view == View::Detail);
  CHECK(h->app.detail_origin == View::Browse);
  tick(h->app, Event{press(U' ')});  // and straight back out (§7.4 demote).
  CHECK(h->app.view == View::Browse);

  // Empty Browse list: nothing selected, Space stays inert.
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  h->app.catalog.clear();
  tick(h->app, Event{press(U' ')});
  CHECK(h->app.view == View::Browse);

  // Discover: Space lands exactly where Enter does — the zoom over the card.
  h->app.view = View::Discover;
  std::vector<CatalogRow> cards;
  cards.push_back(row_for(200, "Card"));
  h->app.discover.on_feed(DiscoverAxis::Trending, 1, cards, /*has_next=*/false,
                          nullptr, 500, h->app.discover.filter_gen());
  tick(h->app, Event{press(U' ')});
  CHECK(h->app.view == View::Detail);
  CHECK(h->app.detail_origin == View::Discover);
}

TEST_CASE("/ searches from anywhere: non-Browse/History views jump to Browse (F1)") {
  auto h = HistoryHarness::make();
  // Settings: `/` used to arm the bar over a view that never shows results
  // (and the debounce fired regardless, replacing app.catalog invisibly).
  h->app.view = View::Settings;
  tick(h->app, Event{press(U'/')});
  CHECK(h->app.view == View::Browse);
  CHECK(h->app.pane == Pane::List);
  CHECK(h->app.bar == BarMode::Search);
  h->app.bar = BarMode::Idle;

  // Detail zoom: the worst case pre-F1 — a Browse-origin zoom reads
  // app.catalog live, so an invisible search could swap the show under it.
  h->app.catalog.push_back(row_for(4, "Zoomable"));
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  tick(h->app, Event{press(U'/')});
  CHECK(h->app.view == View::Browse);
  CHECK(h->app.bar == BarMode::Search);
  h->app.bar = BarMode::Idle;

  // Schedule jumps too.
  h->app.view = View::Schedule;
  tick(h->app, Event{press(U'/')});
  CHECK(h->app.view == View::Browse);
  CHECK(h->app.bar == BarMode::Search);
  h->app.bar = BarMode::Idle;

  // History keeps its own local filter — no jump.
  h->app.view = View::History;
  h->app.pane = Pane::List;
  tick(h->app, Event{press(U'/')});
  CHECK(h->app.view == View::History);
  CHECK(h->app.bar == BarMode::Search);
}

TEST_CASE("g/G jump the Discover grid and the focused rec section (F3/F4)") {
  auto h = HistoryHarness::make();
  h->app.win = WinSize{100, 40, 0, 0};

  // F3: Discover — first/last card of the active axis slot.
  h->app.view = View::Discover;
  std::vector<CatalogRow> cards;
  for (int i = 0; i < 3; ++i) cards.push_back(row_for(300 + i, "Card"));
  h->app.discover.on_feed(DiscoverAxis::Trending, 1, cards, /*has_next=*/false,
                          nullptr, 500, h->app.discover.filter_gen());
  tick(h->app, Event{press(U'G')});
  CHECK(h->app.discover.slot().cursor == 2);
  tick(h->app, Event{press(U'g')});
  CHECK(h->app.discover.slot().cursor == 0);

  // F4: in the zoom with the `c` section Tab-focused, g/G own the
  // recommendation cursor (the rule j/k already follow); the episode grid
  // cursor must not move underneath.
  h->app.catalog.push_back(row_for(9, "Zoomed"));
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  h->app.episode.episodes = {"01", "02", "03"};
  h->app.episode.cursor = 0;
  h->app.char_recs.visible = true;
  h->app.char_recs.focused = true;
  h->app.char_recs.data.recommendations.push_back(row_for(801, "RecA").meta);
  h->app.char_recs.data.recommendations.push_back(row_for(802, "RecB").meta);
  tick(h->app, Event{press(U'G')});
  CHECK(h->app.char_recs.cursor == 1);
  CHECK(h->app.episode.cursor == 0);  // grid untouched while focus is away.
  tick(h->app, Event{press(U'g')});
  CHECK(h->app.char_recs.cursor == 0);

  // Focus back on the grid: g/G return to the episode cursor.
  h->app.char_recs.focused = false;
  tick(h->app, Event{press(U'G')});
  CHECK(h->app.episode.cursor == 2);
}

// --- schedule notices (P37 slice 3) -----------------------------------------

namespace {
// Patch a show's airing data (the P21 refresh-on-view shape the notice
// machinery actually feeds on).
void patch_airing(Store& store, std::int64_t aid, const char* title, std::int64_t at,
                  std::uint32_t episode) {
  Enrichment e;
  e.anilist_id = aid;
  e.title_romaji = title;
  e.next_airing_at = at;
  e.next_airing_episode = episode;
  REQUIRE(store.patch_show_enrichment(e, /*stamp_fresh=*/true, test_now_secs()).has_value());
}
}  // namespace

TEST_CASE("schedule notices: first look seeds quietly, the next airing raises ONE toast") {
  auto h = HistoryHarness::make();
  h->add(1, "Airing Now", 12);
  h->add(2, "Not Airing", 12);
  // ep 6 aired an hour ago; the row has never been looked at (NULL mark).
  patch_airing(h->store, 1, "Airing Now", test_now_secs() - 3600, 6);
  h->reload();
  const std::size_t toasts_before = h->app.toasts.size();

  // First look: the mark SEEDS quietly — no toast, no NEW marker (P37
  // review: a mature library's stale next_airing_* rows must not storm the
  // first post-migration boot with months-old notices).
  tick(h->app, Event{SyncFlushed{SyncOutcome::Completed, 0, 0, 0, 0}});
  CHECK(h->app.toasts.size() == toasts_before);
  {
    auto g = h->store.get_show(1);
    REQUIRE(g->has_value());
    REQUIRE((*g)->notice_last_episode.has_value());
    CHECK(*(*g)->notice_last_episode == 6);  // the episode whose airtime passed.
    CHECK_FALSE((*g)->notice_pending);
  }

  // A later refresh moves the pair forward and ep 7's airtime passes: NOW it
  // raises — ONE aggregate toast + the NEW marker.
  patch_airing(h->store, 1, "Airing Now", test_now_secs() - 60, 7);
  h->reload();
  tick(h->app, Event{SyncFlushed{SyncOutcome::Completed, 0, 0, 0, 0}});
  CHECK(h->app.toasts.size() == toasts_before + 1);
  {
    auto g = h->store.get_show(1);
    REQUIRE(g->has_value());
    CHECK(*(*g)->notice_last_episode == 7);
    CHECK((*g)->notice_pending);
  }
  {
    auto g = h->store.get_show(2);
    REQUIRE(g->has_value());
    CHECK_FALSE((*g)->notice_pending);  // never had an airing time: untouched.
  }
  // History's in-memory copy picked up the marker without a manual H reload.
  const Show* row = nullptr;
  for (const Show& r : h->app.history.rows) {
    if (r.enrichment.anilist_id == 1) row = &r;
  }
  REQUIRE(row != nullptr);
  CHECK(row->notice_pending);
}

TEST_CASE("schedule notices: a second sync-flush never re-raises the same episode") {
  auto h = HistoryHarness::make();
  h->add(1, "Airing Now", 12);
  patch_airing(h->store, 1, "Airing Now", test_now_secs() - 7200, 6);
  h->reload();
  tick(h->app, Event{SyncFlushed{SyncOutcome::Completed, 0, 0, 0, 0}});  // seeds (mark 6).
  patch_airing(h->store, 1, "Airing Now", test_now_secs() - 3600, 7);
  h->reload();

  tick(h->app, Event{SyncFlushed{SyncOutcome::Completed, 0, 0, 0, 0}});  // raises for ep 7.
  const std::size_t toasts_after_first = h->app.toasts.size();

  // Same next_airing_at/episode (no fresh enrichment landed): a second flush
  // must not raise a second toast for the same aired episode.
  tick(h->app, Event{SyncFlushed{SyncOutcome::Completed, 0, 0, 0, 0}});
  CHECK(h->app.toasts.size() == toasts_after_first);
}

TEST_CASE("schedule notices: opening the show clears NEW but keeps the dedup mark") {
  auto h = HistoryHarness::make();
  h->add(1, "Airing Now", 12);
  patch_airing(h->store, 1, "Airing Now", test_now_secs() - 7200, 6);
  h->reload();
  tick(h->app, Event{SyncFlushed{SyncOutcome::Completed, 0, 0, 0, 0}});  // seeds (mark 6).
  patch_airing(h->store, 1, "Airing Now", test_now_secs() - 3600, 7);
  h->reload();
  tick(h->app, Event{SyncFlushed{SyncOutcome::Completed, 0, 0, 0, 0}});  // raises for ep 7.
  {
    auto g = h->store.get_show(1);
    REQUIRE(g->has_value());
    CHECK((*g)->notice_pending);
  }

  // Enter on the History row opens detail — clear-on-open, regardless of the
  // (store-less-registry) degraded resolve path this harness exercises.
  h->app.history.select_aid(1, 24);
  tick(h->app, Event{special_key(KeyEvent::Special::Enter)});

  auto g = h->store.get_show(1);
  REQUIRE(g->has_value());
  CHECK_FALSE((*g)->notice_pending);          // marker cleared.
  REQUIRE((*g)->notice_last_episode.has_value());
  CHECK(*(*g)->notice_last_episode == 7);     // dedup mark untouched.

  // A further sync-flush with the same stale airing data must not re-raise.
  const std::size_t toasts_before = h->app.toasts.size();
  tick(h->app, Event{SyncFlushed{SyncOutcome::Completed, 0, 0, 0, 0}});
  CHECK(h->app.toasts.size() == toasts_before);
}

// --- P38 filter overlay: app-level dispatch (review follow-up) --------------

TEST_CASE("filter overlay: Discover-only, keys swallowed, Esc discards, Enter commits") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  tick(h->app, Event{press(U'f')});
  CHECK_FALSE(h->app.discover_filters.visible);  // f is Discover's own key.

  h->app.view = View::Discover;
  tick(h->app, Event{press(U'f')});
  REQUIRE(h->app.discover_filters.visible);

  // The overlay swallows the underlying view's keys (connect-modal tier).
  tick(h->app, Event{press(U'B')});
  CHECK(h->app.view == View::Discover);
  CHECK(h->app.discover_filters.visible);

  // Edit the draft (j to the Year row, l cycles a year in), then Esc: the
  // draft is DISCARDED — nothing committed, and re-opening re-seeds from the
  // committed (empty) set.
  tick(h->app, Event{press(U'j')});
  tick(h->app, Event{press(U'l')});
  CHECK_FALSE(h->app.discover_filters.draft.empty());
  tick(h->app, Event{special_key(KeyEvent::Special::Escape)});
  CHECK_FALSE(h->app.discover_filters.visible);
  CHECK(h->app.discover.filters().empty());

  tick(h->app, Event{press(U'f')});
  CHECK(h->app.discover_filters.draft.empty());  // Esc really discarded.

  // c = clear-all on a re-edited draft.
  tick(h->app, Event{press(U'j')});
  tick(h->app, Event{press(U'l')});
  REQUIRE_FALSE(h->app.discover_filters.draft.empty());
  tick(h->app, Event{press(U'c')});
  CHECK(h->app.discover_filters.draft.empty());

  // Enter commits: filters land on DiscoverState, generation bumps once.
  tick(h->app, Event{press(U'j')});   // -> Status row.
  tick(h->app, Event{press(U'l')});   // -> RELEASING.
  const std::uint32_t gen_before = h->app.discover.filter_gen();
  tick(h->app, Event{special_key(KeyEvent::Special::Enter)});
  CHECK_FALSE(h->app.discover_filters.visible);
  CHECK_FALSE(h->app.discover.filters().empty());
  CHECK(h->app.discover.filter_gen() == gen_before + 1);
}

TEST_CASE("schedule countdown text boundary table (P37 DoD)") {
  using shigoku::tui::detail::countdown_text;
  CHECK(countdown_text(0) == "<1m");
  CHECK(countdown_text(59) == "<1m");
  CHECK(countdown_text(60) == "1m");
  CHECK(countdown_text(37 * 60) == "37m");
  CHECK(countdown_text(3600) == "1h 0m");
  CHECK(countdown_text(4 * 3600 + 12 * 60) == "4h 12m");
  CHECK(countdown_text(24 * 3600) == "1d 0h");
  CHECK(countdown_text(2 * 24 * 3600 + 4 * 3600 + 30 * 60) == "2d 4h");
  CHECK(countdown_text(9 * 24 * 3600) == "9d 0h");
}

TEST_CASE("Browse P adds the selected row to the watchlist (05 §4)") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  h->app.catalog.push_back(row_for(77, "Planned From Browse"));
  h->app.list_cursor = 0;
  tick(h->app, Event{press(U'P')});
  auto g = h->store.get_show(77);
  REQUIRE(g.has_value());
  REQUIRE(g->has_value());
  CHECK((*g)->list_status == ListStatus::Planning);
  // And the Browse detail pane is untouched (add, not open).
  CHECK(h->app.view == View::Browse);
}

TEST_CASE("search pagination: page 2 appends and keeps cursor; out-of-order drops") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.search_query = "frieren";
  tick(h->app, Event{SearchDone{{row_for(1, "A"), row_for(2, "B")},
                                "frieren", 1, /*has_next=*/true}});
  CHECK(h->app.catalog.size() == 2);
  CHECK(h->app.search_answered == "frieren");
  CHECK(h->app.search_has_next);
  h->app.list_cursor = 1;

  tick(h->app, Event{SearchDone{{row_for(3, "C"), row_for(4, "D")},
                                "frieren", 2, /*has_next=*/false}});
  CHECK(h->app.catalog.size() == 4);       // appended.
  CHECK(h->app.list_cursor == 1);          // cursor kept (05 §16).
  CHECK_FALSE(h->app.search_has_next);
  CHECK(h->app.search_page == 2);

  // Out-of-order page: dropped whole.
  tick(h->app, Event{SearchDone{{row_for(9, "X")}, "frieren", 4, true}});
  CHECK(h->app.catalog.size() == 4);

  // Applied rows landed in catalog_cache (04 §10 nulls-only merge rides it).
  auto hit = h->store.get_catalog(3);
  REQUIRE(hit.has_value());
  CHECK(hit->has_value());
}

TEST_CASE("search failure raises the persistent AniList topic; an applied answer clears it") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.search_query = "frieren";
  tick(h->app, Event{SearchError{ProviderError{}, "frieren"}});
  bool has_topic = false;
  for (const Toast& t : h->app.toasts.visible()) {
    if (t.topic == "anilist" && t.persistent) has_topic = true;
  }
  CHECK(has_topic);

  tick(h->app, Event{SearchDone{{row_for(1, "A")}, "frieren", 1, false}});
  for (const Toast& t : h->app.toasts.visible()) CHECK(t.topic != "anilist");
}

TEST_CASE("browse g/G jump list ends; grid consumes on a detail surface (05 §1.1)") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  for (int i = 0; i < 40; ++i) h->app.catalog.push_back(row_for(i + 1, "Row"));
  tick(h->app, Event{press(U'G')});
  CHECK(h->app.list_cursor == 39);
  CHECK(h->app.browse_scroll > 0);  // scrolled into view (21 content rows).
  tick(h->app, Event{press(U'g')});
  CHECK(h->app.list_cursor == 0);
  CHECK(h->app.browse_scroll == 0);

  // A focused grid consumes the jump instead of the list.
  h->app.pane = Pane::Detail;
  h->app.episode.episodes = {"1", "2", "3"};
  h->app.episode.cursor = 1;
  tick(h->app, Event{press(U'G')});
  CHECK(h->app.episode.cursor == 2);
  CHECK(h->app.list_cursor == 0);  // list untouched.
}

// --- P33: episode-grid arrow/hjkl navigation --------------------------------
// Inside the focused grid, j/k hop a visual row (per_row from
// the grid geometry, clamped) instead of the flat +/-1 the list panes use;
// h/l step the cursor left/right one cell. Everywhere else (list panes,
// Discover) keeps its existing bindings untouched.

TEST_CASE("episode grid: j/k step one visual row (per_row), not a flat +-1") {
  auto h = HistoryHarness::make();
  h->app.pane = Pane::Detail;
  // win.cols == 100 -> pane_split(100).detail_w == 57 -> per_row == 57/5 == 11.
  REQUIRE(detail::episode_grid_per_row(h->app) == 11);
  for (int i = 0; i < 30; ++i) h->app.episode.episodes.push_back(std::to_string(i));
  h->app.episode.cursor = 0;

  tick(h->app, Event{press(U'j')});
  CHECK(h->app.episode.cursor == 11);
  tick(h->app, Event{press(U'j')});
  CHECK(h->app.episode.cursor == 22);
  tick(h->app, Event{press(U'j')});
  CHECK(h->app.episode.cursor == 29);  // clamped at the last cell, not wrapped.

  tick(h->app, Event{press(U'k')});
  CHECK(h->app.episode.cursor == 18);
  h->app.episode.cursor = 5;
  tick(h->app, Event{press(U'k')});
  CHECK(h->app.episode.cursor == 0);  // clamped at 0, not negative.
}

TEST_CASE("episode grid: h/l step the cursor one cell left/right") {
  auto h = HistoryHarness::make();
  h->app.pane = Pane::Detail;
  h->app.episode.episodes = {"1", "2", "3"};
  h->app.episode.cursor = 1;

  tick(h->app, Event{press(U'l')});
  CHECK(h->app.episode.cursor == 2);
  tick(h->app, Event{press(U'l')});
  CHECK(h->app.episode.cursor == 2);  // clamped: no wrap past the last cell.

  tick(h->app, Event{press(U'h')});
  CHECK(h->app.episode.cursor == 1);
  CHECK(h->app.pane == Pane::Detail);  // h stayed in the grid: no demote.
  tick(h->app, Event{press(U'h')});
  CHECK(h->app.episode.cursor == 0);
  tick(h->app, Event{press(U'h')});
  CHECK(h->app.episode.cursor == 0);  // clamped at 0.
  CHECK(h->app.pane == Pane::Detail);  // still no "back to list" — Esc/space own that.
}

TEST_CASE("episode grid: h/j/k/l outside the grid keep their non-grid bindings") {
  auto h = HistoryHarness::make();
  h->add(1, "Show", 12);
  h->add(2, "Other", 12);
  h->reload();
  REQUIRE(h->app.pane == Pane::List);
  h->app.episode.episodes = {"1", "2", "3"};  // stale grid state from a prior open.
  h->app.episode.cursor = 1;

  // List pane: j/k move the row cursor, h/l are list-pane bindings — the
  // episode cursor must not move.
  tick(h->app, Event{press(U'j')});
  CHECK(h->app.episode.cursor == 1);  // untouched.
  tick(h->app, Event{press(U'h')});
  CHECK(h->app.episode.cursor == 1);  // untouched.

  // Discover: h/l cycle the axis, not the (irrelevant, unfetched) grid.
  h->app.view = View::Discover;
  tick(h->app, Event{press(U'l')});
  CHECK(h->app.episode.cursor == 1);  // untouched.
}

TEST_CASE("resume landing fires once on the first real geometry (05 §10.6)") {
  auto h = HistoryHarness::make();
  h->add(5, "Resumed", 12);
  // A played row: record a finish so first_played() finds it.
  REQUIRE(h->store.record_finish(5, Translation::Sub, "1", 1, 96.0, 100.0,
                                 std::nullopt, 500)
              .has_value());
  h->reload();
  h->app.resume_pending = true;  // run() arms this under landing=last_watched.
  tick(h->app, Event{Resize{100, 24, 0, 0}});
  CHECK_FALSE(h->app.resume_pending);       // one-shot.
  CHECK(h->app.view == View::History);
  CHECK(h->app.pane == Pane::Detail);       // two-pane: in-pane grid, no zoom.

  // Narrow variant drills into the zoom instead.
  auto n = HistoryHarness::make();
  n->add(6, "Narrow Resume", 12);
  REQUIRE(n->store.record_finish(6, Translation::Sub, "1", 1, 96.0, 100.0,
                                 std::nullopt, 500)
              .has_value());
  n->reload();
  n->app.resume_pending = true;
  tick(n->app, Event{Resize{50, 24, 0, 0}});
  CHECK(n->app.view == View::Detail);
  CHECK(n->app.detail_origin == View::History);
}

TEST_CASE("season chips: kanji cour form, absent-not-empty, english fallback") {
  // chrome.rs cour_chip_matches_anilist_seasons, ported.
  CHECK(cour_chip(Cour{Season::Winter, 2026}, true) == "\xE5\x86\xAC 2026");
  CHECK(cour_chip(Cour{Season::Fall, 2025}, false) == "Autumn 2025");
  CHECK(season_chip(Season::Summer, 2024, true) ==
        std::optional<std::string>("\xE5\xA4\x8F 2024"));
  CHECK(season_chip(std::nullopt, 2024, true) == std::nullopt);   // no season.
  CHECK(season_chip(Season::Spring, std::nullopt, true) == std::nullopt);  // no year.
}

// --- P32 mouse (§9): hit-testing + on_mouse dispatch ------------------------

namespace {
Event click(int x, int y) {
  return Event{MouseEvent{MouseEvent::Kind::Press, MouseEvent::Button::Left, x, y}};
}
}  // namespace

TEST_CASE("top_bar_tab_at: spans replay draw_top_bar's walk") {
  using shigoku::tui::detail::top_bar_tab_at;
  // Tabs start at 11 ("SHIGOKU" + glyph + spacing): [B]rowse(8) · [H]istory(9)
  // · [D]iscover(10) · [C]alendar(10, P37) · [S]ettings(10), " · " = 3 between.
  CHECK(top_bar_tab_at(10) == std::nullopt);
  CHECK(top_bar_tab_at(11) == View::Browse);
  CHECK(top_bar_tab_at(18) == View::Browse);
  CHECK(top_bar_tab_at(19) == std::nullopt);   // separator.
  CHECK(top_bar_tab_at(22) == View::History);
  CHECK(top_bar_tab_at(30) == View::History);
  CHECK(top_bar_tab_at(34) == View::Discover);
  CHECK(top_bar_tab_at(43) == View::Discover);  // last char: 10 wide from 34.
  CHECK(top_bar_tab_at(47) == View::Schedule);
  CHECK(top_bar_tab_at(56) == View::Schedule);  // last char: 10 wide from 47.
  CHECK(top_bar_tab_at(60) == View::Settings);
  CHECK(top_bar_tab_at(69) == View::Settings);  // last char: 10 wide from 60.
  CHECK(top_bar_tab_at(70) == std::nullopt);   // past the last tab.
}

TEST_CASE("mouse: top-bar tab click rides the view-letter key path") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  tick(h->app, click(23, 0));  // inside [H]istory.
  CHECK(h->app.view == View::History);
  tick(h->app, click(12, 0));  // inside [B]rowse.
  CHECK(h->app.view == View::Browse);
  tick(h->app, click(20, 0));  // separator: no-op.
  CHECK(h->app.view == View::Browse);
}

TEST_CASE("mouse: Browse click selects, double-click opens the detail pane") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  for (int i = 0; i < 5; ++i) h->app.catalog.push_back(row_for(i + 1, "Row"));

  tick(h->app, click(3, 4));  // content row y=4 → catalog index 2 (y0=2).
  CHECK(h->app.list_cursor == 2);
  CHECK(h->app.pane == Pane::List);

  // Second press on the same cell within the window: the view's Enter.
  tick(h->app, click(3, 4));
  CHECK(h->app.pane == Pane::Detail);
  CHECK(h->app.episode.for_id == 3);  // open_detail engaged the selected row.
}

TEST_CASE("mouse: a moved or late second click never doubles") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  for (int i = 0; i < 5; ++i) h->app.catalog.push_back(row_for(i + 1, "Row"));

  tick(h->app, click(3, 2));
  tick(h->app, click(3, 3));  // different row: select only.
  CHECK(h->app.list_cursor == 1);
  CHECK(h->app.pane == Pane::List);

  // Same cell but outside the 400ms window: select only.
  h->app.tick_count += kDoubleClickTicks + 1;
  tick(h->app, click(3, 3));
  CHECK(h->app.pane == Pane::List);
}

TEST_CASE("mouse: inert while the search bar is armed, cancels an armed confirm") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  for (int i = 0; i < 3; ++i) h->app.catalog.push_back(row_for(i + 1, "Row"));
  h->app.bar = BarMode::Search;
  tick(h->app, click(3, 3));
  CHECK(h->app.list_cursor == 0);  // P32 ledger: mouse OFF while `/` is live.
  h->app.bar = BarMode::Idle;

  // Armed hard-delete: a click is the "anything else cancels" arm (§6.5).
  h->app.confirm_delete = 42;
  tick(h->app, click(3, 3));
  CHECK_FALSE(h->app.confirm_delete.has_value());
  CHECK(h->app.list_cursor == 0);  // the cancel consumed the click.
}

namespace {
Event wheel(bool up, int x, int y) {
  return Event{MouseEvent{up ? MouseEvent::Kind::WheelUp : MouseEvent::Kind::WheelDown,
                          MouseEvent::Button::None, x, y}};
}
}  // namespace

TEST_CASE("mouse: wheel aliases the view's Up/Down — Settings rows move") {
  auto h = HistoryHarness::make();
  h->app.view = View::Settings;
  CHECK(h->app.settings.cursor() == 0);
  tick(h->app, wheel(false, 10, 5));
  CHECK(h->app.settings.cursor() == 1);
  tick(h->app, wheel(false, 10, 5));
  CHECK(h->app.settings.cursor() == 2);
  tick(h->app, wheel(true, 10, 5));
  CHECK(h->app.settings.cursor() == 1);
}

TEST_CASE("mouse: wheel scrolls the Browse list and respects the input gates") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  for (int i = 0; i < 5; ++i) h->app.catalog.push_back(row_for(i + 1, "Row"));

  tick(h->app, wheel(false, 3, 3));  // position-blind: aliases Down.
  CHECK(h->app.list_cursor == 1);
  tick(h->app, wheel(true, 3, 3));
  CHECK(h->app.list_cursor == 0);

  // Armed search bar owns input — a notch must not move the list under it.
  h->app.bar = BarMode::Search;
  tick(h->app, wheel(false, 3, 3));
  CHECK(h->app.list_cursor == 0);
  h->app.bar = BarMode::Idle;

  // Wheel traffic between clicks leaves the double-click machinery intact.
  tick(h->app, click(3, 3));         // select row 1, arm the double.
  tick(h->app, wheel(false, 3, 3));  // notch moves the cursor off the row...
  CHECK(h->app.list_cursor == 2);
  tick(h->app, click(3, 3));  // ...so this click re-selects (not a double),
  CHECK(h->app.list_cursor == 1);
  CHECK(h->app.pane == Pane::List);
  tick(h->app, click(3, 3));  // and the next one doubles: Enter → detail.
  CHECK(h->app.pane == Pane::Detail);
}

TEST_CASE("frame chrome: row 1 is a spacer, content starts at kContentY0 (frame_rows)") {
  // layout.rs frame_rows: top bar, spacer, content, bottom bar. The spacer
  // is the visual separation the top bar owes the content (and the cover).
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.pane = Pane::List;
  for (int i = 0; i < 3; ++i) h->app.catalog.push_back(row_for(i + 1, "Row"));
  CellBuffer buf;
  draw(h->app, buf);
  for (int x = 0; x < buf.width(); ++x) CHECK(buf.at(x, 1).glyph == U' ');
  bool content_on_origin = false;
  for (int x = 0; x < buf.width(); ++x) {
    if (buf.at(x, kContentY0).glyph != U' ') content_on_origin = true;
  }
  CHECK(content_on_origin);  // the first list row renders on the origin row.
}

TEST_CASE("zoom cover scales with the canvas: gallery floor, half-height growth") {
  auto h = HistoryHarness::make();
  CatalogRow row = row_for(7, "Poster");
  row.meta.cover_url = "https://c/poster.jpg";
  h->app.catalog.push_back(row);
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  h->app.cover_caps.cell = CellPx{9, 20};

  // 40-row frame: content_h = 37, growth target 37/2 = 18 rows → 26 cols wide
  // (poster_cols), rows re-derived from the 2:3 aspect = 17.
  h->app.win = WinSize{100, 40, 0, 0};
  const Rect grown = detail::cover_rect(h->app);
  CHECK(grown.x == 2);
  CHECK(grown.y == kContentY0);
  CHECK(grown.w == 26);
  CHECK(grown.h == 17);

  // 24-row frame: the growth target (content_h/2 = 10) falls under the
  // gallery-card floor — the tier box holds, same as the pre-§9 behavior.
  h->app.win = WinSize{100, 24, 0, 0};
  const Rect floor = detail::cover_rect(h->app);
  CHECK(floor.w == 20);
  CHECK(floor.h == 13);
}

TEST_CASE("wrap_text: display-column word wrap (zoom synopsis)") {
  using shigoku::tui::detail::wrap_text;
  const auto lines = wrap_text("alpha beta gamma", 10);
  REQUIRE(lines.size() == 2);
  CHECK(lines[0] == "alpha beta");  // exactly 10 columns fits.
  CHECK(lines[1] == "gamma");

  // A word wider than the box hard-breaks on a codepoint boundary.
  const auto hard = wrap_text("abcdefghijkl", 5);
  REQUIRE(hard.size() == 3);
  CHECK(hard[0] == "abcde");
  CHECK(hard[1] == "fghij");
  CHECK(hard[2] == "kl");

  // Wide glyphs budget 2 columns: 4 kana at 4 cols = 2 per line.
  const auto cjk = wrap_text("\xE3\x81\x82\xE3\x81\x82 \xE3\x81\x82\xE3\x81\x82", 4);
  REQUIRE(cjk.size() == 2);
  CHECK(cjk[0] == "\xE3\x81\x82\xE3\x81\x82");

  // Space runs collapse; empty/blank input wraps to nothing.
  CHECK(wrap_text("a  b", 10) == std::vector<std::string>{"a b"});
  CHECK(wrap_text("", 10).empty());
  CHECK(wrap_text("   ", 10).empty());
}

TEST_CASE("synopsis renders right of the cover (zoom and two-pane alike)") {
  auto h = HistoryHarness::make();
  CatalogRow row = row_for(7, "Poster");
  row.meta.cover_url = "https://c/poster.jpg";
  row.meta.description = "An epic tale of terminals and posters, told here.";
  h->app.catalog.push_back(row);
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  h->app.cover_caps.cell = CellPx{9, 20};
  h->app.win = WinSize{100, 40, 0, 0};

  CellBuffer buf;
  draw(h->app, buf);
  auto row_text = [&](int y) {
    std::string s;
    for (int x = 0; x < buf.width(); ++x) s += static_cast<char>(buf.at(x, y).glyph & 0x7F);
    return s;
  };
  // The scaled cover spans x=[2,28); the panel opens on the cover's top row,
  // clear of the poster box, body wrapped beneath the header.
  const std::string top = row_text(kContentY0);
  const std::size_t at = top.find("Synopsis");
  REQUIRE(at != std::string::npos);
  CHECK(at >= 28u);
  CHECK(row_text(kContentY0 + 1).find("An epic tale") != std::string::npos);

  // No description → the region stays blank (no orphaned header).
  h->app.catalog[0].meta.description = std::nullopt;
  draw(h->app, buf);
  CHECK(row_text(kContentY0).find("Synopsis") == std::string::npos);
  h->app.catalog[0].meta.description = "back";

  // The two-pane Browse detail pane carries the panel too
  // (every selection shows its synopsis, like the manga app).
  h->app.view = View::Browse;
  h->app.pane = Pane::Detail;
  draw(h->app, buf);
  bool saw = false;
  for (int y = 0; y < 40; ++y) {
    if (row_text(y).find("Synopsis") != std::string::npos) saw = true;
  }
  CHECK(saw);
}

TEST_CASE("Esc walks back to the previous view, one-shot (HW #6)") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;

  // Browse → Discover, Esc returns; a second Esc is inert (the slot popped).
  tick(h->app, Event{press(U'D')});
  REQUIRE(h->app.view == View::Discover);
  tick(h->app, Event{special_key(KeyEvent::Special::Escape)});
  CHECK(h->app.view == View::Browse);
  tick(h->app, Event{special_key(KeyEvent::Special::Escape)});
  CHECK(h->app.view == View::Browse);  // never forward again.

  // Settings falls through its own key handler to the same back-walk, and
  // the pop rides the letter path (persist-on-leave semantics included).
  tick(h->app, Event{press(U'S')});
  REQUIRE(h->app.view == View::Settings);
  tick(h->app, Event{special_key(KeyEvent::Special::Escape)});
  CHECK(h->app.view == View::Browse);

  // Two hops remember only the last origin: B → D → S, Esc lands on D.
  tick(h->app, Event{press(U'D')});
  tick(h->app, Event{press(U'S')});
  tick(h->app, Event{special_key(KeyEvent::Special::Escape)});
  CHECK(h->app.view == View::Discover);
}

// --- P34 `s` score prompt: dispatch coverage (review follow-up) -------------

namespace {
Event ch(char32_t c) { return Event{KeyEvent{c, KeyEvent::Special::None, false}}; }
Event special(KeyEvent::Special s) { return Event{KeyEvent{0, s, false}}; }
}  // namespace

TEST_CASE("score prompt: s opens pre-filled, Enter commits and reloads") {
  auto h = HistoryHarness::make();
  h->add(1, "Alpha", 12);
  REQUIRE(h->store.set_user_score(1, 75).has_value());
  h->reload();
  REQUIRE(h->app.history.count() == 1);

  tick(h->app, ch(U's'));
  CHECK(h->app.bar == BarMode::Score);
  CHECK(h->app.score_input == "7.5");  // pre-filled, trailing zeros trimmed.

  for (int i = 0; i < 3; ++i) tick(h->app, special(KeyEvent::Special::Backspace));
  tick(h->app, ch(U'9'));
  tick(h->app, special(KeyEvent::Special::Enter));
  CHECK(h->app.bar == BarMode::Idle);
  auto g = h->store.get_show(1);
  REQUIRE(g->has_value());
  REQUIRE((*g)->user_score.has_value());
  CHECK(*(*g)->user_score == 90);
}

TEST_CASE("score prompt: Esc cancels, keystroke filter holds, empty clears") {
  auto h = HistoryHarness::make();
  h->add(1, "Alpha", 12);
  REQUIRE(h->store.set_user_score(1, 60).has_value());
  h->reload();

  // Esc: no write.
  tick(h->app, ch(U's'));
  tick(h->app, ch(U'9'));
  tick(h->app, special(KeyEvent::Special::Escape));
  CHECK(h->app.bar == BarMode::Idle);
  {
    auto g = h->store.get_show(1);
    CHECK(*(*g)->user_score == 60);
  }

  // Per-keystroke filter: digits + one '.' only.
  tick(h->app, ch(U's'));
  CHECK(h->app.score_input == "6");
  tick(h->app, ch(U'x'));  // rejected.
  tick(h->app, ch(U'.'));
  tick(h->app, ch(U'.'));  // second dot rejected.
  tick(h->app, ch(U'5'));
  CHECK(h->app.score_input == "6.5");
  tick(h->app, special(KeyEvent::Special::Escape));

  // Empty input clears the score.
  tick(h->app, ch(U's'));
  while (!h->app.score_input.empty()) tick(h->app, special(KeyEvent::Special::Backspace));
  tick(h->app, special(KeyEvent::Special::Enter));
  {
    auto g = h->store.get_show(1);
    CHECK_FALSE((*g)->user_score.has_value());
  }
}

TEST_CASE("mouse: inert while the score prompt is armed") {
  // A mid-prompt click would silently retarget the commit — Enter re-reads
  // the selected row — so the mouse must be dead until the prompt resolves
  // (the same P32 ledger rule as the `/` search bar).
  auto h = HistoryHarness::make();
  h->add(1, "Alpha", 12);
  h->add(2, "Beta", 12);
  h->reload();
  REQUIRE(h->app.history.count() == 2);
  CHECK(h->app.history.cursor == 0);

  tick(h->app, ch(U's'));
  REQUIRE(h->app.bar == BarMode::Score);
  tick(h->app, ch(U'9'));
  tick(h->app, click(3, 6));  // the second entry's title row (see below).
  CHECK(h->app.history.cursor == 0);  // swallowed: the commit target held.
  tick(h->app, special(KeyEvent::Special::Enter));

  auto a = h->store.get_show(1);
  auto b = h->store.get_show(2);
  REQUIRE((*a)->user_score.has_value());
  CHECK(*(*a)->user_score == 90);
  CHECK_FALSE((*b)->user_score.has_value());
}

TEST_CASE("mouse: History click selects by ord; double-click opens detail") {
  auto h = HistoryHarness::make();
  h->add(1, "Alpha", 12);
  h->add(2, "Beta", 12);
  h->reload();
  REQUIRE(h->app.history.count() == 2);
  // One group: Header(0) Rule(1) Title0(2) Bar0(3) Title1(4) Bar1(5) → the
  // second entry's title row renders at y = 2 + 4.
  tick(h->app, click(3, 6));
  CHECK(h->app.history.cursor == 1);
  CHECK(h->app.pane == Pane::List);
  tick(h->app, click(3, 6));  // double: Enter → in-pane detail (wide).
  CHECK(h->app.pane == Pane::Detail);
}

TEST_CASE("mouse: Settings row click moves the cursor; double-click edits") {
  auto h = HistoryHarness::make();
  h->app.view = View::Settings;
  // Layout: Header(0) Rule(1) rows 0..4 at lines 2..6 → row 2 renders at y=6.
  tick(h->app, click(10, 6));
  CHECK(h->app.settings.cursor() == 2);
  // mpv path (row 0, a Text row) at y=4: select, then double-click to edit.
  tick(h->app, click(10, 4));
  CHECK(h->app.settings.cursor() == 0);
  CHECK_FALSE(h->app.settings.editing());
  tick(h->app, click(10, 4));
  CHECK(h->app.settings.editing());  // Enter on a Text row (05 §13).
}

TEST_CASE("mouse: Discover axis tab and card clicks select") {
  auto h = HistoryHarness::make();
  h->app.view = View::Discover;
  // Axis bar spans: "[1] Trending"(12) at x=2 · "[2] Popular"(11) at x=17,
  // on the content origin row (y=2).
  tick(h->app, click(18, 2));
  CHECK(h->app.discover.axis() == DiscoverAxis::Popular);
  tick(h->app, click(3, 2));
  CHECK(h->app.discover.axis() == DiscoverAxis::Trending);

  // Feed six cards into the active slot, then click card 1's cover rect.
  std::vector<CatalogRow> rows;
  for (int i = 0; i < 6; ++i) rows.push_back(row_for(100 + i, "Card"));
  h->app.discover.on_feed(DiscoverAxis::Trending, 1, rows, /*has_next=*/false,
                          nullptr, 500, h->app.discover.filter_gen());
  const GridGeo geo = grid_geo(100, 21, h->app.cover_caps.cell);
  const Rect r1 = card_cover_rect(geo, 1, 0, 2, 4, 100, 23);
  REQUIRE_FALSE(r1.empty());
  tick(h->app, click(r1.x + 1, r1.y));
  CHECK(h->app.discover.slot().cursor == 1);
  // The caption rows under the cover belong to the same card.
  tick(h->app, click(r1.x, r1.y + r1.h + 1));
  CHECK(h->app.discover.slot().cursor == 1);
}

TEST_CASE("mouse: episode cells hit-test against the drawn grid and select") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.pane = Pane::Detail;
  h->app.catalog.push_back(row_for(7, "Gridded"));
  h->app.episode.for_id = 7;
  h->app.episode.fetched = true;
  h->app.episode.provider_id = "gridded";
  for (int i = 1; i <= 8; ++i) h->app.episode.episodes.push_back(std::to_string(i));

  // Collect every cell's origin by scanning the content region.
  std::vector<std::pair<int, int>> origin(8, {-1, -1});
  for (int y = 1; y < 23; ++y) {
    for (int x = 0; x < 100; ++x) {
      if (const auto c = shigoku::tui::detail::episode_cell_at(h->app, x, y);
          c.has_value() && origin[*c].first < 0) {
        origin[*c] = {x, y};
      }
    }
  }
  for (const auto& [ox, oy] : origin) REQUIRE(ox >= 0);  // all 8 cells on screen.

  // Draw-oracle: each cell origin renders its "[label]" bracket.
  CellBuffer buf;
  draw(h->app, buf);
  for (const auto& [ox, oy] : origin) CHECK(buf.at(ox, oy).glyph == U'[');

  // Click cell 5: cursor follows, focus stays on the detail surface.
  tick(h->app, click(origin[5].first, origin[5].second));
  CHECK(h->app.episode.cursor == 5);
  CHECK(h->app.pane == Pane::Detail);

  // Click a list row: focus returns to the list (focus follows the click).
  tick(h->app, click(3, 2));
  CHECK(h->app.pane == Pane::List);
  CHECK(h->app.list_cursor == 0);
}

// --- P36: detail zoom `c` section (characters + recommendations) -----------

namespace {
CharactersAndRecommendations sample_char_recs() {
  CharactersAndRecommendations cr;
  cr.characters.push_back(CharacterEntry{"Frieren", "MAIN", std::string("Atsumi Tanezaki")});
  cr.characters.push_back(CharacterEntry{"Sein", "SUPPORTING", std::nullopt});
  cr.recommendations.push_back(row_for(101922, "Kimetsu no Yaiba").meta);
  cr.recommendations.push_back(row_for(21, "One Piece").meta);
  return cr;
}
}  // namespace

TEST_CASE("c toggles the zoom section visible/hidden and always drops focus on hide") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.catalog.push_back(row_for(700, "Frieren"));
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;

  CHECK_FALSE(h->app.char_recs.visible);
  tick(h->app, Event{press(U'c')});
  CHECK(h->app.char_recs.visible);

  h->app.char_recs.focused = true;  // simulate a prior Tab into the section.
  tick(h->app, Event{press(U'c')});  // hide again.
  CHECK_FALSE(h->app.char_recs.visible);
  CHECK_FALSE(h->app.char_recs.focused);  // re-opening must always land on the grid.
}

TEST_CASE("History-list c still transitions status; disjoint from the zoom toggle") {
  auto h = HistoryHarness::make();
  h->add(9, "Show", 12);
  h->reload();
  REQUIRE(h->app.view == View::History);
  tick(h->app, Event{press(U'c')});
  auto g = h->store.get_show(9);
  REQUIRE(g->has_value());
  CHECK((*g)->list_status == ListStatus::Completed);
  CHECK_FALSE(h->app.char_recs.visible);  // untouched: not the Detail zoom.
}

TEST_CASE("CharactersRecsDone/Null/Failed: session-cached only, stale answers dropped") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.catalog.push_back(row_for(700, "Frieren"));
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  h->app.char_recs.visible = true;
  h->app.char_recs.for_id = 700;
  h->app.char_recs.loading = true;

  tick(h->app, Event{CharactersRecsDone{700, sample_char_recs()}});
  CHECK_FALSE(h->app.char_recs.loading);
  CHECK(h->app.char_recs.fetched);
  CHECK_FALSE(h->app.char_recs.failed);
  CHECK(h->app.char_recs.data.characters.size() == 2);
  CHECK(h->app.char_recs.data.recommendations.size() == 2);
  // Session-cached only: no store row exists for a recommendation yet.
  CHECK_FALSE(h->store.get_catalog(101922)->has_value());

  // A stale answer for a show the zoom has since left is dropped WHOLE —
  // including its loading clear: when a fetch is out for the current show,
  // `loading` belongs to IT, and a stale answer unlatching the single-flight
  // let the reconciler spawn a duplicate (P36 review).
  h->app.char_recs.loading = true;
  tick(h->app, Event{CharactersRecsFailed{999}});
  CHECK(h->app.char_recs.loading);       // survives: it guards the live fetch.
  CHECK_FALSE(h->app.char_recs.failed);  // the stale for_id=999 answer is dropped.
  CHECK(h->app.char_recs.fetched);       // the real for_id=700 answer stands.

  tick(h->app, Event{CharactersRecsNull{700}});
  CHECK(h->app.char_recs.fetched);
  CHECK(h->app.char_recs.data.characters.empty());  // Null clears to an empty answer.

  h->app.char_recs.loading = true;
  tick(h->app, Event{CharactersRecsFailed{700}});
  CHECK(h->app.char_recs.failed);
  bool has_anilist_toast = false;
  for (const Toast& t : h->app.toasts.visible()) {
    if (t.topic == "anilist" && t.persistent) has_anilist_toast = true;
  }
  CHECK(has_anilist_toast);
}

TEST_CASE("reconcile_char_recs: zoom-only, failed is terminal, c re-open retries") {
  // The first test to actually WIRE deps.char_recs (P36 review found the
  // reconciler had zero coverage): a counting fetch fn + a live queue, so
  // the spawn gating is observed end-to-end.
  auto h = HistoryHarness::make();
  EventQueue queue;
  auto calls = std::make_shared<std::atomic<int>>(0);
  h->deps.char_recs = [calls](std::int64_t)
      -> Result<std::optional<CharactersAndRecommendations>, ProviderError> {
    calls->fetch_add(1);
    return err(ProviderError::network());
  };
  h->app.queue = &queue;
  h->app.catalog.push_back(row_for(700, "Frieren"));
  h->app.list_cursor = 0;

  // Drain the queue until the worker's answer lands (bounded), proving the
  // detached thread finished before the harness leaves scope.
  auto pump_one = [&]() -> std::optional<Event> {
    for (int i = 0; i < 50; ++i) {  // <= ~5s of 100ms waits; real case: one.
      if (auto ev = queue.wait_next(); ev.has_value()) return ev;
    }
    return std::nullopt;
  };

  // visible=true outside the Detail zoom: NO fetch — the section neither
  // renders nor spends network there (the review's per-cursor-move POST bug).
  h->app.view = View::Browse;
  h->app.char_recs.visible = true;
  tick(h->app, Event{Tick{}});
  CHECK(calls->load() == 0);
  CHECK_FALSE(h->app.char_recs.loading);

  // In the zoom: exactly one fetch spawns; its failure lands as terminal.
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  tick(h->app, Event{Tick{}});
  CHECK(h->app.char_recs.loading);
  auto ev = pump_one();
  REQUIRE(ev.has_value());
  tick(h->app, *ev);  // CharactersRecsFailed{700}.
  CHECK(h->app.char_recs.failed);
  CHECK(calls->load() == 1);

  // failed is TERMINAL: further ticks must not respawn (the review's
  // offline thread+network busy-loop).
  tick(h->app, Event{Tick{}});
  tick(h->app, Event{Tick{}});
  CHECK(calls->load() == 1);

  // `c` close + re-open is the manual retry: failed clears, one new fetch.
  tick(h->app, Event{press(U'c')});
  tick(h->app, Event{press(U'c')});
  tick(h->app, Event{Tick{}});
  CHECK(h->app.char_recs.loading);
  ev = pump_one();
  REQUIRE(ev.has_value());
  tick(h->app, *ev);
  CHECK(calls->load() == 2);
  h->app.queue = nullptr;  // detach before the local queue leaves scope.
}

TEST_CASE("Tab swaps grid/section focus; j/k and Enter route to the focused surface") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.catalog.push_back(row_for(700, "Frieren"));
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  h->app.episode.for_id = 700;
  h->app.episode.fetched = true;
  h->app.episode.episodes = {"1", "2", "3"};
  h->app.episode.cursor = 0;
  h->app.char_recs.visible = true;
  h->app.char_recs.fetched = true;
  h->app.char_recs.data = sample_char_recs();

  // Grid-focused (default): j/k move the episode cursor, not the section.
  tick(h->app, Event{press(U'j')});
  CHECK(h->app.char_recs.cursor == 0);

  tick(h->app, Event{special_key(KeyEvent::Special::Tab)});
  CHECK(h->app.char_recs.focused);

  // Section-focused: j/k move the recommendation cursor and clamp at the end.
  tick(h->app, Event{press(U'j')});
  CHECK(h->app.char_recs.cursor == 1);
  tick(h->app, Event{press(U'j')});
  CHECK(h->app.char_recs.cursor == 1);  // clamped: only 2 recommendations.
  tick(h->app, Event{press(U'k')});
  CHECK(h->app.char_recs.cursor == 0);

  tick(h->app, Event{special_key(KeyEvent::Special::Tab)});
  CHECK_FALSE(h->app.char_recs.focused);  // back to the grid.
}

TEST_CASE("Enter on a focused recommendation promotes it: fresh zoom, zero stale grid state") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.catalog.push_back(row_for(700, "Frieren"));
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  // The OLD show's episode grid: cursor parked mid-grid, provider pinned.
  h->app.episode.for_id = 700;
  h->app.episode.fetched = true;
  h->app.episode.provider_id = "megaplay";
  h->app.episode.episodes = {"1", "2", "3", "4"};
  h->app.episode.cursor = 3;
  h->app.char_recs.visible = true;
  h->app.char_recs.focused = true;
  h->app.char_recs.fetched = true;
  h->app.char_recs.data = sample_char_recs();
  h->app.char_recs.cursor = 0;  // Kimetsu no Yaiba, anilist_id 101922.

  tick(h->app, Event{special_key(KeyEvent::Special::Enter)});

  // The promoted row JOINS the catalog under the cursor — the prior list
  // survives (P36 review: a promote must never destroy a live search page).
  REQUIRE(h->app.catalog.size() == 2);
  CHECK(h->app.catalog[0].meta.anilist_id == 700);
  CHECK(h->app.catalog[1].meta.anilist_id == 101922);
  CHECK(h->app.list_cursor == 1);
  CHECK(h->app.view == View::Detail);
  CHECK(h->app.detail_origin == View::Browse);
  const Enrichment* shown = shigoku::tui::detail::selected_enrichment(h->app);
  REQUIRE(shown != nullptr);
  CHECK(shown->anilist_id == 101922);

  // catalog_cache upserted (02 L2) so the row survives a promote-and-return.
  auto hit = h->store.get_catalog(101922);
  REQUIRE(hit.has_value());
  CHECK(hit->has_value());

  // Zero provider-identity leakage: the new show's episode grid is a clean
  // slate keyed to 101922, NOT the previous show's provider/cursor/episodes.
  CHECK(h->app.episode.for_id != 700);
  CHECK(h->app.episode.provider_id.empty());
  CHECK(h->app.episode.cursor == 0);
  CHECK(h->app.episode.episodes.empty());

  // The section itself reset: the new zoom opens clean, no leftover focus.
  CHECK_FALSE(h->app.char_recs.visible);
  CHECK_FALSE(h->app.char_recs.focused);
  CHECK_FALSE(h->app.char_recs.fetched);
}

TEST_CASE("zoom section pty-style render: characters/recommendations draw with a live cursor") {
  auto h = HistoryHarness::make();
  h->app.view = View::Browse;
  h->app.catalog.push_back(row_for(700, "Frieren"));
  h->app.list_cursor = 0;
  h->app.detail_origin = View::Browse;
  h->app.view = View::Detail;
  h->app.win = WinSize{100, 30, 0, 0};
  h->app.char_recs.visible = true;
  h->app.char_recs.fetched = true;
  h->app.char_recs.data = sample_char_recs();
  h->app.char_recs.focused = true;
  h->app.char_recs.cursor = 1;  // One Piece.

  CellBuffer buf;
  draw(h->app, buf);

  // The section rendered somewhere on screen: its header text is on a row.
  auto row_text = [&](int y) {
    std::string s;
    for (int x = 0; x < buf.width(); ++x) s += static_cast<char>(buf.at(x, y).glyph & 0x7F);
    return s;
  };
  bool saw_characters_header = false, saw_recs_header = false, saw_cursor_glyph = false;
  for (int y = 0; y < buf.height(); ++y) {
    const std::string line = row_text(y);
    if (line.find("Characters") != std::string::npos) saw_characters_header = true;
    if (line.find("Recommendations") != std::string::npos) saw_recs_header = true;
  }
  for (int y = 0; y < buf.height(); ++y) {
    for (int x = 0; x < buf.width(); ++x) {
      if (buf.at(x, y).glyph == U'▸') saw_cursor_glyph = true;  // ▸
    }
  }
  CHECK(saw_characters_header);
  CHECK(saw_recs_header);
  CHECK(saw_cursor_glyph);  // the focused recommendation row drew its cursor mark.
}

TEST_CASE("resize purge deletes every held placement and empties the records") {
  // The placement model must not survive a geometry change: terminals
  // reflow, re-anchor, or drop images on resize, so run()'s post-SIGWINCH
  // paint purges everything and lets compose_* re-transmit from scratch.
  detail::PlacedCover cover;
  cover.id = 7;
  cover.for_id = 42;
  std::map<std::string, detail::Placed> grid;
  grid["https://c/a.png"] = detail::Placed{11, Rect{}};
  grid["https://c/b.png"] = detail::Placed{12, Rect{}};
  std::string purge;
  CHECK(detail::purge_placements(CoverBackend::Kitty, cover, grid, purge));
  CHECK(cover.id == 0);
  CHECK(grid.empty());
  // One a=d,d=I delete per held id (the A4 free-stored-data form).
  CHECK(purge.find("a=d,d=I,i=7,") != std::string::npos);
  CHECK(purge.find("a=d,d=I,i=11,") != std::string::npos);
  CHECK(purge.find("a=d,d=I,i=12,") != std::string::npos);
  // Nothing held -> nothing to purge, no forced repaint owed.
  CHECK(!detail::purge_placements(CoverBackend::Kitty, cover, grid, purge));
  CHECK(purge.empty());
}

TEST_CASE("purge_placements: iterm sends no bytes but still forces repaint") {
  // OSC 1337 has no delete command — the resize ED clear wipes the image with
  // its cells. The purge must still empty the records and return true so the
  // caller repaints and compose re-transmits.
  detail::PlacedCover cover;
  cover.id = 7;
  cover.for_id = 42;
  std::map<std::string, detail::Placed> grid;
  grid["https://c/a.png"] = detail::Placed{11, Rect{}};
  std::string purge;
  CHECK(detail::purge_placements(CoverBackend::Iterm, cover, grid, purge));
  CHECK(purge.empty());
  CHECK(cover.id == 0);
  CHECK(grid.empty());
}
