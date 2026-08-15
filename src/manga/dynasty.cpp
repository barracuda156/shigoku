// dynasty.cpp — implements dynasty.hpp: two JSON parsers (the site's Rails
// .json endpoints) plus one string-scan HTML parser (search has no JSON
// twin), plus a small blocking transport.
//
// Shared helpers are borrowed, never re-implemented — the law this codebase
// already runs on ("the source that was born with a generic helper owns it"):
// url_encode / filename_safe / chapter_less live in mangadex.hpp's detail::,
// and html_unescape lives in weebcentral.hpp's wc:: (the search titles are
// Rails-escaped HTML — "&amp;" must not reach the UI). That is the only
// reason this file includes weebcentral.hpp; a third consumer would earn a
// shared scrape module instead.
//
// nlohmann parse is non-throwing (allow_exceptions=false + is_discarded()
// gate) and every field read is is_*()-guarded before .get<T>() — the
// mangadex.cpp posture: a hostile body degrades a field or drops a row, it
// never throws past this file.

#include "dynasty.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

#include <nlohmann/json.hpp>

#include "mangadex.hpp"
#include "weebcentral.hpp"

namespace shigoku::manga {

namespace dy {

namespace {

using json = nlohmann::json;

// Absent/null/wrong-typed string field degrades to "" (mangadex.cpp idiom).
std::string str_or_empty(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || j.at(key).is_null()) return {};
  const json& v = j.at(key);
  if (!v.is_string()) return {};
  return v.get<std::string>();
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\n' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\n' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "01" -> "1", "000" -> "0": the integer part's spelling must not decide
// chapter_less ties (it compares the full string when the values are equal).
std::string strip_leading_zeros(std::string_view digits) {
  std::size_t at = 0;
  while (at + 1 < digits.size() && digits[at] == '0') ++at;
  return std::string(digits.substr(at));
}

// The permalink's trailing "chNN(_M)*" run as a decimal number; "" when the
// tail carries anything else (a slug that merely contains "_ch", a "v2"
// suffix, …) — the caller falls back to the title.
std::string number_from_permalink(std::string_view permalink) {
  const auto at = permalink.rfind("_ch");
  if (at == std::string_view::npos) return {};
  std::string_view tail = permalink.substr(at + 3);
  std::size_t i = 0;
  while (i < tail.size() && is_digit(tail[i])) ++i;
  if (i == 0) return {};
  std::string out = strip_leading_zeros(tail.substr(0, i));
  while (i < tail.size() && tail[i] == '_') {
    const std::size_t start = ++i;
    while (i < tail.size() && is_digit(tail[i])) ++i;
    if (i == start) return {};  // "_" with no digits behind it: refuse.
    out += '.';
    out.append(tail.substr(start, i - start));
  }
  if (i != tail.size()) return {};  // trailing junk: refuse.
  return out;
}

// "Chapter 5.5: Chocolate and Kase-san" -> "5.5"; anything else -> "".
std::string number_from_title(std::string_view title) {
  static constexpr std::string_view kPrefix = "Chapter ";
  if (title.size() <= kPrefix.size() ||
      title.compare(0, kPrefix.size(), kPrefix) != 0) {
    return {};
  }
  std::string_view rest = title.substr(kPrefix.size());
  std::size_t i = 0;
  while (i < rest.size() && (is_digit(rest[i]) || rest[i] == '.')) ++i;
  std::string_view num = rest.substr(0, i);
  while (!num.empty() && num.back() == '.') num.remove_suffix(1);  // "5:" case.
  if (num.empty() || !is_digit(num.front())) return {};
  return std::string(num);
}

}  // namespace

bool slug_shaped(std::string_view s) {
  if (s.size() > kDySlugMax) return false;
  return detail::filename_safe(s);  // non-empty, [A-Za-z0-9._-], no "..".
}

std::string search_url(std::string_view host, std::string_view query) {
  std::string url(host);
  url += "/search?q=";
  url += detail::url_encode(query);
  url += "&classes%5B%5D=Series";
  return url;
}

std::string series_url(std::string_view host, std::string_view slug) {
  std::string url(host);
  url += "/series/";
  url += slug;
  url += ".json";
  return url;
}

std::string chapter_url(std::string_view host, std::string_view permalink) {
  std::string url(host);
  url += "/chapters/";
  url += permalink;
  url += ".json";
  return url;
}

std::string chapter_number(std::string_view permalink, std::string_view title) {
  std::string n = number_from_permalink(permalink);
  if (!n.empty()) return n;
  return number_from_title(title);
}

std::string chapter_title(std::string_view title) {
  static constexpr std::string_view kPrefix = "Chapter ";
  if (title.size() > kPrefix.size() &&
      title.compare(0, kPrefix.size(), kPrefix) == 0) {
    const auto colon = title.find(':');
    if (colon != std::string_view::npos) {
      return std::string(trim(title.substr(colon + 1)));
    }
  }
  return std::string(trim(title));
}

Result<std::vector<MdManga>, ProviderError> parse_search(std::string_view raw) {
  static constexpr std::string_view kLink = "href=\"/series/";
  std::vector<MdManga> out;
  std::size_t pos = raw.find(kLink);
  while (pos != std::string_view::npos) {
    const std::size_t slug_at = pos + kLink.size();
    pos = raw.find(kLink, slug_at);

    const std::string_view rest = raw.substr(slug_at);
    const auto quote = rest.find('"');
    if (quote == std::string_view::npos) continue;
    const std::string_view slug = rest.substr(0, quote);
    // The vet is the row's admission ticket: the slug becomes a URL path
    // segment and the library dir's [id8] anchor.
    if (!slug_shaped(slug)) continue;
    // One result per slug — the same series never earns two rows even if the
    // page ever links it twice.
    if (std::any_of(out.begin(), out.end(),
                    [&](const MdManga& m) { return m.id == slug; })) {
      continue;
    }

    // The anchor text: past the tag's '>' up to the closing tag.
    const auto gt = rest.find('>', quote);
    if (gt == std::string_view::npos) continue;
    const std::string_view after_gt = rest.substr(gt + 1);
    const auto lt = after_gt.find('<');
    if (lt == std::string_view::npos) continue;

    MdManga m;
    m.id = std::string(slug);
    m.title = wc::html_unescape(trim(after_gt.substr(0, lt)));
    // year/status/description/cover_filename stay empty, al/mal stay nullopt
    // (the search page carries none of them).
    out.push_back(std::move(m));
  }
  return out;
}

Result<std::vector<MdChapter>, ProviderError> parse_series(std::string_view raw) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode("series: invalid json"));
  }
  if (!j.contains("taggings") || !j.at("taggings").is_array()) {
    return err(ProviderError::decode("series: missing taggings[]"));
  }

  std::vector<MdChapter> out;
  for (const json& row : j.at("taggings")) {
    if (!row.is_object()) continue;
    // Volume markers ({"header": "Volume 1…"}, and the null-valued one the
    // fixture pins at the tail) carry no permalink — they are not chapters.
    const std::string permalink = str_or_empty(row, "permalink");
    if (permalink.empty() || !slug_shaped(permalink)) continue;

    const std::string title = str_or_empty(row, "title");
    MdChapter c;
    c.id = permalink;
    c.chapter = chapter_number(permalink, title);
    c.title = chapter_title(title);
    c.lang = "en";  // EN-only site; no per-row language exists.
    c.publish_at = str_or_empty(row, "released_on");
    // pages stays 0 — the chapter fetch is the page-count authority.
    out.push_back(std::move(c));
  }
  // The site lists in volume order (with the extras trailing); reading order
  // is ours. NO dedupe by chapter string: Dynasty reuses one number across
  // release variants and they are distinct chapters.
  std::sort(out.begin(), out.end(), [](const MdChapter& a, const MdChapter& b) {
    if (detail::chapter_less(a.chapter, b.chapter)) return true;
    if (detail::chapter_less(b.chapter, a.chapter)) return false;
    if (a.publish_at != b.publish_at) return a.publish_at < b.publish_at;
    return a.id < b.id;
  });
  return out;
}

Result<PageSet, ProviderError> parse_chapter(std::string_view raw,
                                             std::string_view host) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode("chapter: invalid json"));
  }
  if (!j.contains("pages") || !j.at("pages").is_array()) {
    return err(ProviderError::decode("chapter: missing pages[]"));
  }

  PageSet set;
  for (const json& row : j.at("pages")) {
    const std::string url = row.is_object() ? str_or_empty(row, "url") : std::string();
    // Every entry of pages[] IS a page — unlike a scraped <img> list there is
    // no chrome to skip here, so a malformed row fails the whole chapter.
    if (url.empty()) return err(ProviderError::decode("page with no url"));

    std::string absolute;
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
      absolute = url;  // never seen live; taken verbatim if it ever appears.
    } else if (url.front() == '/') {
      absolute = std::string(host);
      absolute += url;
    } else {
      return err(ProviderError::decode("page url is neither absolute nor rooted"));
    }

    std::string_view path(url);
    if (const auto q = path.find('?'); q != std::string_view::npos) {
      path = path.substr(0, q);
    }
    const auto slash = path.rfind('/');
    const std::string_view hint =
        slash == std::string_view::npos ? path : path.substr(slash + 1);
    // The hint names the file on disk (pages.cpp page_filename): a vet
    // failure fails the WHOLE chapter (the parse_at_home posture).
    if (!detail::filename_safe(hint)) {
      return err(ProviderError::decode("unsafe page filename"));
    }
    set.units.push_back(
        PageUnit{std::move(absolute), std::string(host), std::string(hint)});
  }
  if (set.units.empty()) {
    return err(ProviderError::decode("no pages in chapter"));
  }
  return set;
}

}  // namespace dy

// ---------------------------------------------------------------------------
// Transport.
// ---------------------------------------------------------------------------

Result<Dynasty, ProviderError> Dynasty::create() { return with_host(kDyHost); }

Result<Dynasty, ProviderError> Dynasty::with_host(std::string host) {
  while (!host.empty() && host.back() == '/') host.pop_back();
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return Dynasty(std::move(*client), std::move(host));
}

Result<std::vector<std::uint8_t>, ProviderError> Dynasty::get(
    const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.accept = http::Accept::Any2xx;
  auto resp = http_.fetch(req);
  if (!resp.has_value() && resp.error().kind == ProviderError::Kind::RateLimited) {
    // The MangaDex politeness floor, kept for every source.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    resp = http_.fetch(req);
  }
  return resp;
}

Result<std::vector<MdManga>, ProviderError> Dynasty::search(
    std::string_view query) const {
  const std::string url = dy::search_url(host_, query);
  auto body = get(url);
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  return dy::parse_search(text);
}

Result<std::vector<MdChapter>, ProviderError> Dynasty::chapters(
    std::string_view manga_id, std::string_view /*lang: EN-only site*/) const {
  // Ids may later arrive from persisted store space rather than a fresh
  // parse; a corrupt one must not shape a request path.
  if (!dy::slug_shaped(manga_id)) {
    return err(ProviderError::decode("bad series slug"));
  }
  const std::string url = dy::series_url(host_, manga_id);
  auto body = get(url);
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  return dy::parse_series(text);
}

Result<PageSet, ProviderError> Dynasty::pages(
    std::string_view chapter_id, bool /*data_saver: one quality only*/) const {
  if (!dy::slug_shaped(chapter_id)) {
    return err(ProviderError::decode("bad chapter permalink"));
  }
  const std::string url = dy::chapter_url(host_, chapter_id);
  auto body = get(url);
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  return dy::parse_chapter(text, host_);
}

std::string Dynasty::cover_thumb_url(const MdManga& /*m*/) const {
  return {};  // no cover in the search HTML, and no fetch here.
}

}  // namespace shigoku::manga
