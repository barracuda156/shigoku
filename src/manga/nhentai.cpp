// nhentai.cpp — implements nhentai.hpp: three JSON parsers over the v2
// FastAPI (search / cdn / gallery detail), the gallery≡oneshot adapter, and
// a blocking transport with the adaptive TLS-fingerprint posture (plain
// first, sticky Chrome escalation on 403 — the edge has been observed to
// fluctuate between probes, see the header).
//
// Shared helpers are borrowed, never re-implemented: url_encode /
// filename_safe live in mangadex.hpp's detail:: (the source born with a
// generic helper owns it). No HTML anywhere → no weebcentral.hpp dependency.
//
// nlohmann parse is non-throwing (allow_exceptions=false + is_discarded()
// gate) and every field read is is_*()-guarded before .get<T>() — the
// mangadex.cpp posture: a hostile body degrades a field or drops a row, it
// never throws past this file.

#include "nhentai.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>

#include <nlohmann/json.hpp>

#include "mangadex.hpp"

namespace shigoku::manga {

namespace nh {

namespace {

using json = nlohmann::json;

// Absent/null/wrong-typed string field degrades to "" (mangadex.cpp idiom).
std::string str_or_empty(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || j.at(key).is_null()) return {};
  const json& v = j.at(key);
  if (!v.is_string()) return {};
  return v.get<std::string>();
}

// Integer field as its decimal string; "" when absent/null/non-integral (a
// float id is nobody's id).
std::string int_as_string(const json& j, const char* key) {
  if (!j.is_object() || !j.contains(key)) return {};
  const json& v = j.at(key);
  if (!v.is_number_integer()) return {};
  return std::to_string(v.get<std::int64_t>());
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// The language-tag name → MdChapter.lang code map. Unknown names pass
// verbatim (a niche language beats a lie); "translated"/"rewrite" are
// release-property tags that coexist with the real language and never win.
std::string lang_code(std::string_view name) {
  if (name == "english") return "en";
  if (name == "japanese") return "ja";
  if (name == "chinese") return "zh";
  return std::string(name);
}

}  // namespace

bool id_shaped(std::string_view s) {
  if (s.empty() || s.size() > kNhIdMax) return false;
  return std::all_of(s.begin(), s.end(), is_digit);
}

bool path_shaped(std::string_view s) {
  if (s.empty() || s.size() > kNhPathMax) return false;
  if (s.front() == '/' || s.back() == '/') return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    is_digit(c) || c == '.' || c == '_' || c == '-' || c == '/';
    if (!ok) return false;
  }
  if (s.find("//") != std::string_view::npos) return false;
  if (s.find("..") != std::string_view::npos) return false;
  const auto slash = s.rfind('/');
  const std::string_view last =
      slash == std::string_view::npos ? s : s.substr(slash + 1);
  return detail::filename_safe(last);  // the on-disk filename authority.
}

std::string search_url(std::string_view host, std::string_view query,
                       std::uint32_t page) {
  std::string url(host);
  url += "/api/v2/search?query=";
  url += detail::url_encode(query);
  url += "&page=";
  url += std::to_string(page);
  return url;
}

std::string gallery_url(std::string_view host, std::string_view id) {
  std::string url(host);
  url += "/api/v2/galleries/";
  url += id;
  return url;
}

std::string cdn_url(std::string_view host) {
  std::string url(host);
  url += "/api/v2/cdn";
  return url;
}

std::string page_url(std::string_view server, std::string_view path) {
  std::string url(server);
  url += '/';
  url += path;
  return url;
}

std::string iso_from_unix(std::int64_t secs) {
  if (secs <= 0) return {};
  const std::time_t t = static_cast<std::time_t>(secs);
  std::tm tm{};
  if (::gmtime_r(&t, &tm) == nullptr) return {};
  char buf[40];
  if (std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S+00:00", &tm) == 0) {
    return {};
  }
  return buf;
}

Result<std::vector<MdManga>, ProviderError> parse_search(std::string_view raw) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode("search: invalid json"));
  }
  if (!j.contains("result") || !j.at("result").is_array()) {
    return err(ProviderError::decode("search: missing result[]"));
  }

  std::vector<MdManga> out;
  for (const json& row : j.at("result")) {
    if (!row.is_object()) continue;
    // The vet is the row's admission ticket: the id becomes a URL path
    // segment and the library dir's [id8] anchor.
    const std::string id = int_as_string(row, "id");
    if (!id_shaped(id)) continue;
    if (std::any_of(out.begin(), out.end(),
                    [&](const MdManga& m) { return m.id == id; })) {
      continue;
    }

    MdManga m;
    m.id = id;
    m.title = str_or_empty(row, "english_title");
    if (m.title.empty()) m.title = str_or_empty(row, "japanese_title");
    // A bad thumb path loses the cover, never the row.
    const std::string thumb = str_or_empty(row, "thumbnail");
    if (path_shaped(thumb)) m.cover_filename = thumb;
    // year/status/description stay empty, al/mal stay nullopt (nhentai
    // offers no tracker links); `blacklisted` is ignored for now.
    out.push_back(std::move(m));
  }
  return out;
}

Result<NhCdn, ProviderError> parse_cdn(std::string_view raw) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode("cdn: invalid json"));
  }
  const auto pool = [&](const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key) || !j.at(key).is_array()) return out;
    for (const json& v : j.at(key)) {
      if (!v.is_string()) continue;
      std::string s = v.get<std::string>();
      if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) continue;
      while (!s.empty() && s.back() == '/') s.pop_back();
      if (!s.empty()) out.push_back(std::move(s));
    }
    return out;
  };
  NhCdn cdn;
  cdn.image_servers = pool("image_servers");
  cdn.thumb_servers = pool("thumb_servers");
  if (cdn.image_servers.empty()) {
    // No usable image pool = no fetchable page anywhere; an empty thumb pool
    // just means no covers.
    return err(ProviderError::decode("cdn: no image servers"));
  }
  return cdn;
}

Result<NhGallery, ProviderError> parse_gallery(std::string_view raw) {
  const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return err(ProviderError::decode("gallery: invalid json"));
  }

  NhGallery g;
  g.id = int_as_string(j, "id");
  if (!id_shaped(g.id)) return err(ProviderError::decode("gallery: bad id"));
  g.media_id = str_or_empty(j, "media_id");
  if (!id_shaped(g.media_id)) {
    return err(ProviderError::decode("gallery: bad media_id"));
  }

  if (j.contains("title") && j.at("title").is_object()) {
    const json& t = j.at("title");
    g.title = str_or_empty(t, "pretty");
    if (g.title.empty()) g.title = str_or_empty(t, "english");
    if (g.title.empty()) g.title = str_or_empty(t, "japanese");
  }

  if (j.contains("tags") && j.at("tags").is_array()) {
    for (const json& t : j.at("tags")) {
      if (!t.is_object()) continue;
      if (str_or_empty(t, "type") != "language") continue;
      const std::string name = str_or_empty(t, "name");
      if (name.empty() || name == "translated" || name == "rewrite") continue;
      g.lang = lang_code(name);
      break;
    }
  }

  if (j.contains("num_pages") && j.at("num_pages").is_number_integer()) {
    const auto n = j.at("num_pages").get<std::int64_t>();
    if (n > 0) g.num_pages = static_cast<std::uint32_t>(n);
  }
  if (j.contains("upload_date") && j.at("upload_date").is_number_integer()) {
    g.publish_at = iso_from_unix(j.at("upload_date").get<std::int64_t>());
  }
  if (j.contains("thumbnail") && j.at("thumbnail").is_object()) {
    const std::string thumb = str_or_empty(j.at("thumbnail"), "path");
    if (path_shaped(thumb)) g.thumbnail = thumb;
  }

  // pages[] paths verbatim, document order (the API emits reading order).
  // One bad path fails the WHOLE parse — a page gap is worse than a failed
  // chapter (header posture). A missing/ill-typed pages[] parses as empty:
  // chapters() has no use for it, and pages() refuses an empty set itself.
  if (j.contains("pages") && j.at("pages").is_array()) {
    for (const json& p : j.at("pages")) {
      const std::string path =
          p.is_object() ? str_or_empty(p, "path") : std::string();
      if (!path_shaped(path)) {
        return err(ProviderError::decode("gallery: unsafe page path"));
      }
      g.pages.push_back(path);
    }
  }
  return g;
}

MdChapter gallery_chapter(const NhGallery& g) {
  MdChapter c;
  c.id = g.id;
  c.chapter = "";  // oneshot (the library dir spells it out).
  c.title = "Oneshot";
  c.pages = g.pages.empty() ? g.num_pages
                            : static_cast<std::uint32_t>(g.pages.size());
  c.lang = g.lang;
  c.publish_at = g.publish_at;
  return c;
}

}  // namespace nh

// ---------------------------------------------------------------------------
// Transport.
// ---------------------------------------------------------------------------

Result<Nhentai, ProviderError> Nhentai::create() {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return Nhentai(std::move(*client), kNhHost, http::Fingerprint::Chrome);
}

Result<Nhentai, ProviderError> Nhentai::with_host(std::string host) {
  while (!host.empty() && host.back() == '/') host.pop_back();
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  // Escalation off: a fixture server must be able to answer 403 in a test
  // without the transport reaching for the impersonate dylib.
  return Nhentai(std::move(*client), std::move(host), http::Fingerprint::None);
}

Result<std::vector<std::uint8_t>, ProviderError> Nhentai::get(
    const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.accept = http::Accept::Any2xx;
  if (state_->fingerprinted.load(std::memory_order_relaxed)) {
    req.fingerprint = escalate_;
  }
  auto resp = http_.fetch(req);
  if (!resp.has_value() && resp.error().kind == ProviderError::Kind::RateLimited) {
    // The MangaDex politeness floor, kept for every source.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    resp = http_.fetch(req);
  }
  if (!resp.has_value() && resp.error().kind == ProviderError::Kind::Forbidden &&
      escalate_ != http::Fingerprint::None &&
      req.fingerprint == http::Fingerprint::None) {
    // The edge blocked our plain ClientHello: retry once through the
    // impersonate seam and stay escalated. If the dylib is absent the
    // retry's own error surfaces — it names the actual fix, and plain just
    // proved useless anyway.
    state_->fingerprinted.store(true, std::memory_order_relaxed);
    req.fingerprint = escalate_;
    resp = http_.fetch(req);
  }
  return resp;
}

Result<nh::NhCdn, ProviderError> Nhentai::ensure_cdn() const {
  // The fetch happens OUTSIDE the lock: cover_thumb_url takes cdn_mu_ on the
  // UI thread, so the critical sections here must stay pointer-copy sized
  // (the UI thread must never block on a lock a worker could hold during a
  // network call). Two workers racing the first fill may both fetch — one
  // wasted request, and the second write is a no-op.
  {
    std::lock_guard<std::mutex> lock(state_->cdn_mu);
    if (!state_->cdn.image_servers.empty()) return state_->cdn;
  }
  auto body = get(nh::cdn_url(host_));
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  auto cdn = nh::parse_cdn(text);
  if (!cdn.has_value()) return err(cdn.error());
  std::lock_guard<std::mutex> lock(state_->cdn_mu);
  if (state_->cdn.image_servers.empty()) state_->cdn = std::move(*cdn);
  return state_->cdn;
}

Result<std::vector<MdManga>, ProviderError> Nhentai::search(
    std::string_view query) const {
  auto body = get(nh::search_url(host_, query, 1));
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  auto rows = nh::parse_search(text);
  if (!rows.has_value()) return rows;
  // Prime the cdn cache so cover_thumb_url (UI thread, never fetches) can
  // join thumb URLs for these rows. Best-effort: covers degrade to none.
  (void)ensure_cdn();
  return rows;
}

Result<std::vector<MdChapter>, ProviderError> Nhentai::chapters(
    std::string_view manga_id, std::string_view /*lang: single release*/) const {
  // Ids may later arrive from persisted store space rather than a fresh
  // parse; a corrupt one must not shape a request path.
  if (!nh::id_shaped(manga_id)) {
    return err(ProviderError::decode("bad gallery id"));
  }
  auto body = get(nh::gallery_url(host_, manga_id));
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  auto g = nh::parse_gallery(text);
  if (!g.has_value()) return err(g.error());
  return std::vector<MdChapter>{nh::gallery_chapter(*g)};
}

Result<PageSet, ProviderError> Nhentai::pages(
    std::string_view chapter_id, bool /*data_saver: one quality only*/) const {
  if (!nh::id_shaped(chapter_id)) {
    return err(ProviderError::decode("bad gallery id"));
  }
  auto cdn = ensure_cdn();
  if (!cdn.has_value()) return err(cdn.error());
  auto body = get(nh::gallery_url(host_, chapter_id));
  if (!body.has_value()) return err(body.error());
  const std::string_view text(reinterpret_cast<const char*>(body->data()),
                              body->size());
  auto g = nh::parse_gallery(text);
  if (!g.has_value()) return err(g.error());
  if (g->pages.empty()) {
    return err(ProviderError::decode("gallery has no pages"));
  }

  // Round-robin over the pool: each resolution (initial fetch and rotation
  // refetch alike) lands on the next server, so a refetch hops for free.
  const auto slot = state_->rotate.fetch_add(1, std::memory_order_relaxed);
  const std::string& server =
      cdn->image_servers[slot % cdn->image_servers.size()];

  PageSet set;
  for (const std::string& path : g->pages) {
    const auto slash = path.rfind('/');
    const std::string_view hint =
        slash == std::string::npos ? std::string_view(path)
                                   : std::string_view(path).substr(slash + 1);
    // hint passed filename_safe inside path_shaped at parse time.
    set.units.push_back(
        PageUnit{nh::page_url(server, path), host_, std::string(hint)});
  }
  return set;
}

std::string Nhentai::cover_thumb_url(const MdManga& m) const {
  if (m.cover_filename.empty()) return {};
  std::lock_guard<std::mutex> lock(state_->cdn_mu);
  if (state_->cdn.thumb_servers.empty()) return {};  // pool not primed (or none).
  // Server [0] always: a deterministic URL keeps the disk cover cache warm.
  return nh::page_url(state_->cdn.thumb_servers.front(), m.cover_filename);
}

}  // namespace shigoku::manga
