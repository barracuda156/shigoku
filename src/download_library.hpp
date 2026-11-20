// download_library.hpp — what is on disk under the download root, read back
// from the layout download.hpp writes:
//
//   <download_dir>/<show folder>/<sub|dub>/<label>.<ext>        a finished file
//   <download_dir>/<show folder>/<sub|dub>/<label>.<ext>.part   an unfinished one
//
// The show folder is an AniList id (negative for a show only MyAnimeList
// names) or `<provider>-<provider id>` for a show only its source names.
// Nothing here touches the network or the store; titles are the caller's.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"

namespace shigoku::download {

// One file in a show's track folder.
struct LocalEpisode {
  std::string label;  // the file's stem: the episode label as it was saved.
  Translation translation = Translation::Sub;
  std::string path;   // the full path, download root included.
  std::uint64_t bytes = 0;
  // An unfinished transfer's leftover (`<label>.<ext>.part`): a download of
  // the same episode picks it up where it stopped.
  bool partial = false;

  friend bool operator==(const LocalEpisode&, const LocalEpisode&) = default;
};

// One show folder that holds at least one episode file.
struct LocalShow {
  std::string folder;                      // the directory's own name.
  std::optional<std::int64_t> anilist_id;  // set when the folder is a catalogue id.
  std::vector<LocalEpisode> episodes;      // sub before dub, then episode order.

  friend bool operator==(const LocalShow&, const LocalShow&) = default;
};

// Every show folder under `download_dir` with what it holds. "" (downloads
// disabled), a missing root and an unreadable one all read as empty.
//
//   * A show folder is a directory whose name does not start with '.'.
//     `anilist_id` is set when the whole name is a decimal integer, with an
//     optional leading '-', that fits std::int64_t.
//   * Only the `sub` and `dub` directories inside it are read; only regular
//     files inside those count (symlinks are followed, as stat does).
//   * `<stem>.<ext>` with a non-empty, dot-free <ext> other than "part" is a
//     finished file, label = <stem> (the split is at the LAST dot, so the
//     label "12.5" saved as "12.5.mp4" reads back as "12.5").
//   * `<stem>.<ext>.part` is an unfinished one, label = <stem>. It is left out
//     when the same track already holds a finished file with that label.
//   * Several finished files for one label and track (different extensions):
//     the lexicographically smallest path is the one listed — the file
//     find_local_episode would play.
//   * A show folder left with no episodes is not listed.
//
// Shows come back ordered by folder name (byte order); a show's episodes sub
// before dub and, within a track, by episode_label_cmp, equal keys by label.
[[nodiscard]] std::vector<LocalShow> scan_library(std::string_view download_dir);

// Delete one listed file, then its track folder and its show folder when
// that leaves them empty. True when the file is gone (it was removed, or was
// not there). The folders are best effort and never change the answer.
bool remove_local_episode(const LocalEpisode& episode);

}  // namespace shigoku::download
