// hls.cpp — join_url (P12 seed) + parse_master_playlist/select_variant (P23).
// Ported 1:1 from providers/hls.rs.

#include "hls.hpp"

#include <cstdint>
#include <limits>

namespace shigoku::hls {

std::optional<std::string> join_url(std::string_view base,
                                    std::string_view reference) {
  if (is_absolute_url(reference)) {
    return std::string(reference);
  }
  const std::size_t scheme = base.find("://");
  if (scheme == std::string_view::npos) return std::nullopt;
  const std::size_t scheme_end = scheme + 3;
  // First '/' at or past the authority marks the end of scheme+host.
  const std::size_t slash = base.substr(scheme_end).find('/');
  const std::size_t host_end =
      (slash == std::string_view::npos) ? base.size() : scheme_end + slash;
  if (!reference.empty() && reference.front() == '/') {
    return std::string(base.substr(0, host_end)) + std::string(reference);
  }
  // Directory of the base = up to and including the last '/' at or past the
  // host; if none, the host itself (no path to strip).
  const std::size_t last_slash = base.rfind('/');
  const std::size_t dir_end =
      (last_slash != std::string_view::npos && last_slash >= host_end)
          ? last_slash + 1
          : host_end;
  return std::string(base.substr(0, dir_end)) + std::string(reference);
}

namespace {

// Height from `RESOLUTION=WxH` on EXT-X-STREAM-INF; nullopt if absent or
// malformed.
std::optional<std::uint32_t> stream_inf_height(std::string_view inf_line) {
  constexpr std::string_view kKey = "RESOLUTION=";
  const auto key_pos = inf_line.find(kKey);
  if (key_pos == std::string_view::npos) return std::nullopt;
  std::string_view rest = inf_line.substr(key_pos + kKey.size());
  const auto x_pos = rest.find('x');
  if (x_pos == std::string_view::npos) return std::nullopt;
  rest = rest.substr(x_pos + 1);
  std::size_t end = 0;
  while (end < rest.size() && rest[end] >= '0' && rest[end] <= '9') ++end;
  if (end == 0) return std::nullopt;
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < end; ++i) {
    if (v > (std::numeric_limits<std::uint32_t>::max() - 9) / 10) return std::nullopt;
    v = v * 10 + static_cast<std::uint32_t>(rest[i] - '0');
  }
  return v;
}

// Trim ' ', '\t', '\r' from both ends (mirrors Rust's
// trim_matches([' ', '\t', '\r'])); '\n' is already the split delimiter.
std::string_view trim_line(std::string_view line) {
  constexpr std::string_view kTrim = " \t\r";
  const auto begin = line.find_first_not_of(kTrim);
  if (begin == std::string_view::npos) return {};
  const auto end = line.find_last_not_of(kTrim);
  return line.substr(begin, end - begin + 1);
}

// Cap rank for a single `>` comparison. <= cap: non-negative, rises with res
// (highest-<=-cap wins). Over budget: negative, rises toward zero as res
// shrinks (smallest over-budget wins). Any in-budget always outranks any
// over-budget.
std::int64_t quality_rank(std::uint32_t res, std::uint32_t cap_px) {
  if (res <= cap_px) return static_cast<std::int64_t>(res);
  return -static_cast<std::int64_t>(res);
}

// Whether candidate `a` beats incumbent `b` for `quality`.
// Landmine: a KNOWN resolution always beats unknown (nullopt). Under a rung
// cap, a BANDWIDTH-only STREAM-INF (no res) could be any bitrate; treating it
// as "0p, in budget" would hand a capped user the firehose the cap prevents.
// Unknowns are last resort, only when EVERY candidate is unknown.
bool preferred(const StreamLink& a, const StreamLink& b, Quality quality) {
  if (!a.resolution.has_value()) return false;
  if (!b.resolution.has_value()) return true;
  const std::uint32_t ra = *a.resolution;
  const std::uint32_t rb = *b.resolution;
  const auto cap = quality_cap(quality);
  if (!cap.has_value()) {
    return quality == Quality::Worst ? ra < rb : ra > rb;
  }
  return quality_rank(ra, *cap) > quality_rank(rb, *cap);
}

}  // namespace

std::vector<Variant> parse_master_playlist(std::string_view text) {
  std::vector<Variant> out;
  std::optional<std::optional<std::uint32_t>> pending_res;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto nl = text.find('\n', pos);
    const std::string_view raw =
        (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    const std::string_view line = trim_line(raw);
    if (!line.empty()) {
      if (line.starts_with("#EXT-X-STREAM-INF")) {
        pending_res = stream_inf_height(line);
      } else if (line.front() == '#') {
        // comment/tag, not STREAM-INF: skip.
      } else if (pending_res.has_value()) {
        out.push_back(Variant{std::string(line), *pending_res});
        pending_res.reset();
      }
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return out;
}

const StreamLink* select_variant(const std::vector<StreamLink>& variants, Quality quality) {
  if (variants.empty()) return nullptr;
  const StreamLink* pick = &variants.front();
  for (std::size_t i = 1; i < variants.size(); ++i) {
    if (preferred(variants[i], *pick, quality)) pick = &variants[i];
  }
  return pick;
}

}  // namespace shigoku::hls
