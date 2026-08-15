// mconfig.hpp — config-lite for the manga app: its own tiny config.json
// under the shigoku-manga dirs (the two apps must not share state). The same
// contract as the anime app's config, in miniature: total load with
// per-field defaulting (a typo'd key degrades at the call site, never fails
// the load), atomic tmp+rename save. save_default_if_missing gives the user
// a file to edit — there is no Settings view in the manga app yet, the file
// IS the UI.

#pragma once

#include <string>

#include "../result.hpp"

namespace shigoku::manga {

struct MangaConfig {
  std::string viewer = "builtin";  // builtin | preview | custom.
  std::string viewer_path;         // custom only; "" degrades to builtin.
  bool rtl = false;                // right-to-left page order default.
  bool data_saver = false;         // MangaDex compressed page variant.
  std::string chapter_lang = "en";
  // Registry-lite: the active source key ("md"/"wc"/"dy"/"nh"; an unknown
  // key degrades to the first eligible source via pick_source) and the nsfw
  // gate (off = nsfw sources are invisible everywhere).
  std::string source = "md";
  bool nsfw_sources = false;
  // The AniList master switch: off means no chapter read ever leaves the
  // machine, connected or not. On by default — a connected account with a
  // silent tracker would be the surprising state.
  bool anilist_sync = true;
  friend bool operator==(const MangaConfig&, const MangaConfig&) = default;
};

// Missing file, unreadable, oversized, or non-JSON → all defaults;
// individual missing/ill-typed keys default individually.
[[nodiscard]] MangaConfig load_manga_config(const std::string& path);

// Parse seam for the tests (bytes → config, same defaulting rules).
[[nodiscard]] MangaConfig parse_manga_config(const std::string& raw);

// Atomic save (tmp + rename). Err = a one-line reason.
[[nodiscard]] Result<Unit, std::string> save_manga_config(
    const std::string& path, const MangaConfig& c);

// Write defaults if `path` does not exist yet (best-effort; the app boots
// fine either way).
void save_default_if_missing(const std::string& path);

}  // namespace shigoku::manga
