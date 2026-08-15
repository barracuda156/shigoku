// mangadex.cpp — implements mangadex.hpp: pure URL builders + parsers over
// api.mangadex.org's documented JSON (no auth to read), plus a small
// blocking transport (worker-thread use, A1 shape).
//
// nlohmann parse is non-throwing everywhere (allow_exceptions=false,
// is_discarded() gate); every field read past that point is guarded by an
// is_*() check before .get<T>() — a hostile/malformed body degrades a row or
// a field, it never throws past this file (same posture as anilist.cpp /
// megaplay.cpp's DTO walks).

#include "mangadex.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <nlohmann/json.hpp>

namespace shigoku::manga {

namespace {

using json = nlohmann::json;

// num_opt (anilist.cpp idiom): absent/null/wrong-typed -> nullopt.
template <class T>
std::optional<T> num_opt(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || j.at(key).is_null()) return std::nullopt;
  const json& v = j.at(key);
  if (!v.is_number()) return std::nullopt;
  return v.get<T>();
}

// Absent/null/wrong-typed string field degrades to "" (never throws, never
// drops the row over it — only id-shape and the external-chapter rule drop
// rows in this file).
std::string str_or_empty(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || j.at(key).is_null()) return {};
  const json& v = j.at(key);
  if (!v.is_string()) return {};
  return v.get<std::string>();
}

std::uint32_t uint_or(const json& j, const char* key, std::uint32_t def) {
  if (!j.is_object() || !j.contains(key) || j.at(key).is_null()) return def;
  const json& v = j.at(key);
  if (!v.is_number()) return def;
  return v.get<std::uint32_t>();
}

bool bool_or(const json& j, const char* key, bool def) {
  if (!j.is_object() || !j.contains(key) || j.at(key).is_null()) return def;
  const json& v = j.at(key);
  if (!v.is_boolean()) return def;
  return v.get<bool>();
}

// Title rule (pinned, fixture captured from a live response): title.en,
// else title["ja-ro"], else the first entry, else "". "First entry" walks
// whatever order nlohmann's object type iterates in (insertion order is NOT
// preserved by the default json — this project doesn't use ordered_json);
// only reached when neither en nor ja-ro is a string, a case the fixture
// never exercises (single-key ja-ro-only or en-present rows only).
std::string pick_title(const json& attrs) {
  if (!attrs.is_object() || !attrs.contains("title") || !attrs.at("title").is_object()) {
    return {};
  }
  const json& title = attrs.at("title");
  if (title.contains("en") && title.at("en").is_string()) {
    return title.at("en").get<std::string>();
  }
  if (title.contains("ja-ro") && title.at("ja-ro").is_string()) {
    return title.at("ja-ro").get<std::string>();
  }
  for (const auto& item : title.items()) {
    if (item.value().is_string()) return item.value().get<std::string>();
  }
  return {};
}

std::string pick_description(const json& attrs) {
  if (!attrs.is_object() || !attrs.contains("description") || !attrs.at("description").is_object()) {
    return {};
  }
  return str_or_empty(attrs.at("description"), "en");
}

// The one relationships[] entry with type=="cover_art" (present because
// search_url sends includes[]=cover_art). Missing rel, missing attributes,
// or a fileName failing filename_safe() -> "".
std::string pick_cover(const json& row) {
  if (!row.is_object() || !row.contains("relationships") || !row.at("relationships").is_array()) {
    return {};
  }
  for (const auto& rel : row.at("relationships")) {
    if (!rel.is_object()) continue;
    if (!rel.contains("type") || !rel.at("type").is_string()) continue;
    if (rel.at("type").get<std::string>() != "cover_art") continue;
    if (!rel.contains("attributes") || !rel.at("attributes").is_object()) return {};
    const std::string fname = str_or_empty(rel.at("attributes"), "fileName");
    if (!detail::filename_safe(fname)) return {};
    return fname;
  }
  return {};
}

// links.al/.mal are STRINGS in the API; the whole string must be digits
// (strtoll semantics, reject "12a") -> absent/malformed never fails the row,
// it just yields nullopt.
std::optional<std::int64_t> parse_link_id(const json& links, const char* key) {
  if (!links.is_object() || !links.contains(key) || !links.at(key).is_string()) {
    return std::nullopt;
  }
  const std::string s = links.at(key).get<std::string>();
  if (s.empty()) return std::nullopt;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
  }
  errno = 0;
  char* endptr = nullptr;
  const long long v = std::strtoll(s.c_str(), &endptr, 10);
  if (endptr != s.c_str() + s.size() || errno == ERANGE) return std::nullopt;
  return static_cast<std::int64_t>(v);
}

}  // namespace

namespace detail {

std::string url_encode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '.' || c == '_' || c == '~' || c == '-') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::string search_url(std::string_view host, std::string_view query, std::uint32_t limit) {
  std::string out;
  out.append(host);
  out.append("/manga?title=");
  out.append(url_encode(query));
  out.append("&limit=");
  out.append(std::to_string(limit));
  out.append("&includes%5B%5D=cover_art");
  return out;
}

std::string feed_url(std::string_view host, std::string_view manga_id, std::string_view lang,
                     std::uint32_t limit, std::uint32_t offset) {
  std::string out;
  out.append(host);
  out.append("/manga/");
  out.append(manga_id);
  out.append("/feed?translatedLanguage%5B%5D=");
  out.append(url_encode(lang));
  out.append("&order%5Bchapter%5D=asc&limit=");
  out.append(std::to_string(limit));
  out.append("&offset=");
  out.append(std::to_string(offset));
  return out;
}

std::string at_home_url(std::string_view host, std::string_view chapter_id) {
  std::string out;
  out.append(host);
  out.append("/at-home/server/");
  out.append(chapter_id);
  return out;
}

std::string cover_url(std::string_view uploads_host, std::string_view manga_id,
                      std::string_view filename) {
  std::string out;
  out.append(uploads_host);
  out.append("/covers/");
  out.append(manga_id);
  out.push_back('/');
  out.append(filename);
  out.append(".512.jpg");
  return out;
}

std::string page_url(const MdAtHome& at_home, std::size_t index, bool data_saver) {
  const std::vector<std::string>& files = data_saver ? at_home.data_saver : at_home.data;
  if (index >= files.size()) return {};
  std::string out;
  out.append(at_home.base_url);
  out.append(data_saver ? "/data-saver/" : "/data/");
  out.append(at_home.hash);
  out.push_back('/');
  out.append(files[index]);
  return out;
}

bool uuid_shaped(std::string_view s) {
  if (s.size() != 36) return false;
  for (std::size_t i = 0; i < 36; ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool filename_safe(std::string_view s) {
  if (s.empty()) return false;
  if (s.front() == '.') return false;
  for (const char c : s) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == '-')) {
      return false;
    }
  }
  if (s.find("..") != std::string_view::npos) return false;
  return true;
}

Result<std::vector<MdManga>, ProviderError> parse_search(std::string_view raw) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode("search: invalid json"));
  }
  if (!j.contains("data") || !j.at("data").is_array()) {
    return err(ProviderError::decode("search: missing data[]"));
  }

  const json empty_attrs = json::object();
  std::vector<MdManga> out;
  for (const auto& row : j.at("data")) {
    if (!row.is_object()) continue;
    if (!row.contains("id") || !row.at("id").is_string()) continue;
    const std::string id = row.at("id").get<std::string>();
    if (!uuid_shaped(id)) continue;

    const json& attrs = (row.contains("attributes") && row.at("attributes").is_object())
                            ? row.at("attributes")
                            : empty_attrs;

    MdManga m;
    m.id = id;
    m.title = pick_title(attrs);
    m.year = num_opt<int>(attrs, "year");
    m.status = str_or_empty(attrs, "status");
    m.description = pick_description(attrs);
    m.cover_filename = pick_cover(row);
    if (attrs.is_object() && attrs.contains("links") && attrs.at("links").is_object()) {
      const json& links = attrs.at("links");
      m.al_id = parse_link_id(links, "al");
      m.mal_id = parse_link_id(links, "mal");
    }
    out.push_back(std::move(m));
  }
  return out;
}

Result<MdFeedPage, ProviderError> parse_feed(std::string_view raw) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode("feed: invalid json"));
  }
  if (!j.contains("data") || !j.at("data").is_array()) {
    return err(ProviderError::decode("feed: missing data[]"));
  }

  MdFeedPage page;
  page.total = uint_or(j, "total", 0);

  const json empty_attrs = json::object();
  for (const auto& row : j.at("data")) {
    if (!row.is_object()) continue;
    if (!row.contains("id") || !row.at("id").is_string()) continue;
    const std::string id = row.at("id").get<std::string>();
    if (!uuid_shaped(id)) continue;

    const json& attrs = (row.contains("attributes") && row.at("attributes").is_object())
                            ? row.at("attributes")
                            : empty_attrs;

    // External chapters (publisher redirects, no at-home pages): externalUrl
    // non-null OR pages == 0 OR isUnavailable == true.
    const bool has_external = attrs.is_object() && attrs.contains("externalUrl") &&
                              !attrs.at("externalUrl").is_null();
    const std::uint32_t pages = uint_or(attrs, "pages", 0);
    const bool unavailable = bool_or(attrs, "isUnavailable", false);
    if (has_external || pages == 0 || unavailable) continue;

    MdChapter c;
    c.id = id;
    c.chapter = str_or_empty(attrs, "chapter");
    c.title = str_or_empty(attrs, "title");
    c.pages = pages;
    c.lang = str_or_empty(attrs, "translatedLanguage");
    c.publish_at = str_or_empty(attrs, "publishAt");
    page.chapters.push_back(std::move(c));
  }
  return page;
}

Result<MdAtHome, ProviderError> parse_at_home(std::string_view raw) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode("at_home: invalid json"));
  }
  if (!j.contains("baseUrl") || !j.at("baseUrl").is_string()) {
    return err(ProviderError::decode("at_home: missing baseUrl"));
  }
  if (!j.contains("chapter") || !j.at("chapter").is_object()) {
    return err(ProviderError::decode("at_home: missing chapter"));
  }
  const json& chapter = j.at("chapter");

  if (!chapter.contains("hash") || !chapter.at("hash").is_string()) {
    return err(ProviderError::decode("at_home: missing hash"));
  }
  const std::string hash = chapter.at("hash").get<std::string>();
  // hash becomes a URL segment (page_url) and a cache-dir segment; vet it
  // the same as a page filename.
  if (!filename_safe(hash)) return err(ProviderError::decode("at_home: bad hash"));

  if (!chapter.contains("data") || !chapter.at("data").is_array()) {
    return err(ProviderError::decode("at_home: missing data[]"));
  }
  if (chapter.at("data").empty()) return err(ProviderError::decode("at_home: empty data[]"));
  if (!chapter.contains("dataSaver") || !chapter.at("dataSaver").is_array()) {
    return err(ProviderError::decode("at_home: missing dataSaver[]"));
  }

  std::vector<std::string> data;
  for (const auto& f : chapter.at("data")) {
    if (!f.is_string()) return err(ProviderError::decode("at_home: bad data[] entry"));
    const std::string fname = f.get<std::string>();
    // A single bad filename fails the WHOLE parse (Decode) — a chapter with
    // page gaps is worse than a failed chapter.
    if (!filename_safe(fname)) return err(ProviderError::decode("at_home: bad data[] filename"));
    data.push_back(std::move(fname));
  }
  std::vector<std::string> data_saver;
  for (const auto& f : chapter.at("dataSaver")) {
    if (!f.is_string()) return err(ProviderError::decode("at_home: bad dataSaver[] entry"));
    const std::string fname = f.get<std::string>();
    if (!filename_safe(fname)) return err(ProviderError::decode("at_home: bad dataSaver[] filename"));
    data_saver.push_back(std::move(fname));
  }

  MdAtHome out;
  out.base_url = j.at("baseUrl").get<std::string>();
  out.hash = hash;
  out.data = std::move(data);
  out.data_saver = std::move(data_saver);
  return out;
}

namespace {

// Leading decimal via strtod on a null-terminated copy (from_chars<double>
// isn't reliably available across this tree's legacy toolchains). "Consumed
// at least one digit" is endptr != the buffer start, not merely != nullptr.
struct LeadingNumber {
  bool has_value = false;
  double value = 0.0;
};

LeadingNumber parse_leading_number(std::string_view s) {
  const std::string buf(s);
  char* endptr = nullptr;
  const double v = std::strtod(buf.c_str(), &endptr);
  LeadingNumber out;
  if (endptr != buf.c_str()) {
    out.has_value = true;
    out.value = v;
  }
  return out;
}

}  // namespace

bool chapter_less(std::string_view a, std::string_view b) {
  const LeadingNumber na = parse_leading_number(a);
  const LeadingNumber nb = parse_leading_number(b);
  if (na.has_value && nb.has_value) {
    if (na.value != nb.value) return na.value < nb.value;
    return a < b;  // tie -> lexicographic on the full string.
  }
  if (na.has_value != nb.has_value) {
    return na.has_value;  // numeric sorts before non-numeric.
  }
  return a < b;  // both non-numeric (incl. "") -> lexicographic.
}

std::vector<MdChapter> dedupe_chapters(std::vector<MdChapter> in) {
  // Group by exact chapter string, keeping the newest publish_at seen so far
  // (strict '>' so an equal-or-older duplicate never displaces the
  // first-seen incumbent — the tie rule).
  std::vector<MdChapter> kept;
  kept.reserve(in.size());
  for (auto& c : in) {
    auto it = std::find_if(kept.begin(), kept.end(),
                           [&](const MdChapter& k) { return k.chapter == c.chapter; });
    if (it == kept.end()) {
      kept.push_back(std::move(c));
    } else if (c.publish_at > it->publish_at) {
      *it = std::move(c);
    }
  }
  std::sort(kept.begin(), kept.end(), [](const MdChapter& x, const MdChapter& y) {
    if (chapter_less(x.chapter, y.chapter)) return true;
    if (chapter_less(y.chapter, x.chapter)) return false;
    return x.id < y.id;  // final tie-break for determinism.
  });
  return kept;
}

std::optional<std::uint32_t> next_offset(std::uint32_t offset, std::uint32_t limit,
                                         std::uint32_t total, std::uint32_t pages_fetched) {
  if (pages_fetched >= kMaxFeedPages) return std::nullopt;
  const std::uint64_t next = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(limit);
  if (next >= static_cast<std::uint64_t>(total)) return std::nullopt;
  return static_cast<std::uint32_t>(next);
}

}  // namespace detail

std::string scoped_id(std::string_view uuid) {
  std::string out;
  out.reserve(uuid.size() + 3);
  out.append("md:");
  out.append(uuid);
  return out;
}

// ---------------------------------------------------------------------------
// Transport.
// ---------------------------------------------------------------------------

Result<MangaDex, ProviderError> MangaDex::create() { return with_host(kApiHost); }

Result<MangaDex, ProviderError> MangaDex::with_host(std::string api_host) {
  // Trim a trailing '/' so the URL splices ("{host}/manga/...") stay
  // single-slash regardless of how the fixture server hands back its base
  // (the megaplay/senshi with_host pattern).
  while (!api_host.empty() && api_host.back() == '/') api_host.pop_back();
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return MangaDex(std::move(*client), std::move(api_host));
}

Result<std::vector<std::uint8_t>, ProviderError> MangaDex::get(const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.accept = http::Accept::Any2xx;
  auto resp = http_.fetch(req);
  if (!resp.has_value() && resp.error().kind == ProviderError::Kind::RateLimited) {
    // The politeness floor (~5 req/s): sleep 1s, retry the same request ONCE.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    resp = http_.fetch(req);
  }
  return resp;
}

Result<std::vector<MdManga>, ProviderError> MangaDex::search(std::string_view query,
                                                             std::uint32_t limit) const {
  const std::string url = detail::search_url(api_host_, query, limit);
  auto body = get(url);
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()), body->size());
  return detail::parse_search(text);
}

Result<std::vector<MdChapter>, ProviderError> MangaDex::chapters(std::string_view manga_id,
                                                                  std::string_view lang) const {
  std::vector<MdChapter> all;
  std::uint32_t offset = 0;
  std::uint32_t pages_fetched = 0;
  for (;;) {
    const std::string url = detail::feed_url(api_host_, manga_id, lang, kFeedLimit, offset);
    auto body = get(url);
    if (!body.has_value()) return err(body.error());
    const std::string_view text(reinterpret_cast<const char*>(body->data()), body->size());
    auto page = detail::parse_feed(text);
    if (!page.has_value()) return err(page.error());

    all.insert(all.end(), std::make_move_iterator(page->chapters.begin()),
              std::make_move_iterator(page->chapters.end()));
    ++pages_fetched;

    const auto next = detail::next_offset(offset, kFeedLimit, page->total, pages_fetched);
    if (!next.has_value()) break;
    offset = *next;
  }
  return detail::dedupe_chapters(std::move(all));
}

Result<MdAtHome, ProviderError> MangaDex::at_home(std::string_view chapter_id) const {
  const std::string url = detail::at_home_url(api_host_, chapter_id);
  auto body = get(url);
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()), body->size());
  return detail::parse_at_home(text);
}

Result<PageSet, ProviderError> MangaDex::pages(std::string_view chapter_id,
                                               bool data_saver) const {
  auto ah = at_home(chapter_id);
  if (!ah.has_value()) return err(ah.error());
  // Saver fallback: an empty dataSaver list must not brick the chapter
  // (moved here from the chapter-fetch core with the multi-source seam).
  const bool saver = data_saver && !ah->data_saver.empty();
  const std::vector<std::string>& files = saver ? ah->data_saver : ah->data;
  PageSet set;
  set.units.reserve(files.size());
  for (std::size_t i = 0; i < files.size(); ++i) {
    set.units.push_back(
        PageUnit{detail::page_url(*ah, i, saver), std::string(), files[i]});
  }
  return set;
}

std::string MangaDex::cover_thumb_url(const MdManga& m) const {
  if (m.cover_filename.empty()) return {};
  return detail::cover_url(kUploadsHost, m.id, m.cover_filename);
}

}  // namespace shigoku::manga
