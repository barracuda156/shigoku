// mal_catalog.cpp — MAL API v2 catalog: URL builders, JSON -> Enrichment
// mappers, client-side filters, and the thin transport (mal_catalog.hpp).

#include "mal_catalog.hpp"

#include "mal.hpp"  // list_status_from_mal: the one status table, both directions.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "idmap.hpp"

namespace shigoku::mal_catalog {

namespace {

using json = nlohmann::json;

std::string to_upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A string field that is absent, null, non-string or blank -> nullopt;
// otherwise control-stripped.
std::optional<std::string> str_opt(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key)) return std::nullopt;
  const auto& v = j.at(key);
  if (!v.is_string()) return std::nullopt;
  std::string s = strip_controls(v.get<std::string>());
  if (s.empty()) return std::nullopt;
  return s;
}

template <class T>
std::optional<T> num_opt(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key)) return std::nullopt;
  const auto& v = j.at(key);
  if (!v.is_number()) return std::nullopt;
  return v.get<T>();
}

// The AniList genre vocabulary the overlay speaks (anilist.rs GenreCollection
// answer, pinned here because MAL has no equivalent call).
constexpr const char* kGenres[] = {
    "Action",  "Adventure", "Comedy",  "Drama",         "Ecchi",  "Fantasy",
    "Hentai",  "Horror",    "Mahou Shoujo", "Mecha",    "Music",  "Mystery",
    "Psychological", "Romance", "Sci-Fi", "Slice of Life", "Sports", "Supernatural",
    "Thriller",
};

// MAL spellings that mean the same as an AniList genre.
std::string_view genre_alias(std::string_view anilist_genre) {
  if (anilist_genre == "Thriller") return "Suspense";
  return anilist_genre;
}

// Map one MAL anime node (list item's "node" or the by-id top level) onto the
// Enrichment the UI reads. `keep_anilist_id` != 0 pins the key.
Enrichment map_node(const json& m, std::int64_t keep_anilist_id, const IdBridge& bridge) {
  Enrichment e;
  const auto mal_id = num_opt<std::int64_t>(m, "id").value_or(0);
  e.mal_id = mal_id > 0 ? std::optional<std::int64_t>(mal_id) : std::nullopt;
  if (keep_anilist_id != 0) {
    e.anilist_id = keep_anilist_id;
  } else if (mal_id > 0) {
    e.anilist_id = bridge(mal_id);
  }

  e.title_romaji = str_opt(m, "title").value_or(std::string{});
  if (m.contains("alternative_titles") && m.at("alternative_titles").is_object()) {
    const json& alt = m.at("alternative_titles");
    e.title_english = str_opt(alt, "en");
    e.title_native = str_opt(alt, "ja");
  }
  if (m.contains("main_picture") && m.at("main_picture").is_object()) {
    const json& pic = m.at("main_picture");
    e.cover_url = str_opt(pic, "large");
    if (!e.cover_url.has_value()) e.cover_url = str_opt(pic, "medium");
  }

  // MAL reports 0 episodes for "unknown"; AniList reports null. Same for
  // duration.
  if (const auto eps = num_opt<std::uint32_t>(m, "num_episodes"); eps.has_value() && *eps > 0) {
    e.total_episodes = eps;
  }
  if (const auto secs = num_opt<std::int64_t>(m, "average_episode_duration"); secs.has_value()) {
    e.duration_minutes = detail::minutes_from_seconds(*secs);
  }

  if (m.contains("start_season") && m.at("start_season").is_object()) {
    const json& ss = m.at("start_season");
    e.year = num_opt<std::uint32_t>(ss, "year");
    if (const auto s = str_opt(ss, "season"); s.has_value()) e.season = parse_season(*s);
  }
  if (const auto sd = str_opt(m, "start_date"); sd.has_value()) {
    e.start_date = detail::parse_date(*sd);
    // A show without a start_season (movies, specials) still has a year.
    if (!e.year.has_value() && e.start_date.has_value()) e.year = e.start_date->year;
  }

  if (const auto st = str_opt(m, "status"); st.has_value()) e.status = detail::map_status(*st);
  if (const auto mt = str_opt(m, "media_type"); mt.has_value()) e.kind = detail::map_media_type(*mt);
  if (const auto src = str_opt(m, "source"); src.has_value()) {
    e.source_material = detail::map_source(*src);
  }
  // The raw string, not str_opt: the cleaner needs the newlines (as word
  // separators) that strip_controls would drop first; it strips at the end.
  if (m.contains("synopsis") && m.at("synopsis").is_string()) {
    e.description = detail::clean_synopsis(m.at("synopsis").get<std::string>());
  }
  if (m.contains("mean") && m.at("mean").is_number()) {
    e.score = detail::score_from_mean(m.at("mean").get<double>());
  }

  // MAL's `rank` is the all-time by-score position; `popularity` the by-members
  // one. AniList's contextual rankings have no MAL counterpart, so this is
  // always an all-time rank (rank_year stays nullopt), RATED preferred.
  if (const auto r = num_opt<std::uint32_t>(m, "rank"); r.has_value() && *r > 0) {
    e.rank = r;
    e.rank_type = "RATED";
  } else if (const auto p = num_opt<std::uint32_t>(m, "popularity"); p.has_value() && *p > 0) {
    e.rank = p;
    e.rank_type = "POPULAR";
  }

  if (m.contains("genres") && m.at("genres").is_array()) {
    for (const auto& g : m.at("genres")) {
      if (const auto name = str_opt(g, "name"); name.has_value()) e.genres.push_back(*name);
    }
  }
  if (m.contains("studios") && m.at("studios").is_array()) {
    for (const auto& s : m.at("studios")) {
      if (const auto name = str_opt(s, "name"); name.has_value()) e.studios.push_back(*name);
    }
  }
  // No wire field for next airing episode or country of origin.
  return e;
}

Result<json, ProviderError> parse_json(std::string_view raw) {
  try {
    return json::parse(raw.begin(), raw.end());
  } catch (const json::parse_error& ex) {
    return err(ProviderError::decode(ex.what()));
  }
}

http::Request get_request(std::string url, std::string_view client_id) {
  http::Request req;
  req.method = http::Method::Get;
  req.url = std::move(url);
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"X-MAL-CLIENT-ID", std::string(client_id)});
  return req;
}

Result<std::vector<std::uint8_t>, ProviderError> fetch_with_id(const http::Client& client,
                                                                std::string_view client_id,
                                                                std::string url) {
  if (client_id.empty()) return err(ProviderError::forbidden(403));
  return client.fetch(get_request(std::move(url), client_id));
}

std::string_view as_view(const std::vector<std::uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

std::string effective_client_id(std::string_view config_override) {
  if (!config_override.empty()) return std::string(config_override);
  return kClientId;
}

std::int64_t default_bridge(std::int64_t mal_id) {
  if (const auto real = idmap::to_anilist(mal_id); real.has_value()) return *real;
  return -mal_id;
}

std::vector<std::string> genre_vocabulary() {
  return std::vector<std::string>(std::begin(kGenres), std::end(kGenres));
}

Result<Page, ProviderError> search(const http::Client& client, std::string_view client_id,
                                   std::string_view query, std::uint32_t page,
                                   const IdBridge& bridge) {
  if (detail::query_too_short(query)) return Page{};
  auto resp = fetch_with_id(client, client_id, detail::search_url(query, page));
  if (!resp.has_value()) return err(resp.error());
  return detail::parse_page(as_view(*resp), bridge);
}

Result<Page, ProviderError> discover(const http::Client& client, std::string_view client_id,
                                     DiscoverAxis axis, std::uint32_t page, std::int64_t unix_secs,
                                     const DiscoverFilters& filters, const IdBridge& bridge) {
  const std::uint32_t limit = filters.empty() ? kDiscoverPageSize : kFilteredPageSize;
  auto resp = fetch_with_id(client, client_id, detail::discover_url(axis, page, unix_secs, limit));
  if (!resp.has_value()) return err(resp.error());
  auto parsed = detail::parse_page(as_view(*resp), bridge);
  if (!parsed.has_value()) return parsed;
  if (filters.empty()) return parsed;
  Page out;
  out.has_next = parsed->has_next;
  for (auto& e : parsed->entries) {
    if (detail::passes_filters(e, filters)) out.entries.push_back(std::move(e));
  }
  return out;
}

Result<std::vector<UserListEntry>, ProviderError> user_list(const http::Client& client,
                                                            std::string_view token,
                                                            const IdBridge& bridge) {
  std::vector<UserListEntry> all;
  std::string url = detail::user_list_url();
  for (std::uint32_t page = 0; page < kUserListPages; ++page) {
    http::Request req;
    req.method = http::Method::Get;
    req.url = url;
    req.accept = http::Accept::Any2xx;
    req.extra_headers.push_back(http::Header{"Authorization", "Bearer " + std::string(token)});
    auto resp = client.fetch(req);
    if (!resp.has_value()) return err(resp.error());
    auto parsed = detail::parse_user_list_page(as_view(*resp), bridge);
    if (!parsed.has_value()) return err(parsed.error());
    for (auto& e : parsed->entries) all.push_back(std::move(e));
    if (!parsed->next.has_value() || parsed->next->empty()) break;
    if (parsed->next->rfind(kApiBase, 0) != 0) break;  // never follow off-host.
    url = *parsed->next;
  }
  return all;
}

Result<std::optional<Detail>, ProviderError> by_id(const http::Client& client,
                                                   std::string_view client_id, std::int64_t mal_id,
                                                   std::int64_t keep_anilist_id,
                                                   const IdBridge& bridge) {
  auto resp = fetch_with_id(client, client_id, detail::by_id_url(mal_id));
  if (!resp.has_value()) {
    // 404 is MAL's "no such anime": a true negative, not a failure.
    if (resp.error().kind == ProviderError::Kind::Http && resp.error().status == 404) {
      return std::optional<Detail>{};
    }
    return err(resp.error());
  }
  auto parsed = detail::parse_detail(as_view(*resp), keep_anilist_id, bridge);
  if (!parsed.has_value()) return err(parsed.error());
  return std::optional<Detail>(std::move(*parsed));
}

// ---------------------------------------------------------------------------
// detail
// ---------------------------------------------------------------------------
namespace detail {

std::string user_list_url() {
  // list_status expands to its whole object (status, score, num_episodes_
  // watched, is_rewatching, updated_at); the node fields are the subset
  // map_node reads for a seed row. nsfw=true: the account's list is the
  // account's business, nothing is hidden from it.
  return std::string(kApiBase) +
         "/users/@me/animelist?fields=list_status,alternative_titles,num_episodes,status,"
         "start_season,start_date,media_type,mean&nsfw=true&limit=" +
         std::to_string(kUserListPageSize);
}

std::int64_t parse_iso8601_utc(std::string_view s) {
  auto digits = [&](std::size_t pos, std::size_t len, int lo, int hi) -> std::optional<int> {
    if (pos + len > s.size()) return std::nullopt;
    int v = 0;
    for (std::size_t i = 0; i < len; ++i) {
      const char c = s[pos + i];
      if (c < '0' || c > '9') return std::nullopt;
      v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) return std::nullopt;
    return v;
  };
  if (s.size() < 19 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
      s[16] != ':') {
    return 0;
  }
  const auto y = digits(0, 4, 1970, 9999), mo = digits(5, 2, 1, 12), d = digits(8, 2, 1, 31);
  const auto hh = digits(11, 2, 0, 23), mi = digits(14, 2, 0, 59), ss = digits(17, 2, 0, 60);
  if (!y || !mo || !d || !hh || !mi || !ss) return 0;
  // Days from civil (proleptic Gregorian), the usual era arithmetic.
  const int yy = *y - (*mo <= 2 ? 1 : 0);
  const int era = yy / 400;
  const int yoe = yy - era * 400;
  const int doy = (153 * (*mo + (*mo > 2 ? -3 : 9)) + 2) / 5 + *d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
  std::int64_t secs = days * 86400 + *hh * 3600 + *mi * 60 + *ss;
  // Offset suffix: Z, or ±HH:MM to subtract back to UTC. Anything else fails.
  if (s.size() == 20 && s[19] == 'Z') return secs;
  if (s.size() == 25 && (s[19] == '+' || s[19] == '-') && s[22] == ':') {
    const auto oh = digits(20, 2, 0, 23), om = digits(23, 2, 0, 59);
    if (!oh || !om) return 0;
    const std::int64_t off = *oh * 3600 + *om * 60;
    return s[19] == '+' ? secs - off : secs + off;
  }
  return 0;
}

Result<UserListPage, ProviderError> parse_user_list_page(std::string_view raw_json,
                                                         const IdBridge& bridge) {
  auto j = parse_json(raw_json);
  if (!j.has_value()) return err(j.error());
  if (!j->is_object()) return err(ProviderError::decode("malformed response"));
  UserListPage out;
  if (j->contains("data") && j->at("data").is_array()) {
    for (const auto& row : j->at("data")) {
      if (!row.is_object() || !row.contains("node") || !row.at("node").is_object()) continue;
      UserListEntry e;
      e.seed = map_node(row.at("node"), 0, bridge);
      if (!e.seed.mal_id.has_value()) continue;  // a node without an id keys nothing.
      if (row.contains("list_status") && row.at("list_status").is_object()) {
        const json& ls = row.at("list_status");
        const auto status = str_opt(ls, "status");
        e.status = mal::detail::list_status_from_mal(
            status.has_value() ? std::optional<std::string_view>(*status) : std::nullopt);
        e.progress = num_opt<std::uint32_t>(ls, "num_episodes_watched").value_or(0);
        e.score = num_opt<std::uint32_t>(ls, "score").value_or(0);
        if (const auto u = str_opt(ls, "updated_at"); u.has_value()) {
          e.updated_at = parse_iso8601_utc(*u);
        }
      }
      out.entries.push_back(std::move(e));
    }
  }
  if (j->contains("paging") && j->at("paging").is_object()) {
    out.next = str_opt(j->at("paging"), "next");
  }
  return out;
}

std::string fields_param() {
  return "id,title,main_picture,alternative_titles,start_date,synopsis,mean,rank,"
         "popularity,num_episodes,start_season,source,average_episode_duration,"
         "studios,genres,media_type,status,recommendations";
}

std::string percent_encode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

bool query_too_short(std::string_view query) {
  // Count codepoints, not bytes: a two-kanji title is two characters.
  std::size_t chars = 0;
  for (const unsigned char c : query) {
    if ((c & 0xC0) != 0x80) ++chars;
  }
  return chars < kMinQueryChars;
}

std::string search_url(std::string_view query, std::uint32_t page) {
  const std::uint32_t p = page == 0 ? 1 : page;
  const std::uint32_t offset = (p - 1) * kSearchPageSize;
  return std::string(kApiBase) + "/anime?q=" + percent_encode(query) +
         "&limit=" + std::to_string(kSearchPageSize) + "&offset=" + std::to_string(offset) +
         "&nsfw=true&fields=" + fields_param();
}

std::optional<std::string_view> ranking_type(DiscoverAxis axis) {
  switch (axis) {
    case DiscoverAxis::Trending: return "airing";
    case DiscoverAxis::Popular: return "bypopularity";
    case DiscoverAxis::TopRated: return "all";
    case DiscoverAxis::ThisSeason: return std::nullopt;
  }
  return std::nullopt;  // unreachable (closed enum).
}

std::string discover_url(DiscoverAxis axis, std::uint32_t page, std::int64_t unix_secs,
                         std::uint32_t limit) {
  const std::uint32_t p = page == 0 ? 1 : page;
  const std::uint32_t offset = (p - 1) * limit;
  const std::string tail = "&limit=" + std::to_string(limit) + "&offset=" + std::to_string(offset) +
                           "&nsfw=true&fields=" + fields_param();
  if (const auto rt = ranking_type(axis); rt.has_value()) {
    return std::string(kApiBase) + "/anime/ranking?ranking_type=" + std::string(*rt) + tail;
  }
  const Cour c = current_cour(unix_secs);
  return std::string(kApiBase) + "/anime/season/" + std::to_string(c.year) + "/" +
         std::string(season_as_str(c.season)) + "?sort=anime_num_list_users" + tail;
}

std::string by_id_url(std::int64_t mal_id) {
  return std::string(kApiBase) + "/anime/" + std::to_string(mal_id) + "?fields=" + fields_param();
}

Result<Page, ProviderError> parse_page(std::string_view raw_json, const IdBridge& bridge) {
  auto parsed = parse_json(raw_json);
  if (!parsed.has_value()) return err(parsed.error());
  const json& resp = *parsed;
  // An API change must not read as "no results": a body without the list
  // shape is a decode failure, never an empty page.
  if (!resp.is_object() || !resp.contains("data") || !resp.at("data").is_array()) {
    return err(ProviderError::decode("data is not an array"));
  }
  Page page;
  for (const auto& item : resp.at("data")) {
    if (!item.is_object() || !item.contains("node") || !item.at("node").is_object()) continue;
    Enrichment e = map_node(item.at("node"), 0, bridge);
    if (!e.mal_id.has_value()) continue;  // a node without an id is unusable.
    page.entries.push_back(std::move(e));
  }
  if (resp.contains("paging") && resp.at("paging").is_object()) {
    page.has_next = str_opt(resp.at("paging"), "next").has_value();
  }
  return page;
}

Result<Detail, ProviderError> parse_detail(std::string_view raw_json, std::int64_t keep_anilist_id,
                                           const IdBridge& bridge) {
  auto parsed = parse_json(raw_json);
  if (!parsed.has_value()) return err(parsed.error());
  const json& m = *parsed;
  if (!m.is_object() || !num_opt<std::int64_t>(m, "id").has_value()) {
    return err(ProviderError::decode("anime object without id"));
  }
  Detail d;
  d.show = map_node(m, keep_anilist_id, bridge);
  if (m.contains("recommendations") && m.at("recommendations").is_array()) {
    for (const auto& r : m.at("recommendations")) {
      if (!r.is_object() || !r.contains("node") || !r.at("node").is_object()) continue;
      Enrichment e = map_node(r.at("node"), 0, bridge);
      if (!e.mal_id.has_value()) continue;
      d.recommendations.push_back(std::move(e));
    }
  }
  return d;
}

bool passes_filters(const Enrichment& e, const DiscoverFilters& f) {
  for (const auto& want : f.genres) {
    const std::string_view alias = genre_alias(want);
    const bool has = std::any_of(e.genres.begin(), e.genres.end(), [&](const std::string& g) {
      return g == want || g == alias;
    });
    if (!has) return false;
  }
  if (f.year.has_value()) {
    if (!e.year.has_value() || *e.year != *f.year) return false;
  }
  if (f.status.has_value()) {
    if (!e.status.has_value() || *e.status != *f.status) return false;
  }
  if (f.min_score.has_value()) {
    if (!e.score.has_value() || *e.score <= *f.min_score) return false;
  }
  return true;
}

std::optional<Date> parse_date(std::string_view s) {
  // "YYYY", "YYYY-MM", "YYYY-MM-DD"; anything else is not a date.
  auto digits = [](std::string_view part, std::size_t n) -> std::optional<std::uint32_t> {
    if (part.size() != n) return std::nullopt;
    std::uint32_t v = 0;
    for (const char c : part) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
      v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return v;
  };
  std::vector<std::string_view> parts;
  std::string_view rest = s;
  while (true) {
    const std::size_t dash = rest.find('-');
    parts.push_back(rest.substr(0, dash));
    if (dash == std::string_view::npos) break;
    rest.remove_prefix(dash + 1);
    if (parts.size() == 3) return std::nullopt;  // a fourth component
  }
  if (parts.empty() || parts.size() > 3) return std::nullopt;
  const auto year = digits(parts[0], 4);
  if (!year.has_value() || *year == 0) return std::nullopt;
  Date d;
  d.year = *year;
  if (parts.size() >= 2) {
    const auto month = digits(parts[1], 2);
    if (!month.has_value() || *month < 1 || *month > 12) return std::nullopt;
    d.month = month;
  }
  if (parts.size() == 3) {
    const auto day = digits(parts[2], 2);
    if (!day.has_value() || *day < 1 || *day > 31) return std::nullopt;
    d.day = day;
  }
  return d;
}

std::optional<std::string> map_status(std::string_view mal_status) {
  const std::string s = to_lower(mal_status);
  if (s == "currently_airing") return "RELEASING";
  if (s == "finished_airing") return "FINISHED";
  if (s == "not_yet_aired") return "NOT_YET_RELEASED";
  if (s.empty()) return std::nullopt;
  return to_upper(s);  // an unknown spelling passes through, never a crash.
}

std::optional<std::string> map_media_type(std::string_view media_type) {
  const std::string s = to_lower(media_type);
  if (s.empty() || s == "unknown") return std::nullopt;
  if (s == "tv_special") return "SPECIAL";
  return to_upper(s);  // tv/ova/movie/special/ona/music/pv/cm: AniList's spellings.
}

std::optional<std::string> map_source(std::string_view source) {
  const std::string s = to_lower(source);
  if (s.empty()) return std::nullopt;
  if (s == "4_koma_manga" || s == "web_manga" || s == "digital_manga") return "MANGA";
  if (s == "game") return "VIDEO_GAME";
  if (s == "mixed_media") return "MULTIMEDIA_PROJECT";
  return to_upper(s);  // original/manga/light_novel/visual_novel/novel/other/… already match.
}

std::optional<std::uint32_t> score_from_mean(double mean) {
  if (!(mean > 0.0) || mean > 10.0) return std::nullopt;
  return static_cast<std::uint32_t>(std::lround(mean * 10.0));
}

std::optional<std::uint32_t> minutes_from_seconds(std::int64_t secs) {
  if (secs <= 0) return std::nullopt;
  const std::int64_t minutes = (secs + 30) / 60;
  if (minutes <= 0) return std::nullopt;
  return static_cast<std::uint32_t>(minutes);
}

std::optional<std::string> clean_synopsis(std::string_view raw) {
  static constexpr std::string_view kTrailer = "[Written by MAL Rewrite]";
  std::string text(raw);
  if (const auto pos = text.rfind(kTrailer); pos != std::string::npos) {
    text.erase(pos);
  }
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.empty()) return std::nullopt;
  return strip_controls(out);
}

}  // namespace detail

}  // namespace shigoku::mal_catalog
