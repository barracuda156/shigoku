// anilist.cpp — P3 (search) + P17 (discover) + P19/P20 (auth/sync) + P21
// (by-id enrichment). Ported from sabigoku src/anilist.rs: MEDIA_FIELDS,
// {search,discover,by_id,viewer,list}_query/_body, GqlMedia -> Enrichment
// mapping, classify_{page,by_id,viewer,list,entry,save}.
//
// P21 widened the shared media->Enrichment mapper to the full MEDIA_FIELDS set
// (06 §8b: one fieldset, card vs detail is UI-side selection) — duration,
// startDate, source, countryOfOrigin, studios, rankings (ROD-261 contextual
// rank), nextAiringEpisode, description. search/discover ride the same mapper,
// so their goldens carry these fields now too.

#include "anilist.hpp"

#include <algorithm>
#include <ctime>
#include <iterator>

#include <nlohmann/json.hpp>

namespace shigoku::anilist {

namespace {

using json = nlohmann::json;

// Shared selection set (anilist.rs MEDIA_FIELDS, :26) so by-id, search, and
// discover never drift (06 §8b: one fieldset; card vs detail is UI-side
// selection). Byte-identical to the Rust document.
constexpr const char* kMediaFields =
    "id idMal title{romaji english native} episodes duration averageScore "
    "status season seasonYear startDate{year month day} format source "
    "countryOfOrigin genres studios(isMain:true){nodes{name}} "
    "rankings{rank type year allTime} nextAiringEpisode{episode airingAt} "
    "description(asHtml:false) coverImage{large}";

// strip_controls over an optional JSON string field; absent/null -> nullopt.
// Blank-after-strip is preserved as-is (Enrichment fields don't coalesce
// blank-to-absent except title_romaji, matched explicitly below).
std::optional<std::string> strip_opt(const json& j, const char* key) {
  if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
  const auto& v = j.at(key);
  if (!v.is_string()) return std::nullopt;
  return strip_controls(v.get<std::string>());
}

template <class T>
std::optional<T> num_opt(const json& j, const char* key) {
  if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
  const auto& v = j.at(key);
  if (!v.is_number()) return std::nullopt;
  return v.get<T>();
}

// `description(asHtml:false)` still carries tags and entities (anilist.rs
// sanitize_description, :321): strip tags, decode the common entities, collapse
// newline/tab runs to one space, trim. Byte-identical to the Rust entity table
// and state machine.
std::string sanitize_description(std::string_view raw) {
  static constexpr std::pair<std::string_view, std::string_view> kEntities[] = {
      {"&amp;", "&"},  {"&quot;", "\""}, {"&#039;", "'"}, {"&lt;", "<"},
      {"&gt;", ">"},   {"&mdash;", "--"}, {"&ndash;", "-"},
  };
  std::string out;
  out.reserve(raw.size());
  bool in_tag = false;
  std::size_t i = 0;
  while (i < raw.size()) {
    const char c = raw[i];
    if (in_tag) {
      if (c == '>') in_tag = false;
      ++i;
      continue;
    }
    if (c == '<') {
      in_tag = true;
      ++i;
    } else if (c == '&') {
      const std::string_view rest = raw.substr(i);
      const auto* hit = std::find_if(std::begin(kEntities), std::end(kEntities),
                                     [&](const auto& pr) { return rest.rfind(pr.first, 0) == 0; });
      if (hit != std::end(kEntities)) {
        out.append(hit->second);
        i += hit->first.size();
      } else {
        out.push_back(c);
        ++i;
      }
    } else if (c == '\n' || c == '\r' || c == '\t') {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      ++i;
    } else {
      out.push_back(c);
      ++i;
    }
  }
  // trim_matches(' '): leading/trailing spaces only (Rust trims the space char).
  const std::size_t lo = out.find_first_not_of(' ');
  if (lo == std::string::npos) return std::string{};
  const std::size_t hi = out.find_last_not_of(' ');
  return out.substr(lo, hi - lo + 1);
}

struct SelectedRank {
  std::uint32_t rank = 0;
  std::optional<std::string> kind;
  std::optional<std::uint32_t> year;
};

// Best ranking (anilist.rs select_rank, ROD-261 §5.3a): contextual over
// all-time; within a tier RATED over POPULAR. Strict-greater keeps the first on
// ties. Iterates the wire array in order so the tie-break is stable.
std::optional<SelectedRank> select_rank(const json& rankings) {
  auto score = [](const json& r) -> int {
    int s = 0;
    if (!r.value("allTime", false)) s += 2;
    if (r.contains("type") && r.at("type").is_string() && r.at("type") == "RATED") s += 1;
    return s;
  };
  const json* best = nullptr;
  for (const auto& r : rankings) {
    if (!r.is_object()) continue;
    if (best == nullptr || score(r) > score(*best)) best = &r;
  }
  if (best == nullptr) return std::nullopt;
  SelectedRank sel;
  sel.rank = best->value("rank", std::uint32_t{0});
  sel.kind = strip_opt(*best, "type");
  sel.year = best->value("allTime", false) ? std::nullopt : num_opt<std::uint32_t>(*best, "year");
  return sel;
}

// media_to_enrichment (anilist.rs:373), full MEDIA_FIELDS mapping.
Enrichment media_to_enrichment(const json& m) {
  Enrichment e;
  e.anilist_id = m.value("id", std::int64_t{0});
  e.mal_id = num_opt<std::int64_t>(m, "idMal");

  const json empty_title = json::object();
  const json& title = m.contains("title") && m.at("title").is_object() ? m.at("title") : empty_title;
  // Blank romaji is absence (anilist.rs:383 comment) -> default-constructed "".
  e.title_romaji = strip_opt(title, "romaji").value_or(std::string{});
  e.title_english = strip_opt(title, "english");
  e.title_native = strip_opt(title, "native");

  if (m.contains("coverImage") && m.at("coverImage").is_object()) {
    e.cover_url = strip_opt(m.at("coverImage"), "large");
  }

  e.total_episodes = num_opt<std::uint32_t>(m, "episodes");
  e.duration_minutes = num_opt<std::uint32_t>(m, "duration");
  e.year = num_opt<std::uint32_t>(m, "seasonYear");
  if (const auto season_str = strip_opt(m, "season"); season_str.has_value()) {
    e.season = parse_season(*season_str);
  }
  e.status = strip_opt(m, "status");
  if (const auto desc = strip_opt(m, "description"); desc.has_value()) {
    e.description = strip_controls(sanitize_description(*desc));
  }
  e.score = num_opt<std::uint32_t>(m, "averageScore");
  e.kind = strip_opt(m, "format");

  // startDate maps to Date only when the year is present (a null year drops the
  // whole date, matching the Rust `sd.year.map(...)`).
  if (m.contains("startDate") && m.at("startDate").is_object()) {
    const json& sd = m.at("startDate");
    if (const auto year = num_opt<std::uint32_t>(sd, "year"); year.has_value()) {
      Date d;
      d.year = *year;
      d.month = num_opt<std::uint32_t>(sd, "month");
      d.day = num_opt<std::uint32_t>(sd, "day");
      e.start_date = d;
    }
  }

  if (m.contains("genres") && m.at("genres").is_array()) {
    for (const auto& g : m.at("genres")) {
      if (g.is_string()) e.genres.push_back(strip_controls(g.get<std::string>()));
    }
  }
  // studios(isMain:true){nodes{name}}: filter_map drops null names.
  if (m.contains("studios") && m.at("studios").is_object()) {
    const json& st = m.at("studios");
    if (st.contains("nodes") && st.at("nodes").is_array()) {
      for (const auto& n : st.at("nodes")) {
        if (n.is_object() && n.contains("name") && n.at("name").is_string()) {
          e.studios.push_back(strip_controls(n.at("name").get<std::string>()));
        }
      }
    }
  }
  e.source_material = strip_opt(m, "source");
  e.country = strip_opt(m, "countryOfOrigin");

  if (m.contains("rankings") && m.at("rankings").is_array()) {
    if (const auto sel = select_rank(m.at("rankings")); sel.has_value()) {
      e.rank = sel->rank;
      e.rank_type = sel->kind;
      e.rank_year = sel->year;
    }
  }

  if (m.contains("nextAiringEpisode") && m.at("nextAiringEpisode").is_object()) {
    const json& na = m.at("nextAiringEpisode");
    e.next_airing_at = num_opt<std::int64_t>(na, "airingAt");
    e.next_airing_episode = num_opt<std::uint32_t>(na, "episode");
  }
  return e;
}

// --- sync mappers (P20, 06 §5) ----------------------------------------------

// AniList MediaListStatus -> domain (anilist.rs list_status_from_anilist).
// REPEATING folds to Watching at ingest so the merge never sees it;
// unknown/absent -> Planning (06 §5.4).
ListStatus list_status_from_anilist(std::optional<std::string_view> s) {
  if (!s.has_value()) return ListStatus::Planning;
  if (*s == "CURRENT" || *s == "REPEATING") return ListStatus::Watching;
  if (*s == "PLANNING") return ListStatus::Planning;
  if (*s == "PAUSED") return ListStatus::Paused;
  if (*s == "COMPLETED") return ListStatus::Completed;
  if (*s == "DROPPED") return ListStatus::Dropped;
  return ListStatus::Planning;
}

// Domain -> AniList MediaListStatus for the push mutation (06 §5.3).
const char* list_status_to_anilist(ListStatus s) {
  switch (s) {
    case ListStatus::Watching:  return "CURRENT";
    case ListStatus::Planning:  return "PLANNING";
    case ListStatus::Paused:    return "PAUSED";
    case ListStatus::Completed: return "COMPLETED";
    case ListStatus::Dropped:   return "DROPPED";
  }
  return "PLANNING";  // unreachable (closed enum).
}

// Sparse enrichment for auto-importing a list-only show (06 O3): title +
// episodes only, control-stripped like every render-surface string. The rest
// backfills through the TTL enrichment repull.
Enrichment list_import_seed(std::int64_t media_id, const json& m) {
  Enrichment e;
  e.anilist_id = media_id;
  const json empty_title = json::object();
  const json& title = m.contains("title") && m.at("title").is_object() ? m.at("title") : empty_title;
  e.title_romaji = strip_opt(title, "romaji").value_or(std::string{});
  e.title_english = strip_opt(title, "english");
  e.title_native = strip_opt(title, "native");
  e.total_episodes = num_opt<std::uint32_t>(m, "episodes");
  return e;
}

}  // namespace

namespace detail {

namespace {

// The MediaType enum spelling every query document scopes itself with.
const char* type_gql(MediaKind kind) {
  switch (kind) {
    case MediaKind::Anime: return "ANIME";
    case MediaKind::Manga: return "MANGA";
  }
  return "ANIME";  // unreachable (closed enum).
}

}  // namespace

std::string media_fields(MediaKind kind) {
  // `chapters volumes` are the manga counterparts of `episodes`; asking for
  // them on an anime query would be answered with nulls, so they ride only
  // the manga fieldset and the anime document stays byte-identical.
  switch (kind) {
    case MediaKind::Anime: return kMediaFields;
    case MediaKind::Manga: return std::string(kMediaFields) + " chapters volumes";
  }
  return kMediaFields;  // unreachable (closed enum).
}

std::string search_query(MediaKind kind) {
  return std::string(
             "query($search:String!,$perPage:Int!,$page:Int!){Page(page:$page,"
             "perPage:$perPage){pageInfo{hasNextPage} "
             "media(search:$search,type:") +
         type_gql(kind) + ",sort:SEARCH_MATCH){" + media_fields(kind) + "}}}";
}

std::string search_body(std::string_view query_text, std::uint32_t page, MediaKind kind) {
  json body = {
      {"query", search_query(kind)},
      {"variables",
       {{"search", std::string(query_text)}, {"perPage", kSearchPageSize}, {"page", page}}},
  };
  return body.dump();
}

namespace {

// Secondary sort key stabilizes page order under primary ties (anilist.rs
// sort_keys, ROD-334 §9.6). Returned as a JSON array of MediaSort enum names.
json sort_keys(DiscoverAxis axis) {
  switch (axis) {
    case DiscoverAxis::Trending:
      return json::array({"TRENDING_DESC", "POPULARITY_DESC"});
    case DiscoverAxis::Popular:
    case DiscoverAxis::ThisSeason:
      return json::array({"POPULARITY_DESC", "ID_DESC"});
    case DiscoverAxis::TopRated:
      return json::array({"SCORE_DESC", "ID_DESC"});
  }
  return json::array();  // unreachable (closed enum).
}

// AniList MediaSeason spelling (anilist.rs season_gql).
const char* season_gql(Season s) {
  switch (s) {
    case Season::Winter: return "WINTER";
    case Season::Spring: return "SPRING";
    case Season::Summer: return "SUMMER";
    case Season::Fall:   return "FALL";
  }
  return "WINTER";  // unreachable (closed enum).
}

}  // namespace

std::string discover_query(MediaKind kind) {
  // P38: $genre_in/$status/$averageScore_greater are always declared (unlike
  // $season/$seasonYear, which even the query doc keeps conditional-free —
  // GraphQL allows a declared-but-unbound-per-call variable to simply go
  // unused when the caller omits it from `variables`, same as season/
  // seasonYear already do off This Season).
  return std::string(
             "query($page:Int!,$perPage:Int!,$sort:[MediaSort],$season:"
             "MediaSeason,$seasonYear:Int,$genre_in:[String],$status:"
             "MediaStatus,$averageScore_greater:Int){Page(page:$page,perPage:"
             "$perPage){pageInfo{hasNextPage} media(type:") +
         type_gql(kind) +
         ",sort:$sort,"
         "season:$season,seasonYear:$seasonYear,genre_in:$genre_in,"
         "status:$status,averageScore_greater:$averageScore_greater){" +
         media_fields(kind) + "}}}";
}

std::string discover_body(DiscoverAxis axis, std::uint32_t page, std::int64_t unix_secs,
                          const DiscoverFilters& filters, MediaKind kind) {
  json vars = {
      {"page", page},
      {"perPage", kDiscoverPageSize},
      {"sort", sort_keys(axis)},
  };
  // season/seasonYear OMITTED off This Season: an explicit null would filter
  // season==null; omitted leaves the GraphQL arg unset (06 §8b serde-parity).
  if (axis == DiscoverAxis::ThisSeason) {
    const Cour c = current_cour(unix_secs);
    vars["season"] = season_gql(c.season);
    vars["seasonYear"] = c.year;
  }
  // P38 filter vars: each OMITTED (never explicit null) when inactive, the
  // same serde-parity discipline as season/seasonYear above. `year` binds
  // seasonYear directly — off This Season that arg is otherwise unset, and
  // on This Season an explicit filter year simply overrides the cour's own
  // binding (last write wins; both write the same key).
  if (!filters.genres.empty()) {
    vars["genre_in"] = filters.genres;
  }
  if (filters.year.has_value()) {
    vars["seasonYear"] = *filters.year;
  }
  if (filters.status.has_value()) {
    vars["status"] = *filters.status;
  }
  if (filters.min_score.has_value()) {
    vars["averageScore_greater"] = *filters.min_score;
  }
  json body = {{"query", discover_query(kind)}, {"variables", vars}};
  return body.dump();
}

Result<CatalogPage, ProviderError> classify_page(std::string_view raw_json) {
  json resp;
  try {
    resp = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }

  if (!resp.is_object() || !resp.contains("data") || resp.at("data").is_null()) {
    return err(ProviderError::decode("data is null"));
  }
  const json& data = resp.at("data");

  CatalogPage page;
  if (!data.contains("Page") || !data.at("Page").is_object()) {
    // Missing Page = exhausted (classify_page doc comment): empty, no error.
    return page;
  }
  const json& gql_page = data.at("Page");

  if (gql_page.contains("media") && gql_page.at("media").is_array()) {
    for (const auto& m : gql_page.at("media")) {
      if (m.is_object()) page.entries.push_back(media_to_enrichment(m));
    }
  }

  page.has_next = gql_page.contains("pageInfo") && gql_page.at("pageInfo").is_object() &&
                  gql_page.at("pageInfo").value("hasNextPage", false);
  return page;
}

std::string by_id_query(MediaKind kind) {
  // Deterministic join when the AniList id is known (anilist.rs by_id_query,
  // ROD-181); no title match.
  return std::string("query($id:Int!){Media(id:$id,type:") + type_gql(kind) + "){" +
         media_fields(kind) + "}}";
}

std::string by_id_body(std::int64_t anilist_id, MediaKind kind) {
  json body = {{"query", by_id_query(kind)}, {"variables", {{"id", anilist_id}}}};
  return body.dump();
}

Result<std::optional<Enrichment>, ProviderError> classify_by_id(std::string_view raw_json) {
  json resp;
  try {
    resp = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }
  // data:null / garbage = no answer (Err). Media:null = confirmed no-match
  // (Ok(nullopt)); a present Media object = Ok(some) (anilist.rs classify_by_id,
  // :454). The three-state gate the 05 §8 refresh law rests on.
  if (!resp.is_object() || !resp.contains("data") || resp.at("data").is_null()) {
    return err(ProviderError::decode("data is null"));
  }
  const json& data = resp.at("data");
  if (!data.contains("Media") || !data.at("Media").is_object()) {
    return std::optional<Enrichment>(std::nullopt);  // confirmed no-match.
  }
  return std::optional<Enrichment>(media_to_enrichment(data.at("Media")));
}

// Characters page 1 (~12): role + name + first Japanese VA, if staffed
// (voiceActors filtered server-side to JAPANESE so v1 never has to pick a
// dub locale). Recommendations (~10, sorted by rating so the best-matched
// shows lead) ride kMediaFields via mediaRecommendation — the same fieldset
// by-id/search/discover use (06 §8b), so a recommendation row is already a
// full Enrichment ready for catalog_cache.
std::string characters_recs_query(MediaKind kind) {
  return std::string("query($id:Int!){Media(id:$id,type:") + type_gql(kind) +
         "){"
         "characters(page:1,perPage:12,sort:[ROLE,RELEVANCE]){edges{"
         "role node{name{full}} voiceActors(language:JAPANESE){name{full}}"
         "}} "
         "recommendations(perPage:10,sort:RATING_DESC){nodes{"
         // `type` rides alongside the shared fieldset (not inside it — the
         // fieldset's other queries are already MediaType-scoped):
         // cross-type recs (anime->manga/novel) exist, and a MANGA row
         // would zoom into a detail whose resolve/play machinery can
         // never serve it (P36 review).
         "mediaRecommendation{type " +
         media_fields(kind) + "}}}}}";
}

std::string characters_recs_body(std::int64_t anilist_id, MediaKind kind) {
  json body = {{"query", characters_recs_query(kind)}, {"variables", {{"id", anilist_id}}}};
  return body.dump();
}

// character edge -> CharacterEntry. A missing/blank node name is skipped (no
// usable row); role/VA best-effort ("" / nullopt on absence, matching
// strip_opt's own coalescing elsewhere in this file).
std::optional<CharacterEntry> edge_to_character(const json& edge) {
  if (!edge.is_object() || !edge.contains("node") || !edge.at("node").is_object()) {
    return std::nullopt;
  }
  std::string full_name;
  if (edge.at("node").contains("name") && edge.at("node").at("name").is_object()) {
    full_name = strip_opt(edge.at("node").at("name"), "full").value_or(std::string{});
  }
  if (full_name.empty()) return std::nullopt;

  CharacterEntry c;
  c.name = full_name;
  c.role = strip_opt(edge, "role").value_or(std::string{});
  if (edge.contains("voiceActors") && edge.at("voiceActors").is_array() &&
      !edge.at("voiceActors").empty()) {
    const json& va = edge.at("voiceActors").front();
    if (va.is_object() && va.contains("name") && va.at("name").is_object()) {
      c.va_name = strip_opt(va.at("name"), "full");
    }
  }
  return c;
}

Result<std::optional<CharactersAndRecommendations>, ProviderError> classify_characters_recs(
    std::string_view raw_json, MediaKind kind) {
  json resp;
  try {
    resp = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }
  // Same three-state gate as classify_by_id: data:null/garbage = no answer
  // (Err); Media:null = confirmed no-match (Ok(nullopt)). A present Media
  // with empty characters/recommendations arrays is still Ok(some) — a quiet
  // show is an answer, not a failure.
  if (!resp.is_object() || !resp.contains("data") || resp.at("data").is_null()) {
    return err(ProviderError::decode("data is null"));
  }
  const json& data = resp.at("data");
  if (!data.contains("Media") || !data.at("Media").is_object()) {
    return std::optional<CharactersAndRecommendations>(std::nullopt);
  }
  const json& m = data.at("Media");

  CharactersAndRecommendations out;
  if (m.contains("characters") && m.at("characters").is_object()) {
    const json& ch = m.at("characters");
    if (ch.contains("edges") && ch.at("edges").is_array()) {
      for (const auto& edge : ch.at("edges")) {
        if (auto c = edge_to_character(edge); c.has_value()) out.characters.push_back(*c);
      }
    }
  }
  if (m.contains("recommendations") && m.at("recommendations").is_object()) {
    const json& recs = m.at("recommendations");
    if (recs.contains("nodes") && recs.at("nodes").is_array()) {
      for (const auto& node : recs.at("nodes")) {
        if (!node.is_object() || !node.contains("mediaRecommendation") ||
            !node.at("mediaRecommendation").is_object()) {
          continue;  // a null mediaRecommendation is a deleted/private show.
        }
        const json& rec = node.at("mediaRecommendation");
        // Cross-type recs (anime->manga/novel) are dropped: only rows of the
        // queried kind can ever be opened. Absent `type` (older cached
        // answers) is kept — skip only on a positive wrong-type signal.
        if (const auto type = strip_opt(rec, "type");
            type.has_value() && *type != type_gql(kind)) {
          continue;
        }
        out.recommendations.push_back(media_to_enrichment(rec));
      }
    }
  }
  return std::optional<CharactersAndRecommendations>(std::move(out));
}

// P38 (§9 — no Rust precedent): the full genre vocabulary for the Discover
// filter overlay's genre picker. A bare argument-free query, same no-variable
// shape as viewer_body below.
std::string genre_collection_query() { return "query{GenreCollection}"; }

std::string genre_collection_body() {
  json body = {{"query", genre_collection_query()}};
  return body.dump();
}

Result<std::vector<std::string>, ProviderError> classify_genre_collection(
    std::string_view raw_json) {
  json resp;
  try {
    resp = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }
  if (!resp.is_object() || !resp.contains("data") || resp.at("data").is_null()) {
    return err(ProviderError::decode("data is null"));
  }
  const json& data = resp.at("data");
  if (!data.contains("GenreCollection") || !data.at("GenreCollection").is_array()) {
    return err(ProviderError::decode("GenreCollection missing"));
  }
  std::vector<std::string> out;
  for (const auto& g : data.at("GenreCollection")) {
    if (g.is_string()) out.push_back(g.get<std::string>());
  }
  return out;
}

std::string viewer_body() {
  json body = {{"query", "query{Viewer{id name mediaListOptions{scoreFormat}}}"}};
  return body.dump();
}

Result<std::optional<Viewer>, ProviderError> classify_viewer(std::string_view raw_json) {
  json resp;
  try {
    resp = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }

  if (!resp.is_object() || !resp.contains("data") || resp.at("data").is_null()) {
    return err(ProviderError::decode("data is null"));
  }
  const json& data = resp.at("data");

  if (!data.contains("Viewer") || data.at("Viewer").is_null()) {
    return std::optional<Viewer>(std::nullopt);  // confirmed rejection.
  }
  const json& v = data.at("Viewer");
  if (!v.is_object()) return std::optional<Viewer>(std::nullopt);

  Viewer out;
  out.id = v.value("id", std::int64_t{0});
  if (v.contains("name") && v.at("name").is_string()) {
    out.name = strip_controls(v.at("name").get<std::string>());
  }
  if (v.contains("mediaListOptions") && v.at("mediaListOptions").is_object()) {
    const json& opts = v.at("mediaListOptions");
    if (opts.contains("scoreFormat") && opts.at("scoreFormat").is_string()) {
      out.score_format = parse_score_format(opts.at("scoreFormat").get<std::string>());
    }
  }
  return std::optional<Viewer>(std::move(out));
}

// --- AniList sync bodies/classifiers (P20, 06 §5) --------------------------

std::string list_collection_body(std::int64_t user_id, MediaKind kind) {
  json body = {
      {"query", std::string("query($userId:Int!){MediaListCollection(userId:$userId,type:") +
                    type_gql(kind) +
                    "){lists{entries{"
                    "mediaId status progress score updatedAt media{title{romaji english native} "
                    "episodes}}}}}"},
      {"variables", {{"userId", user_id}}},
  };
  return body.dump();
}

std::string list_entry_body(std::int64_t user_id, std::int64_t media_id, MediaKind kind) {
  json body = {
      {"query",
       std::string("query($userId:Int!,$mediaId:Int!){Page(perPage:1){mediaList(userId:$userId,"
                   "mediaId:$mediaId,type:") +
           type_gql(kind) + "){status progress score}}}"},
      {"variables", {{"userId", user_id}, {"mediaId", media_id}}},
  };
  return body.dump();
}

std::string save_entry_body(std::int64_t media_id, ListStatus status, std::uint32_t progress,
                            double score) {
  json body = {
      {"query",
       "mutation($mediaId:Int!,$status:MediaListStatus!,$progress:Int!,$score:Float!){"
       "SaveMediaListEntry(mediaId:$mediaId,status:$status,progress:$progress,score:$score)"
       "{id}}"},
      {"variables",
       {{"mediaId", media_id},
        {"status", list_status_to_anilist(status)},
        {"progress", progress},
        {"score", score}}},
  };
  return body.dump();
}

Result<std::vector<RemoteEntry>, ProviderError> classify_list(std::string_view raw_json,
                                                               ScoreFormat format) {
  json resp;
  try {
    resp = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }
  if (!resp.is_object() || !resp.contains("data") || resp.at("data").is_null()) {
    return err(ProviderError::decode("data is null"));
  }
  const json& data = resp.at("data");

  std::vector<RemoteEntry> out;
  if (!data.contains("MediaListCollection") || !data.at("MediaListCollection").is_object()) {
    return out;  // null collection: an empty list, not an error.
  }
  const json& coll = data.at("MediaListCollection");
  if (!coll.contains("lists") || !coll.at("lists").is_array()) return out;

  for (const auto& group : coll.at("lists")) {
    if (!group.is_object() || !group.contains("entries") || !group.at("entries").is_array()) {
      continue;
    }
    for (const auto& e : group.at("entries")) {
      if (!e.is_object()) continue;
      RemoteEntry re;
      re.anilist_id = e.value("mediaId", std::int64_t{0});
      const std::optional<std::string> status =
          e.contains("status") && e.at("status").is_string()
              ? std::optional<std::string>(e.at("status").get<std::string>())
              : std::nullopt;
      re.status = list_status_from_anilist(
          status.has_value() ? std::optional<std::string_view>(*status) : std::nullopt);
      re.progress = e.value("progress", std::uint32_t{0});
      re.score = from_anilist_score(e.value("score", 0.0), format);
      re.updated_at = e.contains("updatedAt") && !e.at("updatedAt").is_null()
                          ? e.at("updatedAt").get<std::int64_t>()
                          : 0;
      if (e.contains("media") && e.at("media").is_object()) {
        re.import_seed = list_import_seed(re.anilist_id, e.at("media"));
      }
      out.push_back(std::move(re));
    }
  }
  return out;
}

Result<std::optional<ListEntry>, ProviderError> classify_entry(std::string_view raw_json,
                                                                ScoreFormat format) {
  json resp;
  try {
    resp = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }
  if (!resp.is_object()) return err(ProviderError::decode("malformed response"));
  // Read here and nowhere else: a partial GraphQL failure nulls the errored
  // field, rides HTTP 200, and reports itself only in this array. Every other
  // classifier can treat that null as "absent" harmlessly; for the push
  // guard, absent means "write", so it has to tell the two apart.
  if (resp.contains("errors") && resp.at("errors").is_array() && !resp.at("errors").empty()) {
    return err(ProviderError::decode("entry read reported errors"));
  }
  if (!resp.contains("data") || resp.at("data").is_null()) {
    return err(ProviderError::decode("data is null"));
  }
  const json& data = resp.at("data");
  if (!data.contains("Page") || data.at("Page").is_null()) {
    return err(ProviderError::decode("Page is null"));
  }
  const json& page = data.at("Page");
  if (!page.is_object() || !page.contains("mediaList") || page.at("mediaList").is_null()) {
    return err(ProviderError::decode("mediaList is null"));
  }
  const json& list = page.at("mediaList");
  if (!list.is_array()) return err(ProviderError::decode("mediaList is null"));
  if (list.empty()) {
    return std::optional<ListEntry>(std::nullopt);
  }
  const json& e = list.front();
  const std::optional<std::string> status =
      e.is_object() && e.contains("status") && e.at("status").is_string()
          ? std::optional<std::string>(e.at("status").get<std::string>())
          : std::nullopt;
  const ListStatus ls = list_status_from_anilist(
      status.has_value() ? std::optional<std::string_view>(*status) : std::nullopt);
  const std::uint32_t progress =
      e.is_object() ? e.value("progress", std::uint32_t{0}) : std::uint32_t{0};
  const double wire_score = e.is_object() ? e.value("score", 0.0) : 0.0;
  return std::optional<ListEntry>(ListEntry{ls, progress, from_anilist_score(wire_score, format)});
}

Result<std::int64_t, ProviderError> classify_save(std::string_view raw_json) {
  json resp;
  try {
    resp = json::parse(raw_json.begin(), raw_json.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(e.what()));
  }
  if (!resp.is_object() || !resp.contains("data") || resp.at("data").is_null()) {
    return err(ProviderError::decode("data is null"));
  }
  const json& data = resp.at("data");
  if (!data.contains("SaveMediaListEntry") || data.at("SaveMediaListEntry").is_null()) {
    return err(ProviderError::decode("SaveMediaListEntry.id missing"));
  }
  const json& entry = data.at("SaveMediaListEntry");
  if (!entry.is_object() || !entry.contains("id") || entry.at("id").is_null()) {
    return err(ProviderError::decode("SaveMediaListEntry.id missing"));
  }
  return entry.at("id").get<std::int64_t>();
}

}  // namespace detail

Result<std::optional<Viewer>, ProviderError> viewer(const http::Client& client,
                                                     std::string_view token) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body = detail::viewer_body();
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer " + std::string(token)});

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_viewer(raw);
}

Result<CatalogPage, ProviderError> search(const http::Client& client, std::string_view query,
                                           std::uint32_t page, MediaKind kind) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body = detail::search_body(query, page, kind);
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_page(raw);
}

Result<CatalogPage, ProviderError> discover(const http::Client& client, DiscoverAxis axis,
                                            std::uint32_t page, const DiscoverFilters& filters,
                                            MediaKind kind) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body = detail::discover_body(
      axis, page, static_cast<std::int64_t>(::time(nullptr)), filters, kind);
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_page(raw);
}

Result<std::vector<std::string>, ProviderError> genre_collection(const http::Client& client) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body = detail::genre_collection_body();
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_genre_collection(raw);
}

Result<std::optional<Enrichment>, ProviderError> enrich(const http::Client& client,
                                                        std::int64_t anilist_id, MediaKind kind) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body = detail::by_id_body(anilist_id, kind);
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_by_id(raw);
}

Result<std::optional<CharactersAndRecommendations>, ProviderError> characters_and_recommendations(
    const http::Client& client, std::int64_t anilist_id, MediaKind kind) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body = detail::characters_recs_body(anilist_id, kind);
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_characters_recs(raw, kind);
}

// --- AniList sync (P20, 06 §5) -----------------------------------------------

Result<std::vector<RemoteEntry>, ProviderError> pull_list(const http::Client& client,
                                                           std::string_view token,
                                                           std::int64_t user_id,
                                                           ScoreFormat format, MediaKind kind) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body = detail::list_collection_body(user_id, kind);
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer " + std::string(token)});

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_list(raw, format);
}

Result<std::optional<ListEntry>, ProviderError> pull_entry(const http::Client& client,
                                                            std::string_view token,
                                                            std::int64_t user_id,
                                                            std::int64_t media_id,
                                                            ScoreFormat format, MediaKind kind) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body = detail::list_entry_body(user_id, media_id, kind);
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer " + std::string(token)});

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_entry(raw, format);
}

Result<std::int64_t, ProviderError> push_entry(const http::Client& client,
                                                std::string_view token, std::int64_t media_id,
                                                ListStatus status, std::uint32_t progress,
                                                std::uint32_t score, ScoreFormat format) {
  http::Request req;
  req.method = http::Method::Post;
  req.url = kEndpoint;
  req.content_type = "application/json";
  const std::string body =
      detail::save_entry_body(media_id, status, progress, to_anilist_score(score, format));
  req.body.assign(body.begin(), body.end());
  req.accept = http::Accept::Any2xx;
  req.extra_headers.push_back(http::Header{"Authorization", "Bearer " + std::string(token)});

  auto resp = client.fetch(req);
  if (!resp.has_value()) return err(resp.error());

  const std::string_view raw(reinterpret_cast<const char*>(resp->data()), resp->size());
  return detail::classify_save(raw);
}

}  // namespace shigoku::anilist
