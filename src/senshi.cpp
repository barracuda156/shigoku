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
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "crypto.hpp"
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

Result<Sources, ProviderError> parse_sources(std::string_view raw_json) {
  json parsed;
  try {
    parsed = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(std::string("sources: ") + e.what()));
  }
  // Array-or-object; the site's own page takes the first element.
  const json* row = nullptr;
  if (parsed.is_array()) {
    if (!parsed.empty() && parsed.front().is_object()) row = &parsed.front();
  } else if (parsed.is_object()) {
    row = &parsed;
  }
  if (row == nullptr) return err(ProviderError::decode("sources: not an object"));
  Sources out;
  if (row->contains("source") && row->at("source").is_object()) {
    out.src = opt_str_opt(row->at("source"), "src");
  }
  if (row->contains("tracks") && row->at("tracks").is_array()) {
    for (const auto& t : row->at("tracks")) {
      if (!t.is_object()) continue;
      SubTrack st;
      // WebVTT is what the player wants; the .ass url is the fallback.
      st.src = opt_str_opt(t, "vtt_url");
      if (!st.src.has_value() || st.src->empty()) st.src = opt_str_opt(t, "url");
      if (st.src.has_value() && st.src->empty()) st.src = std::nullopt;
      st.label = opt_str_opt(t, "label");
      st.is_default = t.value("default", false);
      // A chapter/storyboard track is not a subtitle.
      if (st.label.has_value() && *st.label == "chapter") continue;
      out.tracks.push_back(std::move(st));
    }
  }
  return out;
}

namespace {

constexpr std::uint8_t kBakedA[32] = {226, 24, 149, 40, 170, 108, 184, 157, 168, 18, 90, 64, 186, 69, 66, 110, 109, 169, 203, 138, 29, 188, 78, 25, 203, 185, 211, 252, 76, 126, 134, 42};
constexpr std::uint8_t kBakedB[32] = {140, 250, 231, 59, 141, 129, 254, 6, 30, 203, 96, 249, 13, 237, 122, 106, 60, 57, 126, 48, 152, 101, 128, 186, 122, 88, 171, 249, 187, 202, 40, 220};

// "[n,n,...]" at `pos` (js[pos] == '['): the numbers, or empty on any
// non-numeric content / no closing bracket within a sane span.
std::vector<int> int_array_at(std::string_view js, std::size_t pos) {
  std::vector<int> out;
  if (pos >= js.size() || js[pos] != '[') return out;
  std::size_t i = pos + 1;
  int cur = -1;
  const std::size_t limit = std::min(js.size(), pos + 4096);
  while (i < limit) {
    const char c = js[i];
    if (c >= '0' && c <= '9') {
      cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
      if (cur > 100000) return {};
    } else if (c == ',' || c == ']') {
      if (cur < 0) return {};
      out.push_back(cur);
      cur = -1;
      if (c == ']') return out;
    } else if (c != ' ' && c != '\n') {
      return {};
    }
    ++i;
  }
  return {};
}

bool identifier_byte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '$';
}

}  // namespace

BundleKey baked_bundle() {
  BundleKey b;
  b.key.resize(32);
  for (std::size_t i = 0; i < 32; ++i) b.key[i] = static_cast<std::uint8_t>(kBakedA[i] ^ kBakedB[i]);
  b.sources_base = kSourcesBase;
  return b;
}

std::optional<BundleKey> scrape_bundle(std::string_view watch_js) {
  constexpr std::string_view kFrom = "Uint8Array.from(";
  std::vector<std::vector<int>> arrays;
  for (std::size_t pos = watch_js.find(kFrom); pos != std::string_view::npos && arrays.size() < 2;
       pos = watch_js.find(kFrom, pos + 1)) {
    auto v = int_array_at(watch_js, pos + kFrom.size());
    if (v.size() != 32) continue;
    bool bytes = true;
    for (const int x : v) bytes = bytes && x >= 0 && x <= 255;
    if (bytes) arrays.push_back(std::move(v));
  }
  if (arrays.size() < 2) return std::nullopt;
  BundleKey out;
  out.key.resize(32);
  for (std::size_t i = 0; i < 32; ++i) {
    out.key[i] = static_cast<std::uint8_t>(arrays[0][i] ^ arrays[1][i]);
  }
  out.sources_base = kSourcesBase;
  // The prefix: a char-code array bound to `ar` (`const ar=[...]`).
  constexpr std::string_view kAr = "ar=[";
  for (std::size_t pos = watch_js.find(kAr); pos != std::string_view::npos;
       pos = watch_js.find(kAr, pos + 1)) {
    if (pos > 0 && identifier_byte(watch_js[pos - 1])) continue;  // `var=[`, `bar=[`...
    auto v = int_array_at(watch_js, pos + kAr.size() - 1);
    if (v.empty()) continue;
    std::string s;
    for (const int x : v) {
      if (x < 0x21 || x > 0x7e) {
        s.clear();
        break;
      }
      s.push_back(static_cast<char>(x));
    }
    if (s.rfind("https://", 0) == 0 && s.find("?id=") != std::string::npos &&
        http::guard_fetch_url(s + "1").has_value()) {
      out.sources_base = s;
    }
    break;
  }
  return out;
}

std::optional<std::string> index_bundle_path(std::string_view html) {
  constexpr std::string_view kNeedle = "src=\"/assets/index-";
  const auto pos = html.find(kNeedle);
  if (pos == std::string_view::npos) return std::nullopt;
  const std::size_t start = pos + 5;  // past `src="`.
  const auto end = html.find('"', start);
  if (end == std::string_view::npos || end - start > 200) return std::nullopt;
  const std::string path(html.substr(start, end - start));
  if (path.size() < 4 || path.compare(path.size() - 3, 3, ".js") != 0 || !clean_arg(path)) {
    return std::nullopt;
  }
  return path;
}

std::optional<std::string> watch_chunk_path(std::string_view index_js) {
  constexpr std::string_view kNeedle = "./WatchPage-";
  const auto pos = index_js.find(kNeedle);
  if (pos == std::string_view::npos) return std::nullopt;
  const std::size_t start = pos + 2;  // past `./`.
  const auto end = index_js.find(".js", start);
  if (end == std::string_view::npos || end - start > 120) return std::nullopt;
  const std::string name(index_js.substr(start, end + 3 - start));
  if (!clean_arg(name) || name.find('/') != std::string::npos) return std::nullopt;
  return "/assets/" + name;
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

struct Senshi::BundleCache {
  std::mutex mu;
  std::optional<detail::BundleKey> key;
};

Senshi::Senshi(http::Client client, std::string api, std::string sources_base)
    : http_(std::move(client)),
      api_(std::move(api)),
      sources_base_(std::move(sources_base)),
      bundle_(std::make_shared<BundleCache>()) {}

Result<Senshi, ProviderError> Senshi::create() { return with_endpoint(kApi); }

Result<Senshi, ProviderError> Senshi::with_endpoint(std::string api) {
  return with_endpoints(std::move(api), kSourcesBase);
}

Result<Senshi, ProviderError> Senshi::with_endpoints(std::string api, std::string sources_base) {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return Senshi(std::move(*client), std::move(api), std::move(sources_base));
}

const detail::BundleKey& Senshi::bundle_key() const {
  std::lock_guard<std::mutex> lock(bundle_->mu);
  if (bundle_->key.has_value()) return *bundle_->key;
  // index.html -> the index bundle -> the WatchPage chunk -> the two arrays.
  // Any miss along the way lands on the baked copy; the outcome is cached
  // either way (one scrape per process, not one per resolve).
  auto as_text = [](const std::vector<std::uint8_t>& b) {
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
  };
  std::optional<detail::BundleKey> scraped;
  if (auto html = request(http::Method::Get, api_ + "/", std::nullopt); html.has_value()) {
    if (auto index_path = detail::index_bundle_path(as_text(*html)); index_path.has_value()) {
      if (auto index_js = request(http::Method::Get, api_ + *index_path, std::nullopt);
          index_js.has_value()) {
        if (auto chunk_path = detail::watch_chunk_path(as_text(*index_js));
            chunk_path.has_value()) {
          if (auto chunk = request(http::Method::Get, api_ + *chunk_path, std::nullopt);
              chunk.has_value()) {
            scraped = detail::scrape_bundle(as_text(*chunk));
          }
        }
      }
    }
  }
  bundle_->key = scraped.has_value() ? *scraped : detail::baked_bundle();
  // A test seam pointing the sources hop elsewhere outranks whatever the
  // bundle says; the live prefix only applies when the default was in use.
  if (sources_base_ != kSourcesBase) bundle_->key->sources_base = sources_base_;
  return *bundle_->key;
}

Result<std::vector<std::uint8_t>, ProviderError> Senshi::sources_get(const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = http::kBrowserUserAgent;
  req.extra_headers.push_back({"Accept", "application/json"});
  req.extra_headers.push_back({"Referer", kStreamReferer});
  req.extra_headers.push_back({"Origin", kApi});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
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

std::optional<std::string> Senshi::cap_variant(const std::string& master_url, Quality quality,
                                                const std::vector<std::uint8_t>& key) const {
  auto body = cdn_get(master_url);
  if (!body.has_value()) return std::nullopt;
  std::string_view body_view(reinterpret_cast<const char*>(body->data()), body->size());
  // An encrypted master opens here the same way the proxy opens it for mpv.
  std::vector<std::uint8_t> opened;
  const std::string_view magic = kPlaylistMagic;
  if (body_view.size() > magic.size() && body_view.substr(0, magic.size()) == magic) {
    auto plain = crypto::open_b64_gcm(body_view.substr(magic.size()), key);
    if (!plain.has_value()) return std::nullopt;
    opened = std::move(*plain);
    body_view = std::string_view(reinterpret_cast<const char*>(opened.data()), opened.size());
  }
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
    if (e.contains("remote_source_id") && e.at("remote_source_id").is_number_integer()) {
      const auto id = e.at("remote_source_id").get<std::int64_t>();
      if (id > 0) em.remote_source_id = id;
    }
    embeds.push_back(std::move(em));
  }

  auto picked = detail::pick_embed(embeds, translation);
  if (!picked.has_value()) return err(ProviderError::decode("no stream for track"));

  // The sources hop (a row with a remote_source_id) answers the real master
  // and the subtitle tracks; a row without one is the older direct shape.
  std::string stream;
  std::vector<detail::SubTrack> tracks;
  bool from_sources = false;
  const detail::BundleKey& bundle = bundle_key();
  if (picked->remote_source_id.has_value()) {
    auto raw_src = sources_get(bundle.sources_base + std::to_string(*picked->remote_source_id));
    if (!raw_src.has_value()) return err(raw_src.error());
    const std::string_view src_view(reinterpret_cast<const char*>(raw_src->data()),
                                    raw_src->size());
    auto sources = detail::parse_sources(src_view);
    if (!sources.has_value()) return err(sources.error());
    if (!sources->src.has_value()) return err(ProviderError::decode("no stream source"));
    stream = *sources->src;
    tracks = std::move(sources->tracks);
    from_sources = true;
  } else {
    if (!picked->url.has_value()) return err(ProviderError::decode("no stream for track"));
    stream = *picked->url;
  }

  if (!is_absolute_url(stream) || !clean_arg(stream)) {
    return err(ProviderError::decode("bad stream url"));
  }

  // `best` leaves mpv on the master ladder; a cap fetches variants.
  // Best-effort: failure falls back to the adaptive master.
  std::string chosen = stream;
  if (quality != Quality::Best) {
    if (auto capped = cap_variant(stream, quality, bundle.key); capped.has_value()) {
      chosen = std::move(*capped);
    }
  }

  std::optional<std::string> sub_url;
  if (translation == Translation::Sub) {
    if (from_sources) {
      sub_url = detail::guarded_sub_track(tracks);
    } else {
      sub_url = fetch_subtitle(picked->server_fm.has_value()
                                   ? std::optional<std::string_view>(*picked->server_fm)
                                   : std::nullopt);
    }
  }

  StreamLink link;
  link.url = std::move(chosen);
  link.resolution = std::nullopt;
  link.referer = kStreamReferer;
  link.user_agent = http::kBrowserUserAgent;
  // The CDN serves .ts behind a .jpg extension; mpv must relax its demuxer (A6).
  link.cloaked_segments = true;
  link.decloak_segments = false;
  link.sub_url = sub_url;
  // Every playlist comes back enveloped: the proxy opens them for mpv.
  link.playlist_cipher = StreamLink::PlaylistCipher{bundle.key, kPlaylistMagic};
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
