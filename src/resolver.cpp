// resolver.cpp — Tier-B/C candidate scoring (P15). See resolver.hpp.
//
// Byte-exact port of sabigoku src/resolver.rs + the title_score cluster in
// src/anilist.rs. Every numeric constant, tier, threshold, and branch order is
// transcribed from the Rust; the tests (tests/resolver_tests.cpp) are the
// golden vectors and pin each value.

#include "resolver.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace shigoku::resolver {
namespace {

// Unsigned absolute difference — matches Rust u32::abs_diff, overflow-safe at
// the u32::MAX garbage-episode-count test (resolver.rs:546).
[[nodiscard]] std::uint32_t abs_diff_u32(std::uint32_t a, std::uint32_t b) {
  return a > b ? a - b : b - a;
}

// ASCII lowercase of a single byte (no locale, §3).
[[nodiscard]] char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool ascii_alnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] bool ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }

// Case-insensitive ASCII equality of a status against a literal (Rust
// str::eq_ignore_ascii_case).
[[nodiscard]] bool eq_ignore_ascii_case(std::string_view s, std::string_view lit) {
  if (s.size() != lit.size()) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (ascii_lower(s[i]) != ascii_lower(lit[i])) return false;
  }
  return true;
}

}  // namespace

std::string normalize_title(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c < 0x80) {
      if (ascii_alnum(c)) out.push_back(ascii_lower(static_cast<char>(c)));
      continue;  // drop ASCII non-alphanumerics (space, punctuation).
    }
    out.push_back(static_cast<char>(c));  // non-ASCII byte: verbatim.
  }
  return out;
}

std::string canon_season(std::string_view s) {
  const std::string_view kKw = "season";
  const std::size_t idx = s.find(kKw);
  if (idx == std::string_view::npos) return std::string(s);

  const std::string_view before = s.substr(0, idx);
  const std::string_view after = s.substr(idx + kKw.size());

  // Form A: digits directly after the keyword ("season2").
  std::size_t dlen = 0;
  while (dlen < after.size() && ascii_digit(static_cast<unsigned char>(after[dlen]))) ++dlen;

  std::string_view base_pre;
  std::string_view num;
  std::string_view tail;
  if (dlen > 0) {
    base_pre = before;
    num = after.substr(0, dlen);
    tail = after.substr(dlen);
  } else {
    // Form B: digits (+ ordinal) directly before the keyword ("2ndseason").
    std::string_view b = before;
    for (std::string_view ord : {"st", "nd", "rd", "th"}) {
      if (b.size() >= ord.size() && b.substr(b.size() - ord.size()) == ord) {
        b = b.substr(0, b.size() - ord.size());
        break;  // only ONE ordinal stripped (Rust `break`).
      }
    }
    std::size_t digits = 0;
    while (digits < b.size() &&
           ascii_digit(static_cast<unsigned char>(b[b.size() - 1 - digits])))
      ++digits;
    if (digits == 0) return std::string(s);  // "season" wasn't a season marker.
    const std::size_t dstart = b.size() - digits;
    base_pre = b.substr(0, dstart);
    num = b.substr(dstart);
    tail = after;
  }

  std::string out;
  out.reserve(base_pre.size() + 1 + num.size() + tail.size());
  out.append(base_pre);
  out.push_back('s');
  out.append(num);
  out.append(tail);
  return out;
}

std::int32_t title_score(std::string_view a, std::optional<std::string_view> b) {
  if (!b.has_value()) return -5000;
  std::string_view bv = *b;
  if (a.empty() || bv.empty()) return -5000;
  const std::string na = canon_season(normalize_title(a));
  const std::string nb = canon_season(normalize_title(bv));
  if (na.empty() || nb.empty()) return -5000;
  if (na == nb) return 1600;
  if (nb.rfind(na, 0) == 0 || na.rfind(nb, 0) == 0) return 1250;  // starts_with.
  if (nb.find(na) != std::string::npos || na.find(nb) != std::string::npos) return 900;
  return -5000;
}

std::uint32_t candidate_episodes(const SearchHit& cand) {
  if (cand.total_episodes.has_value()) return *cand.total_episodes;
  return std::max(cand.eps_sub, cand.eps_dub);
}

bool total_is_authoritative(std::optional<std::string_view> status) {
  if (!status.has_value()) return true;  // null status IS authoritative.
  return !eq_ignore_ascii_case(*status, "RELEASING") &&
         !eq_ignore_ascii_case(*status, "HIATUS");
}

namespace {

// A view over an optional<string> for the title cross product.
[[nodiscard]] std::optional<std::string_view> as_view(const std::optional<std::string>& o) {
  if (!o.has_value()) return std::nullopt;
  return std::string_view(*o);
}

// id_match_contradicted (resolver.rs:48-59): strong-evidence veto on a
// same-id candidate — an authoritative episode gap > 3, or a year gap > 1.
[[nodiscard]] bool id_match_contradicted(const Enrichment& canonical, const SearchHit& cand) {
  const std::uint32_t known_eps = canonical.total_episodes.value_or(0);
  const std::uint32_t cand_eps = candidate_episodes(cand);
  if (known_eps > 0 && cand_eps > 0 &&
      total_is_authoritative(canonical.status.has_value()
                                 ? std::optional<std::string_view>(*canonical.status)
                                 : std::nullopt) &&
      abs_diff_u32(known_eps, cand_eps) > 3) {
    return true;
  }
  if (canonical.year.has_value() && cand.year.has_value() &&
      abs_diff_u32(*canonical.year, *cand.year) > 1) {
    return true;
  }
  return false;
}

// id_match_corroborated (resolver.rs:63-70): positive agreement — an episode
// gap <= 3 (NOT gated on authoritative), or a year gap <= 1.
[[nodiscard]] bool id_match_corroborated(const Enrichment& canonical, const SearchHit& cand) {
  const std::uint32_t known_eps = canonical.total_episodes.value_or(0);
  const std::uint32_t cand_eps = candidate_episodes(cand);
  if (known_eps > 0 && cand_eps > 0 && abs_diff_u32(known_eps, cand_eps) <= 3) {
    return true;
  }
  if (canonical.year.has_value() && cand.year.has_value() &&
      abs_diff_u32(*canonical.year, *cand.year) <= 1) {
    return true;
  }
  return false;
}

}  // namespace

std::int32_t candidate_score(const Enrichment& canonical, const SearchHit& cand) {
  std::int32_t score = std::numeric_limits<std::int32_t>::min() / 4;  // -536870912.

  // Best title agreement over known × candidate titles (3x3 cross product).
  const std::array<std::optional<std::string_view>, 3> known = {
      std::optional<std::string_view>(std::string_view(canonical.title_romaji)),
      as_view(canonical.title_english),
      as_view(canonical.title_native),
  };
  const std::array<std::optional<std::string_view>, 3> cand_titles = {
      std::optional<std::string_view>(std::string_view(cand.title)),
      as_view(cand.title_english),
      as_view(cand.title_native),
  };
  for (const auto& k : known) {
    if (!k.has_value() || k->empty()) continue;  // flatten + skip empty known.
    for (const auto& c : cand_titles) {
      score = std::max(score, title_score(*k, c));
    }
  }
  if (score < 0) return score;  // no title overlap → reject before tie-breakers.

  const std::uint32_t known_eps = canonical.total_episodes.value_or(0);
  const std::uint32_t cand_eps = candidate_episodes(cand);
  if (known_eps > 0 && cand_eps > 0) {
    const std::uint32_t diff = abs_diff_u32(known_eps, cand_eps);
    if (diff == 0) {
      score += 180;
    } else if (diff <= 1) {
      score += 120;
    } else if (diff <= 3) {
      score += 60;
    } else if (total_is_authoritative(canonical.status.has_value()
                                          ? std::optional<std::string_view>(*canonical.status)
                                          : std::nullopt)) {
      return -4000;  // movie↔series guard.
    } else {
      score -= 120;
    }
  }

  if (canonical.year.has_value() && cand.year.has_value()) {
    const std::uint32_t diff = abs_diff_u32(*canonical.year, *cand.year);
    if (diff == 0) {
      score += 120;
    } else if (diff == 1) {
      score += 40;
    } else {
      score -= 160;
    }
  }

  return score;
}

std::optional<std::size_t> best_id_match(const Enrichment& canonical,
                                         const std::vector<SearchHit>& candidates) {
  std::optional<std::size_t> bare;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const SearchHit& cand = candidates[i];
    const bool id_agrees =
        (cand.anilist_id.has_value() && *cand.anilist_id == canonical.anilist_id) ||
        (canonical.mal_id.has_value() && cand.mal_id == canonical.mal_id);
    if (!id_agrees) continue;
    if (id_match_contradicted(canonical, cand)) continue;  // skip mis-stamp.
    if (id_match_corroborated(canonical, cand)) return i;  // beats list order.
    if (!bare.has_value()) bare = i;                       // remember first bare.
  }
  return bare;
}

std::optional<std::size_t> best_provider_match(const Enrichment& canonical,
                                               const std::vector<SearchHit>& candidates) {
  std::optional<std::size_t> best_idx;
  std::int32_t best_score = std::numeric_limits<std::int32_t>::min();
  std::int32_t second_score = std::numeric_limits<std::int32_t>::min();
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const std::int32_t score = candidate_score(canonical, candidates[i]);
    if (!best_idx.has_value() || score > best_score) {
      second_score = best_score;  // old best falls to second.
      best_score = score;
      best_idx = i;
    } else if (score > second_score) {
      second_score = score;
    }
  }
  if (!best_idx.has_value()) return std::nullopt;
  if (best_score < kBestFloor) return std::nullopt;
  if (second_score >= 0 && best_score - second_score < kMatchMargin) return std::nullopt;
  return best_idx;
}

}  // namespace shigoku::resolver
