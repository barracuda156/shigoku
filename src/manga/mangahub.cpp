// mangahub.cpp — implements mangahub.hpp.

#include "mangahub.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "mangadex.hpp"

namespace shigoku::manga {

namespace mh {

namespace {

using json = nlohmann::json;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_lower_alnum(char c) { return (c >= 'a' && c <= 'z') || is_digit(c); }

bool path_chars_ok(std::string_view s) {
  if (s.empty() || s.size() > kMhPathMax || s.front() == '/') return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) ||
                    c == '.' || c == '_' || c == '-' || c == '/';
    if (!ok) return false;
  }
  return s.find("//") == std::string_view::npos && s.find("..") == std::string_view::npos;
}

std::string str_or_empty(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key)) return {};
  const json& v = j.at(key);
  return v.is_string() ? v.get<std::string>() : std::string();
}

// The API prints chapter numbers as JSON numbers: 12, 93.5. A float that
// happens to be whole ("12.0") is spelled like the integer.
std::string number_text(const json& v) {
  if (v.is_number_unsigned()) return std::to_string(v.get<std::uint64_t>());
  if (v.is_number_integer()) return std::to_string(v.get<std::int64_t>());
  if (v.is_number_float()) {
    std::string s = v.dump();
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
  }
  if (v.is_string()) return v.get<std::string>();
  return {};
}

bool date_shaped(std::string_view s) {
  if (s.empty() || s.size() > 40) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return is_digit(c) || c == '-' || c == ':' || c == 'T' || c == '.' || c == 'Z' ||
           c == '+';
  });
}

std::string body_of(const std::string& query, json variables) {
  json body;
  body["query"] = query;
  body["variables"] = std::move(variables);
  return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string first_error(const json& j) {
  if (!j.contains("errors") || !j.at("errors").is_array() || j.at("errors").empty()) return {};
  return str_or_empty(j.at("errors").front(), "message");
}

bool mentions_rate_limit(std::string_view msg) {
  std::string lower(msg);
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return lower.find("rate limit") != std::string::npos;
}

// The parsed envelope's data.<field>: RateLimited when the errors say so
// (the API throttles with a 200 and a null field), Decode when the body is
// not JSON or carries no data object (a schema error), Http 404 when the
// field is null (an unknown slug or chapter).
Result<json, ProviderError> data_field(std::string_view raw, const char* field) {
  json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode(std::string(field) + ": invalid json"));
  }
  const std::string msg = first_error(j);
  if (mentions_rate_limit(msg)) return err(ProviderError::rate_limited());
  if (!j.contains("data") || !j.at("data").is_object()) {
    std::string why = std::string(field) + ": no data";
    if (!msg.empty()) why += " (" + msg + ")";
    return err(ProviderError::decode(std::move(why)));
  }
  const json& data = j.at("data");
  if (!data.contains(field) || data.at(field).is_null()) {
    return err(ProviderError::http(404));
  }
  return data.at(field);
}

}  // namespace

bool slug_shaped(std::string_view s) {
  if (s.empty() || s.size() > kMhSlugMax) return false;
  if (!is_lower_alnum(s.front()) || !is_lower_alnum(s.back())) return false;
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return is_lower_alnum(c) || c == '-' || c == '_'; });
}

bool number_shaped(std::string_view s) {
  if (s.empty() || s.size() > kMhNumberMax) return false;
  const auto dot = s.find('.');
  const std::string_view whole = s.substr(0, dot);
  if (whole.empty() || !std::all_of(whole.begin(), whole.end(), is_digit)) return false;
  if (dot == std::string_view::npos) return true;
  const std::string_view frac = s.substr(dot + 1);
  return !frac.empty() && std::all_of(frac.begin(), frac.end(), is_digit);
}

bool prefix_shaped(std::string_view s) {
  return path_chars_ok(s) && s.back() == '/';
}

bool cover_shaped(std::string_view s) {
  if (!path_chars_ok(s) || s.back() == '/') return false;
  const auto slash = s.rfind('/');
  return detail::filename_safe(slash == std::string_view::npos ? s : s.substr(slash + 1));
}

std::string chapter_id(std::string_view slug, std::string_view number) {
  std::string id(slug);
  id += '_';
  id += number;
  return id;
}

std::optional<std::pair<std::string, std::string>> split_chapter_id(std::string_view id) {
  const auto at = id.rfind('_');
  if (at == std::string_view::npos) return std::nullopt;
  const std::string_view slug = id.substr(0, at);
  const std::string_view number = id.substr(at + 1);
  if (!slug_shaped(slug) || !number_shaped(number)) return std::nullopt;
  return std::make_pair(std::string(slug), std::string(number));
}

std::string chapter_title(std::string_view title, std::string_view number) {
  const auto b = title.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  title = title.substr(b, title.find_last_not_of(" \t\r\n") - b + 1);

  std::string folded;
  bool space = false;
  for (const char c : title) {
    if (c == ' ' || c == '\t') {
      space = true;
      continue;
    }
    if (space && !folded.empty()) folded += ' ';
    space = false;
    folded += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  if (folded == "chapter " + std::string(number)) return {};
  return std::string(title);
}

std::string search_body(std::string_view query, std::uint32_t limit) {
  return body_of("query($q:String!){search(x:m01,q:$q,limit:" + std::to_string(limit) +
                     "){rows{title,slug,image,status,isSafe}}}",
                 json{{"q", std::string(query)}});
}

std::string manga_body(std::string_view slug) {
  return body_of("query($s:String!){manga(x:m01,slug:$s){chapters{number,title,date}}}",
                 json{{"s", std::string(slug)}});
}

std::string chapter_body(std::string_view slug, std::string_view number) {
  // number_shaped text is a valid JSON number literal; nlohmann's parser is
  // locale-independent, unlike strtod.
  json n = json::parse(number, nullptr, /*allow_exceptions=*/false);
  return body_of("query($s:String!,$n:Float!){chapter(x:m01,slug:$s,number:$n){pages}}",
                 json{{"s", std::string(slug)}, {"n", std::move(n)}});
}

std::string page_url(std::string_view image_host, std::string_view prefix,
                     std::string_view file) {
  std::string url(image_host);
  url += '/';
  url += prefix;
  url += file;
  return url;
}

std::string thumb_url(std::string_view thumb_host, std::string_view image) {
  std::string url(thumb_host);
  url += '/';
  url += image;
  return url;
}

Result<std::vector<MdManga>, ProviderError> parse_search(std::string_view raw,
                                                         bool allow_unsafe) {
  auto search = data_field(raw, "search");
  if (!search.has_value()) {
    if (search.error().kind == ProviderError::Kind::Http) {
      return err(ProviderError::decode("search: null"));
    }
    return err(search.error());
  }
  if (!search->is_object() || !search->contains("rows") || !search->at("rows").is_array()) {
    return err(ProviderError::decode("search: missing rows[]"));
  }

  std::vector<MdManga> out;
  for (const json& row : search->at("rows")) {
    if (!row.is_object()) continue;
    const std::string slug = str_or_empty(row, "slug");
    if (!slug_shaped(slug)) continue;
    const bool safe = row.contains("isSafe") && row.at("isSafe").is_boolean() &&
                      row.at("isSafe").get<bool>();
    if (!safe && !allow_unsafe) continue;
    if (std::any_of(out.begin(), out.end(), [&](const MdManga& m) { return m.id == slug; })) {
      continue;
    }
    MdManga m;
    m.id = slug;
    m.title = str_or_empty(row, "title");
    if (m.title.empty()) m.title = slug;
    m.status = str_or_empty(row, "status");
    const std::string image = str_or_empty(row, "image");
    if (cover_shaped(image)) m.cover_filename = image;
    out.push_back(std::move(m));
  }
  return out;
}

Result<std::vector<MdChapter>, ProviderError> parse_manga(std::string_view raw,
                                                          std::string_view slug) {
  auto manga = data_field(raw, "manga");
  if (!manga.has_value()) return err(manga.error());
  if (!manga->is_object() || !manga->contains("chapters") ||
      !manga->at("chapters").is_array()) {
    return err(ProviderError::decode("manga: missing chapters[]"));
  }

  std::vector<MdChapter> out;
  for (const json& row : manga->at("chapters")) {
    if (!row.is_object() || !row.contains("number")) continue;
    const std::string number = number_text(row.at("number"));
    if (!number_shaped(number)) continue;
    MdChapter c;
    c.id = chapter_id(slug, number);
    c.chapter = number;
    c.title = chapter_title(str_or_empty(row, "title"), number);
    c.lang = "en";
    const std::string date = str_or_empty(row, "date");
    if (date_shaped(date)) c.publish_at = date;
    out.push_back(std::move(c));
  }
  return detail::dedupe_chapters(std::move(out));
}

Result<PageSet, ProviderError> parse_chapter(std::string_view raw,
                                             std::string_view image_host) {
  auto chapter = data_field(raw, "chapter");
  if (!chapter.has_value()) return err(chapter.error());
  const std::string pages_text = str_or_empty(*chapter, "pages");
  if (pages_text.empty()) return err(ProviderError::decode("chapter: no pages"));
  const json pages = json::parse(pages_text, nullptr, /*allow_exceptions=*/false);
  if (pages.is_discarded() || !pages.is_object()) {
    return err(ProviderError::decode("chapter: pages is not json"));
  }
  const std::string prefix = str_or_empty(pages, "p");
  if (!prefix_shaped(prefix)) return err(ProviderError::decode("chapter: unsafe prefix"));
  if (!pages.contains("i") || !pages.at("i").is_array() || pages.at("i").empty()) {
    return err(ProviderError::decode("chapter: no images"));
  }

  PageSet set;
  for (const json& v : pages.at("i")) {
    const std::string file = v.is_string() ? v.get<std::string>() : std::string();
    if (!detail::filename_safe(file)) {
      return err(ProviderError::decode("chapter: unsafe image name"));
    }
    set.units.push_back(PageUnit{page_url(image_host, prefix, file), kMhReferer, file});
  }
  return set;
}

}  // namespace mh

Result<MangaHub, ProviderError> MangaHub::create(bool allow_unsafe) {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return MangaHub(std::move(*client), kMhApi, http::Fingerprint::Chrome, allow_unsafe);
}

Result<MangaHub, ProviderError> MangaHub::with_endpoint(std::string api, bool allow_unsafe) {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return MangaHub(std::move(*client), std::move(api), http::Fingerprint::None, allow_unsafe);
}

Result<std::vector<std::uint8_t>, ProviderError> MangaHub::post(std::string body) const {
  http::Request req;
  req.method = http::Method::Post;
  req.url = api_;
  req.content_type = "application/json";
  req.body.assign(body.begin(), body.end());
  req.user_agent = kUserAgent;
  // No Origin/Referer: requests claiming the site's origin without its
  // mhub_access key are throttled within a handful of calls.
  req.extra_headers.push_back({"Accept", "application/json"});
  req.accept = http::Accept::Any2xx;
  req.fingerprint = fingerprint_;
  auto resp = http_.fetch(req);
  if (!resp.has_value() && resp.error().kind == ProviderError::Kind::RateLimited) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    resp = http_.fetch(req);
  }
  return resp;
}

Result<std::vector<MdManga>, ProviderError> MangaHub::search(std::string_view query) const {
  auto body = post(mh::search_body(query, kMhSearchLimit));
  if (!body.has_value()) return err(body.error());
  return mh::parse_search(
      std::string_view(reinterpret_cast<const char*>(body->data()), body->size()),
      allow_unsafe_);
}

Result<std::vector<MdChapter>, ProviderError> MangaHub::chapters(
    std::string_view manga_id, std::string_view /*lang*/) const {
  if (!mh::slug_shaped(manga_id)) return err(ProviderError::decode("bad manga id"));
  auto body = post(mh::manga_body(manga_id));
  if (!body.has_value()) return err(body.error());
  return mh::parse_manga(
      std::string_view(reinterpret_cast<const char*>(body->data()), body->size()), manga_id);
}

Result<PageSet, ProviderError> MangaHub::pages(std::string_view chapter_id,
                                               bool /*data_saver*/) const {
  const auto parts = mh::split_chapter_id(chapter_id);
  if (!parts.has_value()) return err(ProviderError::decode("bad chapter id"));
  auto body = post(mh::chapter_body(parts->first, parts->second));
  if (!body.has_value()) return err(body.error());
  return mh::parse_chapter(
      std::string_view(reinterpret_cast<const char*>(body->data()), body->size()),
      kMhImageHost);
}

std::string MangaHub::cover_thumb_url(const MdManga& m) const {
  if (!mh::cover_shaped(m.cover_filename)) return {};
  return mh::thumb_url(kMhThumbHost, m.cover_filename);
}

}  // namespace shigoku::manga
