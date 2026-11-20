// hls.cpp — join_url (P12 seed) + parse_master_playlist/select_variant (P23).
// Ported 1:1 from providers/hls.rs.

#include "hls.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "provider.hpp"  // clean_arg.

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

// The value of attribute `name` in a tag line's attribute list (after the tag's
// ':'), without its surrounding quotes; nullopt when absent. A comma inside
// quotes does not end an attribute, so a name inside a quoted value is not one.
std::optional<std::string_view> attribute(std::string_view line, std::string_view name) {
  const std::size_t colon = line.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  std::string_view rest = line.substr(colon + 1);
  while (!rest.empty()) {
    std::size_t end = 0;
    bool quoted = false;
    for (; end < rest.size(); ++end) {
      if (rest[end] == '"') {
        quoted = !quoted;
      } else if (rest[end] == ',' && !quoted) {
        break;
      }
    }
    const std::string_view attr = rest.substr(0, end);
    rest = end < rest.size() ? rest.substr(end + 1) : std::string_view{};
    const std::size_t eq = attr.find('=');
    if (eq == std::string_view::npos || attr.substr(0, eq) != name) continue;
    std::string_view value = attr.substr(eq + 1);
    if (value.starts_with('"')) {
      value.remove_prefix(1);
      value = value.substr(0, value.find('"'));
    }
    return value;
  }
  return std::nullopt;
}

// All-digit decimal; nullopt when empty, not all digits, or over T's range.
template <class T>
std::optional<T> decimal(std::string_view s) {
  if (s.empty()) return std::nullopt;
  T v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    const T d = static_cast<T>(c - '0');
    if (v > (std::numeric_limits<T>::max() - d) / 10) return std::nullopt;
    v = static_cast<T>(v * 10 + d);
  }
  return v;
}

// What an `#EXT-X-STREAM-INF` line advertises; `url` is left for the URI line.
Variant stream_inf(std::string_view line) {
  Variant v;
  v.resolution = stream_inf_height(line);
  if (const auto res = attribute(line, "RESOLUTION")) {
    const std::size_t x = res->find('x');
    if (x != std::string_view::npos) v.width = decimal<std::uint32_t>(res->substr(0, x));
  }
  if (const auto bw = attribute(line, "BANDWIDTH")) v.bandwidth = decimal<std::uint64_t>(*bw);
  v.codecs = std::string(attribute(line, "CODECS").value_or(""));
  v.audio_group = std::string(attribute(line, "AUDIO").value_or(""));
  return v;
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
  std::optional<Variant> pending;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto nl = text.find('\n', pos);
    const std::string_view raw =
        (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    const std::string_view line = trim_line(raw);
    if (!line.empty()) {
      if (line.starts_with("#EXT-X-STREAM-INF")) {
        pending = stream_inf(line);
      } else if (line.front() == '#') {
        // comment/tag, not STREAM-INF: skip.
      } else if (pending.has_value()) {
        pending->url = std::string(line);
        out.push_back(std::move(*pending));
        pending.reset();
      }
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return out;
}

namespace {

// `attr` (`NAME=` or `NAME=VALUE`) as one attribute of a tag line: right
// after the tag's ':' or an attribute-separating ','.
bool has_attribute(std::string_view line, std::string_view attr) {
  for (std::size_t at = line.find(attr); at != std::string_view::npos;
       at = line.find(attr, at + 1)) {
    if (at > 0 && (line[at - 1] == ':' || line[at - 1] == ',')) return true;
  }
  return false;
}

}  // namespace

bool has_separate_audio(std::string_view text) {
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto nl = text.find('\n', pos);
    const std::string_view raw =
        (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    const std::string_view line = trim_line(raw);
    if (line.starts_with("#EXT-X-MEDIA:") && has_attribute(line, "TYPE=AUDIO") &&
        has_attribute(line, "URI=")) {
      return true;
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return false;
}

namespace {

// The quoted value of `name` in a tag line (`NAME="v"`), or nullopt.
std::optional<std::string_view> quoted_attribute(std::string_view line, std::string_view name) {
  const std::string key = std::string(name) + "=\"";
  for (std::size_t at = line.find(key); at != std::string_view::npos;
       at = line.find(key, at + 1)) {
    if (at == 0 || (line[at - 1] != ':' && line[at - 1] != ',')) continue;
    const std::size_t start = at + key.size();
    const std::size_t end = line.find('"', start);
    if (end == std::string_view::npos) return std::nullopt;
    return line.substr(start, end - start);
  }
  return std::nullopt;
}

// ISO 639 primary subtag, lowercased, two-letter codes folded to
// the three-letter ones masters use ("en-US" -> "eng", "ja" -> "jpn").
std::string language_key(std::string_view tag) {
  std::string out;
  for (const char c : tag) {
    if (c == '-' || c == '_') break;
    out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  if (out == "en") return "eng";
  if (out == "ja") return "jpn";
  return out;
}

bool is_audio_rendition(std::string_view line) {
  return line.starts_with("#EXT-X-MEDIA:") && has_attribute(line, "TYPE=AUDIO");
}

// `name=YES|NO` set to `value` in an attribute list, appended when absent.
std::string set_flag(std::string_view line, std::string_view name, bool value) {
  const std::string want = std::string(name) + (value ? "=YES" : "=NO");
  for (const char* v : {"=YES", "=NO"}) {
    const std::string have = std::string(name) + v;
    for (std::size_t at = line.find(have); at != std::string_view::npos;
         at = line.find(have, at + 1)) {
      if (at > 0 && (line[at - 1] == ':' || line[at - 1] == ',')) {
        return std::string(line.substr(0, at)) + want + std::string(line.substr(at + have.size()));
      }
    }
  }
  return std::string(line) + "," + want;
}

// DEFAULT=YES requires AUTOSELECT=YES when the attribute is present.
std::string make_default(std::string_view line, bool yes) {
  std::string out = set_flag(line, "DEFAULT", yes);
  if (yes && has_attribute(out, "AUTOSELECT=NO")) out = set_flag(out, "AUTOSELECT", true);
  return out;
}

}  // namespace

std::string keep_audio_language(std::string_view text, std::string_view language) {
  const std::string want = language_key(language);
  if (want.empty()) return std::string(text);

  std::vector<std::string_view> lines;
  for (std::size_t pos = 0; pos <= text.size();) {
    const auto nl = text.find('\n', pos);
    lines.push_back(nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos));
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }

  // Groups that carry the language; a group without it is left whole.
  std::vector<std::string> matched_groups;
  auto group_of = [](std::string_view line) {
    return std::string(quoted_attribute(line, "GROUP-ID").value_or(""));
  };
  auto matches = [&](std::string_view line) {
    const auto lang = quoted_attribute(line, "LANGUAGE");
    return lang.has_value() && language_key(*lang) == want;
  };
  for (const std::string_view raw : lines) {
    const std::string_view line = trim_line(raw);
    if (is_audio_rendition(line) && matches(line)) matched_groups.push_back(group_of(line));
  }
  if (matched_groups.empty()) return std::string(text);

  auto in_matched = [&](const std::string& g) {
    return std::find(matched_groups.begin(), matched_groups.end(), g) != matched_groups.end();
  };
  std::vector<std::string> defaulted;
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = trim_line(lines[i]);
    std::string emitted(lines[i]);
    if (is_audio_rendition(line) && in_matched(group_of(line))) {
      if (!matches(line)) continue;
      const std::string g = group_of(line);
      const bool first = std::find(defaulted.begin(), defaulted.end(), g) == defaulted.end();
      if (first) defaulted.push_back(g);
      emitted = make_default(line, first);
    }
    out += emitted;
    if (i + 1 < lines.size()) out.push_back('\n');
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

std::optional<std::string> choose_variant(std::string_view playlist, std::string_view base,
                                          Quality quality) {
  const auto variants = parse_master_playlist(playlist);
  if (variants.empty()) return std::nullopt;
  if (has_separate_audio(playlist)) return std::nullopt;

  std::vector<StreamLink> links;
  for (const auto& v : variants) {
    const auto joined = join_url(base, v.url);
    if (!joined.has_value()) continue;
    if (!clean_arg(*joined)) continue;
    StreamLink link;
    link.url = *joined;
    link.resolution = v.resolution;
    links.push_back(std::move(link));
  }
  const auto* pick = select_variant(links, quality);
  if (pick == nullptr) return std::nullopt;
  return pick->url;
}

std::vector<AudioRendition> parse_audio_renditions(std::string_view text) {
  std::vector<AudioRendition> out;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto nl = text.find('\n', pos);
    const std::string_view raw =
        (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    const std::string_view line = trim_line(raw);
    if (line.starts_with("#EXT-X-MEDIA:") && attribute(line, "TYPE").value_or("") == "AUDIO") {
      auto value = [&](std::string_view name) {
        return std::string(attribute(line, name).value_or(""));
      };
      AudioRendition r;
      r.group = value("GROUP-ID");
      r.name = value("NAME");
      r.language = value("LANGUAGE");
      r.uri = value("URI");
      r.is_default = value("DEFAULT") == "YES";
      out.push_back(std::move(r));
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return out;
}

namespace {

std::uint64_t bandwidth_of(const Variant& v) { return v.bandwidth.value_or(0); }

// The first variant `keep` admits that no later admitted one `beats`; nullptr
// when none is admitted.
template <class Keep, class Beats>
const Variant* first_best(const std::vector<Variant>& variants, Keep keep, Beats beats) {
  const Variant* pick = nullptr;
  for (const auto& v : variants) {
    if (!keep(v)) continue;
    if (pick == nullptr || beats(v, *pick)) pick = &v;
  }
  return pick;
}

}  // namespace

const Variant* pick_variant(const std::vector<Variant>& variants, const Rung& rung) {
  if (variants.empty()) return nullptr;
  const auto any = [](const Variant&) { return true; };
  const auto higher_bw = [](const Variant& a, const Variant& b) {
    return bandwidth_of(a) > bandwidth_of(b);
  };
  const auto lower_bw = [](const Variant& a, const Variant& b) {
    return bandwidth_of(a) < bandwidth_of(b);
  };

  const Variant* low = first_best(
      variants, [](const Variant& v) { return v.resolution.has_value(); },
      [](const Variant& a, const Variant& b) { return *a.resolution < *b.resolution; });
  if (low == nullptr) {
    if (rung.lowest) return first_best(variants, any, lower_bw);
    if (rung.bandwidth > 0) {
      const Variant* under = first_best(
          variants, [&](const Variant& v) { return bandwidth_of(v) <= rung.bandwidth; },
          higher_bw);
      return under != nullptr ? under : first_best(variants, any, lower_bw);
    }
    if (rung.height == 0) return first_best(variants, any, higher_bw);
    return &variants.front();
  }

  std::uint32_t target = *low->resolution;
  bool reaching_low = true;
  if (!rung.lowest) {
    const std::uint32_t cap =
        rung.height == 0 ? std::numeric_limits<std::uint32_t>::max() : rung.height;
    const Variant* under = first_best(
        variants, [&](const Variant& v) { return v.resolution && *v.resolution <= cap; },
        [](const Variant& a, const Variant& b) { return *a.resolution > *b.resolution; });
    if (under != nullptr) {
      target = *under->resolution;
      reaching_low = false;
    }
  }

  const auto at_target = [&](const Variant& v) { return v.resolution == target; };
  if (rung.bandwidth > 0) {
    const auto distance = [&](const Variant& v) {
      const std::uint64_t bw = bandwidth_of(v);
      return bw > rung.bandwidth ? bw - rung.bandwidth : rung.bandwidth - bw;
    };
    return first_best(variants, at_target, [&](const Variant& a, const Variant& b) {
      return distance(a) < distance(b);
    });
  }
  if (reaching_low) return first_best(variants, at_target, lower_bw);
  return first_best(variants, at_target, higher_bw);
}

const AudioRendition* audio_for(const Variant& variant,
                                const std::vector<AudioRendition>& renditions) {
  if (variant.audio_group.empty()) return nullptr;
  const AudioRendition* first = nullptr;
  for (const auto& r : renditions) {
    if (r.group != variant.audio_group || r.uri.empty()) continue;
    if (r.is_default) return &r;
    if (first == nullptr) first = &r;
  }
  return first;
}

std::optional<Pair> choose_pair(std::string_view playlist, std::string_view base,
                                const Rung& rung) {
  std::vector<Variant> usable;
  for (auto& v : parse_master_playlist(playlist)) {
    auto joined = join_url(base, v.url);
    if (!joined.has_value() || !clean_arg(*joined)) continue;
    v.url = std::move(*joined);
    usable.push_back(std::move(v));
  }
  const Variant* pick = pick_variant(usable, rung);
  if (pick == nullptr) return std::nullopt;

  Pair pair;
  pair.video_url = pick->url;
  pair.variant = *pick;
  const auto renditions = parse_audio_renditions(playlist);
  if (const AudioRendition* audio = audio_for(*pick, renditions)) {
    auto joined = join_url(base, audio->uri);
    if (!joined.has_value() || !clean_arg(*joined)) return std::nullopt;
    pair.audio_url = std::move(*joined);
  }
  return pair;
}

}  // namespace shigoku::hls
