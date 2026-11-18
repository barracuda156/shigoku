// util.hpp — helpers every manga source shares: URL and filename vets, text
// cleanup, chapter ordering. Free functions in `detail` so the offline tests
// can reach them.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "model.hpp"

namespace shigoku::manga::detail {

// RFC 3986: unreserved [A-Za-z0-9._~-] pass, everything else %XX (uppercase
// hex, UTF-8 bytes escaped individually). Space is %20, never '+'.
[[nodiscard]] std::string url_encode(std::string_view s);

// Charset vet: non-empty, [A-Za-z0-9._-]+ only, no ".." run and no leading
// '.' — a name that passes can reach a URL or a cache path.
[[nodiscard]] bool filename_safe(std::string_view s);

// The pinned chapter order: parse the leading decimal number of each ("10.5"
// → 10.5, strtod semantics); both numeric → compare by value, ties →
// lexicographic on the full string; numeric sorts BEFORE non-numeric
// (oneshots/"extras" trail the numbered run); both non-numeric (incl. "") →
// lexicographic.
[[nodiscard]] bool chapter_less(std::string_view a, std::string_view b);

// chapter_less on the chapter string, then publish_at, then id.
[[nodiscard]] bool chapter_before(const MdChapter& a, const MdChapter& b);

// Duplicate scanlations of the same chapter STRING (exact match — "1" and
// "1.0" are distinct): keep the newest publish_at (lexicographic max; ties →
// first seen), drop the rest. Output sorted by chapter_less (id as the
// final tie-break for determinism). Input is one lang already (the feed
// filtered server-side).
[[nodiscard]] std::vector<MdChapter> dedupe_chapters(std::vector<MdChapter> in);

// Minimal HTML entity unescape for scraped text (titles/labels): the five
// named entities (&amp; &lt; &gt; &quot; &#39;/&apos;) plus decimal and hex
// numeric references, emitted as UTF-8. Unknown entities pass through
// verbatim (never drop text over markup trivia).
[[nodiscard]] std::string html_unescape(std::string_view s);

// Spaces, tabs, CR and LF stripped from both ends.
[[nodiscard]] std::string_view trim(std::string_view s);

}  // namespace shigoku::manga::detail
