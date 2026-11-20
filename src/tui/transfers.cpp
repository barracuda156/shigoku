// transfers.cpp: the download queue's state machine, the Transfers view and
// the download picker. See transfers.hpp.

#include "transfers.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../domain.hpp"
#include "../offer.hpp"
#include "cells.hpp"
#include "layout.hpp"
#include "theme.hpp"
#include "views.hpp"

namespace shigoku::tui {

namespace {

constexpr std::string_view kSep = " \xC2\xB7 ";  // a middle dot between spaces

// `part` after `line`, with the separator when `line` already says something.
void add_part(std::string& line, std::string_view part) {
  if (!line.empty()) line += kSep;
  line += part;
}

// Every field a transfer reports, back to nothing (the state aside).
void clear_report(DownloadJob& job) {
  job.height.reset();
  job.bytes = 0;
  job.total = 0;
  job.estimate = 0;
  job.dest.clear();
  job.error.clear();
  job.started_tick = 0;
  job.cancel.reset();
  job.child_pid.reset();
}

// The job a worker's report is about, or nullptr when it is to be ignored.
DownloadJob* reporting(DownloadQueue& queue, std::uint64_t id) {
  DownloadJob* job = queue.find(id);
  return job != nullptr && job->in_flight() ? job : nullptr;
}

// The queue section's group a job is listed under; -1 = not listed.
int queue_group(const DownloadJob& job) {
  switch (job.state) {
    case DownloadJob::State::Resolving:
    case DownloadJob::State::Running:
      return 0;
    case DownloadJob::State::Queued:
      return 1;
    case DownloadJob::State::Failed:
    case DownloadJob::State::Cancelled:
      return 2;
    case DownloadJob::State::Done:
      return -1;
  }
  return -1;
}

const char* spinner_frame(int phase) {
  const int n = kSpinnerFrameCount;
  return kSpinnerFrames[((phase % n) + n) % n];
}

const char* state_glyph(const DownloadJob& job, int phase) {
  switch (job.state) {
    case DownloadJob::State::Running:   return "\xE2\x87\xA3";  // a down arrow
    case DownloadJob::State::Resolving: return spinner_frame(phase);
    case DownloadJob::State::Queued:    return "\xC2\xB7";
    case DownloadJob::State::Failed:    return "!";
    case DownloadJob::State::Cancelled: return "-";
    case DownloadJob::State::Done:      return " ";
  }
  return " ";
}

// "1080p" once the transfer said what it fetches, else the rung asked for.
std::string job_quality_text(const DownloadJob& job) {
  return job.height.has_value() ? std::to_string(*job.height) + "p"
                                : detail::rung_text(job.rung);
}

// The status column: job_status_text, a resolving job's spinner after it.
std::string job_tail(const DownloadJob& job, int phase) {
  std::string tail = detail::job_status_text(job);
  if (job.state == DownloadJob::State::Resolving && !job.cancelling()) {
    tail += ' ';
    tail += spinner_frame(phase);
  }
  return tail;
}

// What follows the title on a job row: episode, dub, quality, provider.
std::string job_row_suffix(const DownloadJob& job) {
  std::string s(kSep);
  s += "ep ";
  s += strip_controls(job.episode);
  if (job.translation == Translation::Dub) {
    s += kSep;
    s += "dub";
  }
  s += kSep;
  s += job_quality_text(job);
  const std::string provider = strip_controls(job.provider_label);
  if (!provider.empty()) {
    s += kSep;
    s += provider;
  }
  return s;
}

std::string_view track_text(Translation t) {
  switch (t) {
    case Translation::Sub: return "sub";
    case Translation::Dub: return "dub";
  }
  return "sub";
}

void draw_rule(CellBuffer& buf, int y, int list_w) {
  buf.fill(Rect{2, y, list_w, 1}, theme::bg);
  for (int cx = 0; cx < list_w; ++cx) {
    buf.put_str(2 + cx, y, "\xE2\x94\x80", theme::chrome, theme::bg);
  }
}

// The x a tail of `tail_w` columns starts at, right-aligned in the list.
int tail_x(int list_w, int tail_w) { return 2 + list_w - 1 - tail_w; }

void draw_job_line(CellBuffer& buf, int y, int list_w, const DownloadJob& job, bool selected,
                   int phase) {
  const Rgb bg = selected ? theme::surface : theme::bg;
  buf.fill(Rect{2, y, list_w, 1}, bg);

  std::string tail = job_tail(job, phase);
  // A long failure reason must not crowd the title out of the row.
  if (job.state == DownloadJob::State::Failed) {
    tail = truncate_to_cols(tail, std::max(0, list_w / 2));
  }
  const int tx = tail_x(list_w, str_width(tail));
  constexpr int kLeftX = 3;
  const int budget = tx - 1 - kLeftX;

  const std::string prefix = std::string(state_glyph(job, phase)) + "  ";
  const std::string suffix = job_row_suffix(job);
  const std::string title = truncate_to_cols(
      strip_controls(job.title), budget - str_width(prefix) - str_width(suffix));
  const std::string left = truncate_to_cols(prefix + title + suffix, budget);

  Rgb left_col = theme::fg2;
  if (selected) {
    left_col = theme::focus;
  } else if (job.state == DownloadJob::State::Running) {
    left_col = theme::fg;
  } else if (job.state == DownloadJob::State::Failed) {
    left_col = theme::warn;
  }
  buf.put_str(kLeftX, y, left, left_col, bg, selected ? Style::Bold : Style::None);
  buf.put_str(tx, y, tail, job.state == DownloadJob::State::Failed ? theme::warn : theme::fg2,
              bg);
}

void draw_show_line(CellBuffer& buf, int y, int list_w, const DiskShow& show, bool selected) {
  const Rgb bg = selected ? theme::surface : theme::bg;
  buf.fill(Rect{2, y, list_w, 1}, bg);
  const std::size_t n = show.files.size();
  std::string tail = std::to_string(n) + (n == 1 ? " file" : " files");
  add_part(tail, human_bytes(show.bytes()));
  const int tx = tail_x(list_w, str_width(tail));
  constexpr int kLeftX = 3;
  const std::string title = truncate_to_cols(strip_controls(show.title), tx - 1 - kLeftX);
  buf.put_str(kLeftX, y, title, selected ? theme::focus : theme::fg, bg, Style::Bold);
  buf.put_str(tx, y, tail, theme::fg2, bg);
}

void draw_file_line(CellBuffer& buf, int y, int list_w, const DiskFile& file, bool selected,
                    bool armed) {
  const Rgb bg = selected ? theme::surface : theme::bg;
  buf.fill(Rect{2, y, list_w, 1}, bg);
  std::string tail = human_bytes(file.bytes);
  if (file.partial) add_part(tail, "unfinished");
  const int tx = tail_x(list_w, str_width(tail));
  constexpr int kLeftX = 7;
  std::string left = "ep " + strip_controls(file.label);
  add_part(left, track_text(file.translation));
  left = truncate_to_cols(left, tx - 1 - kLeftX);

  Rgb left_col = theme::fg;
  Style st = Style::None;
  if (armed) {
    left_col = theme::warn;
    st = Style::Bold;
  } else if (selected) {
    left_col = theme::focus;
    st = Style::Bold;
  }
  buf.put_str(kLeftX, y, left, left_col, bg, st);
  buf.put_str(tx, y, tail, file.partial ? theme::warn : theme::fg2, bg);
}

// --- the picker ---

constexpr int kPickerW = 64;
constexpr int kPickerH = 16;
constexpr int kPickerMinW = 40;
constexpr int kPickerMinH = 8;

void put_centered(CellBuffer& buf, const Rect& box, int row, const std::string& s, Rgb fg,
                  Style st = Style::None) {
  if (row < 0 || row >= box.h) return;
  const std::string text = truncate_to_cols(s, std::max(0, box.w - 4));
  const int x = box.x + std::max(0, (box.w - str_width(text)) / 2);
  buf.put_str(x, box.y + row, text, fg, theme::elevated, st);
}

void put_left(CellBuffer& buf, const Rect& box, int row, const std::string& s, Rgb fg) {
  if (row < 0 || row >= box.h) return;
  buf.put_str(box.x + 2, box.y + row, truncate_to_cols(s, std::max(0, box.w - 4)), fg,
              theme::elevated);
}

// `text` word-wrapped from `row`, stopping short of `limit`.
void put_wrapped(CellBuffer& buf, const Rect& box, int row, int limit, std::string_view text,
                 Rgb fg) {
  for (const std::string& line : detail::wrap_text(text, std::max(0, box.w - 4))) {
    if (row >= limit) break;
    put_left(buf, box, row, line, fg);
    ++row;
  }
}

void put_footer(CellBuffer& buf, const Rect& box, const std::string& keys) {
  put_centered(buf, box, box.h - 2, keys, theme::fg2, Style::Italic);
}

void draw_picker_job(const DownloadPicker& picker, const DownloadQueue& queue, int phase,
                     const Rect& box, CellBuffer& buf, int top) {
  const DownloadJob* job = queue.find(*picker.job);
  if (job == nullptr) {
    put_centered(buf, box, top, "that download is gone \xC2\xB7 esc close", theme::fg2);
    return;
  }
  const bool failed = job->state == DownloadJob::State::Failed;
  std::string line = job_quality_text(*job);
  if (const std::string provider = strip_controls(job->provider_label); !provider.empty()) {
    add_part(line, provider);
  }
  // A failed job's reason gets the lines below to itself.
  add_part(line, failed ? std::string("failed") : job_tail(*job, phase));
  put_left(buf, box, top, line, failed ? theme::warn : theme::fg);
  if (failed) {
    put_wrapped(buf, box, top + 2, box.h - 3, strip_controls(job->error), theme::warn);
  }

  std::string keys;
  if (job->active()) {
    keys = "x cancel";
  } else if (failed || job->state == DownloadJob::State::Cancelled) {
    keys = "r retry";
    add_part(keys, "x remove");
  }
  add_part(keys, "esc close");
  put_footer(buf, box, keys);
}

void draw_picker_offer(const DownloadPicker& picker, const Rect& box, CellBuffer& buf, int top) {
  const DownloadJob& draft = picker.draft;
  const std::vector<OfferRow>& rows = picker.offer.rows;

  struct Note {
    std::string text;
    Rgb fg;
  };
  std::vector<Note> notes;
  if (std::string shape = offer_shape_note(picker.offer.shape); !shape.empty()) {
    notes.push_back(Note{std::move(shape), theme::fg3});
  }
  if (picker.on_disk.has_value()) {
    std::string text = "already on disk (" + human_bytes(*picker.on_disk) + ")";
    add_part(text, "enter fetches it again");
    notes.push_back(Note{std::move(text), theme::warn});
  }
  if (const std::string from = strip_controls(draft.provider_label); !from.empty()) {
    notes.push_back(Note{"from " + from, theme::fg3});
  }

  // Rows, then a blank and the notes, then a blank above the footer.
  const int footer_row = box.h - 2;
  const int notes_h = notes.empty() ? 0 : static_cast<int>(notes.size()) + 1;
  const std::size_t capacity =
      static_cast<std::size_t>(std::max(1, footer_row - 1 - top - notes_h));
  const std::size_t cur = rows.empty() ? 0 : std::min(picker.cursor, rows.size() - 1);
  const std::size_t first = cur >= capacity ? cur - capacity + 1 : 0;
  const int text_w = std::max(0, box.w - 6);

  int row = top;
  for (std::size_t i = first; i < rows.size() && i < first + capacity; ++i, ++row) {
    const std::string text =
        truncate_to_cols(detail::offer_row_text(rows[i], draft.duration_secs), text_w);
    if (i == cur) {
      buf.fill(Rect{box.x + 1, box.y + row, box.w - 2, 1}, theme::surface);
      buf.put_str(box.x + 2, box.y + row, "\xE2\x96\xB8 " + text, theme::focus,
                  theme::surface, Style::Bold);
    } else {
      buf.put_str(box.x + 2, box.y + row, "  " + text, theme::fg2, theme::elevated);
    }
  }
  if (!notes.empty()) ++row;
  for (const Note& note : notes) {
    if (row >= footer_row) break;
    put_left(buf, box, row, note.text, note.fg);
    ++row;
  }

  put_footer(buf, box,
             rows.size() > 1 ? "jk move \xC2\xB7 enter download \xC2\xB7 esc close"
                             : "enter download \xC2\xB7 esc close");
}

}  // namespace

// --- DownloadJob ------------------------------------------------------------

bool DownloadJob::active() const { return state == State::Queued || in_flight(); }

bool DownloadJob::in_flight() const {
  return state == State::Resolving || state == State::Running;
}

bool DownloadJob::cancelling() const {
  return in_flight() && cancel != nullptr && cancel->load();
}

// --- DownloadQueue ----------------------------------------------------------

std::uint64_t DownloadQueue::add(DownloadJob job) {
  job.id = next_id++;
  job.state = DownloadJob::State::Queued;
  clear_report(job);
  jobs.push_back(std::move(job));
  return jobs.back().id;
}

DownloadJob* DownloadQueue::find(std::uint64_t id) {
  for (DownloadJob& job : jobs) {
    if (job.id == id) return &job;
  }
  return nullptr;
}

const DownloadJob* DownloadQueue::find(std::uint64_t id) const {
  for (const DownloadJob& job : jobs) {
    if (job.id == id) return &job;
  }
  return nullptr;
}

const DownloadJob* DownloadQueue::for_episode(std::int64_t show_id, Translation translation,
                                              std::string_view episode) const {
  for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
    if (it->show_id == show_id && it->translation == translation && it->episode == episode) {
      return &*it;
    }
  }
  return nullptr;
}

std::size_t DownloadQueue::in_flight() const {
  return static_cast<std::size_t>(std::count_if(
      jobs.begin(), jobs.end(), [](const DownloadJob& j) { return j.in_flight(); }));
}

std::size_t DownloadQueue::waiting() const {
  return static_cast<std::size_t>(
      std::count_if(jobs.begin(), jobs.end(),
                    [](const DownloadJob& j) { return j.state == DownloadJob::State::Queued; }));
}

std::size_t DownloadQueue::pending() const { return in_flight() + waiting(); }

DownloadJob* DownloadQueue::next_to_start(std::uint32_t parallel) {
  if (in_flight() >= parallel) return nullptr;
  for (DownloadJob& job : jobs) {
    if (job.state == DownloadJob::State::Queued) return &job;
  }
  return nullptr;
}

bool DownloadQueue::begin(std::uint64_t id, TickCount now) {
  DownloadJob* job = find(id);
  if (job == nullptr || job->state != DownloadJob::State::Queued) return false;
  clear_report(*job);
  job->state = DownloadJob::State::Resolving;
  job->cancel = std::make_shared<std::atomic<bool>>(false);
  job->child_pid = std::make_shared<std::atomic<long>>(0);
  job->started_tick = now;
  return true;
}

void DownloadQueue::on_started(std::uint64_t id, std::string dest,
                               std::optional<std::uint32_t> height, std::uint64_t estimate) {
  DownloadJob* job = reporting(*this, id);
  if (job == nullptr) return;
  job->state = DownloadJob::State::Running;
  job->dest = std::move(dest);
  job->height = height;
  job->estimate = estimate;
}

void DownloadQueue::on_progress(std::uint64_t id, std::uint64_t bytes, std::uint64_t total) {
  DownloadJob* job = reporting(*this, id);
  if (job == nullptr) return;
  job->bytes = bytes;
  if (total > 0) job->total = total;
}

void DownloadQueue::on_done(std::uint64_t id, std::uint64_t bytes) {
  DownloadJob* job = reporting(*this, id);
  if (job == nullptr) return;
  job->state = DownloadJob::State::Done;
  if (bytes > 0) job->bytes = bytes;
}

void DownloadQueue::on_failed(std::uint64_t id, bool cancelled, std::string reason) {
  DownloadJob* job = reporting(*this, id);
  if (job == nullptr) return;
  if (cancelled) {
    job->state = DownloadJob::State::Cancelled;
    job->error.clear();
  } else {
    job->state = DownloadJob::State::Failed;
    job->error = std::move(reason);
  }
}

bool DownloadQueue::cancel(std::uint64_t id) {
  DownloadJob* job = find(id);
  if (job == nullptr) return false;
  if (job->state == DownloadJob::State::Queued) {
    job->state = DownloadJob::State::Cancelled;
    return true;
  }
  if (job->in_flight()) {
    if (job->cancel != nullptr) job->cancel->store(true);
    return true;
  }
  return false;
}

bool DownloadQueue::retry(std::uint64_t id) {
  DownloadJob* job = find(id);
  if (job == nullptr) return false;
  if (job->state != DownloadJob::State::Failed && job->state != DownloadJob::State::Cancelled) {
    return false;
  }
  clear_report(*job);
  job->state = DownloadJob::State::Queued;
  return true;
}

bool DownloadQueue::remove(std::uint64_t id) {
  auto it = std::find_if(jobs.begin(), jobs.end(),
                         [id](const DownloadJob& j) { return j.id == id; });
  if (it == jobs.end() || it->active()) return false;
  jobs.erase(it);
  return true;
}

// --- DiskShow / TransfersState ----------------------------------------------

std::uint64_t DiskShow::bytes() const {
  std::uint64_t sum = 0;
  for (const DiskFile& f : files) sum += f.bytes;
  return sum;
}

std::vector<TransfersState::Item> TransfersState::items(const DownloadQueue& queue) const {
  std::vector<Item> out;
  for (int group = 0; group < 3; ++group) {
    for (const DownloadJob& job : queue.jobs) {
      if (queue_group(job) == group) out.push_back(Item{Item::Kind::Job, job.id, 0, 0});
    }
  }
  for (std::size_t s = 0; s < disk.size(); ++s) {
    out.push_back(Item{Item::Kind::Show, 0, s, 0});
    for (std::size_t f = 0; f < disk[s].files.size(); ++f) {
      out.push_back(Item{Item::Kind::File, 0, s, f});
    }
  }
  return out;
}

std::optional<TransfersState::Item> TransfersState::selected(const DownloadQueue& queue) const {
  const std::vector<Item> list = items(queue);
  if (cursor >= list.size()) return std::nullopt;
  return list[cursor];
}

std::vector<TransfersState::Line> TransfersState::layout(const DownloadQueue& queue) const {
  std::vector<Line> lines;
  const std::vector<Item> list = items(queue);
  std::size_t ix = 0;
  while (ix < list.size() && list[ix].kind == Item::Kind::Job) ++ix;
  const std::size_t job_count = ix;

  if (job_count > 0) {
    lines.push_back(Line{Line::Kind::Header, false, 0});
    lines.push_back(Line{Line::Kind::Rule, false, 0});
    for (std::size_t i = 0; i < job_count; ++i) lines.push_back(Line{Line::Kind::Job, false, i});
  }
  if (!disk.empty()) {
    if (job_count > 0) lines.push_back(Line{Line::Kind::Blank, false, 0});
    lines.push_back(Line{Line::Kind::Header, true, 0});
    lines.push_back(Line{Line::Kind::Rule, false, 0});
    bool first_show = true;
    for (; ix < list.size(); ++ix) {
      if (list[ix].kind == Item::Kind::Show) {
        if (!first_show) lines.push_back(Line{Line::Kind::Blank, false, 0});
        first_show = false;
        lines.push_back(Line{Line::Kind::Show, false, ix});
      } else {
        lines.push_back(Line{Line::Kind::File, false, ix});
      }
    }
  }
  return lines;
}

void TransfersState::nav(int dy, int visible, const DownloadQueue& queue) {
  const std::size_t total = items(queue).size();
  if (total == 0) return;
  const long max = static_cast<long>(total) - 1;
  long next = static_cast<long>(cursor) + dy;
  if (next < 0) next = 0;
  if (next > max) next = max;
  cursor = static_cast<std::size_t>(next);
  scroll_into_view(visible, queue);
}

void TransfersState::jump(bool top, int visible, const DownloadQueue& queue) {
  const std::size_t total = items(queue).size();
  if (total == 0) return;
  cursor = top ? 0 : total - 1;
  scroll_into_view(visible, queue);
}

void TransfersState::clamp(int visible, const DownloadQueue& queue) {
  const std::size_t total = items(queue).size();
  if (total == 0) {
    cursor = 0;
    scroll = 0;
    return;
  }
  if (cursor >= total) cursor = total - 1;
  // No scrolling past the point where the last line sits on the bottom row.
  const std::size_t lines = layout(queue).size();
  const std::size_t span = visible > 0 ? static_cast<std::size_t>(visible) : 1;
  const std::size_t max_scroll = lines > span ? lines - span : 0;
  if (scroll > max_scroll) scroll = max_scroll;
  scroll_into_view(visible, queue);
}

bool TransfersState::select(const Item& item, int visible, const DownloadQueue& queue) {
  const std::vector<Item> list = items(queue);
  auto it = std::find(list.begin(), list.end(), item);
  if (it == list.end()) return false;
  cursor = static_cast<std::size_t>(it - list.begin());
  scroll_into_view(visible, queue);
  return true;
}

void TransfersState::scroll_into_view(int visible, const DownloadQueue& queue) {
  if (visible <= 0) return;
  const std::vector<Line> lines = layout(queue);
  auto it = std::find_if(lines.begin(), lines.end(), [&](const Line& li) {
    return (li.kind == Line::Kind::Job || li.kind == Line::Kind::Show ||
            li.kind == Line::Kind::File) &&
           li.item == cursor;
  });
  if (it == lines.end()) return;
  const std::size_t line_ix = static_cast<std::size_t>(it - lines.begin());
  const std::size_t top_pad = line_ix >= 2 ? line_ix - 2 : 0;
  if (top_pad < scroll) scroll = top_pad;
  const std::size_t bottom = line_ix + 1;
  const std::size_t visible_u = static_cast<std::size_t>(visible);
  if (bottom > scroll + visible_u) scroll = bottom - visible_u;
}

// --- draw -------------------------------------------------------------------

void draw_transfers(const App& app, CellBuffer& buf, int y0, int y1) {
  const int w = buf.width();
  if (y1 <= y0 || w <= 0) return;
  const TransfersState& ts = app.transfers;
  const DownloadQueue& queue = app.downloads;
  const std::vector<TransfersState::Line> lines = ts.layout(queue);

  if (lines.empty()) {
    const std::string msg = "nothing downloaded yet \xC2\xB7 d on an episode starts one";
    const int y = y0 + (y1 - y0) / 2;
    buf.put_str(std::max(0, (w - str_width(msg)) / 2), y, msg, theme::fg3, theme::bg,
                Style::Italic);
    return;
  }

  const std::vector<TransfersState::Item> items = ts.items(queue);
  std::size_t job_count = 0;
  for (const TransfersState::Item& it : items) {
    if (it.kind == TransfersState::Item::Kind::Job) ++job_count;
  }
  std::size_t file_count = 0;
  std::uint64_t disk_bytes = 0;
  for (const DiskShow& show : ts.disk) {
    file_count += show.files.size();
    disk_bytes += show.bytes();
  }

  const int list_w = w - 3;
  int y = y0;
  for (std::size_t i = ts.scroll; i < lines.size() && y < y1; ++i, ++y) {
    const TransfersState::Line& li = lines[i];
    switch (li.kind) {
      case TransfersState::Line::Kind::Blank:
        break;
      case TransfersState::Line::Kind::Rule:
        draw_rule(buf, y, list_w);
        break;
      case TransfersState::Line::Kind::Header: {
        int x = buf.put_str(2, y, li.disk ? "on disk" : "queue", theme::fg, theme::bg,
                            Style::Bold);
        const std::size_t n = li.disk ? file_count : job_count;
        buf.put_str(x, y, " (" + std::to_string(n) + ")", theme::fg2, theme::bg);
        if (li.disk) {
          const std::string total = human_bytes(disk_bytes);
          buf.put_str(tail_x(list_w, str_width(total)), y, total, theme::fg2, theme::bg);
        }
        break;
      }
      case TransfersState::Line::Kind::Job: {
        if (li.item >= items.size()) break;
        const DownloadJob* job = queue.find(items[li.item].job);
        if (job == nullptr) break;
        draw_job_line(buf, y, list_w, *job, li.item == ts.cursor, app.spinner_phase);
        break;
      }
      case TransfersState::Line::Kind::Show: {
        if (li.item >= items.size()) break;
        const std::size_t s = items[li.item].show;
        if (s >= ts.disk.size()) break;
        draw_show_line(buf, y, list_w, ts.disk[s], li.item == ts.cursor);
        break;
      }
      case TransfersState::Line::Kind::File: {
        if (li.item >= items.size()) break;
        const TransfersState::Item& it = items[li.item];
        if (it.show >= ts.disk.size() || it.file >= ts.disk[it.show].files.size()) break;
        const DiskFile& file = ts.disk[it.show].files[it.file];
        const bool armed = ts.confirm_delete.has_value() && *ts.confirm_delete == file.path;
        draw_file_line(buf, y, list_w, file, li.item == ts.cursor, armed);
        break;
      }
    }
  }
}

void draw_download_picker(const DownloadPicker& picker, const DownloadQueue& queue,
                          int spinner_phase, const Rect& area, CellBuffer& buf) {
  if (!picker.open || area.empty()) return;

  const int bw = std::min(kPickerW, std::max(0, area.w - 4));
  const int bh = std::min(kPickerH, std::max(0, area.h - 2));
  if (bw < kPickerMinW || bh < kPickerMinH) {
    const int row = area.h / 2;
    const Rect strip{area.x, area.y + row, area.w, 1};
    buf.fill(strip, theme::elevated);
    const std::string msg = "download: esc to close";
    buf.put_str(area.x + std::max(0, (area.w - str_width(msg)) / 2), area.y + row, msg,
                theme::fg2, theme::elevated);
    return;
  }

  const Rect box{area.x + std::max(0, (area.w - bw) / 2), area.y + std::max(0, (area.h - bh) / 2),
                 bw, bh};
  buf.fill(box, theme::elevated);

  // The show's title is the part cut so the episode always shows.
  const DownloadJob& draft = picker.draft;
  const std::string head = "Download" + std::string(kSep);
  std::string tail(kSep);
  tail += "ep ";
  tail += strip_controls(draft.episode);
  if (draft.translation == Translation::Dub) add_part(tail, "dub");
  const int title_w = bw - 4 - str_width(head) - str_width(tail);
  put_centered(buf, box, 0, head + truncate_to_cols(strip_controls(draft.title), title_w) + tail,
               theme::hot, Style::Bold);

  constexpr int kTop = 2;
  if (picker.job.has_value()) {
    draw_picker_job(picker, queue, spinner_phase, box, buf, kTop);
    return;
  }
  if (picker.loading) {
    put_left(buf, box, kTop, std::string("reading the stream ") + spinner_frame(spinner_phase),
             theme::fg2);
    put_footer(buf, box, "esc close");
    return;
  }
  if (!picker.failed.empty()) {
    put_wrapped(buf, box, kTop, box.h - 3, strip_controls(picker.failed), theme::warn);
    put_footer(buf, box, "esc close");
    return;
  }
  draw_picker_offer(picker, box, buf, kTop);
}

// --- detail -----------------------------------------------------------------

namespace detail {

std::string rung_text(const Rung& rung) {
  if (rung.lowest) return "lowest";
  if (rung.height > 0) return std::to_string(rung.height) + "p";
  if (rung.bandwidth > 0) {
    const std::uint64_t tenths = rung.bandwidth / 100000ull;
    return "under " + std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) +
           " Mbit/s";
  }
  return "best";
}

std::string job_status_text(const DownloadJob& job) {
  switch (job.state) {
    case DownloadJob::State::Queued:
      return "queued";
    case DownloadJob::State::Resolving:
      return job.cancelling() ? "cancelling" : "resolving";
    case DownloadJob::State::Running: {
      if (job.cancelling()) return "cancelling";
      if (job.bytes == 0) return "starting";
      const std::string got = human_bytes(job.bytes);
      if (job.total > 0) {
        const std::uint64_t pct = std::min<std::uint64_t>(100, job.bytes * 100 / job.total);
        return got + " / " + human_bytes(job.total) + "  " + std::to_string(pct) + "%";
      }
      if (job.estimate > 0) return got + " of ~" + human_bytes(job.estimate);
      return got;
    }
    case DownloadJob::State::Done: {
      std::string text = "done";
      add_part(text, human_bytes(job.bytes));
      return text;
    }
    case DownloadJob::State::Failed: {
      std::string text = "failed";
      const std::string reason = strip_controls(job.error);
      if (!reason.empty()) add_part(text, reason);
      return text;
    }
    case DownloadJob::State::Cancelled:
      return "cancelled";
  }
  return {};
}

std::string offer_row_text(const OfferRow& row, std::uint32_t duration_secs) {
  return offer_row_line(row, duration_secs);
}

std::optional<std::size_t> transfers_item_at(const App& app, int px, int py) {
  if (app.view != View::Transfers) return std::nullopt;
  const int list_w = static_cast<int>(app.win.cols) - 3;
  if (px < 2 || px >= 2 + list_w) return std::nullopt;
  const int y0 = kContentY0;
  const int y1 = static_cast<int>(app.win.rows) - 1;
  if (py < y0 || py >= y1) return std::nullopt;
  const std::vector<TransfersState::Line> lines = app.transfers.layout(app.downloads);
  const std::size_t idx = app.transfers.scroll + static_cast<std::size_t>(py - y0);
  if (idx >= lines.size()) return std::nullopt;
  const TransfersState::Line& li = lines[idx];
  switch (li.kind) {
    case TransfersState::Line::Kind::Job:
    case TransfersState::Line::Kind::Show:
    case TransfersState::Line::Kind::File:
      return li.item;
    case TransfersState::Line::Kind::Header:
    case TransfersState::Line::Kind::Rule:
    case TransfersState::Line::Kind::Blank:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace detail

}  // namespace shigoku::tui
