// archive_names.hpp — pure name handling for comic archives (CBZ/CBR/CBT):
// turning a raw archive entry name into a safe flat basename (the zip-slip
// fence), disambiguating collisions, and picking the page images out of
// everything else an archive may hold. No libarchive include anywhere near
// this header — ArchiveSource is the only thing that opens a real archive;
// this is what viewer_tests links against to prove the naming rule alone.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku::view {

// Turn a raw archive entry name into a safe flat basename, or nullopt when
// nothing usable survives. Backslashes become '/', then the name is walked
// segment by segment: empty and '.' segments are dropped, a '..' segment
// pops the last real segment already kept (or is itself dropped if there is
// none to pop — an escape above the root simply vanishes rather than erroring),
// and what is left is joined with '_'. So `ch1/010.png` -> `ch1_010.png`,
// `sub/../weird/../ch1/002.png` -> `ch1_002.png` (the '..'s cancel their
// siblings), and `../evil.png` -> `evil.png`. No '/' survives the join, so a
// sanitized name can never name a path outside the directory it is written
// into — ArchiveSource's whole zip-slip fence lives in this one rule.
// nullopt when every segment was dropped (an empty result) or `raw` holds a
// control byte anywhere.
[[nodiscard]] std::optional<std::string> sanitize_entry_name(
    std::string_view raw);

// The name to use for the `count`-th time `name` is produced (1-based:
// count == 1 returns `name` unchanged). count > 1 splices `~count` in just
// before the extension (`page.png` -> `page~2.png`), or appends it when
// there is none. Exposed on its own (not just folded into dedupe_names) so
// ArchiveSource can apply the identical rule while it streams entries one at
// a time, without buffering the whole archive to build a list first.
[[nodiscard]] std::string disambiguate(const std::string& name, int count);

// Make `names` pairwise distinct, order preserved: a repeat gets `~2`, `~3`,
// … via disambiguate(), counting occurrences of each exact name in order.
[[nodiscard]] std::vector<std::string> dedupe_names(
    std::vector<std::string> names);

// Keep only the names is_image_ext() (pager.hpp) accepts, order preserved —
// an archive entry that is not a page image (notes.txt, a directory record)
// is simply not a page.
[[nodiscard]] std::vector<std::string> select_image_entries(
    std::vector<std::string> names);

}  // namespace shigoku::view
