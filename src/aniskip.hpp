// aniskip.hpp — AniSkip intro/outro auto-skip for mpv (P22, 03 §9). Ported
// from sabigoku src/aniskip.rs. Community OP/ED timestamps keyed on MAL id:
// fetch intervals, drop skip.lua into cache, hand mpv `--script` +
// `--script-opts`. Best-effort: any failure (no MAL id, network down, empty
// data, unwritable cache) collapses to plain play with no error shown.
// Network runs on the playback worker, never the UI thread.
//
// shigoku-only addition (no 1:1 Rust source; sabigoku deliberately ships
// without a jikan module, ROD-439): when enrichment carries no MAL id, fall
// back to a Jikan title lookup so AniSkip still has a shot. Best-effort same
// as the rest of this module — a failed/ambiguous lookup just means no skip.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "http.hpp"
#include "player.hpp"

namespace shigoku::aniskip {

inline constexpr const char* kEndpoint = "https://api.aniskip.com/v2/skip-times";
inline constexpr const char* kUserAgent = "shigoku";
inline constexpr const char* kJikanEndpoint = "https://api.jikan.moe/v4/anime";

// Which segments to auto-skip. Mirrors config.skip_mode (06 §2.2).
enum class SkipMode {
  None,
  Intro,
  Outro,
  Both,
};

// Unrecognized config string -> Both. A typo must not silently disable skip
// (03 §9).
[[nodiscard]] inline SkipMode parse_skip_mode(std::string_view s) {
  if (s == "none") return SkipMode::None;
  if (s == "intro") return SkipMode::Intro;
  if (s == "outro") return SkipMode::Outro;
  return SkipMode::Both;
}

// Skip window for one episode. nullopt = AniSkip had no timestamp for that
// segment.
struct SkipTimes {
  std::optional<std::pair<double, double>> op;
  std::optional<std::pair<double, double>> ed;
  friend bool operator==(const SkipTimes&, const SkipTimes&) = default;
};

// Integer from the provider label; else the 1-based ordinal. Non-integer
// labels (OVA, "1.5") or gapped numbering can disagree with the real episode
// number: a wrong AniSkip lookup at worst, which yields no skip.
[[nodiscard]] std::uint32_t episode_number(std::string_view raw, std::uint32_t ordinal);

// Everything mpv needs to auto-skip, or nullopt for plain play. Runs network:
// call on the playback worker only. Mode None returns before any I/O. mal_id
// nullopt falls back to a Jikan title lookup (shigoku addition) before giving
// up.
[[nodiscard]] std::optional<player::SkipScript> prepare(
    std::optional<std::int64_t> mal_id, std::string_view title,
    std::uint32_t episode, SkipMode mode, std::string_view cache_dir);

// --- Internals, exposed for the golden tests (aniskip.rs mod tests) --------
namespace detail {

// First op/ed wins. Degenerate intervals are dropped (the community DB has
// them); a 0->0 window would make mpv seek-loop at the start.
[[nodiscard]] SkipTimes times_from_body(std::string_view body);

// `--script-opts` for `times` under `mode`, or nullopt (mode None / no
// relevant interval). Missing segments emit as -1 (Lua: disabled).
[[nodiscard]] std::optional<std::string> build_opts(const SkipTimes& t, SkipMode mode);

// Fetch OP/ED for (mal_id, episode). Never errors: every failure returns
// empty times.
[[nodiscard]] SkipTimes fetch(std::int64_t mal_id, std::uint32_t episode);

// Best-effort Jikan MAL-id-by-title lookup (shigoku addition, no Rust
// reference). Never errors: ambiguous/absent/network failure all -> nullopt.
[[nodiscard]] std::optional<std::int64_t> jikan_mal_id(std::string_view title);

// Write skip.lua into the cache dir; absolute path back. Always rewrite: the
// script evolves across versions and ~900B once per play is cheaper than
// staleness tracking. Failures collapse to nullopt (plain play).
[[nodiscard]] std::optional<std::string> ensure_script(std::string_view cache_dir);

}  // namespace detail

}  // namespace shigoku::aniskip
