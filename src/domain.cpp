// domain.cpp — pure helper implementations (P1). Ported 1:1 from domain.rs.
//
// All byte/codepoint handling is explicit and byte-wise (§3: never pack bytes
// into multi-byte ints; no locale wcwidth/tolower). UTF-8 is decoded by hand
// so behavior is identical big- and little-endian.

#include "domain.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace shigoku {

namespace {

// Minimal UTF-8 decoder: returns the codepoint starting at bytes[i] and
// advances i past it. On a malformed lead byte, emits the raw byte as U+00xx
// and advances one (lenient — strip_controls only needs to *classify*, and a
// mangled byte < 0x20 or in the C1 range is exactly what we want to drop).
char32_t decode_utf8(std::string_view s, std::size_t& i) {
  const unsigned char b0 = static_cast<unsigned char>(s[i]);
  auto cont = [&](std::size_t k) -> unsigned char {
    return (i + k < s.size()) ? static_cast<unsigned char>(s[i + k]) : 0;
  };
  if (b0 < 0x80) {
    ++i;
    return b0;
  }
  if ((b0 & 0xE0) == 0xC0) {
    const char32_t cp = ((b0 & 0x1Fu) << 6) | (cont(1) & 0x3Fu);
    i += 2;
    return cp;
  }
  if ((b0 & 0xF0) == 0xE0) {
    const char32_t cp =
        ((b0 & 0x0Fu) << 12) | ((cont(1) & 0x3Fu) << 6) | (cont(2) & 0x3Fu);
    i += 3;
    return cp;
  }
  if ((b0 & 0xF8) == 0xF0) {
    const char32_t cp = ((b0 & 0x07u) << 18) | ((cont(1) & 0x3Fu) << 12) |
                        ((cont(2) & 0x3Fu) << 6) | (cont(3) & 0x3Fu);
    i += 4;
    return cp;
  }
  ++i;  // stray continuation / invalid lead — consume one byte.
  return b0;
}

// The banned class from domain.rs::strip_controls::banned.
bool banned_codepoint(char32_t c) {
  return c < 0x20 ||                     // C0 controls
         (c >= 0x7F && c <= 0x9F) ||     // DEL + C1 controls
         (c >= 0x202A && c <= 0x202E) || // bidi overrides
         (c >= 0x2066 && c <= 0x2069) || // bidi isolates
         (c >= 0x200B && c <= 0x200D) || // zero-width space/joiners
         c == 0xFEFF;                    // BOM / zero-width no-break space
}

// Encode a codepoint back to UTF-8 into out.
void encode_utf8(char32_t c, std::string& out) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

bool non_empty(std::optional<std::string_view> s) {
  return s.has_value() && !s->empty();
}

// ASCII case-insensitive compare (no locale, §3).
bool eq_ci(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

}  // namespace

std::string strip_controls(std::string_view s) {
  // Fast path (domain.rs): if nothing is banned, return the input unchanged.
  bool any = false;
  for (std::size_t i = 0; i < s.size();) {
    if (banned_codepoint(decode_utf8(s, i))) {
      any = true;
      break;
    }
  }
  if (!any) return std::string(s);

  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const char32_t c = decode_utf8(s, i);
    if (!banned_codepoint(c)) encode_utf8(c, out);
  }
  return out;
}

std::string_view preferred_title(std::string_view romaji,
                                 std::optional<std::string_view> english,
                                 std::optional<std::string_view> native,
                                 TitleLanguage pref) {
  const bool rom = !romaji.empty();
  switch (pref) {
    case TitleLanguage::Romaji:
      if (rom) return romaji;
      if (non_empty(english)) return *english;
      if (non_empty(native)) return *native;
      break;
    case TitleLanguage::English:
      if (non_empty(english)) return *english;
      if (rom) return romaji;
      if (non_empty(native)) return *native;
      break;
    case TitleLanguage::Native:
      if (non_empty(native)) return *native;
      if (rom) return romaji;
      if (non_empty(english)) return *english;
      break;
  }
  return romaji;  // unwrap_or(romaji): empty romaji is still returned last.
}

double episode_sort_key(std::string_view label) {
  // Leading run of ASCII digits and '.'; parse as double. Non-numeric -> +inf.
  std::size_t end = 0;
  while (end < label.size()) {
    const char c = label[end];
    if (!(c >= '0' && c <= '9') && c != '.') break;
    ++end;
  }
  if (end == 0) return std::numeric_limits<double>::infinity();
  // Parse the prefix. std::from_chars<double> avoids locale surprises, but the
  // prefix may be malformed ("1.2.3", "."); on any parse failure, Rust's
  // `.parse().unwrap_or(INF)` yields +inf, so mirror that.
  const std::string prefix(label.substr(0, end));
  try {
    std::size_t consumed = 0;
    const double v = std::stod(prefix, &consumed);
    if (consumed != prefix.size()) return std::numeric_limits<double>::infinity();
    return v;
  } catch (...) {
    return std::numeric_limits<double>::infinity();
  }
}

int episode_label_cmp(std::string_view a, std::string_view b) {
  const double ka = episode_sort_key(a);
  const double kb = episode_sort_key(b);
  // total_cmp semantics: +inf == +inf compares equal (specials keep order
  // under a stable sort). Regular comparison suffices here since we never
  // produce NaN and both +inf compare equal under <.
  if (ka < kb) return -1;
  if (ka > kb) return 1;
  return 0;
}

ParsedScore parse_user_score(std::string_view input) {
  // Trim ASCII whitespace only (§3: no locale-dependent classification).
  std::size_t begin = 0;
  std::size_t end = input.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
  const std::string_view trimmed = input.substr(begin, end - begin);

  if (trimmed.empty()) return ParsedScore{ScoreParse::Clear, 0};

  // Full-string parse only: "7.5x" or "1.2.3" is junk, not a truncated 7.5/1.2.
  double v = 0.0;
  try {
    std::size_t consumed = 0;
    const std::string s(trimmed);
    v = std::stod(s, &consumed);
    if (consumed != s.size()) return ParsedScore{ScoreParse::Invalid, 0};
  } catch (...) {
    return ParsedScore{ScoreParse::Invalid, 0};
  }
  if (!std::isfinite(v) || v < 0.0 || v > 10.0) return ParsedScore{ScoreParse::Invalid, 0};

  const auto raw = static_cast<std::uint32_t>(std::lround(v * 10.0));
  return ParsedScore{ScoreParse::Value, std::min<std::uint32_t>(raw, 100)};
}

double to_anilist_score(std::uint32_t raw, ScoreFormat format) {
  const double clamped = static_cast<double>(std::min<std::uint32_t>(raw, 100));
  switch (format) {
    case ScoreFormat::Point100:      return clamped;
    case ScoreFormat::Point10Decimal: return std::lround(clamped) / 10.0;
    case ScoreFormat::Point10:       return std::lround(clamped / 10.0);
    case ScoreFormat::Point5:        return std::lround(clamped / 20.0);
    case ScoreFormat::Point3:        return std::lround(clamped / 100.0 * 3.0);
  }
  return clamped;  // unreachable (closed enum).
}

std::uint32_t from_anilist_score(double wire, ScoreFormat format) {
  if (!std::isfinite(wire) || wire <= 0.0) return 0;
  double raw = 0.0;
  switch (format) {
    case ScoreFormat::Point100:      raw = wire; break;
    case ScoreFormat::Point10Decimal: raw = wire * 10.0; break;
    case ScoreFormat::Point10:       raw = wire * 10.0; break;
    case ScoreFormat::Point5:        raw = wire * 20.0; break;
    case ScoreFormat::Point3:        raw = wire / 3.0 * 100.0; break;
  }
  return std::min<std::uint32_t>(static_cast<std::uint32_t>(std::lround(raw)), 100);
}

std::optional<std::size_t> map_episode_index(const std::vector<std::string>& episodes,
                                             std::string_view raw,
                                             std::uint32_t ordinal) {
  for (std::size_t i = 0; i < episodes.size(); ++i) {
    if (episodes[i] == raw) return i;
  }
  if (ordinal == 0) return std::nullopt;  // checked_sub(1) on 0 -> None.
  const std::size_t i = static_cast<std::size_t>(ordinal - 1);
  if (i < episodes.size()) return i;
  return std::nullopt;
}

bool is_still_airing(std::optional<std::string_view> status) {
  if (!status.has_value()) return true;
  const std::string_view s = *status;
  return !(eq_ci(s, "FINISHED") || eq_ci(s, "CANCELLED"));
}

bool is_absolute_url(std::string_view s) {
  return s.substr(0, 8) == "https://" || s.substr(0, 7) == "http://";
}

std::optional<Season> parse_season(std::string_view s) {
  if (eq_ci(s, "winter")) return Season::Winter;
  if (eq_ci(s, "spring")) return Season::Spring;
  if (eq_ci(s, "summer")) return Season::Summer;
  if (eq_ci(s, "fall") || eq_ci(s, "autumn")) return Season::Fall;
  return std::nullopt;
}

namespace {

// Days since 1970-01-01 → (year, month, day), proleptic Gregorian (Howard
// Hinnant's civil_from_days, domain.rs). Valid for any date this app can see.
struct Ymd {
  std::int64_t year;
  std::uint32_t month;
  std::uint32_t day;
};

Ymd civil_from_days(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;                       // [0, 146096]
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
  const std::int64_t mp = (5 * doy + 2) / 153;                       // [0, 11]
  const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;             // [1, 31]
  const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;             // [1, 12]
  return Ymd{y + (month <= 2 ? 1 : 0), static_cast<std::uint32_t>(month),
             static_cast<std::uint32_t>(day)};
}

}  // namespace

Cour current_cour(std::int64_t unix_secs) {
  // div_euclid over a clamped-nonneg instant: a pre-epoch time is 1970 Winter.
  const std::int64_t clamped = unix_secs > 0 ? unix_secs : 0;
  const std::int64_t days = clamped / 86400;
  const Ymd ymd = civil_from_days(days);
  const std::uint32_t year =
      ymd.month == 12 ? static_cast<std::uint32_t>(ymd.year) + 1
                      : static_cast<std::uint32_t>(ymd.year);
  return Cour{season_from_month(ymd.month), year};
}

// --- Schedule (P37 slice 1) -------------------------------------------------

Weekday weekday_of(std::int64_t unix_secs) {
  // 1970-01-01 (epoch day 0) was a Thursday. Floor-divide so a pre-epoch
  // instant still lands on the correct weekday rather than truncating toward
  // zero (C++'s / truncates; div_euclid-style floor needed for negatives).
  const std::int64_t days = unix_secs >= 0
                                ? unix_secs / 86400
                                : -((-unix_secs + 86399) / 86400);
  // Thursday == 3 in the Monday-first enum below; shift so day 0 lands there.
  std::int64_t idx = (days + 3) % 7;
  if (idx < 0) idx += 7;
  return static_cast<Weekday>(idx);
}

std::vector<ScheduleGroup> schedule(const std::vector<Show>& shows,
                                    std::int64_t now_secs) {
  // One bucket per weekday, Monday..Sunday, populated only where non-empty.
  std::array<std::vector<ScheduleEntry>, 7> buckets;
  for (std::size_t i = 0; i < shows.size(); ++i) {
    const Enrichment& e = shows[i].enrichment;
    if (!e.next_airing_at.has_value()) continue;
    const std::int64_t at = *e.next_airing_at;
    if (at <= now_secs) continue;  // already aired, or unknown-past.
    ScheduleEntry entry;
    entry.show_index = i;
    entry.weekday = weekday_of(at);
    entry.airing_at = at;
    entry.episode = e.next_airing_episode.value_or(0);
    entry.seconds_until = at - now_secs;
    buckets[static_cast<std::size_t>(entry.weekday)].push_back(entry);
  }
  std::vector<ScheduleGroup> out;
  for (std::size_t w = 0; w < 7; ++w) {
    if (buckets[w].empty()) continue;
    std::sort(buckets[w].begin(), buckets[w].end(),
              [](const ScheduleEntry& a, const ScheduleEntry& b) {
                return a.airing_at < b.airing_at;
              });
    out.push_back(ScheduleGroup{static_cast<Weekday>(w), std::move(buckets[w])});
  }
  return out;
}

// --- Schedule notices (P37 slice 3) -----------------------------------------

std::vector<ScheduleNotice> detect_schedule_notices(const std::vector<Show>& shows,
                                                     std::int64_t now_secs) {
  std::vector<ScheduleNotice> out;
  for (std::size_t i = 0; i < shows.size(); ++i) {
    const Enrichment& e = shows[i].enrichment;
    if (!e.next_airing_at.has_value() || !e.next_airing_episode.has_value()) continue;
    if (*e.next_airing_at > now_secs) continue;  // still in the future: nothing new aired.
    if (*e.next_airing_episode == 0) continue;   // defensive: ordinals are 1-based.
    // "Episode N airs at T", T has passed -> N itself aired (see domain.hpp;
    // the previous N-1 reading missed every premiere — P37 review).
    const std::uint32_t aired = *e.next_airing_episode;
    const std::uint32_t high_water = shows[i].notice_last_episode.value_or(0);
    if (aired <= high_water) continue;  // already notified (or older than the mark).
    out.push_back(ScheduleNotice{i, aired});
  }
  return out;
}

}  // namespace shigoku
