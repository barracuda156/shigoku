// weebcentral.cpp — implements weebcentral.hpp: string-scan parsers over the
// site's HTMX fragments (the anime scraper idiom — no HTML lib, the
// committed fixtures are the contract) plus a small blocking transport. The
// generic pieces (url_encode, filename_safe, chapter_less) are shared with
// the MangaDex source — they live in mangadex.hpp's detail namespace and
// chapter_less is already documented as the cross-source chapter order.

#include "weebcentral.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <thread>

#include "mangadex.hpp"

namespace shigoku::manga {

namespace wc {

namespace {

// The scan primitives (anibd.cpp idiom, fragment-shaped): everything is a
// find + slice over string_views into the response; nothing allocates until
// a field is actually kept.

std::optional<std::string_view> after(std::string_view hay, std::string_view marker) {
  const auto at = hay.find(marker);
  if (at == std::string_view::npos) return std::nullopt;
  return hay.substr(at + marker.size());
}

std::optional<std::string_view> until(std::string_view hay, char stop) {
  const auto at = hay.find(stop);
  if (at == std::string_view::npos) return std::nullopt;
  return hay.substr(0, at);
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

// Tag text content: the marker's trailing text up to the next '<', trimmed
// and entity-unescaped; "" when the marker or the closing tag never comes.
std::string text_after(std::string_view hay, std::string_view marker) {
  const auto rest = after(hay, marker);
  if (!rest.has_value()) return {};
  const auto txt = until(*rest, '<');
  if (!txt.has_value()) return {};
  return html_unescape(trim(*txt));
}

// "&#39;" / "&#x27;" → codepoint. Nullopt on anything malformed or hostile
// (0, > U+10FFFF, overlong digit runs) — the caller passes the raw text
// through untouched instead.
std::optional<std::uint32_t> parse_numeric_entity(std::string_view ent) {
  std::string_view digits = ent.substr(1);  // past '#'.
  std::uint32_t base = 10;
  if (!digits.empty() && (digits.front() == 'x' || digits.front() == 'X')) {
    base = 16;
    digits.remove_prefix(1);
  }
  if (digits.empty() || digits.size() > 7) return std::nullopt;
  std::uint32_t v = 0;
  for (const char c : digits) {
    std::uint32_t d = 0;
    if (c >= '0' && c <= '9') {
      d = static_cast<std::uint32_t>(c - '0');
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      d = static_cast<std::uint32_t>(c - 'a') + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      d = static_cast<std::uint32_t>(c - 'A') + 10;
    } else {
      return std::nullopt;
    }
    v = v * base + d;
    if (v > 0x10FFFF) return std::nullopt;
  }
  if (v == 0) return std::nullopt;
  return v;
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}  // namespace

bool ulid_shaped(std::string_view s) {
  if (s.size() != 26) return false;
  for (const char c : s) {
    const bool digit = c >= '0' && c <= '9';
    const bool upper = c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' &&
                       c != 'O' && c != 'U';
    if (!digit && !upper) return false;
  }
  return true;
}

std::string html_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') {
      out += s[i++];
      continue;
    }
    const auto semi = s.find(';', i);
    // No terminator nearby → a bare ampersand, not an entity.
    if (semi == std::string_view::npos || semi - i > 10) {
      out += s[i++];
      continue;
    }
    const std::string_view ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") {
      out += '&';
    } else if (ent == "lt") {
      out += '<';
    } else if (ent == "gt") {
      out += '>';
    } else if (ent == "quot") {
      out += '"';
    } else if (ent == "apos") {
      out += '\'';
    } else if (!ent.empty() && ent.front() == '#') {
      const auto cp = parse_numeric_entity(ent);
      if (!cp.has_value()) {
        out += s[i++];
        continue;
      }
      append_utf8(out, *cp);
    } else {
      // Unknown named entity passes through verbatim.
      out += s[i++];
      continue;
    }
    i = semi + 1;
  }
  return out;
}

bool looks_like_challenge(std::string_view body) {
  // Cloudflare interstitials plus known parked/redirect-shell markers. The
  // haystack is lowercased by hand (ASCII only — no locale surprises).
  std::string lower(body);
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  }
  static constexpr const char* kMarkers[] = {
      "<title>just a moment",           // Cloudflare JS challenge
      "cf-browser-verification",        // Cloudflare (legacy marker)
      "challenge-platform",             // Cloudflare challenge script path
      "<title>redirecting...</title>",  // known redirect shell
      "adblockingdetected",             // known ad-wall shell
      "parklogic",                      // known parked-domain marker
  };
  for (const char* m : kMarkers) {
    if (lower.find(m) != std::string::npos) return true;
  }
  return false;
}

std::string search_url(std::string_view host, std::string_view query,
                       std::uint32_t limit) {
  std::string url(host);
  url += "/search/data?limit=";
  url += std::to_string(limit);
  url += "&offset=0&text=";
  url += detail::url_encode(query);
  url += "&sort=Best+Match&order=Ascending&official=Any&display_mode=Full+Display";
  return url;
}

std::string chapter_list_url(std::string_view host, std::string_view series_id) {
  std::string url(host);
  url += "/series/";
  url += series_id;
  url += "/full-chapter-list";
  return url;
}

std::string images_url(std::string_view host, std::string_view chapter_id) {
  std::string url(host);
  url += "/chapters/";
  url += chapter_id;
  url += "/images?is_prev=False&current_page=1&reading_style=long_strip";
  return url;
}

std::string cover_url(std::string_view cover_host, std::string_view series_id) {
  std::string url(cover_host);
  url += "/cover/fallback/";
  url += series_id;
  url += ".jpg";
  return url;
}

Result<std::vector<MdManga>, ProviderError> parse_search(std::string_view raw) {
  static constexpr std::string_view kBlock = "<article class=\"bg-base-300";
  std::vector<MdManga> out;
  std::size_t pos = raw.find(kBlock);
  while (pos != std::string_view::npos) {
    const std::size_t next = raw.find(kBlock, pos + kBlock.size());
    const std::string_view block =
        raw.substr(pos, (next == std::string_view::npos ? raw.size() : next) - pos);
    pos = next;

    // id: the ULID segment of the first /series/{ULID}/{slug} link. The vet
    // is the row's admission ticket — it becomes a URL path segment and the
    // library dir's [id8] anchor.
    const auto rest = after(block, "/series/");
    if (!rest.has_value()) continue;
    const auto id_end = rest->find_first_of("/\"");
    if (id_end == std::string_view::npos) continue;
    const std::string_view id = rest->substr(0, id_end);
    if (!ulid_shaped(id)) continue;

    MdManga m;
    m.id = std::string(id);
    // Title: the desktop-layout anchor, else the cover-overlay div (the two
    // spellings the fragment carries; both hold the same text).
    m.title = text_after(block, "class=\"line-clamp-1 link link-hover\">");
    if (m.title.empty()) {
      if (const auto div = after(block, "class=\"text-ellipsis truncate")) {
        if (const auto tail = after(*div, ">")) {
          const auto txt = until(*tail, '<');
          if (txt.has_value()) m.title = html_unescape(trim(*txt));
        }
      }
    }
    if (const auto y = after(block, "<strong>Year:</strong>")) {
      const std::string ytext = text_after(*y, "<span>");
      if (!ytext.empty()) {
        char* end = nullptr;
        const long v = std::strtol(ytext.c_str(), &end, 10);
        if (end != ytext.c_str() && *end == '\0') m.year = static_cast<int>(v);
      }
    }
    if (const auto st = after(block, "<strong>Status:</strong>")) {
      m.status = text_after(*st, "<span>");
    }
    // description/cover_filename stay "", al/mal stay nullopt (WeebCentral
    // offers none of them).
    out.push_back(std::move(m));
  }
  return out;
}

Result<std::vector<MdChapter>, ProviderError> parse_chapters(std::string_view raw) {
  static constexpr std::string_view kAnchor = "href=\"/chapters/";
  static constexpr std::string_view kChapterPrefix = "Chapter ";
  std::vector<MdChapter> out;
  std::size_t pos = raw.find(kAnchor);
  while (pos != std::string_view::npos) {
    const std::size_t next = raw.find(kAnchor, pos + kAnchor.size());
    const std::string_view block =
        raw.substr(pos, (next == std::string_view::npos ? raw.size() : next) - pos);
    pos = next;

    const auto rest = after(block, kAnchor);
    const auto id = rest.has_value() ? until(*rest, '"') : std::nullopt;
    if (!id.has_value() || !ulid_shaped(*id)) continue;

    MdChapter c;
    c.id = std::string(*id);
    c.lang = "en";  // EN-only site; no per-row language exists.
    // "Chapter {n}" → chapter = the raw remainder; any other label (specials,
    // oneshots) rides `title` with chapter = "" — chapter_less already sorts
    // those after the numbered run.
    const std::string label = text_after(block, "<span class=\"\">");
    if (label.size() > kChapterPrefix.size() &&
        label.compare(0, kChapterPrefix.size(), kChapterPrefix) == 0) {
      c.chapter = label.substr(kChapterPrefix.size());
    } else {
      c.title = label;
    }
    if (const auto dt = after(block, "datetime=\"")) {
      const auto v = until(*dt, '"');
      if (v.has_value()) c.publish_at = std::string(*v);
    }
    out.push_back(std::move(c));
  }
  // The site lists newest-first; reading order is ours. publish_at then id
  // break ties so unnumbered specials order chronologically, always
  // deterministically.
  std::sort(out.begin(), out.end(), [](const MdChapter& a, const MdChapter& b) {
    if (detail::chapter_less(a.chapter, b.chapter)) return true;
    if (detail::chapter_less(b.chapter, a.chapter)) return false;
    if (a.publish_at != b.publish_at) return a.publish_at < b.publish_at;
    return a.id < b.id;
  });
  return out;
}

Result<PageSet, ProviderError> parse_images(std::string_view raw) {
  PageSet set;
  std::size_t pos = raw.find("<img");
  while (pos != std::string_view::npos) {
    const std::size_t close = raw.find('>', pos);
    const std::string_view tag =
        raw.substr(pos, (close == std::string_view::npos ? raw.size() : close) - pos);
    pos = raw.find("<img", pos + 4);

    const auto src = after(tag, "src=\"");
    const auto url = src.has_value() ? until(*src, '"') : std::nullopt;
    if (!url.has_value()) continue;
    // Absolute http(s) only: page images always are (varying per-series
    // hosts); relative srcs are site chrome (badges, placeholders).
    if (url->rfind("http://", 0) != 0 && url->rfind("https://", 0) != 0) continue;

    std::string_view path = *url;
    if (const auto q = path.find('?'); q != std::string_view::npos) {
      path = path.substr(0, q);
    }
    const auto slash = path.rfind('/');
    const std::string_view hint =
        slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    // The hint names the file on disk (pages.cpp page_filename): a vet
    // failure fails the WHOLE chapter — a page gap is worse than a failed
    // chapter (the parse_at_home posture).
    if (!detail::filename_safe(hint)) {
      return err(ProviderError::decode("unsafe page filename in fragment"));
    }
    set.units.push_back(PageUnit{std::string(*url), kWcHost, std::string(hint)});
  }
  if (set.units.empty()) {
    return err(ProviderError::decode("no pages in chapter fragment"));
  }
  return set;
}

}  // namespace wc

// ---------------------------------------------------------------------------
// Transport.
// ---------------------------------------------------------------------------

Result<WeebCentral, ProviderError> WeebCentral::create() { return with_host(kWcHost); }

Result<WeebCentral, ProviderError> WeebCentral::with_host(std::string host) {
  while (!host.empty() && host.back() == '/') host.pop_back();
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return WeebCentral(std::move(*client), std::move(host));
}

Result<std::vector<std::uint8_t>, ProviderError> WeebCentral::get(
    const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.accept = http::Accept::Any2xx;
  auto resp = http_.fetch(req);
  if (!resp.has_value() && resp.error().kind == ProviderError::Kind::RateLimited) {
    // The MangaDex politeness floor, kept for the scrape source too.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    resp = http_.fetch(req);
  }
  if (resp.has_value()) {
    const std::string_view body(reinterpret_cast<const char*>(resp->data()),
                                resp->size());
    if (wc::looks_like_challenge(body)) {
      // A 2xx challenge shell, not the site. Forbidden = "WeebCentral
      // blocked us" in the UI, and the flag to route this source through the
      // libcurl-impersonate fingerprint seam. status 0: the block wasn't an
      // HTTP status.
      return err(ProviderError{ProviderError::Kind::Forbidden, 0, "challenge page"});
    }
  }
  return resp;
}

Result<std::vector<MdManga>, ProviderError> WeebCentral::search(
    std::string_view query) const {
  const std::string url = wc::search_url(host_, query, kWcSearchLimit);
  auto body = get(url);
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  return wc::parse_search(text);
}

Result<std::vector<MdChapter>, ProviderError> WeebCentral::chapters(
    std::string_view manga_id, std::string_view /*lang: EN-only site*/) const {
  // Ids may later arrive from persisted store space rather than a fresh
  // parse; a corrupt one must not shape a request path (mangadex trusts its
  // parse-time uuid vet; a scrape site's path splice earns the extra check).
  if (!wc::ulid_shaped(manga_id)) {
    return err(ProviderError::decode("bad series id"));
  }
  const std::string url = wc::chapter_list_url(host_, manga_id);
  auto body = get(url);
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  return wc::parse_chapters(text);
}

Result<PageSet, ProviderError> WeebCentral::pages(
    std::string_view chapter_id, bool /*data_saver: one quality only*/) const {
  if (!wc::ulid_shaped(chapter_id)) {
    return err(ProviderError::decode("bad chapter id"));
  }
  const std::string url = wc::images_url(host_, chapter_id);
  auto body = get(url);
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  return wc::parse_images(text);
}

std::string WeebCentral::cover_thumb_url(const MdManga& m) const {
  if (!wc::ulid_shaped(m.id)) return {};
  return wc::cover_url(kWcCoverHost, m.id);
}

}  // namespace shigoku::manga
