// transfers.hpp — the download queue, the picker `d` opens over an episode,
// and the Transfers view: the queue plus what is on disk. Pure state and
// render (the render core's rule: no network, no store, no filesystem) —
// the workers, the disk scan and every side effect are app.cpp's.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../domain.hpp"
#include "../offer.hpp"
#include "cells.hpp"
#include "toast.hpp"  // TickCount.

namespace shigoku::tui {

// One episode download: what to fetch (fixed when it is queued) and what its
// transfer has reported so far. Session-scoped: nothing here is persisted.
struct DownloadJob {
  enum class State {
    Queued,     // waiting for a free slot.
    Resolving,  // its worker is asking the source for the stream.
    Running,    // the transfer is writing the .part.
    Done,
    Failed,
    Cancelled,
  };

  std::uint64_t id = 0;  // unique for the session; DownloadQueue::add stamps it.
  State state = State::Queued;

  // --- what to fetch ---
  std::int64_t show_id = 0;    // the catalogue id; the folder the file lands in.
  std::string title;           // the show's display title when queued.
  std::string episode;         // the raw episode label.
  std::uint32_t ordinal = 0;   // its 1-based position in the grid when queued.
  Translation translation = Translation::Sub;
  std::string provider;        // the source's registry name ("" = the primary).
  std::string provider_label;  // its display name, for the row.
  std::string provider_id;     // the show's id at that source.
  // What the source is asked for when the job resolves (a source that picks
  // its stream by quality needs it), and the rung then taken off the
  // stream's own ladder.
  Quality quality = Quality::Best;
  Rung rung;
  // The catalogue's episode length, 0 = unknown: with a variant's bitrate it
  // gives the size estimate.
  std::uint32_t duration_secs = 0;

  // --- what the transfer reports ---
  std::optional<std::uint32_t> height;  // the variant actually being fetched.
  std::uint64_t bytes = 0;
  std::uint64_t total = 0;     // 0 = the server gave none.
  std::uint64_t estimate = 0;  // 0 = none; shown only while `total` is 0.
  std::string dest;            // the final path, known once resolved.
  std::string error;           // Failed: the reason, one short clause.
  TickCount started_tick = 0;  // when it left the queue (the row's slow colour).

  // Shared with the job's worker while it is in flight. `cancel` set = the
  // worker stops and reports back; `child_pid` is the live ffmpeg (0 = none),
  // which quit signals directly.
  std::shared_ptr<std::atomic<bool>> cancel;
  std::shared_ptr<std::atomic<long>> child_pid;

  // Queued, Resolving or Running.
  [[nodiscard]] bool active() const;
  // Resolving or Running: a worker holds it.
  [[nodiscard]] bool in_flight() const;
  // In flight with its cancel flag set: the worker has not answered yet.
  [[nodiscard]] bool cancelling() const;
};

// The queue: jobs in the order they were queued. Every transition a job can
// make is a method here, so the state machine is testable without a worker.
struct DownloadQueue {
  std::vector<DownloadJob> jobs;
  std::uint64_t next_id = 1;

  // Append `job` as Queued under a fresh id (its id, state and report fields
  // are overwritten); returns the id.
  std::uint64_t add(DownloadJob job);

  [[nodiscard]] DownloadJob* find(std::uint64_t id);
  [[nodiscard]] const DownloadJob* find(std::uint64_t id) const;

  // The job for one episode of one track: the most recently queued one that
  // matches, whatever its state. nullptr when there is none.
  [[nodiscard]] const DownloadJob* for_episode(std::int64_t show_id, Translation translation,
                                               std::string_view episode) const;

  [[nodiscard]] std::size_t in_flight() const;  // Resolving + Running.
  [[nodiscard]] std::size_t waiting() const;    // Queued.
  // in_flight() + waiting(): what the top bar's chip counts.
  [[nodiscard]] std::size_t pending() const;

  // The job to start next: the first Queued one, while fewer than `parallel`
  // are in flight; else nullptr. The caller moves it on with begin().
  [[nodiscard]] DownloadJob* next_to_start(std::uint32_t parallel);

  // Queued -> Resolving: fresh cancel flag and child pid (both heap-shared,
  // both zero), the report fields cleared, started_tick stamped. False (and
  // nothing changed) for an unknown id or a job that is not Queued.
  bool begin(std::uint64_t id, TickCount now);

  // --- the worker's reports. Each is ignored for an unknown id or a job
  // that is not in flight. ---
  // Resolving -> Running with what the transfer turned out to be.
  void on_started(std::uint64_t id, std::string dest, std::optional<std::uint32_t> height,
                  std::uint64_t estimate);
  // Byte counts (also taken while still Resolving: a report can overtake
  // the start notice). `total` 0 = unknown, and never replaces a known one.
  void on_progress(std::uint64_t id, std::uint64_t bytes, std::uint64_t total);
  // -> Done; `bytes` is the finished file's size (0 = keep the last count).
  void on_done(std::uint64_t id, std::uint64_t bytes);
  // -> Cancelled when `cancelled`, else Failed with `reason`.
  void on_failed(std::uint64_t id, bool cancelled, std::string reason);

  // --- the user's moves; true when something changed. ---
  // Queued -> Cancelled at once. In flight -> its cancel flag is set and it
  // stays in flight until the worker reports (the caller also signals the
  // child). Anything else: false.
  bool cancel(std::uint64_t id);
  // Failed or Cancelled -> Queued again, at its place in the list, the
  // report fields cleared.
  bool retry(std::uint64_t id);
  // Take a job that is not active (Done, Failed, Cancelled) off the list.
  bool remove(std::uint64_t id);
};

// The modal `d` opens over an episode. Two faces: the offer (the stream's
// variants, Enter queues the one under the cursor), or — when the episode
// already has a job that is not Done — that job (cancel / retry).
struct DownloadPicker {
  bool open = false;
  // Stamps the probe in flight; an answer under another token is dropped.
  std::uint64_t token = 0;
  // The job Enter will queue: everything about the episode, with `rung`
  // filled from the picked row at that moment.
  DownloadJob draft;
  bool loading = false;  // the probe is out.
  std::string failed;    // why the probe failed ("" = it did not).
  StreamOffer offer;     // meaningful once !loading && failed.empty().
  std::size_t cursor = 0;  // a row of `offer`.
  // A finished file for this episode is already on disk (its size): taking a
  // row fetches it again over that file.
  std::optional<std::uint64_t> on_disk;
  // Set = the job face, showing this job; the offer fields are unused.
  std::optional<std::uint64_t> job;
};

// One file on disk, as the view lists it.
struct DiskFile {
  std::string label;
  Translation translation = Translation::Sub;
  std::string path;
  std::uint64_t bytes = 0;
  bool partial = false;  // an unfinished transfer's leftover.

  friend bool operator==(const DiskFile&, const DiskFile&) = default;
};

// One show on disk. `title` is the library's title for it, else the folder's
// name; `entry` is the library's catalogue row when it has one — Enter opens
// that show's page, and a file's play is filed under it.
struct DiskShow {
  std::string folder;
  std::optional<std::int64_t> show_id;
  std::string title;
  std::optional<Enrichment> entry;
  std::vector<DiskFile> files;

  [[nodiscard]] std::uint64_t bytes() const;  // every file's size, summed.
};

// The Transfers view: two sections, "queue" (the jobs that are not Done:
// those in flight first, then the queued, then the failed and cancelled, each
// group in queue order) and "on disk" (each show, then its files). The cursor
// walks one flat list of items over both.
struct TransfersState {
  std::vector<DiskShow> disk;  // as last scanned.
  std::size_t cursor = 0;      // an index into items().
  std::size_t scroll = 0;      // the first visible line of layout().
  // The file armed for deletion, by path: the next key confirms or cancels.
  std::optional<std::string> confirm_delete;
  // The show whose page was opened from this view, for as long as that page
  // is up: the page renders this copy, so the list may regroup and be read
  // again underneath it.
  std::optional<Enrichment> zoomed;

  struct Item {
    enum class Kind { Job, Show, File } kind = Kind::Job;
    std::uint64_t job = 0;  // Job: its id.
    std::size_t show = 0;   // Show, File: an index into `disk`.
    std::size_t file = 0;   // File: an index into disk[show].files.
    friend bool operator==(const Item&, const Item&) = default;
  };
  // Everything the cursor can rest on, in display order.
  [[nodiscard]] std::vector<Item> items(const DownloadQueue& queue) const;
  [[nodiscard]] std::optional<Item> selected(const DownloadQueue& queue) const;

  // The line list draw and nav share. A section with nothing in it is left
  // out whole; with both empty the list is empty (the view then draws its
  // one centred line). A section is Header, Rule, then its item lines; one
  // Blank between the two sections; inside "on disk" one Blank before each
  // show after the first.
  struct Line {
    enum class Kind { Header, Rule, Job, Show, File, Blank } kind = Kind::Blank;
    bool disk = false;     // Header: which section.
    std::size_t item = 0;  // Job, Show, File: the index into items().
    friend bool operator==(const Line&, const Line&) = default;
  };
  [[nodiscard]] std::vector<Line> layout(const DownloadQueue& queue) const;

  void nav(int dy, int visible, const DownloadQueue& queue);
  void jump(bool top, int visible, const DownloadQueue& queue);
  // Pull the cursor and the scroll back into range after the lists changed
  // (a job finished, a rescan), keeping the cursor line on screen.
  void clamp(int visible, const DownloadQueue& queue);
  // Move the cursor onto an item; false when it is not listed.
  bool select(const Item& item, int visible, const DownloadQueue& queue);

 private:
  void scroll_into_view(int visible, const DownloadQueue& queue);
};

// The picker modal, centred over `area` (the whole screen), drawn last. A
// screen too small for the box gets the one-line strip the other modals fall
// back to. `spinner_phase` indexes kSpinnerFrames.
void draw_download_picker(const DownloadPicker& picker, const DownloadQueue& queue,
                          int spinner_phase, const Rect& area, CellBuffer& buf);

namespace detail {
// "1080p", "best", "lowest", "under 2.4 Mbit/s": a rung in words, for a row
// whose transfer has not said yet what it is fetching.
[[nodiscard]] std::string rung_text(const Rung& rung);

// A job row's right-hand status, without colour:
//   Queued     "queued"
//   Resolving  "resolving"            (the draw appends the spinner)
//   Running    "412 MB / 1.3 GB  31%" with a total,
//              "412 MB of ~1.3 GB"    with an estimate only,
//              "412 MB"               with neither, "starting" at 0 bytes
//   cancelling "cancelling"           (in flight, cancel flag set)
//   Done       "done · 1.3 GB"
//   Failed     "failed · <reason>"
//   Cancelled  "cancelled"
[[nodiscard]] std::string job_status_text(const DownloadJob& job);

// One picker row, in offer_row_line's words (offer.hpp): "1920×1080 ·
// ~1.3 GB · avc1.64001f,mp4a.40.2 · audio: Native".
[[nodiscard]] std::string offer_row_text(const OfferRow& row, std::uint32_t duration_secs);
}  // namespace detail

}  // namespace shigoku::tui
