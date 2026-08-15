// pages.hpp — chapter fetch-to-cache.
//
// Offline-first is the manga contract: there is NO streaming path — reading
// a chapter always materializes its pages on disk first, and a fetched
// chapter stays readable offline forever. The root the app passes lives
// under the DATA dir, not the cache dir, for exactly that reason: these are
// durable downloads, not evictable cache.
//
// Layout (the store is a browsable COLLECTION, not an opaque cache, so the
// on-disk names must carry the titles):
//   <root>/<title [mid8]>/<ch NNNN… [lang cid8]>/NNN.<ext>
// via library_chapter_dir below; page files named by 1-based index,
// zero-padded to 3 (natural sort in the viewer tolerates the >999 overflow),
// extension carried over from the unit's name hint. Each page writes
// "<name>.part" and atomically renames on success (a corrupt final file must
// be impossible); a completed chapter is marked by an empty ".complete"
// file, and fetch_chapter_pages short-circuits page-by-page on re-entry
// (partial fetches resume where they died). The uuid layout
// (<root>/<manga_uuid>/<chapter_uuid>, chapter_dir) survives as a read-only
// legacy fallback so pre-rename downloads stay readable — offline-first
// forbids orphaning fetched pages over a naming change.
//
// Node rotation (MangaDex@Home semantics, generalized by the multi-source
// seam): a PageSet is an ephemeral resolution — at-home node grants expire,
// scrapes go stale. On a page GET failing 403/404 rotate immediately; on
// 408/429/5xx/transport retry once with a 1s backoff, then rotate. Rotation
// re-resolves ONCE per chapter fetch (the `refetch` callback — for MangaDex
// a fresh node grant, for a scrape source a re-scrape) and resumes from the
// failed page; a second failure fails the chapter, completed pages kept. A
// rotated set with fewer pages than the first is refused at the point the
// index overruns it (never a hole in the chapter).
//
// Guard contract (the download.hpp mechanism/policy split): this core does
// NOT re-guard — the orchestrator (mapp's pages worker) runs
// http::guard_fetch_url over EVERY unit's url BEFORE handing a set in, the
// initial resolution and each refetch alike (the loopback test fixtures
// depend on the core not re-guarding).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../error.hpp"
#include "../http.hpp"
#include "../result.hpp"
#include "model.hpp"
#include "source.hpp"

namespace shigoku::manga {

// <root>/<manga_uuid>/<chapter_uuid> — the legacy layout, now only the
// fallback read path for pre-library downloads. Both ids are uuid_shaped
// (vetted at parse time) — this builder trusts its inputs.
[[nodiscard]] std::string chapter_dir(std::string_view root,
                                      std::string_view manga_id,
                                      std::string_view chapter_id);

// A human title as ONE filesystem segment, deterministically (the dir must be
// re-derivable from the API objects on every run): '/', '\', ':' and control
// bytes become '_' (colon = HFS+/Finder separator on the PPC target),
// whitespace runs collapse to one space, a leading '.' or '-' is shielded
// with '_' (hidden / flag-looking names), trailing spaces and dots trimmed,
// UTF-8 kept and capped at 100 bytes on a codepoint boundary; empty result ->
// "untitled".
[[nodiscard]] std::string fs_name(std::string_view title);

// "5" -> "0005", "10.5" -> "0010.5": the leading integer run is zero-padded
// to 4 so a lexicographic dir listing IS chapter order. Non-numeric or empty
// input passes through unchanged.
[[nodiscard]] std::string pad_chapter(std::string_view chapter);

// <root>/<fs_name(title)> [mid8]/ch <pad_chapter(n)> [lang cid8] — mid8/cid8
// are the first 8 id chars, the uniqueness anchor (two manga sharing a
// sanitized title; the same chapter number across scanlation groups). An
// empty chapter number (oneshot) becomes "oneshot". cid8 skips a manga-id
// prefix on the chapter id first (Dynasty permalinks are "{series_slug}_ch01",
// so the raw first-8 is the same string for the whole series); uuid/ULID ids
// never carry that prefix, so md/wc dirs are unchanged.
[[nodiscard]] std::string library_chapter_dir(std::string_view root,
                                              const MdManga& manga,
                                              const MdChapter& chapter);

// The ".complete" marker exists.
[[nodiscard]] bool chapter_complete(const std::string& dir);

// The chapter's page files (final names only — never .part, never the
// marker), absolute paths, sorted ascending. The zero-padded names make
// lexicographic == page order. Recognized extensions are the viewer's
// (jpg/jpeg/png/webp).
[[nodiscard]] std::vector<std::string> list_pages(const std::string& dir);

// Per-page tick, fired after each page lands (or is found already on disk).
using PagesProgressFn =
    std::function<void(std::uint32_t done, std::uint32_t total)>;

// One fresh page resolution, called at most once per fetch_chapter_pages
// (the rotation pin above).
using PagesRefetchFn = std::function<Result<PageSet, ProviderError>()>;

// Blocking (worker thread, A1). Fetches every unit of `pages` into `dir`,
// then writes the .complete marker. Ok = the page count. Site policy
// (data-saver variants, referers, URL shapes) is the SOURCE's business —
// already baked into the set by MangaSource::pages().
[[nodiscard]] Result<std::uint32_t, ProviderError> fetch_chapter_pages(
    const http::Client& client, const PageSet& pages, const std::string& dir,
    const PagesProgressFn& progress, const PagesRefetchFn& refetch);

namespace detail {

// "NNN.<ext>" for 0-based `index`; extension from the unit's name hint
// (source-vetted at parse time), "jpg" when it has none.
[[nodiscard]] std::string page_filename(std::size_t index,
                                        std::string_view name_hint);

// mkdir -p (0755). Ok on already-exists.
[[nodiscard]] Result<Unit, ProviderError> mkdir_p(const std::string& dir);

}  // namespace detail

}  // namespace shigoku::manga
