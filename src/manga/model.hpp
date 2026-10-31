// model.hpp — the cross-source manga model. MdManga / MdChapter were born in
// mangadex.hpp and became the app-wide model the moment the source seam
// landed; the "Md" prefix is historical, kept to avoid churning every
// consumer. Every source's parsers produce these; ids
// inside are the source's NATIVE ids (uuid for md, ULID for wc, slug for dy and
// mh, numeric for nh) — anything crossing into store space rides scoped_id
// (source.hpp).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shigoku::manga {

struct MdManga {
  // Native source id (vetted at parse time). Empty = the row did not come
  // from a source (an AniList recommendation): nothing keyed on the source
  // — chapters, covers, the store — applies until it is bound to one.
  std::string id;
  std::string title;           // title.en → title["ja-ro"] → first entry.
  std::optional<int> year;
  std::string status;          // raw API string: "ongoing"/"completed"/…
  std::string description;     // description.en → first entry → "".
  std::string cover_filename;  // cover_art rel fileName; "" = no/vetoed cover.
  std::optional<std::int64_t> al_id;   // attributes.links.al ("105778", string
  std::optional<std::int64_t> mal_id;  // in the API); non-numeric → nullopt.
  // An absolute cover URL for a row without a source ("" = ask the source).
  std::string cover_url;
  // Other titles the row is known by, for matching it across sources; never
  // shown.
  std::vector<std::string> alt_titles;
  friend bool operator==(const MdManga&, const MdManga&) = default;
};

// A row from a source, as opposed to an AniList row still to be bound.
[[nodiscard]] inline bool bound(const MdManga& m) { return !m.id.empty(); }

struct MdChapter {
  std::string id;        // native source id (vetted).
  std::string chapter;   // "10.5" — string, may be "" (oneshot: null in API).
  std::string title;     // may be "".
  std::uint32_t pages = 0;
  std::string lang;      // attributes.translatedLanguage.
  std::string publish_at;  // ISO8601 as-is; API always emits +00:00, so
                           // lexicographic compare IS chronological compare.
  friend bool operator==(const MdChapter&, const MdChapter&) = default;
};

}  // namespace shigoku::manga
