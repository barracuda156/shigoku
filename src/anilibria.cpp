// anilibria.cpp — AniLibria (AniLiberty API v1) provider: pure parsers in
// detail:: plus the small blocking transport. See anilibria.hpp for the chain.

#include "anilibria.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "provider.hpp"  // clean_arg, guard_show_id (shared provider guards).

namespace shigoku::anilibria {
namespace detail {

namespace {

// name.main / name.english out of a release object's "name" block.
std::pair<std::string, std::optional<std::string>> parse_names(const nlohmann::json& row) {
  std::string main;
  std::optional<std::string> english;
  if (auto it = row.find("name"); it != row.end() && it->is_object()) {
    if (auto m = it->find("main"); m != it->end() && m->is_string()) {
      main = m->get<std::string>();
    }
    if (auto e = it->find("english"); e != it->end() && e->is_string()) {
      std::string s = e->get<std::string>();
      if (!s.empty()) english = std::move(s);
    }
  }
  return {std::move(main), std::move(english)};
}

std::optional<std::uint32_t> u32_field(const nlohmann::json& row, const char* key) {
  if (auto it = row.find(key); it != row.end() && it->is_number_integer()) {
    const std::int64_t v = it->get<std::int64_t>();
    if (v > 0 && v <= static_cast<std::int64_t>(UINT32_MAX)) {
      return static_cast<std::uint32_t>(v);
    }
  }
  return std::nullopt;
}

// A non-empty string field, nullopt otherwise (the API nulls absent urls).
std::optional<std::string> str_field(const nlohmann::json& row, const char* key) {
  if (auto it = row.find(key); it != row.end() && it->is_string()) {
    std::string s = it->get<std::string>();
    if (!s.empty()) return s;
  }
  return std::nullopt;
}

bool bool_field(const nlohmann::json& row, const char* key) {
  if (auto it = row.find(key); it != row.end() && it->is_boolean()) {
    return it->get<bool>();
  }
  return false;
}

}  // namespace

Result<std::vector<Hit>, ProviderError> parse_search(std::string_view raw_json) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(raw_json);
  } catch (const nlohmann::json::exception& e) {
    return err(ProviderError::decode(std::string("search: ") + e.what()));
  }
  if (!j.is_array()) return err(ProviderError::decode("search: not an array"));
  std::vector<Hit> hits;
  for (const auto& row : j) {
    if (!row.is_object()) continue;
    std::int64_t id = 0;
    if (auto it = row.find("id"); it != row.end() && it->is_number_integer()) {
      id = it->get<std::int64_t>();
    }
    if (id <= 0) continue;
    auto [main, english] = parse_names(row);
    if (main.empty()) continue;
    Hit hit;
    hit.id = id;
    hit.title_main = std::move(main);
    hit.title_english = std::move(english);
    hit.year = u32_field(row, "year");
    hit.episodes_total = u32_field(row, "episodes_total");
    hits.push_back(std::move(hit));
  }
  return hits;
}

Result<Release, ProviderError> parse_release(std::string_view raw_json) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(raw_json);
  } catch (const nlohmann::json::exception& e) {
    return err(ProviderError::decode(std::string("release: ") + e.what()));
  }
  if (!j.is_object()) return err(ProviderError::decode("release: not an object"));
  Release rel;
  rel.blocked_geo = bool_field(j, "is_blocked_by_geo");
  rel.blocked_copyright = bool_field(j, "is_blocked_by_copyrights");
  if (auto it = j.find("episodes"); it != j.end() && it->is_array()) {
    for (const auto& row : *it) {
      if (!row.is_object()) continue;
      double ordinal = -1.0;
      if (auto o = row.find("ordinal"); o != row.end() && o->is_number()) {
        ordinal = o->get<double>();
      }
      if (!std::isfinite(ordinal) || ordinal < 0.0 || ordinal > kMaxOrdinal) continue;
      Episode ep;
      ep.ordinal = ordinal;
      if (auto n = str_field(row, "name")) ep.name = std::move(*n);
      ep.hls480 = str_field(row, "hls_480");
      ep.hls720 = str_field(row, "hls_720");
      ep.hls1080 = str_field(row, "hls_1080");
      rel.episodes.push_back(std::move(ep));
    }
  }
  // Stable sort by ordinal so dedup keeps the row the site listed first.
  std::stable_sort(rel.episodes.begin(), rel.episodes.end(),
                   [](const Episode& a, const Episode& b) { return a.ordinal < b.ordinal; });
  rel.episodes.erase(
      std::unique(rel.episodes.begin(), rel.episodes.end(),
                  [](const Episode& a, const Episode& b) {
                    return format_ordinal(a.ordinal) == format_ordinal(b.ordinal);
                  }),
      rel.episodes.end());
  return rel;
}

std::string format_ordinal(double ordinal) {
  // %.10g: integral values print bare ("7"), fractional ones shortest-round
  // ("11.5"); the parse ceiling keeps the magnitude out of scientific range.
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.10g", ordinal);
  return buf;
}

bool label_matches(double ordinal, std::string_view label) {
  if (label.empty()) return false;
  bool digits_only = label.size() <= 9;
  for (const char c : label) {
    if (c < '0' || c > '9') {
      digits_only = false;
      break;
    }
  }
  if (digits_only) {
    // Numeric compare so a zero-padded label ("01") still names ordinal 1.
    std::uint32_t v = 0;
    for (const char c : label) v = v * 10 + static_cast<std::uint32_t>(c - '0');
    return static_cast<double>(v) == ordinal;
  }
  return format_ordinal(ordinal) == label;
}

std::optional<std::string> pick_hls(const Episode& ep, Quality quality) {
  const std::optional<std::string>* ladder[3];
  switch (quality) {
    case Quality::Best:
    case Quality::P1080:
      ladder[0] = &ep.hls1080;
      ladder[1] = &ep.hls720;
      ladder[2] = &ep.hls480;
      break;
    case Quality::P720:
      ladder[0] = &ep.hls720;
      ladder[1] = &ep.hls480;
      ladder[2] = &ep.hls1080;
      break;
    case Quality::P480:
    case Quality::Worst:
      ladder[0] = &ep.hls480;
      ladder[1] = &ep.hls720;
      ladder[2] = &ep.hls1080;
      break;
  }
  for (const auto* rung : ladder) {
    if (rung->has_value()) return **rung;
  }
  return std::nullopt;
}

std::string form_urlencode(std::string_view s) {
  // application/x-www-form-urlencoded byte_serialize: unreserved (A-Za-z0-9 and
  // *-._) pass, ' ' -> '+', everything else -> %XX (uppercase hex).
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '*' || c == '-' ||
                            c == '.' || c == '_';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

}  // namespace detail

// ===========================================================================
// The provider transport.
// ===========================================================================

Result<AniLibria, ProviderError> AniLibria::create() { return with_endpoint(kApi); }

Result<AniLibria, ProviderError> AniLibria::with_endpoint(std::string api) {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return AniLibria(std::move(*client), std::move(api));
}

Result<std::vector<std::uint8_t>, ProviderError> AniLibria::json_get(
    const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.extra_headers.push_back({"Accept", "application/json"});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
}

Result<detail::Release, ProviderError> AniLibria::fetch_release(
    std::string_view show_id) const {
  const std::string url = api_ + "/api/v1/anime/releases/" + std::string(show_id);
  auto raw = json_get(url);
  if (!raw.has_value()) return err(raw.error());
  auto rel = detail::parse_release(
      std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
  if (!rel.has_value()) return err(rel.error());
  if (rel->blocked_geo || rel->blocked_copyright) {
    // The API's own verdict for THIS vantage: hop, don't record an absence.
    return err(ProviderError::forbidden(451));
  }
  return rel;
}

// -- StreamProvider surface --

std::optional<std::string> AniLibria::canonical_key(const Enrichment& /*show*/) const {
  return std::nullopt;  // no canonical ids in the API; tier-C search binds it.
}

Result<std::vector<SearchHit>, ProviderError> AniLibria::search(
    std::string_view query, const SearchOptions& opts) const {
  // No paging on this endpoint: page 2 would repeat page 1.
  if (opts.page > 1 || query.empty()) return std::vector<SearchHit>{};
  const std::string url =
      api_ + "/api/v1/app/search/releases?query=" + detail::form_urlencode(query);
  auto raw = json_get(url);
  if (!raw.has_value()) return err(raw.error());
  auto parsed = detail::parse_search(
      std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
  if (!parsed.has_value()) return err(parsed.error());

  std::vector<SearchHit> hits;
  hits.reserve(parsed->size());
  for (auto& h : *parsed) {
    SearchHit hit;
    hit.provider_id = std::to_string(h.id);
    hit.title = std::move(h.title_main);
    hit.title_english = std::move(h.title_english);
    hit.year = h.year;
    hit.total_episodes = h.episodes_total;
    hits.push_back(std::move(hit));
  }
  if (hits.size() > opts.limit) hits.resize(opts.limit);
  return hits;
}

Result<std::vector<std::string>, ProviderError> AniLibria::episodes(
    std::string_view provider_id, Translation /*translation*/,
    std::optional<std::uint32_t> /*count_hint*/) const {
  // Single-track voiceover: both translations list the same episodes.
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  auto rel = fetch_release(provider_id);
  if (!rel.has_value()) return err(rel.error());
  std::vector<std::string> out;
  out.reserve(rel->episodes.size());
  for (const auto& ep : rel->episodes) out.push_back(detail::format_ordinal(ep.ordinal));
  return out;
}

Result<StreamLink, ProviderError> AniLibria::resolve(std::string_view provider_id,
                                                     std::string_view episode,
                                                     Translation /*translation*/,
                                                     Quality quality) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  auto rel = fetch_release(provider_id);
  if (!rel.has_value()) return err(rel.error());
  const detail::Episode* ep = nullptr;
  for (const auto& e : rel->episodes) {
    if (detail::label_matches(e.ordinal, episode)) {
      ep = &e;
      break;
    }
  }
  if (ep == nullptr) return err(ProviderError::decode("no such episode"));
  auto url = detail::pick_hls(*ep, quality);
  if (!url.has_value()) return err(ProviderError::decode("no playable source"));
  if (!is_absolute_url(*url) || !clean_arg(*url) ||
      !http::guard_fetch_url(*url).has_value()) {
    return err(ProviderError::decode("bad stream url"));
  }
  StreamLink link;
  link.url = std::move(*url);
  // Plain HLS on the provider's own CDN: no referer/UA gate, nothing cloaked.
  link.cloaked_segments = false;
  link.decloak_segments = false;
  return link;
}

Result<CoverRequest, ProviderError> AniLibria::cover_request(
    std::string_view cover_ref) const {
  if (cover_ref.empty() || cover_ref.size() > kMaxCoverRefLen || !clean_arg(cover_ref)) {
    return err(ProviderError::decode("invalid cover ref"));
  }
  CoverRequest req;
  if (cover_ref.front() == '/') {
    req.url = api_ + std::string(cover_ref);
  } else if (is_absolute_url(cover_ref)) {
    req.url = std::string(cover_ref);
  } else {
    return err(ProviderError::decode("invalid cover ref"));
  }
  return req;
}

}  // namespace shigoku::anilibria
