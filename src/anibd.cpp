// anibd.cpp — P12a. Ported from sabigoku src/providers/anibd.rs.
//
// No crypto, no proxy: this is the senshi-shaped path with an extra hop
// (apilink.php) and a player-page HTML scrape. The one new primitive is the URL
// origin/join helper (Rust used the `url` crate); here it goes through libcurl's
// CURLU URL API — already linked, RFC 3986 relative resolution, no new dep.
//
// cap_variant does not apply here: anibd.rs never calls hls::cap_variant
// (verified in source, P23) — resolve() always takes the master URL
// regardless of `Quality`, same as the Rust. Not a deferred-scope gap.

#include "anibd.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "provider.hpp"

namespace shigoku::anibd {

namespace {

using json = nlohmann::json;

std::optional<std::string> opt_str_opt(const json& j, const char* key) {
  if (!j.contains(key) || j.at(key).is_null() || !j.at(key).is_string()) return std::nullopt;
  return j.at(key).get<std::string>();
}

std::string to_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

// Episode number from name (preferred) or slug; leading zeros stripped by the
// parse, zero and negative rejected (anibd.rs parse_ep_number). name/slug are
// the raw catalog strings; nullopt when neither is a positive integer.
std::optional<std::uint32_t> parse_ep_number(std::optional<std::string_view> name,
                                             std::optional<std::string_view> slug) {
  const std::optional<std::string_view> src = name.has_value() ? name : slug;
  if (!src.has_value() || src->empty()) return std::nullopt;
  std::uint32_t v = 0;
  for (const char c : *src) {
    if (c < '0' || c > '9') return std::nullopt;
    if (v > (0xFFFFFFFFu - 9) / 10) return std::nullopt;  // overflow guard
    v = v * 10 + static_cast<std::uint32_t>(c - '0');
  }
  return v >= 1 ? std::optional<std::uint32_t>(v) : std::nullopt;
}

}  // namespace

namespace detail {

std::string_view audio_kind(std::optional<std::string_view> server_name) {
  if (server_name.has_value() && to_lower(*server_name).find("dub") != std::string::npos) {
    return "dub";
  }
  return "sub";
}

Result<std::vector<CatalogGroup>, ProviderError> parse_catalog(std::string_view raw_json) {
  json groups;
  try {
    groups = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(std::string("catalog: ") + e.what()));
  }
  if (!groups.is_array()) return err(ProviderError::decode("catalog: not an array"));

  std::vector<CatalogGroup> out;
  for (const auto& g : groups) {
    if (!g.is_object()) continue;
    const std::optional<std::string> server_name = opt_str_opt(g, "server_name");
    const std::string_view audio =
        audio_kind(server_name.has_value() ? std::optional<std::string_view>(*server_name)
                                           : std::nullopt);
    std::vector<EpisodeLink> episodes;
    if (g.contains("server_data") && g.at("server_data").is_array()) {
      for (const auto& e : g.at("server_data")) {
        if (!e.is_object()) continue;
        const std::optional<std::string> name = opt_str_opt(e, "name");
        const std::optional<std::string> slug = opt_str_opt(e, "slug");
        const std::optional<std::string> link = opt_str_opt(e, "link");
        const std::optional<std::uint32_t> num = parse_ep_number(
            name.has_value() ? std::optional<std::string_view>(*name) : std::nullopt,
            slug.has_value() ? std::optional<std::string_view>(*slug) : std::nullopt);
        if (!num.has_value()) continue;
        if (!link.has_value() || link->empty()) continue;
        episodes.push_back(EpisodeLink{*num, *link});
      }
    }
    if (!episodes.empty()) {
      CatalogGroup cg;
      cg.audio = std::string(audio);
      cg.episodes = std::move(episodes);
      out.push_back(std::move(cg));
    }
  }
  return out;
}

std::vector<std::string> episode_labels(const std::vector<CatalogGroup>& groups) {
  std::vector<std::uint32_t> nums;
  for (const auto& g : groups) {
    for (const auto& e : g.episodes) nums.push_back(e.number);
  }
  std::sort(nums.begin(), nums.end());
  nums.erase(std::unique(nums.begin(), nums.end()), nums.end());
  std::vector<std::string> labels;
  labels.reserve(nums.size());
  for (const auto n : nums) labels.push_back(std::to_string(n));
  return labels;
}

std::optional<std::string> find_link(const std::vector<CatalogGroup>& groups,
                                     std::uint32_t episode, Translation tt) {
  const std::string_view want = (tt == Translation::Dub) ? "dub" : "sub";
  for (const auto& g : groups) {
    if (g.audio != want) continue;
    for (const auto& e : g.episodes) {
      if (e.number == episode) return e.link;
    }
  }
  return std::nullopt;
}

std::optional<std::string> extract_config_value(std::string_view html, std::string_view key,
                                                std::string_view page_url) {
  const auto at = html.find(key);
  if (at == std::string_view::npos) return std::nullopt;
  std::string_view rest = html.substr(at + key.size());
  // trim_start (whitespace).
  auto ltrim = [](std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
  };
  rest = ltrim(rest);
  if (rest.empty() || rest.front() != ':') return std::nullopt;
  rest = ltrim(rest.substr(1));
  if (rest.empty()) return std::nullopt;
  const char quote = rest.front();
  if (quote != '"' && quote != '\'') return std::nullopt;
  const std::string_view inner = rest.substr(1);
  const auto end = inner.find(quote);
  if (end == std::string_view::npos) return std::nullopt;
  const std::string_view raw = inner.substr(0, end);
  if (raw.empty()) return std::nullopt;
  return resolve_url(raw, page_url);
}

std::optional<std::string> extract_video_url(std::string_view html, std::string_view page_url) {
  if (auto v = extract_config_value(html, "videoUrl", page_url); v.has_value()) return v;
  return extract_config_value(html, "url", page_url);
}

std::string url_origin(std::string_view url) {
  const std::string owned(url);
  CURLU* h = curl_url();
  if (h == nullptr) return owned;
  std::string result = owned;
  if (curl_url_set(h, CURLUPART_URL, owned.c_str(), 0) == CURLUE_OK) {
    char* scheme = nullptr;
    char* host = nullptr;
    char* port = nullptr;
    if (curl_url_get(h, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(h, CURLUPART_HOST, &host, 0) == CURLUE_OK) {
      std::string origin = std::string(scheme) + "://" + host;
      // Only an explicit port is part of the ascii origin serialization; a
      // default port stays implicit (matches url crate origin()).
      if (curl_url_get(h, CURLUPART_PORT, &port, 0) == CURLUE_OK && port != nullptr) {
        origin += ":";
        origin += port;
      }
      result = std::move(origin);
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
  }
  curl_url_cleanup(h);
  return result;
}

std::string resolve_url(std::string_view raw, std::string_view base_url) {
  if (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0) {
    return std::string(raw);
  }
  const std::string owned_base(base_url);
  const std::string owned_raw(raw);
  CURLU* h = curl_url();
  if (h == nullptr) return owned_raw;
  std::string result = owned_raw;
  if (curl_url_set(h, CURLUPART_URL, owned_base.c_str(), 0) == CURLUE_OK &&
      curl_url_set(h, CURLUPART_URL, owned_raw.c_str(), 0) == CURLUE_OK) {
    char* joined = nullptr;
    if (curl_url_get(h, CURLUPART_URL, &joined, 0) == CURLUE_OK && joined != nullptr) {
      result = joined;
    }
    curl_free(joined);
  }
  curl_url_cleanup(h);
  return result;
}

std::optional<std::string> json_string_field(std::string_view obj, std::string_view name) {
  const std::string needle = "\"" + std::string(name) + "\"";
  const auto at = obj.find(needle);
  if (at == std::string_view::npos) return std::nullopt;
  std::string_view rest = obj.substr(at + needle.size());
  auto ltrim = [](std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
  };
  rest = ltrim(rest);
  if (rest.empty() || rest.front() != ':') return std::nullopt;
  rest = ltrim(rest.substr(1));
  if (rest.empty() || rest.front() != '"') return std::nullopt;
  rest = rest.substr(1);
  const auto end = rest.find('"');
  if (end == std::string_view::npos) return std::nullopt;
  const std::string_view val = rest.substr(0, end);
  if (val.empty()) return std::nullopt;
  return std::string(val);
}

std::vector<SubTrack> parse_subtitles(std::string_view html) {
  const auto at = html.find("tracks");
  if (at == std::string_view::npos) return {};
  std::string_view rest = html.substr(at + std::string_view("tracks").size());
  auto ltrim = [](std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
  };
  rest = ltrim(rest);
  if (rest.empty() || rest.front() != ':') return {};
  rest = ltrim(rest.substr(1));
  if (rest.empty() || rest.front() != '[') return {};
  rest = rest.substr(1);
  const auto end = rest.find(']');
  if (end == std::string_view::npos) return {};
  const std::string_view block = rest.substr(0, end);

  std::vector<SubTrack> tracks;
  std::size_t pos = 0;
  while (pos < block.size()) {
    const auto obj_rel = block.substr(pos).find('{');
    if (obj_rel == std::string_view::npos) break;
    const std::size_t obj_start = pos + obj_rel;
    const auto obj_end_rel = block.substr(obj_start).find('}');
    if (obj_end_rel == std::string_view::npos) break;
    const std::string_view obj = block.substr(obj_start, obj_end_rel + 1);

    // parse_sub_track: file must be http; kind (if present) captions/subtitles;
    // label defaults to "Subtitle".
    if (auto file = json_string_field(obj, "file");
        file.has_value() && file->rfind("http", 0) == 0) {
      bool kind_ok = true;
      if (auto kind = json_string_field(obj, "kind");
          kind.has_value() && *kind != "captions" && *kind != "subtitles") {
        kind_ok = false;
      }
      if (kind_ok) {
        std::string label = json_string_field(obj, "label").value_or("");
        if (label.empty()) label = "Subtitle";
        tracks.push_back(SubTrack{std::move(*file), std::move(label)});
      }
    }
    pos = obj_start + obj_end_rel + 1;
  }
  return tracks;
}

std::optional<std::size_t> pick_subtitle(const std::vector<SubTrack>& tracks) {
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (to_lower(tracks[i].label).rfind("eng", 0) == 0) return i;
  }
  if (!tracks.empty()) return std::size_t{0};
  return std::nullopt;
}

std::string form_urlencode(std::string_view s) {
  // url::form_urlencoded::byte_serialize: unreserved (A-Za-z0-9 and *-._) pass;
  // space -> '+'; everything else -> %XX uppercase.
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_';
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

// ---------------------------------------------------------------------------
// The provider.
// ---------------------------------------------------------------------------

Result<AniBd, ProviderError> AniBd::create() { return with_endpoint(kApi); }

Result<AniBd, ProviderError> AniBd::with_endpoint(std::string api) {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return AniBd(std::move(*client), std::move(api));
}

std::optional<std::string> AniBd::canonical_key(const Enrichment& show) const {
  return std::to_string(show.anilist_id);
}

Result<std::vector<SearchHit>, ProviderError> AniBd::search(std::string_view /*query*/,
                                                            const SearchOptions& /*opts*/) const {
  return err(ProviderError::unsupported());
}

Result<std::vector<std::uint8_t>, ProviderError> AniBd::api_get(const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Referer", kReferer});
  req.extra_headers.push_back({"Accept", "application/json, text/html, */*"});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
}

Result<std::vector<std::uint8_t>, ProviderError> AniBd::player_get(const std::string& url,
                                                                   const std::string& referer) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Referer", referer});
  req.extra_headers.push_back(
      {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
  req.extra_headers.push_back({"Accept-Language", "en-US,en;q=0.9"});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
}

Result<std::vector<detail::CatalogGroup>, ProviderError> AniBd::fetch_catalog(
    std::string_view anilist_id) const {
  const std::string url = api_ + "/api2.php?epid=" + std::string(anilist_id);
  auto raw = api_get(url);
  if (!raw.has_value()) return err(raw.error());
  const std::string_view raw_view(reinterpret_cast<const char*>(raw->data()), raw->size());
  return detail::parse_catalog(raw_view);
}

std::optional<std::string> AniBd::guarded_subtitle(std::string_view html) const {
  const auto tracks = detail::parse_subtitles(html);
  const auto pick = detail::pick_subtitle(tracks);
  if (!pick.has_value()) return std::nullopt;
  const std::string& url = tracks[*pick].file;
  if (is_absolute_url(url) && clean_arg(url) && http::guard_fetch_url(url).has_value()) {
    return url;
  }
  return std::nullopt;
}

Result<std::vector<std::string>, ProviderError> AniBd::episodes(
    std::string_view provider_id, Translation /*translation*/,
    std::optional<std::uint32_t> /*count_hint*/) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  auto groups = fetch_catalog(provider_id);
  if (!groups.has_value()) return err(groups.error());
  return detail::episode_labels(*groups);
}

Result<StreamLink, ProviderError> AniBd::resolve(std::string_view provider_id,
                                                 std::string_view episode, Translation tt,
                                                 Quality /*quality*/) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  // Episode is a positive integer (anibd.rs: episode.parse::<u32>(), reject 0).
  std::uint32_t ep_num = 0;
  if (episode.empty()) return err(ProviderError::decode("invalid episode"));
  for (const char c : episode) {
    if (c < '0' || c > '9') return err(ProviderError::decode("invalid episode"));
    if (ep_num > (0xFFFFFFFFu - 9) / 10) return err(ProviderError::decode("invalid episode"));
    ep_num = ep_num * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (ep_num == 0) return err(ProviderError::decode("invalid episode"));

  auto groups = fetch_catalog(provider_id);
  if (!groups.has_value()) return err(groups.error());
  const auto link = detail::find_link(*groups, ep_num, tt);
  if (!link.has_value()) return err(ProviderError::decode("no stream for track"));

  // Percent-encode the opaque catalog link into the `data` param so it stays a
  // single value (no query injection).
  const std::string encoded = detail::form_urlencode(*link);
  const std::string players_url = api_ + "/apilink.php?data=" + encoded;
  auto raw = api_get(players_url);
  if (!raw.has_value()) return err(raw.error());

  // Player entries: [{ "link": "..." }, ...]. Empty/absent links dropped.
  std::vector<std::string> usable;
  try {
    const auto parsed = nlohmann::json::parse(raw->begin(), raw->end());
    if (parsed.is_array()) {
      for (const auto& p : parsed) {
        if (!p.is_object()) continue;
        if (auto l = opt_str_opt(p, "link"); l.has_value() && !l->empty()) {
          usable.push_back(std::move(*l));
        }
      }
    }
  } catch (const nlohmann::json::parse_error& e) {
    return err(ProviderError::decode(std::string("players: ") + e.what()));
  }
  if (usable.empty()) return err(ProviderError::decode("no players"));

  const std::size_t tries = std::min(usable.size(), kMaxPlayerTries);
  for (std::size_t i = 0; i < tries; ++i) {
    const std::string& player_url = usable[i];
    if (!is_absolute_url(player_url) || !clean_arg(player_url) ||
        !http::guard_fetch_url(player_url).has_value()) {
      continue;
    }
    const std::string origin = detail::url_origin(player_url);
    const std::string referer = origin + "/";
    auto body = player_get(player_url, referer);
    if (!body.has_value()) continue;
    const std::string_view html(reinterpret_cast<const char*>(body->data()), body->size());
    const auto hls = detail::extract_video_url(html, player_url);
    if (!hls.has_value()) continue;
    if (!is_absolute_url(*hls) || !clean_arg(*hls)) continue;

    std::optional<std::string> sub_url;
    if (tt == Translation::Sub) sub_url = guarded_subtitle(html);

    StreamLink out;
    out.url = *hls;
    out.resolution = std::nullopt;
    out.referer = referer;
    out.user_agent = std::string(kUserAgent);
    out.cloaked_segments = true;
    out.decloak_segments = false;
    out.sub_url = std::move(sub_url);
    return out;
  }

  return err(ProviderError::decode("no playable source"));
}

Result<CoverRequest, ProviderError> AniBd::cover_request(std::string_view cover_ref) const {
  // anibd.rs cover_request: empty / non-absolute / dirty -> reject. Unlike
  // senshi it does NOT apply the MAX_COVER_REF_LEN ceiling (ported 1:1).
  if (cover_ref.empty() || !is_absolute_url(cover_ref) || !clean_arg(cover_ref)) {
    return err(ProviderError::decode("invalid cover ref"));
  }
  CoverRequest cr;
  cr.url = std::string(cover_ref);
  return cr;
}

}  // namespace shigoku::anibd
