// transfers_tests.cpp: the download queue's state machine, the Transfers
// view's item list, line layout, cursor and hit-test, the row wording
// helpers, and render smoke tests of the view and the download picker.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../src/tui/app.hpp"
#include "../src/tui/cells.hpp"
#include "../src/tui/layout.hpp"
#include "../src/tui/transfers.hpp"
#include "../src/tui/views.hpp"
// App's destructor needs the two types its unique_ptrs hold.
#include "../src/tui/prewarm.hpp"
#include "../src/tui/resolve_transport.hpp"

using namespace shigoku;
using namespace shigoku::tui;

namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::uint64_t kGiB = 1024ull * kMiB;
constexpr std::size_t kMiss = static_cast<std::size_t>(-1);

using State = DownloadJob::State;
using Item = TransfersState::Item;
using Line = TransfersState::Line;

DownloadJob episode_job(std::int64_t show_id, std::string episode,
                        Translation translation = Translation::Sub) {
  DownloadJob job;
  job.show_id = show_id;
  job.title = "Show";
  job.episode = std::move(episode);
  job.translation = translation;
  return job;
}

DiskFile disk_file(std::string label, Translation translation, std::string path,
                   std::uint64_t bytes, bool partial = false) {
  DiskFile f;
  f.label = std::move(label);
  f.translation = translation;
  f.path = std::move(path);
  f.bytes = bytes;
  f.partial = partial;
  return f;
}

DiskShow disk_show(std::string title, std::vector<DiskFile> files) {
  DiskShow s;
  s.folder = title;
  s.title = std::move(title);
  s.files = std::move(files);
  return s;
}

// Two shows on disk: one with a finished and an unfinished file, one with a
// single finished file.
std::vector<DiskShow> two_shows() {
  std::vector<DiskShow> shows;
  shows.push_back(disk_show(
      "Frieren", {disk_file("1", Translation::Sub, "/d/frieren/1.mp4", 300 * kMiB),
                  disk_file("2", Translation::Dub, "/d/frieren/2.mp4.part", 10 * kMiB, true)}));
  shows.push_back(
      disk_show("Mushishi", {disk_file("5", Translation::Sub, "/d/mushishi/5.mp4", 200 * kMiB)}));
  return shows;
}

Item job_item(std::uint64_t id) { return Item{Item::Kind::Job, id, 0, 0}; }
Item show_item(std::size_t s) { return Item{Item::Kind::Show, 0, s, 0}; }
Item file_item(std::size_t s, std::size_t f) { return Item{Item::Kind::File, 0, s, f}; }

Line header(bool disk) { return Line{Line::Kind::Header, disk, 0}; }
Line rule() { return Line{Line::Kind::Rule, false, 0}; }
Line blank() { return Line{Line::Kind::Blank, false, 0}; }
Line at_line(Line::Kind kind, std::size_t item) { return Line{kind, false, item}; }

// The layout line the cursor's item is drawn on.
std::size_t cursor_line(const TransfersState& ts, const DownloadQueue& q) {
  const std::vector<Line> lines = ts.layout(q);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const Line& li = lines[i];
    if ((li.kind == Line::Kind::Job || li.kind == Line::Kind::Show ||
         li.kind == Line::Kind::File) &&
        li.item == ts.cursor) {
      return i;
    }
  }
  return kMiss;
}

bool cursor_on_screen(const TransfersState& ts, const DownloadQueue& q, int visible) {
  const std::size_t line = cursor_line(ts, q);
  return line != kMiss && line >= ts.scroll &&
         line < ts.scroll + static_cast<std::size_t>(visible);
}

std::string text_from(const CellBuffer& buf, int x0, int y) {
  std::string out;
  for (int x = x0; x < buf.width(); ++x) {
    const Cell& c = buf.at(x, y);
    if (!c.continuation) encode_utf8(c.glyph, out);
  }
  return out;
}

std::string row_text(const CellBuffer& buf, int y) { return text_from(buf, 0, y); }

int find_row(const CellBuffer& buf, std::string_view needle) {
  for (int y = 0; y < buf.height(); ++y) {
    if (row_text(buf, y).find(needle) != std::string::npos) return y;
  }
  return -1;
}

// The cell `needle` starts at on row `y`, or -1.
int find_col(const CellBuffer& buf, int y, std::string_view needle) {
  for (int x = 0; x < buf.width(); ++x) {
    if (buf.at(x, y).continuation) continue;
    if (text_from(buf, x, y).compare(0, needle.size(), needle) == 0) return x;
  }
  return -1;
}

void transfers_app(App& app, int cols = 80, int rows = 24) {
  app.view = View::Transfers;
  app.win = WinSize{static_cast<std::uint16_t>(cols), static_cast<std::uint16_t>(rows), 0, 0};
}

void render(const App& app, CellBuffer& buf) {
  buf.resize(app.win.cols, app.win.rows);
  buf.clear(theme::bg);
  draw_transfers(app, buf, kContentY0, static_cast<int>(app.win.rows) - 1);
}

void render_picker(const DownloadPicker& p, const DownloadQueue& q, CellBuffer& buf,
                   int phase = 0) {
  buf.resize(80, 24);
  buf.clear(theme::bg);
  draw_download_picker(p, q, phase, Rect{0, 0, 80, 24}, buf);
}

std::size_t hit(const App& app, int x, int y) {
  return detail::transfers_item_at(app, x, y).value_or(kMiss);
}

OfferRow sized_row(std::uint32_t w, std::uint32_t h) {
  OfferRow r;
  r.rung = Rung(h);
  r.width = w;
  r.height = h;
  return r;
}

// The 80x24 picker box: 64x16 at (8, 4); its rows start two below the title
// and its footer sits on the box's second-to-last row.
constexpr int kBoxY = 4;
constexpr int kBoxTop = kBoxY + 2;
constexpr int kBoxFooter = kBoxY + 14;

}  // namespace

// --- DownloadJob / DownloadQueue ----------------------------------------------

TEST_CASE("a job's state answers active, in flight and cancelling") {
  DownloadJob j;
  j.state = State::Queued;
  CHECK(j.active());
  CHECK_FALSE(j.in_flight());
  CHECK_FALSE(j.cancelling());

  j.state = State::Resolving;
  CHECK(j.active());
  CHECK(j.in_flight());
  CHECK_FALSE(j.cancelling());  // no flag at all.
  j.cancel = std::make_shared<std::atomic<bool>>(false);
  CHECK_FALSE(j.cancelling());
  j.cancel->store(true);
  CHECK(j.cancelling());

  j.state = State::Running;
  CHECK(j.active());
  CHECK(j.in_flight());
  CHECK(j.cancelling());

  for (State s : {State::Done, State::Failed, State::Cancelled}) {
    j.state = s;
    CHECK_FALSE(j.active());
    CHECK_FALSE(j.in_flight());
    CHECK_FALSE(j.cancelling());
  }
}

TEST_CASE("add stamps ids 1, 2, 3 and resets what a transfer reports") {
  DownloadQueue q;
  DownloadJob j = episode_job(7, "1");
  j.id = 99;
  j.state = State::Failed;
  j.height = 720u;
  j.bytes = 5;
  j.total = 9;
  j.estimate = 3;
  j.dest = "/old";
  j.error = "old";
  j.started_tick = 4;
  j.cancel = std::make_shared<std::atomic<bool>>(true);
  j.child_pid = std::make_shared<std::atomic<long>>(5);

  CHECK(q.add(j) == 1);
  CHECK(q.add(episode_job(7, "2")) == 2);
  CHECK(q.add(episode_job(7, "3")) == 3);
  CHECK(q.next_id == 4);
  REQUIRE(q.jobs.size() == 3);
  CHECK(q.jobs[1].id == 2);
  CHECK(q.jobs[2].id == 3);

  const DownloadJob& s = q.jobs[0];
  CHECK(s.id == 1);
  CHECK(s.state == State::Queued);
  CHECK_FALSE(s.height.has_value());
  CHECK(s.bytes == 0);
  CHECK(s.total == 0);
  CHECK(s.estimate == 0);
  CHECK(s.dest.empty());
  CHECK(s.error.empty());
  CHECK(s.started_tick == 0);
  CHECK(s.cancel == nullptr);
  CHECK(s.child_pid == nullptr);
  CHECK(s.show_id == 7);  // what to fetch is kept.
  CHECK(s.episode == "1");
  CHECK(s.title == "Show");
}

TEST_CASE("begin moves a queued job to resolving with fresh shared flags") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  CHECK(q.begin(1, 42));
  const DownloadJob* j = q.find(1);
  REQUIRE(j != nullptr);
  CHECK(j->state == State::Resolving);
  CHECK(j->started_tick == 42);
  REQUIRE(j->cancel != nullptr);
  CHECK_FALSE(j->cancel->load());
  REQUIRE(j->child_pid != nullptr);
  CHECK(j->child_pid->load() == 0);

  CHECK_FALSE(q.begin(1, 50));  // not queued any more.
  CHECK(j->started_tick == 42);
  CHECK_FALSE(q.begin(9, 50));  // unknown.
}

TEST_CASE("the worker's reports carry a job from resolving to done") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  REQUIRE(q.begin(1, 10));
  const DownloadJob* j = q.find(1);
  REQUIRE(j != nullptr);

  q.on_progress(1, 100, 0);  // a count can overtake the start notice.
  CHECK(j->state == State::Resolving);
  CHECK(j->bytes == 100);
  CHECK(j->total == 0);

  q.on_started(1, "/d/show/1.mp4", 1080u, 5000);
  CHECK(j->state == State::Running);
  CHECK(j->dest == "/d/show/1.mp4");
  CHECK(j->height == std::optional<std::uint32_t>(1080u));
  CHECK(j->estimate == 5000);

  q.on_progress(1, 200, 1000);
  CHECK(j->bytes == 200);
  CHECK(j->total == 1000);
  q.on_progress(1, 300, 0);  // an unknown total keeps the known one.
  CHECK(j->bytes == 300);
  CHECK(j->total == 1000);

  q.on_done(1, 0);
  CHECK(j->state == State::Done);
  CHECK(j->bytes == 300);

  q.add(episode_job(1, "2"));
  REQUIRE(q.begin(2, 0));
  q.on_started(2, "/d/show/2.mp4", std::nullopt, 0);
  CHECK_FALSE(q.find(2)->height.has_value());
  q.on_progress(2, 10, 0);
  q.on_done(2, 900);
  CHECK(q.find(2)->state == State::Done);
  CHECK(q.find(2)->bytes == 900);
}

TEST_CASE("on_failed ends a job failed with its reason, or cancelled") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  q.add(episode_job(1, "2"));
  REQUIRE(q.begin(1, 0));
  REQUIRE(q.begin(2, 0));
  q.on_started(2, "/d/2.mp4", 720u, 0);

  q.on_failed(1, false, "network down");
  CHECK(q.find(1)->state == State::Failed);
  CHECK(q.find(1)->error == "network down");

  q.on_failed(2, true, "stopped");
  CHECK(q.find(2)->state == State::Cancelled);
  CHECK(q.find(2)->error.empty());
}

TEST_CASE("reports for an unknown id or a job not in flight change nothing") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));  // stays queued.
  q.on_started(1, "/x", 720u, 1);
  q.on_progress(1, 10, 20);
  q.on_done(1, 10);
  q.on_failed(1, false, "late");
  const DownloadJob* j = q.find(1);
  REQUIRE(j != nullptr);
  CHECK(j->state == State::Queued);
  CHECK(j->dest.empty());
  CHECK_FALSE(j->height.has_value());
  CHECK(j->bytes == 0);
  CHECK(j->total == 0);
  CHECK(j->estimate == 0);
  CHECK(j->error.empty());

  q.on_started(42, "/y", 720u, 1);
  q.on_progress(42, 1, 2);
  q.on_done(42, 3);
  q.on_failed(42, true, "");
  CHECK(q.jobs.size() == 1);
  CHECK(q.find(42) == nullptr);

  REQUIRE(q.begin(1, 0));
  q.on_done(1, 50);
  q.on_progress(1, 99, 100);
  q.on_started(1, "/z", 480u, 7);
  q.on_failed(1, false, "late");
  CHECK(j->state == State::Done);
  CHECK(j->bytes == 50);
  CHECK(j->total == 0);
  CHECK(j->dest.empty());
  CHECK_FALSE(j->height.has_value());
  CHECK(j->error.empty());

  q.add(episode_job(1, "2"));
  REQUIRE(q.begin(2, 0));
  q.on_failed(2, false, "boom");
  q.on_progress(2, 5, 6);
  q.on_done(2, 5);
  q.on_failed(2, true, "");
  CHECK(q.find(2)->state == State::Failed);
  CHECK(q.find(2)->bytes == 0);
  CHECK(q.find(2)->error == "boom");
}

TEST_CASE("next_to_start keeps to the parallel limit") {
  DownloadQueue q;
  CHECK(q.next_to_start(1) == nullptr);
  q.add(episode_job(1, "1"));
  q.add(episode_job(1, "2"));
  q.add(episode_job(1, "3"));

  DownloadJob* next = q.next_to_start(1);
  REQUIRE(next != nullptr);
  CHECK(next->id == 1);
  REQUIRE(q.begin(1, 0));
  CHECK(q.next_to_start(1) == nullptr);

  next = q.next_to_start(2);
  REQUIRE(next != nullptr);
  CHECK(next->id == 2);
  REQUIRE(q.begin(2, 0));
  CHECK(q.next_to_start(2) == nullptr);

  q.on_done(1, 10);
  next = q.next_to_start(2);
  REQUIRE(next != nullptr);
  CHECK(next->id == 3);
  CHECK(q.next_to_start(1) == nullptr);  // job 2 still holds the one slot.

  REQUIRE(q.begin(3, 0));
  CHECK(q.next_to_start(5) == nullptr);  // nothing left queued.
}

TEST_CASE("cancel: a queued job at once, an in-flight one through its flag") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  q.add(episode_job(1, "2"));
  q.add(episode_job(1, "3"));
  REQUIRE(q.begin(2, 0));

  CHECK(q.cancel(1));
  CHECK(q.find(1)->state == State::Cancelled);

  CHECK(q.cancel(2));
  const DownloadJob* two = q.find(2);
  CHECK(two->state == State::Resolving);
  REQUIRE(two->cancel != nullptr);
  CHECK(two->cancel->load());
  CHECK(two->cancelling());
  CHECK(q.cancel(2));  // asked again: still true.

  q.on_started(2, "/d/2.mp4", 720u, 0);
  CHECK(q.cancel(2));
  CHECK(two->state == State::Running);
  q.on_failed(2, true, "");
  CHECK(two->state == State::Cancelled);

  REQUIRE(q.begin(3, 0));
  q.on_done(3, 10);
  CHECK_FALSE(q.cancel(3));
  CHECK(q.find(3)->state == State::Done);
  CHECK_FALSE(q.cancel(1));  // already cancelled.
  CHECK_FALSE(q.cancel(99));
}

TEST_CASE("cancel of an in-flight job without a flag still answers true") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  REQUIRE(q.begin(1, 0));
  q.find(1)->cancel.reset();
  CHECK(q.cancel(1));
  CHECK(q.find(1)->state == State::Resolving);
}

TEST_CASE("retry requeues a failed or cancelled job in its place") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  q.add(episode_job(1, "2"));
  q.add(episode_job(1, "3"));
  REQUIRE(q.begin(1, 5));
  q.on_progress(1, 50, 100);
  q.on_failed(1, false, "boom");
  REQUIRE(q.cancel(2));

  CHECK(q.retry(1));
  const DownloadJob& first = q.jobs[0];
  CHECK(first.id == 1);
  CHECK(first.state == State::Queued);
  CHECK(first.error.empty());
  CHECK(first.bytes == 0);
  CHECK(first.total == 0);
  CHECK(first.started_tick == 0);
  CHECK(first.cancel == nullptr);
  CHECK(first.child_pid == nullptr);

  CHECK(q.retry(2));
  CHECK(q.jobs[1].id == 2);
  CHECK(q.jobs[1].state == State::Queued);

  CHECK_FALSE(q.retry(3));  // queued.
  REQUIRE(q.begin(3, 0));
  CHECK_FALSE(q.retry(3));  // in flight.
  q.on_done(3, 1);
  CHECK_FALSE(q.retry(3));  // done.
  CHECK_FALSE(q.retry(77));

  DownloadJob* next = q.next_to_start(1);
  REQUIRE(next != nullptr);
  CHECK(next->id == 1);
}

TEST_CASE("remove takes only a job that is not active") {
  DownloadQueue q;
  for (int i = 1; i <= 5; ++i) q.add(episode_job(1, std::to_string(i)));
  REQUIRE(q.begin(2, 0));  // resolving
  REQUIRE(q.begin(3, 0));
  q.on_done(3, 1);  // done
  REQUIRE(q.begin(4, 0));
  q.on_failed(4, false, "x");  // failed
  REQUIRE(q.cancel(5));         // cancelled

  CHECK_FALSE(q.remove(1));
  CHECK_FALSE(q.remove(2));
  CHECK(q.remove(3));
  CHECK(q.remove(4));
  CHECK(q.remove(5));
  CHECK_FALSE(q.remove(3));
  CHECK_FALSE(q.remove(99));
  REQUIRE(q.jobs.size() == 2);
  CHECK(q.jobs[0].id == 1);
  CHECK(q.jobs[1].id == 2);
}

TEST_CASE("for_episode takes the latest match and tells sub from dub") {
  DownloadQueue q;
  q.add(episode_job(5, "3"));                    // 1
  q.add(episode_job(5, "3", Translation::Dub));  // 2
  q.add(episode_job(5, "4"));                    // 3
  q.add(episode_job(5, "3"));                    // 4
  q.add(episode_job(6, "3"));                    // 5
  REQUIRE(q.cancel(1));

  const DownloadJob* hit_job = q.for_episode(5, Translation::Sub, "3");
  REQUIRE(hit_job != nullptr);
  CHECK(hit_job->id == 4);
  hit_job = q.for_episode(5, Translation::Dub, "3");
  REQUIRE(hit_job != nullptr);
  CHECK(hit_job->id == 2);
  hit_job = q.for_episode(5, Translation::Sub, "4");
  REQUIRE(hit_job != nullptr);
  CHECK(hit_job->id == 3);
  hit_job = q.for_episode(6, Translation::Sub, "3");
  REQUIRE(hit_job != nullptr);
  CHECK(hit_job->id == 5);
  CHECK(q.for_episode(6, Translation::Dub, "3") == nullptr);
  CHECK(q.for_episode(5, Translation::Sub, "5") == nullptr);
  CHECK(q.for_episode(7, Translation::Sub, "3") == nullptr);

  REQUIRE(q.cancel(4));
  REQUIRE(q.remove(4));
  hit_job = q.for_episode(5, Translation::Sub, "3");
  REQUIRE(hit_job != nullptr);
  CHECK(hit_job->id == 1);  // whatever its state.
  CHECK(hit_job->state == State::Cancelled);
}

TEST_CASE("in_flight, waiting and pending count the queue") {
  DownloadQueue q;
  CHECK(q.in_flight() == 0);
  CHECK(q.waiting() == 0);
  CHECK(q.pending() == 0);
  for (int i = 1; i <= 5; ++i) q.add(episode_job(1, std::to_string(i)));
  REQUIRE(q.begin(1, 0));
  REQUIRE(q.begin(2, 0));
  q.on_started(2, "/d/2.mp4", std::nullopt, 0);
  REQUIRE(q.begin(3, 0));
  q.on_done(3, 1);
  REQUIRE(q.cancel(4));
  CHECK(q.in_flight() == 2);  // 1 resolving, 2 running.
  CHECK(q.waiting() == 1);    // 5.
  CHECK(q.pending() == 3);
}

// --- TransfersState -------------------------------------------------------------

TEST_CASE("items: in flight, then queued, then failed and cancelled, then the disk") {
  DownloadQueue q;
  for (int i = 1; i <= 7; ++i) q.add(episode_job(1, std::to_string(i)));
  REQUIRE(q.begin(1, 0));
  q.on_failed(1, false, "x");  // 1 failed
                               // 2 queued
  REQUIRE(q.begin(3, 0));
  q.on_started(3, "/d/3.mp4", 720u, 0);  // 3 running
  REQUIRE(q.begin(4, 0));
  q.on_done(4, 1);          // 4 done
  REQUIRE(q.begin(5, 0));   // 5 resolving
  REQUIRE(q.cancel(6));     // 6 cancelled
                            // 7 queued
  TransfersState ts;
  ts.disk = two_shows();

  const std::vector<Item> expected = {
      job_item(3),     job_item(5),     job_item(2),  job_item(7),
      job_item(1),     job_item(6),     show_item(0), file_item(0, 0),
      file_item(0, 1), show_item(1),    file_item(1, 0)};
  CHECK(ts.items(q) == expected);

  ts.cursor = 4;
  CHECK(ts.selected(q) == std::optional<Item>(job_item(1)));
  ts.cursor = 10;
  CHECK(ts.selected(q) == std::optional<Item>(file_item(1, 0)));
  ts.cursor = 11;
  CHECK_FALSE(ts.selected(q).has_value());
}

TEST_CASE("layout: queue only, disk only, both, neither") {
  TransfersState ts;
  DownloadQueue none;
  CHECK(ts.layout(none).empty());

  DownloadQueue done_only;
  done_only.add(episode_job(1, "1"));
  REQUIRE(done_only.begin(1, 0));
  done_only.on_done(1, 1);
  CHECK(ts.layout(done_only).empty());

  DownloadQueue two;
  two.add(episode_job(1, "1"));
  two.add(episode_job(1, "2"));
  const std::vector<Line> queue_only = {header(false), rule(), at_line(Line::Kind::Job, 0),
                                        at_line(Line::Kind::Job, 1)};
  CHECK(ts.layout(two) == queue_only);

  TransfersState ds;
  ds.disk = two_shows();
  const std::vector<Line> disk_only = {
      header(true),  rule(),
      at_line(Line::Kind::Show, 0), at_line(Line::Kind::File, 1), at_line(Line::Kind::File, 2),
      blank(),
      at_line(Line::Kind::Show, 3), at_line(Line::Kind::File, 4)};
  CHECK(ds.layout(none) == disk_only);

  DownloadQueue one;
  one.add(episode_job(1, "1"));
  const std::vector<Line> both = {
      header(false), rule(), at_line(Line::Kind::Job, 0),
      blank(),
      header(true),  rule(),
      at_line(Line::Kind::Show, 1), at_line(Line::Kind::File, 2), at_line(Line::Kind::File, 3),
      blank(),
      at_line(Line::Kind::Show, 4), at_line(Line::Kind::File, 5)};
  CHECK(ds.layout(one) == both);
}

// The list the cursor tests walk: one job and two shows on disk.
//   0 header  1 rule  2 job(0)  3 blank  4 header  5 rule
//   6 show(1)  7 file(2)  8 file(3)  9 blank  10 show(4)  11 file(5)
TEST_CASE("nav and jump keep the cursor in range and its line on screen") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  TransfersState ts;
  ts.disk = two_shows();
  constexpr int kVisible = 4;

  ts.nav(1, kVisible, q);
  CHECK(ts.cursor == 1);
  CHECK(ts.scroll == 3);
  CHECK(cursor_on_screen(ts, q, kVisible));

  ts.nav(1, kVisible, q);
  CHECK(ts.cursor == 2);
  CHECK(ts.scroll == 4);
  CHECK(cursor_on_screen(ts, q, kVisible));

  ts.nav(10, kVisible, q);
  CHECK(ts.cursor == 5);
  CHECK(ts.scroll == 8);
  CHECK(cursor_on_screen(ts, q, kVisible));

  ts.nav(-1, kVisible, q);
  CHECK(ts.cursor == 4);
  CHECK(ts.scroll == 8);
  CHECK(cursor_on_screen(ts, q, kVisible));

  ts.nav(-3, kVisible, q);
  CHECK(ts.cursor == 1);
  CHECK(ts.scroll == 4);  // two lines of context above: the section header.
  CHECK(cursor_on_screen(ts, q, kVisible));

  ts.nav(-100, kVisible, q);
  CHECK(ts.cursor == 0);
  CHECK(ts.scroll == 0);

  ts.jump(false, kVisible, q);
  CHECK(ts.cursor == 5);
  CHECK(ts.scroll == 8);
  CHECK(cursor_on_screen(ts, q, kVisible));

  ts.jump(true, kVisible, q);
  CHECK(ts.cursor == 0);
  CHECK(ts.scroll == 0);
}

TEST_CASE("select moves onto a listed item and refuses one that is not") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  TransfersState ts;
  ts.disk = two_shows();
  constexpr int kVisible = 4;

  CHECK(ts.select(file_item(1, 0), kVisible, q));
  CHECK(ts.cursor == 5);
  CHECK(ts.scroll == 8);
  CHECK(cursor_on_screen(ts, q, kVisible));

  CHECK_FALSE(ts.select(file_item(1, 3), kVisible, q));
  CHECK_FALSE(ts.select(job_item(99), kVisible, q));
  CHECK(ts.cursor == 5);

  CHECK(ts.select(show_item(0), kVisible, q));
  CHECK(ts.cursor == 1);
  CHECK(cursor_on_screen(ts, q, kVisible));

  CHECK(ts.select(job_item(1), kVisible, q));
  CHECK(ts.cursor == 0);
  CHECK(ts.scroll == 0);
}

TEST_CASE("clamp pulls the cursor and the scroll back after the lists shrink") {
  DownloadQueue q;
  q.add(episode_job(1, "1"));
  TransfersState ts;
  ts.disk = two_shows();
  constexpr int kVisible = 4;
  ts.jump(false, kVisible, q);
  REQUIRE(ts.cursor == 5);
  REQUIRE(ts.scroll == 8);

  // A rescan loses the second show: 4 items over 9 lines.
  ts.disk.pop_back();
  ts.clamp(kVisible, q);
  CHECK(ts.cursor == 3);
  CHECK(ts.scroll == 5);
  CHECK(cursor_on_screen(ts, q, kVisible));

  // The job finishes and leaves the queue section: 3 items over 5 lines.
  REQUIRE(q.begin(1, 0));
  q.on_done(1, 1);
  ts.clamp(kVisible, q);
  CHECK(ts.cursor == 2);
  CHECK(ts.scroll == 1);
  CHECK(cursor_on_screen(ts, q, kVisible));

  ts.disk.clear();
  ts.clamp(kVisible, q);
  CHECK(ts.cursor == 0);
  CHECK(ts.scroll == 0);

  ts.nav(1, kVisible, q);
  ts.jump(false, kVisible, q);
  CHECK(ts.cursor == 0);
  CHECK(ts.scroll == 0);
  CHECK_FALSE(ts.selected(q).has_value());
}

// --- detail helpers -----------------------------------------------------------

TEST_CASE("rung_text words a rung") {
  CHECK(detail::rung_text(Rung{}) == "best");
  CHECK(detail::rung_text(Rung(Quality::Best)) == "best");
  CHECK(detail::rung_text(Rung(Quality::Worst)) == "lowest");
  CHECK(detail::rung_text(Rung(Quality::P720)) == "720p");
  CHECK(detail::rung_text(Rung(1080u)) == "1080p");
  CHECK(detail::rung_text(Rung(480u, 2400000u)) == "480p");
  CHECK(detail::rung_text(Rung(480u, 0u, true)) == "lowest");
  CHECK(detail::rung_text(Rung(0u, 2400000u)) == "under 2.4 Mbit/s");
  CHECK(detail::rung_text(Rung(0u, 2450000u)) == "under 2.4 Mbit/s");
  CHECK(detail::rung_text(Rung(0u, 850000u)) == "under 0.8 Mbit/s");
}

TEST_CASE("job_status_text follows the status table") {
  auto in_state = [](State s) {
    DownloadJob j;
    j.state = s;
    return j;
  };
  auto running = [&](std::uint64_t bytes, std::uint64_t total, std::uint64_t estimate) {
    DownloadJob j = in_state(State::Running);
    j.bytes = bytes;
    j.total = total;
    j.estimate = estimate;
    return j;
  };

  CHECK(detail::job_status_text(in_state(State::Queued)) == "queued");
  CHECK(detail::job_status_text(in_state(State::Resolving)) == "resolving");
  DownloadJob asked = in_state(State::Resolving);
  asked.cancel = std::make_shared<std::atomic<bool>>(true);
  CHECK(detail::job_status_text(asked) == "cancelling");

  CHECK(detail::job_status_text(running(0, 2 * kGiB, 0)) == "starting");
  CHECK(detail::job_status_text(running(512 * kMiB, 2 * kGiB, 0)) == "512 MB / 2.0 GB  25%");
  CHECK(detail::job_status_text(running(512 * kMiB, 2 * kGiB, kGiB)) ==
        "512 MB / 2.0 GB  25%");
  CHECK(detail::job_status_text(running(512 * kMiB, 0, 2 * kGiB)) == "512 MB of ~2.0 GB");
  CHECK(detail::job_status_text(running(50 * kMiB, 0, 0)) == "50.0 MB");
  CHECK(detail::job_status_text(running(3 * kGiB, 2 * kGiB, 0)) == "3.0 GB / 2.0 GB  100%");
  DownloadJob stopping = running(512 * kMiB, 2 * kGiB, 0);
  stopping.cancel = std::make_shared<std::atomic<bool>>(false);
  CHECK(detail::job_status_text(stopping) == "512 MB / 2.0 GB  25%");
  stopping.cancel->store(true);
  CHECK(detail::job_status_text(stopping) == "cancelling");

  DownloadJob done = in_state(State::Done);
  done.bytes = 2 * kGiB;
  CHECK(detail::job_status_text(done) == "done \xC2\xB7 2.0 GB");

  DownloadJob failed = in_state(State::Failed);
  failed.error = "network down";
  CHECK(detail::job_status_text(failed) == "failed \xC2\xB7 network down");
  failed.error = "net\x07work";
  CHECK(detail::job_status_text(failed) == "failed \xC2\xB7 network");
  failed.error.clear();
  CHECK(detail::job_status_text(failed) == "failed");

  CHECK(detail::job_status_text(in_state(State::Cancelled)) == "cancelled");
}

TEST_CASE("offer_row_text joins the size and each known part") {
  OfferRow full = sized_row(1920, 1080);
  full.bandwidth = 5000000;
  full.codecs = "avc1.64001f,mp4a.40.2";
  full.separate_audio = true;
  full.audio = "Native";
  // 5 Mbit/s over 24 minutes: 900,000,000 bytes.
  CHECK(detail::offer_row_text(full, 1440) ==
        "1920\xC3\x97" "1080 \xC2\xB7 ~858 MB \xC2\xB7 avc1.64001f,mp4a.40.2 \xC2\xB7 audio: Native");
  CHECK(detail::offer_row_text(full, 0) ==
        "1920\xC3\x97" "1080 \xC2\xB7 avc1.64001f,mp4a.40.2 \xC2\xB7 audio: Native");

  OfferRow height_only;
  height_only.height = 720u;
  CHECK(detail::offer_row_text(height_only, 1440) == "720p");

  OfferRow rate_only;
  rate_only.bandwidth = 2400000;
  CHECK(detail::offer_row_text(rate_only, 1440) == "2.4 Mbit/s \xC2\xB7 ~411 MB");

  OfferRow unnamed_audio;
  unnamed_audio.separate_audio = true;
  CHECK(detail::offer_row_text(unnamed_audio, 0) == "as served \xC2\xB7 separate audio");
  unnamed_audio.audio = "\x1b";
  CHECK(detail::offer_row_text(unnamed_audio, 0) == "as served \xC2\xB7 separate audio");

  OfferRow hostile = sized_row(854, 480);
  hostile.codecs = "avc1\x1b";
  CHECK(detail::offer_row_text(hostile, 0) == "854\xC3\x97" "480 \xC2\xB7 avc1");
}

// --- hit-test -------------------------------------------------------------------

TEST_CASE("transfers_item_at hits job and disk lines, misses headers and gutters") {
  App app;
  transfers_app(app);
  app.downloads.add(episode_job(1, "1"));
  app.transfers.disk = two_shows();
  const int y0 = kContentY0;

  CHECK(hit(app, 10, y0 + 2) == 0);   // the job.
  CHECK(hit(app, 10, y0 + 6) == 1);   // the first show.
  CHECK(hit(app, 10, y0 + 7) == 2);   // its first file.
  CHECK(hit(app, 10, y0 + 11) == 5);  // the second show's file.
  CHECK(hit(app, 10, y0 + 0) == kMiss);   // "queue" header.
  CHECK(hit(app, 10, y0 + 1) == kMiss);   // rule.
  CHECK(hit(app, 10, y0 + 3) == kMiss);   // blank.
  CHECK(hit(app, 10, y0 + 4) == kMiss);   // "on disk" header.
  CHECK(hit(app, 10, y0 + 12) == kMiss);  // past the list.
  CHECK(hit(app, 10, y0 - 1) == kMiss);   // above the content.
  CHECK(hit(app, 1, y0 + 2) == kMiss);    // left gutter.
  CHECK(hit(app, 78, y0 + 2) == 0);       // the list's last column.
  CHECK(hit(app, 79, y0 + 2) == kMiss);   // right gutter.

  app.transfers.scroll = 5;
  CHECK(hit(app, 10, y0 + 2) == 2);
  CHECK(hit(app, 10, y0 + 1) == 1);

  app.view = View::Browse;
  CHECK(hit(app, 10, y0 + 2) == kMiss);
}

// --- render ---------------------------------------------------------------------

TEST_CASE("an empty Transfers view draws its one centred line") {
  App app;
  transfers_app(app);
  CellBuffer buf;
  render(app, buf);
  const std::string msg = "nothing downloaded yet \xC2\xB7 d on an episode starts one";
  const int y = find_row(buf, msg);
  CHECK(y == kContentY0 + (23 - kContentY0) / 2);
  REQUIRE(y >= 0);
  const int x = find_col(buf, y, msg);
  CHECK(x == (80 - str_width(msg)) / 2);
  CHECK(buf.at(x, y).fg == theme::fg3);
  CHECK(has(buf.at(x, y).style, Style::Italic));
}

TEST_CASE("a running job's row: glyph, title, episode, quality, provider, progress") {
  App app;
  transfers_app(app);
  DownloadJob job = episode_job(1, "7");
  job.title = "Frieren";
  job.provider_label = "megaplay";
  app.downloads.add(job);
  DownloadJob other = episode_job(2, "3", Translation::Dub);
  other.title = "Mushishi";
  app.downloads.add(other);
  REQUIRE(app.downloads.begin(1, 0));
  app.downloads.on_started(1, "/d/frieren/7.mp4", 1080u, 0);
  app.downloads.on_progress(1, 512 * kMiB, 2 * kGiB);
  REQUIRE(app.downloads.begin(2, 0));
  app.downloads.on_failed(2, false, "network down");

  CellBuffer buf;
  render(app, buf);
  CHECK(find_row(buf, "queue (2)") == kContentY0);

  const int y = find_row(buf, "Frieren");
  REQUIRE(y == kContentY0 + 2);
  const std::string row = row_text(buf, y);
  CHECK(row.find("\xE2\x87\xA3  Frieren \xC2\xB7 ep 7 \xC2\xB7 1080p \xC2\xB7 megaplay") !=
        std::string::npos);
  CHECK(find_col(buf, y, "\xE2\x87\xA3") == 3);
  // Right-aligned one cell inside the list's right edge (x = 2 + 77 - 1 - 20).
  CHECK(find_col(buf, y, "512 MB / 2.0 GB  25%") == 58);
  CHECK(buf.at(3, y).fg == theme::focus);  // the cursor's row.
  CHECK(buf.at(3, y).bg == theme::surface);
  CHECK(has(buf.at(3, y).style, Style::Bold));

  const int f = find_row(buf, "Mushishi");
  REQUIRE(f == y + 1);
  const std::string failed_row = row_text(buf, f);
  CHECK(failed_row.find("!  Mushishi \xC2\xB7 ep 3 \xC2\xB7 dub \xC2\xB7 best") !=
        std::string::npos);
  CHECK(buf.at(3, f).fg == theme::warn);
  CHECK(buf.at(3, f).bg == theme::bg);
  const int fx = find_col(buf, f, "failed \xC2\xB7 network down");
  REQUIRE(fx >= 0);
  CHECK(buf.at(fx, f).fg == theme::warn);
}

TEST_CASE("a narrow job row cuts the title, never the progress") {
  App app;
  transfers_app(app, 60, 12);
  DownloadJob job = episode_job(1, "7");
  job.title = "A very long show title that never ends";
  app.downloads.add(job);
  REQUIRE(app.downloads.begin(1, 0));
  app.downloads.on_started(1, "/d/x.mp4", 1080u, 0);
  app.downloads.on_progress(1, 512 * kMiB, 2 * kGiB);

  CellBuffer buf;
  render(app, buf);
  const std::string row = row_text(buf, kContentY0 + 2);
  CHECK(row.find("\xE2\x87\xA3  A very long sho\xE2\x80\xA6 \xC2\xB7 ep 7 \xC2\xB7 1080p") !=
        std::string::npos);
  CHECK(find_col(buf, kContentY0 + 2, "512 MB / 2.0 GB  25%") == 38);
}

TEST_CASE("a show on disk with a finished and an unfinished file") {
  App app;
  transfers_app(app);
  app.transfers.disk = two_shows();
  app.transfers.disk.pop_back();
  app.transfers.confirm_delete = "/d/frieren/1.mp4";

  CellBuffer buf;
  render(app, buf);
  const int head = find_row(buf, "on disk (2)");
  REQUIRE(head == kContentY0);
  CHECK(find_col(buf, head, "310 MB") == 2 + 77 - 1 - 6);

  const int show = head + 2;
  CHECK(find_col(buf, show, "Frieren") == 3);
  CHECK(row_text(buf, show).find("2 files \xC2\xB7 310 MB") != std::string::npos);
  CHECK(buf.at(3, show).fg == theme::focus);  // the cursor rests on the show.
  CHECK(buf.at(3, show).bg == theme::surface);

  const int armed = show + 1;
  CHECK(find_col(buf, armed, "ep 1 \xC2\xB7 sub") == 7);
  CHECK(row_text(buf, armed).find("300 MB") != std::string::npos);
  CHECK(buf.at(7, armed).fg == theme::warn);
  CHECK(has(buf.at(7, armed).style, Style::Bold));

  const int partial = show + 2;
  CHECK(find_col(buf, partial, "ep 2 \xC2\xB7 dub") == 7);
  CHECK(buf.at(7, partial).fg == theme::fg);
  const int px = find_col(buf, partial, "10.0 MB \xC2\xB7 unfinished");
  REQUIRE(px >= 0);
  CHECK(buf.at(px, partial).fg == theme::warn);
}

TEST_CASE("the picker's loading face") {
  DownloadPicker p;
  p.open = true;
  p.loading = true;
  p.draft.title = "Frieren";
  p.draft.episode = "7";
  p.draft.translation = Translation::Dub;
  DownloadQueue q;
  CellBuffer buf;
  render_picker(p, q, buf, 3);

  CHECK(find_row(buf, "Download \xC2\xB7 Frieren \xC2\xB7 ep 7 \xC2\xB7 dub") == kBoxY);
  const int tx = find_col(buf, kBoxY, "Download");
  REQUIRE(tx >= 0);
  CHECK(buf.at(tx, kBoxY).fg == theme::hot);
  CHECK(buf.at(8, kBoxY).bg == theme::elevated);
  CHECK(find_row(buf, std::string("reading the stream ") + kSpinnerFrames[3]) == kBoxTop);
  CHECK(find_row(buf, "esc close") == kBoxFooter);
}

TEST_CASE("the picker's offer face marks the cursor row") {
  DownloadPicker p;
  p.open = true;
  p.draft.title = "Frieren";
  p.draft.episode = "7";
  p.draft.provider_label = "megaplay";
  p.offer.shape = StreamOffer::Shape::Ladder;
  p.offer.rows = {sized_row(1920, 1080), sized_row(1280, 720), sized_row(854, 480)};
  p.cursor = 1;
  p.on_disk = 300 * kMiB;
  DownloadQueue q;
  CellBuffer buf;
  render_picker(p, q, buf);

  const int marked = find_row(buf, "\xE2\x96\xB8 1280\xC3\x97" "720");
  REQUIRE(marked == kBoxTop + 1);
  const int mx = find_col(buf, marked, "\xE2\x96\xB8");
  REQUIRE(mx >= 0);
  CHECK(buf.at(mx, marked).fg == theme::focus);
  CHECK(buf.at(mx, marked).bg == theme::surface);
  const int first = find_row(buf, "1920\xC3\x97" "1080");
  CHECK(first == kBoxTop);
  CHECK(row_text(buf, first).find("\xE2\x96\xB8") == std::string::npos);
  CHECK(find_row(buf, "854\xC3\x97" "480") == kBoxTop + 2);

  const int disk = find_row(buf, "already on disk (300 MB) \xC2\xB7 enter fetches it again");
  CHECK(disk == kBoxTop + 4);
  CHECK(find_row(buf, "from megaplay") == kBoxTop + 5);
  CHECK(find_row(buf, "jk move \xC2\xB7 enter download \xC2\xB7 esc close") == kBoxFooter);
}

TEST_CASE("a single-row offer says why and drops the move keys") {
  DownloadPicker p;
  p.open = true;
  p.draft.title = "Frieren";
  p.draft.episode = "7";
  p.offer.shape = StreamOffer::Shape::File;
  p.offer.rows = {OfferRow{}};
  DownloadQueue q;
  CellBuffer buf;
  render_picker(p, q, buf);

  CHECK(find_row(buf, "\xE2\x96\xB8 as served") == kBoxTop);
  CHECK(find_row(buf, "a single file: the source serves one quality") == kBoxTop + 2);
  CHECK(find_row(buf, "enter download \xC2\xB7 esc close") == kBoxFooter);
  CHECK(find_row(buf, "jk move") < 0);
}

TEST_CASE("a long offer scrolls to keep the cursor row in the box") {
  DownloadPicker p;
  p.open = true;
  p.draft.title = "Frieren";
  p.draft.episode = "7";
  p.offer.shape = StreamOffer::Shape::Ladder;
  for (std::uint32_t i = 0; i < 20; ++i) {
    OfferRow r;
    r.height = 100 + i;
    p.offer.rows.push_back(r);
  }
  p.cursor = 15;
  DownloadQueue q;
  CellBuffer buf;
  render_picker(p, q, buf);

  // Eleven rows fit: 105p..115p, the cursor on the last.
  CHECK(find_row(buf, "  105p") == kBoxTop);
  CHECK(find_row(buf, "\xE2\x96\xB8 115p") == kBoxTop + 10);
  CHECK(find_row(buf, "104p") < 0);
  CHECK(find_row(buf, "116p") < 0);
}

TEST_CASE("the picker's failed face") {
  DownloadPicker p;
  p.open = true;
  p.draft.title = "Frieren";
  p.draft.episode = "7";
  p.failed = "the source refused the stream";
  DownloadQueue q;
  CellBuffer buf;
  render_picker(p, q, buf);

  const int y = find_row(buf, "the source refused the stream");
  REQUIRE(y == kBoxTop);
  const int x = find_col(buf, y, "the source");
  CHECK(buf.at(x, y).fg == theme::warn);
  CHECK(find_row(buf, "esc close") == kBoxFooter);
  CHECK(find_row(buf, "enter download") < 0);
}

TEST_CASE("the picker's job face: retry and remove, cancel, or gone") {
  DownloadQueue q;
  DownloadJob j = episode_job(1, "7");
  j.title = "Frieren";
  j.provider_label = "megaplay";
  q.add(j);
  REQUIRE(q.begin(1, 0));
  q.on_failed(1, false, "network down");

  DownloadPicker p;
  p.open = true;
  p.draft = j;
  p.job = 1;
  CellBuffer buf;
  render_picker(p, q, buf);
  CHECK(find_row(buf, "Download \xC2\xB7 Frieren \xC2\xB7 ep 7") == kBoxY);
  // The reason has the lines below to itself; the first says only that it failed.
  CHECK(find_row(buf, "best \xC2\xB7 megaplay \xC2\xB7 failed") == kBoxTop);
  CHECK(row_text(buf, kBoxTop).find("network down") == std::string::npos);
  CHECK(find_row(buf, "network down") == kBoxTop + 2);
  CHECK(find_row(buf, "r retry \xC2\xB7 x remove \xC2\xB7 esc close") == kBoxFooter);

  p.job = q.add(j);
  render_picker(p, q, buf);
  CHECK(find_row(buf, "best \xC2\xB7 megaplay \xC2\xB7 queued") == kBoxTop);
  CHECK(find_row(buf, "x cancel \xC2\xB7 esc close") == kBoxFooter);
  CHECK(find_row(buf, "r retry") < 0);

  p.job = 99;
  render_picker(p, q, buf);
  CHECK(find_row(buf, "that download is gone \xC2\xB7 esc close") >= 0);
}

TEST_CASE("the picker falls back to a strip on a small screen, and a closed one draws nothing") {
  DownloadPicker p;
  p.open = true;
  p.loading = true;
  DownloadQueue q;
  CellBuffer buf(30, 6);
  buf.clear(theme::bg);
  draw_download_picker(p, q, 0, Rect{0, 0, 30, 6}, buf);
  CHECK(find_row(buf, "download: esc to close") == 3);

  p.open = false;
  CellBuffer closed(80, 24);
  closed.clear(theme::bg);
  draw_download_picker(p, q, 0, Rect{0, 0, 80, 24}, closed);
  CHECK(find_row(closed, "Download") < 0);
  CHECK(closed.at(40, 12).bg == theme::bg);
}
