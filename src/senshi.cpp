// senshi.cpp — P4 + P23 (cap_variant). Ported from sabigoku
// src/providers/senshi.rs.
//
// cap_variant fetches the adaptive master and picks the variant matching the
// quality cap via hls::parse_master_playlist/select_variant (P23). Best
// leaves mpv on the master ladder untouched (the v0 behavior stays the Best
// arm, not a regression); any other rung best-effort falls back to the master
// on a fetch/parse miss (resolve must not fail just because capping did).

#include "senshi.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <thread>

#include <nlohmann/json.hpp>

#include "provider.hpp"

namespace shigoku::senshi {

namespace {

using json = nlohmann::json;

std::optional<std::string> opt_str_opt(const json& j, const char* key) {
  if (!j.contains(key) || j.at(key).is_null() || !j.at(key).is_string()) return std::nullopt;
  return j.at(key).get<std::string>();
}

}  // namespace

namespace detail {

std::optional<std::uint32_t> parse_leading_uint(std::string_view s) {
  std::size_t end = 0;
  while (end < s.size() && s[end] >= '0' && s[end] <= '9') ++end;
  if (end == 0) return std::nullopt;
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < end; ++i) {
    // Guard against absurdly long digit runs overflowing (defensive; live
    // payloads are "23 min per ep"-shaped, never this long).
    if (v > (std::numeric_limits<std::uint32_t>::max() - 9) / 10) return std::nullopt;
    v = v * 10 + static_cast<std::uint32_t>(s[i] - '0');
  }
  return v;
}

std::optional<std::string> map_status(std::optional<std::string_view> s) {
  if (!s.has_value()) return std::nullopt;
  std::string lower;
  lower.reserve(s->size());
  for (const char c : *s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower.find("finished") != std::string::npos) return std::string("FINISHED");
  if (lower.find("cancel") != std::string::npos) return std::string("CANCELLED");
  if (lower.find("not yet") != std::string::npos) return std::string("NOT_YET_RELEASED");
  if (lower.find("airing") != std::string::npos || lower.find("current") != std::string::npos)
    return std::string("RELEASING");
  return std::string(*s);  // unknown -> keep raw; is_still_airing defaults safe.
}

SearchHit map_anime(const FilterRow& row) {
  const std::optional<std::uint32_t> total =
      row.ani_episodes.has_value() ? parse_leading_uint(*row.ani_episodes) : std::nullopt;
  const std::optional<std::string> status = map_status(
      row.ani_status.has_value() ? std::optional<std::string_view>(*row.ani_status) : std::nullopt);

  SearchHit hit;
  hit.provider_id = std::to_string(row.id);
  hit.title = row.title.value_or("(untitled)");
  hit.title_english = row.title_english;
  hit.title_native = std::nullopt;  // senshi has no separate native field.
  hit.anilist_id = std::nullopt;
  // u64->i64 wraps only for a hostile id > i64::MAX; mal_id is a non-key hint.
  hit.mal_id = static_cast<std::int64_t>(row.id);
  const std::optional<std::string_view> status_view =
      status.has_value() ? std::optional<std::string_view>(*status) : std::nullopt;
  hit.total_episodes = is_still_airing(status_view) ? std::nullopt : total;
  hit.eps_sub = total.value_or(0);
  hit.eps_dub = 0;
  hit.year = row.ani_year;
  return hit;
}

std::string ep_label(double n) {
  if (!std::isfinite(n) || n < 0.0) return "0";
  if (n < 1'000'000.0 && std::floor(n) == n) {
    return std::to_string(static_cast<std::int64_t>(n));
  }
  // Match Rust's f64::to_string default formatting for the fractional path
  // (only ever "N.5"-shaped in practice; senshi ep_id has one decimal place).
  std::string s = std::to_string(n);
  // Trim trailing zeros (but keep one digit after the dot), matching Rust's
  // shortest round-trip formatting for values like 13.5 -> "13.5" not
  // "13.500000".
  if (s.find('.') != std::string::npos) {
    std::size_t last = s.find_last_not_of('0');
    if (s[last] == '.') ++last;
    s.erase(last + 1);
  }
  return s;
}

Result<std::vector<std::string>, ProviderError> parse_episodes(std::string_view raw_json) {
  json rows;
  try {
    rows = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(std::string("episodes: ") + e.what()));
  }
  if (!rows.is_array()) return err(ProviderError::decode("episodes: not an array"));

  std::vector<std::string> labels;
  for (const auto& row : rows) {
    if (!row.is_object() || !row.contains("ep_id") || !row.at("ep_id").is_number()) continue;
    const double ep_id = row.at("ep_id").get<double>();
    // Drop non-finite AND non-positive here — the single gate (ROD-441): a
    // hostile negative must never reach ep_label and mislabel to "0".
    if (!std::isfinite(ep_id) || ep_id <= 0.0) continue;
    labels.push_back(ep_label(ep_id));
  }
  std::sort(labels.begin(), labels.end(),
            [](const std::string& a, const std::string& b) { return episode_label_cmp(a, b) < 0; });
  return labels;
}

std::uint8_t match_score(std::optional<std::string_view> status, Translation tt) {
  if (!status.has_value()) return 0;
  std::string lower;
  lower.reserve(status->size());
  for (const char c : *status) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  switch (tt) {
    case Translation::Dub:
      return lower.find("dub") != std::string::npos ? 1 : 0;
    case Translation::Sub:
      if (lower.find("dub") != std::string::npos) return 0;
      if (lower.find("soft") != std::string::npos) return 3;
      if (lower.find("hard") != std::string::npos) return 2;
      if (lower.find("sub") != std::string::npos) return 1;
      return 0;
  }
  return 0;  // unreachable (closed enum).
}

std::optional<Embed> pick_embed(const std::vector<Embed>& embeds, Translation tt) {
  const Embed* best = nullptr;
  std::uint8_t best_score = 0;
  for (const auto& e : embeds) {
    if (!e.url.has_value()) continue;
    const std::uint8_t sc = match_score(
        e.status.has_value() ? std::optional<std::string_view>(*e.status) : std::nullopt, tt);
    // Strict '>' : first-wins on a tie (v0.4.7 fold order, not a last-wins max).
    if (sc > best_score) {
      best_score = sc;
      best = &e;
    }
  }
  if (best == nullptr) return std::nullopt;
  return *best;
}

std::string percent_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  auto hex_digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  while (i < s.size()) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex_digit(s[i + 1]);
      const int lo = hex_digit(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 3;
        continue;
      }
    }
    out.push_back(s[i]);
    ++i;
  }
  return out;
}

std::optional<std::string> sub_info_url(std::optional<std::string_view> server_fm) {
  if (!server_fm.has_value()) return std::nullopt;
  constexpr std::string_view kKey = "sub.info=";
  const auto pos = server_fm->find(kKey);
  if (pos == std::string_view::npos) return std::nullopt;
  std::string_view val = server_fm->substr(pos + kKey.size());
  if (const auto amp = val.find('&'); amp != std::string_view::npos) val = val.substr(0, amp);
  if (val.empty()) return std::nullopt;
  return percent_decode(val);
}

std::optional<std::string> pick_sub_track(const std::vector<SubTrack>& tracks) {
  std::optional<std::string> english;
  std::optional<std::string> first;
  for (const auto& t : tracks) {
    if (!t.src.has_value()) continue;
    if (t.is_default) return t.src;
    if (!first.has_value()) first = t.src;
    if (!english.has_value() && t.label.has_value()) {
      std::string lower;
      lower.reserve(t.label->size());
      for (const char c : *t.label) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      if (lower.rfind("eng", 0) == 0) english = t.src;
    }
  }
  return english.has_value() ? english : first;
}

std::optional<std::string> guarded_sub_track(const std::vector<SubTrack>& tracks) {
  const auto src = pick_sub_track(tracks);
  if (!src.has_value()) return std::nullopt;
  if (!is_absolute_url(*src) || !clean_arg(*src)) return std::nullopt;
  if (!http::guard_fetch_url(*src).has_value()) return std::nullopt;
  return src;
}

Result<Unit, ProviderError> guard_ep_label(std::string_view s) {
  if (s.empty()) return err(ProviderError::decode("invalid episode"));
  int dots = 0;
  for (const char c : s) {
    if (c == '.') {
      if (++dots > 1) return err(ProviderError::decode("invalid episode"));
    } else if (c < '0' || c > '9') {
      return err(ProviderError::decode("invalid episode"));
    }
  }
  return Unit{};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The provider.
// ---------------------------------------------------------------------------

Result<Senshi, ProviderError> Senshi::create() { return with_endpoint(kApi); }

Result<Senshi, ProviderError> Senshi::with_endpoint(std::string api) {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return Senshi(std::move(*client), std::move(api));
}

std::optional<std::string> Senshi::canonical_key(const Enrichment& show) const {
  if (!show.mal_id.has_value()) return std::nullopt;
  return std::to_string(*show.mal_id);
}

Result<std::vector<std::uint8_t>, ProviderError> Senshi::request(
    http::Method method, const std::string& url, std::optional<std::string_view> body) const {
  http::Request req;
  req.method = method;
  req.url = url;
  req.user_agent = http::kBrowserUserAgent;
  req.extra_headers.push_back({"Accept", "application/json"});
  req.accept = http::Accept::Any2xx;
  if (body.has_value()) {
    req.content_type = "application/json";
    req.body.assign(body->begin(), body->end());
  }
  return http_.fetch(req);
}

Result<std::vector<std::uint8_t>, ProviderError> Senshi::cdn_get(const std::string& url) const {
  if (!http::guard_fetch_url(url).has_value()) return err(ProviderError::decode("blocked url"));
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = http::kBrowserUserAgent;
  req.extra_headers.push_back({"Referer", kStreamReferer});
  req.accept = http::Accept::OkOnly;
  return http_.fetch(req);
}

std::optional<std::string> Senshi::fetch_subtitle(std::optional<std::string_view> server_fm) const {
  const auto info_url = detail::sub_info_url(server_fm);
  if (!info_url.has_value()) return std::nullopt;
  if (!is_absolute_url(*info_url) || !clean_arg(*info_url)) return std::nullopt;

  // Sidecar CDN empty/403 windows (ROD-309): bounded retries, escalating
  // backoff, first try immediate. A dead sidecar must not stall resolve.
  static constexpr int kBackoffsMs[] = {300, 700, 1200};
  std::optional<std::vector<detail::SubTrack>> tracks;
  for (std::size_t attempt = 0; attempt <= std::size(kBackoffsMs); ++attempt) {
    if (attempt > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffsMs[attempt - 1]));
    }
    auto body = cdn_get(*info_url);
    if (!body.has_value()) continue;
    try {
      const auto parsed = json::parse(body->begin(), body->end());
      if (!parsed.is_array()) continue;
      std::vector<detail::SubTrack> out;
      for (const auto& t : parsed) {
        if (!t.is_object()) continue;
        detail::SubTrack st;
        st.src = opt_str_opt(t, "src");
        st.label = opt_str_opt(t, "label");
        st.is_default = t.value("default", false);
        out.push_back(std::move(st));
      }
      tracks = std::move(out);
      break;
    } catch (const json::parse_error&) {
      continue;
    }
  }
  if (!tracks.has_value()) return std::nullopt;
  return detail::guarded_sub_track(*tracks);
}

Result<std::vector<SearchHit>, ProviderError> Senshi::search(std::string_view query,
                                                              const SearchOptions& opts) const {
  json body = {
      {"searchTerm", std::string(query)},
      {"types", json::array()},
      {"genres", json::array()},
      {"status", json::array()},
      {"seasons", json::array()},
      {"year", ""},
      {"studios", json::array()},
      {"producers", json::array()},
      {"languages", json::array()},
      {"page", opts.page},
      {"limit", kSearchPageSize},
      {"sortBy", "score_desc"},
  };
  const std::string url = api_ + "/anime/filter";
  const std::string bodyStr = body.dump();
  auto raw = request(http::Method::Post, url, std::string_view(bodyStr));
  if (!raw.has_value()) return err(raw.error());

  json resp;
  try {
    resp = json::parse(raw->begin(), raw->end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(std::string("search: ") + e.what()));
  }
  std::vector<SearchHit> hits;
  if (resp.is_object() && resp.contains("data") && resp.at("data").is_array()) {
    for (const auto& row : resp.at("data")) {
      if (!row.is_object() || !row.contains("id") || !row.at("id").is_number_unsigned()) continue;
      detail::FilterRow fr;
      fr.id = row.at("id").get<std::uint64_t>();
      fr.title = opt_str_opt(row, "title");
      fr.title_english = opt_str_opt(row, "title_english");
      fr.ani_episodes = opt_str_opt(row, "ani_episodes");
      fr.ani_status = opt_str_opt(row, "ani_status");
      if (row.contains("ani_year") && row.at("ani_year").is_number()) {
        fr.ani_year = row.at("ani_year").get<std::uint32_t>();
      }
      hits.push_back(detail::map_anime(fr));
    }
  }
  if (hits.size() > opts.limit) hits.resize(opts.limit);
  return hits;
}

Result<std::vector<std::string>, ProviderError> Senshi::episodes(
    std::string_view provider_id, Translation /*translation*/,
    std::optional<std::uint32_t> /*count_hint*/) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  const std::string url = api_ + "/episodes/" + std::string(provider_id);
  auto raw = request(http::Method::Get, url, std::nullopt);
  if (!raw.has_value()) return err(raw.error());
  const std::string_view raw_view(reinterpret_cast<const char*>(raw->data()), raw->size());
  return detail::parse_episodes(raw_view);
}

std::optional<std::string> Senshi::cap_variant(const std::string& master_url,
                                                Quality quality) const {
  auto body = cdn_get(master_url);
  if (!body.has_value()) return std::nullopt;
  const std::string_view body_view(reinterpret_cast<const char*>(body->data()), body->size());
  const auto variants = hls::parse_master_playlist(body_view);
  if (variants.empty()) return std::nullopt;  // media playlist: let mpv take the master.

  std::vector<StreamLink> links;
  for (const auto& v : variants) {
    const auto joined = hls::join_url(master_url, v.url);
    if (!joined.has_value()) continue;
    if (!clean_arg(*joined)) continue;
    StreamLink link;
    link.url = *joined;
    link.resolution = v.resolution;
    links.push_back(std::move(link));
  }
  const auto* pick = hls::select_variant(links, quality);
  if (pick == nullptr) return std::nullopt;
  return pick->url;
}

Result<StreamLink, ProviderError> Senshi::resolve(std::string_view provider_id,
                                                   std::string_view episode,
                                                   Translation translation,
                                                   Quality quality) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  if (auto g = detail::guard_ep_label(episode); !g.has_value()) return err(g.error());

  const std::string url =
      api_ + "/episode-embeds/" + std::string(provider_id) + "/" + std::string(episode);
  auto raw = request(http::Method::Get, url, std::nullopt);
  if (!raw.has_value()) return err(raw.error());

  json parsed;
  try {
    parsed = json::parse(raw->begin(), raw->end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(std::string("embeds: ") + e.what()));
  }
  if (!parsed.is_array()) return err(ProviderError::decode("embeds: not an array"));

  std::vector<detail::Embed> embeds;
  for (const auto& e : parsed) {
    if (!e.is_object()) continue;
    detail::Embed em;
    em.url = opt_str_opt(e, "url");
    em.status = opt_str_opt(e, "status");
    em.server_fm = opt_str_opt(e, "serverFM");
    embeds.push_back(std::move(em));
  }

  auto picked = detail::pick_embed(embeds, translation);
  if (!picked.has_value()) return err(ProviderError::decode("no stream for track"));
  if (!picked->url.has_value()) return err(ProviderError::decode("no stream for track"));
  const std::string& stream = *picked->url;

  if (!is_absolute_url(stream) || !clean_arg(stream)) {
    return err(ProviderError::decode("bad stream url"));
  }

  // `best` leaves mpv on the master ladder; a cap fetches variants.
  // Best-effort: failure falls back to the adaptive master.
  std::string chosen = stream;
  if (quality != Quality::Best) {
    if (auto capped = cap_variant(stream, quality); capped.has_value()) {
      chosen = std::move(*capped);
    }
  }

  std::optional<std::string> sub_url;
  if (translation == Translation::Sub) {
    sub_url = fetch_subtitle(picked->server_fm.has_value()
                                  ? std::optional<std::string_view>(*picked->server_fm)
                                  : std::nullopt);
  }

  StreamLink link;
  link.url = std::move(chosen);
  link.resolution = std::nullopt;
  link.referer = kStreamReferer;
  link.user_agent = http::kBrowserUserAgent;
  // ninstream serves .ts cloaked as .jpg; mpv must relax its demuxer (A6).
  link.cloaked_segments = true;
  link.decloak_segments = false;
  link.sub_url = sub_url;
  return link;
}

Result<CoverRequest, ProviderError> Senshi::cover_request(std::string_view cover_ref) const {
  if (cover_ref.empty() || cover_ref.size() > kMaxCoverRefLen || !clean_arg(cover_ref)) {
    return err(ProviderError::decode("invalid cover ref"));
  }
  if (is_absolute_url(cover_ref)) {
    CoverRequest cr;
    cr.url = std::string(cover_ref);
    return cr;
  }
  const std::string sep = (!cover_ref.empty() && cover_ref.front() == '/') ? "" : "/";
  CoverRequest cr;
  cr.url = std::string(kApi) + sep + std::string(cover_ref);
  cr.referer = kStreamReferer;
  cr.user_agent = http::kBrowserUserAgent;
  return cr;
}

}  // namespace shigoku::senshi
