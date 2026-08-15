// anidbapp.cpp — anidb.app provider (P25). Ported 1:1 from
// src/providers/anidbapp.rs. This file holds the pure parsers (Slice 2); the
// StreamProvider transport (json_get/page_get/search/episodes/resolve) follows
// in Slice 3.

#include "anidbapp.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "hls.hpp"       // parse_master_playlist / join_url / select_variant.
#include "provider.hpp"  // clean_arg, guard_show_id (shared provider guards).

namespace shigoku::anidbapp {
namespace detail {

namespace {

// Inner text of every <tag ...>...</tag> in `block`, outermost-first.
std::vector<std::string> tag_texts(std::string_view block, std::string_view tag) {
  const std::string open = std::string("<") + std::string(tag);
  const std::string close = std::string("</") + std::string(tag) + ">";
  std::vector<std::string> out;
  std::string_view rest = block;
  while (true) {
    const auto at = rest.find(open);
    if (at == std::string_view::npos) break;
    std::string_view after = rest.substr(at + open.size());
    const auto gt = after.find('>');
    if (gt == std::string_view::npos) break;
    std::string_view inner = after.substr(gt + 1);
    const auto end = inner.find(close);
    if (end == std::string_view::npos) break;
    out.emplace_back(inner.substr(0, end));
    rest = inner.substr(end + close.size());
  }
  return out;
}

// Value of `name="..."` in `block` (nullopt if absent/unterminated).
std::optional<std::string> attr_value(std::string_view block, std::string_view name) {
  const std::string needle = std::string(name) + "=\"";
  const auto at = block.find(needle);
  if (at == std::string_view::npos) return std::nullopt;
  std::string_view rest = block.substr(at + needle.size());
  const auto end = rest.find('"');
  if (end == std::string_view::npos) return std::nullopt;
  return std::string(rest.substr(0, end));
}

// Named/numeric entity -> char. nullopt if unrecognized (caller keeps literal).
std::optional<char32_t> entity_char(std::string_view entity) {
  if (entity == "amp") return U'&';
  if (entity == "lt") return U'<';
  if (entity == "gt") return U'>';
  if (entity == "quot") return U'"';
  if (entity == "apos") return U'\'';
  if (entity == "nbsp") return U' ';
  if (entity.empty() || entity.front() != '#') return std::nullopt;
  std::string_view digits = entity.substr(1);
  if (digits.empty()) return std::nullopt;
  int base = 10;
  if (digits.front() == 'x' || digits.front() == 'X') {
    base = 16;
    digits = digits.substr(1);
    if (digits.empty()) return std::nullopt;
  }
  // Parse into a code point.
  char32_t code = 0;
  for (const char c : digits) {
    int d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    } else {
      return std::nullopt;
    }
    code = code * static_cast<char32_t>(base) + static_cast<char32_t>(d);
    if (code > 0x10FFFF) return std::nullopt;
  }
  return code;
}

// Append a code point as UTF-8. (Card titles are UTF-8; the entities we decode
// are almost all ASCII, but &#xNN; can name any BMP char.)
void push_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

// The integer immediately after `marker` in `html` (nullopt if absent/empty).
std::optional<std::int64_t> id_after(std::string_view html, std::string_view marker) {
  const auto at = html.find(marker);
  if (at == std::string_view::npos) return std::nullopt;
  std::string_view rest = html.substr(at + marker.size());
  std::size_t end = 0;
  while (end < rest.size() && rest[end] >= '0' && rest[end] <= '9') ++end;
  if (end == 0) return std::nullopt;
  std::int64_t v = 0;
  for (std::size_t i = 0; i < end; ++i) {
    v = v * 10 + (rest[i] - '0');
  }
  return v;
}

// Quoted value of `key: "..."` / `key: '...'` (nullopt if not present/quoted).
std::optional<std::string> config_value(std::string_view html, std::string_view key) {
  const auto at = html.find(key);
  if (at == std::string_view::npos) return std::nullopt;
  std::string_view rest = html.substr(at + key.size());
  // trim leading whitespace
  std::size_t i = 0;
  while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t' || rest[i] == '\n' ||
                             rest[i] == '\r')) {
    ++i;
  }
  rest = rest.substr(i);
  if (rest.empty() || rest.front() != ':') return std::nullopt;
  rest = rest.substr(1);
  i = 0;
  while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t' || rest[i] == '\n' ||
                             rest[i] == '\r')) {
    ++i;
  }
  rest = rest.substr(i);
  if (rest.empty()) return std::nullopt;
  const char quote = rest.front();
  if (quote != '"' && quote != '\'') return std::nullopt;
  std::string_view inner = rest.substr(1);
  const auto end = inner.find(quote);
  if (end == std::string_view::npos) return std::nullopt;
  return std::string(inner.substr(0, end));
}

// First `.m3u8` on the page, widened to its enclosing quotes.
std::optional<std::string> first_quoted_m3u8(std::string_view html) {
  const auto at = html.find(".m3u8");
  if (at == std::string_view::npos) return std::nullopt;
  const auto start = html.substr(0, at).find_last_of("\"'");
  if (start == std::string_view::npos) return std::nullopt;
  const auto close = html.substr(at).find_first_of("\"'");
  if (close == std::string_view::npos) return std::nullopt;
  return std::string(html.substr(start + 1, (at + close) - (start + 1)));
}

// One card block (between "<a " and "</a>") -> Card, or nullopt.
std::optional<Card> parse_card(std::string_view block) {
  auto href = attr_value(block, "href");
  if (!href) return std::nullopt;
  auto sl = split_slug(*href);
  if (!sl) return std::nullopt;
  auto texts = tag_texts(block, "p");
  if (texts.empty()) return std::nullopt;
  // trim + entity-decode the title
  std::string_view first = texts.front();
  const auto b = first.find_first_not_of(" \t\r\n");
  const auto e = first.find_last_not_of(" \t\r\n");
  std::string title =
      (b == std::string_view::npos) ? std::string()
                                    : decode_entities(first.substr(b, e - b + 1));
  if (title.empty()) return std::nullopt;
  std::optional<std::uint32_t> year;
  if (texts.size() > 1) year = trailing_year(texts[1]);
  return Card{std::move(sl->first), std::move(sl->second), std::move(title), year};
}

}  // namespace

std::vector<Card> parse_cards(std::string_view html) {
  std::vector<Card> out;
  std::string_view rest = html;
  while (true) {
    const auto at = rest.find("<a ");
    if (at == std::string_view::npos) break;
    std::string_view after = rest.substr(at + 3);  // len("<a ")
    std::string_view block;
    const auto end = after.find("</a>");
    if (end != std::string_view::npos) {
      block = after.substr(0, end);
      rest = after.substr(end + 4);  // len("</a>")
    } else {
      block = after;
      rest = std::string_view();
    }
    if (block.find("data-search-item") == std::string_view::npos) continue;
    if (auto card = parse_card(block)) out.push_back(std::move(*card));
  }
  return out;
}

std::optional<std::pair<std::string, std::string>> split_slug(std::string_view href) {
  if (href.find("/anime/") == std::string_view::npos) return std::nullopt;
  // last path segment
  const auto slash = href.find_last_of('/');
  std::string_view slug =
      (slash == std::string_view::npos) ? href : href.substr(slash + 1);
  // strip query/fragment
  const auto qh = slug.find_first_of("?#");
  if (qh != std::string_view::npos) slug = slug.substr(0, qh);
  if (slug.empty()) return std::nullopt;
  for (const char c : slug) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return std::nullopt;
  }
  const auto dash = slug.find_last_of('-');
  if (dash == std::string_view::npos) return std::nullopt;
  std::string_view name = slug.substr(0, dash);
  std::string_view id = slug.substr(dash + 1);
  if (name.empty() || id.empty()) return std::nullopt;
  for (const char c : id) {
    if (c < '0' || c > '9') return std::nullopt;
  }
  return std::make_pair(std::string(slug), std::string(id));
}

std::optional<std::uint32_t> trailing_year(std::string_view meta) {
  std::optional<std::uint32_t> found;
  std::size_t i = 0;
  while (i < meta.size()) {
    if (meta[i] < '0' || meta[i] > '9') {
      ++i;
      continue;
    }
    const std::size_t start = i;
    while (i < meta.size() && meta[i] >= '0' && meta[i] <= '9') ++i;
    if (i - start == 4) {
      std::uint32_t v = 0;
      for (std::size_t k = start; k < i; ++k) v = v * 10 + (meta[k] - '0');
      found = v;
    }
  }
  return found;
}

std::string decode_entities(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  std::string_view rest = s;
  while (true) {
    const auto at = rest.find('&');
    if (at == std::string_view::npos) break;
    out.append(rest.substr(0, at));
    std::string_view tail = rest.substr(at);
    const auto semi = tail.find(';');
    if (semi != std::string_view::npos && semi <= 10) {
      auto c = entity_char(tail.substr(1, semi - 1));
      if (c) {
        push_utf8(out, *c);
      } else {
        out.append(tail.substr(0, semi + 1));
      }
      rest = tail.substr(semi + 1);
    } else {
      out.push_back('&');
      rest = tail.substr(1);
    }
  }
  out.append(rest);
  return out;
}

std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>
parse_external_ids(std::string_view html) {
  return {id_after(html, "anilist.co/anime/"),
          id_after(html, "myanimelist.net/anime/")};
}

bool is_challenge(std::string_view html) {
  if (html.find("cf_chl_opt") != std::string_view::npos) return true;
  if (html.find("__cf_chl") != std::string_view::npos) return true;
  for (const auto& t : tag_texts(html, "title")) {
    if (t.find("Just a moment") != std::string::npos) return true;
  }
  return false;
}

std::optional<std::string> extract_hls(std::string_view html) {
  std::optional<std::string> url = config_value(html, "file");
  if (!url || url->find(".m3u8") == std::string::npos) {
    url = first_quoted_m3u8(html);
  }
  if (!url) return std::nullopt;
  if (url->rfind("http", 0) != 0) return std::nullopt;  // must be absolute-ish.
  return url;
}

bool stream_url_ok(std::string_view url) {
  return is_absolute_url(url) && clean_arg(url) &&
         http::guard_fetch_url(url).has_value();
}

std::string form_urlencode(std::string_view s) {
  // application/x-www-form-urlencoded byte_serialize: unreserved (A-Za-z0-9 and
  // *-._) pass, ' ' -> '+', everything else -> %XX (uppercase hex).
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '*' || c == '-' ||
                            c == '.' || c == '_';
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

Result<std::vector<Episode>, ProviderError> parse_episodes(std::string_view raw_json) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(raw_json);
  } catch (const nlohmann::json::exception& e) {
    return err(ProviderError::decode(std::string("episodes: ") + e.what()));
  }
  std::vector<Episode> eps;
  if (j.contains("episodes") && j["episodes"].is_array()) {
    for (const auto& row : j["episodes"]) {
      if (!row.is_object()) continue;
      // id: positive integer
      std::int64_t id = 0;
      if (auto it = row.find("id"); it != row.end() && it->is_number_integer()) {
        id = it->get<std::int64_t>();
      } else {
        continue;
      }
      if (id <= 0) continue;
      // number: 1..=u32::MAX
      std::int64_t number = 0;
      if (auto it = row.find("number");
          it != row.end() && it->is_number_integer()) {
        number = it->get<std::int64_t>();
      } else {
        continue;
      }
      if (number < 1 || number > static_cast<std::int64_t>(UINT32_MAX)) continue;
      eps.push_back(Episode{id, static_cast<std::uint32_t>(number)});
    }
  }
  // Stable sort by number so dedup keeps the row the site listed first.
  std::stable_sort(eps.begin(), eps.end(),
                   [](const Episode& a, const Episode& b) { return a.number < b.number; });
  eps.erase(std::unique(eps.begin(), eps.end(),
                        [](const Episode& a, const Episode& b) {
                          return a.number == b.number;
                        }),
            eps.end());
  return eps;
}

std::uint32_t base_offset(const std::vector<Episode>& eps) {
  if (eps.empty()) return 0;
  const std::uint32_t first = eps.front().number;
  return first > 0 ? first - 1 : 0;
}

std::string label(const Episode& ep, std::uint32_t offset) {
  const std::uint32_t n = ep.number > offset ? ep.number - offset : 0;
  return std::to_string(n);
}

}  // namespace detail

// ===========================================================================
// The provider transport (anidbapp.rs impl AniDbApp + impl StreamProvider).
// ===========================================================================

using detail::Episode;

Result<AniDbApp, ProviderError> AniDbApp::create() {
  // Live provider: Chrome fingerprint (anidb.app 403s a stock ClientHello).
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return AniDbApp(std::move(*client), kApi, http::Fingerprint::Chrome);
}

Result<AniDbApp, ProviderError> AniDbApp::with_endpoint(std::string api) {
  // Test seam: fixture server is plain HTTP, no fingerprinting -> None, so the
  // transport tests run without the impersonate dylib.
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return AniDbApp(std::move(*client), std::move(api), http::Fingerprint::None);
}

// -- HTTP surfaces. Both set fingerprint = Chrome: anidb.app's edge scores the
//    TLS ClientHello, so a stock handshake 403s (P25 seam).
Result<std::vector<std::uint8_t>, ProviderError> AniDbApp::json_get(
    const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Referer", kReferer});
  req.extra_headers.push_back({"Accept", "application/json"});
  req.accept = http::Accept::Any2xx;
  req.fingerprint = fingerprint_;
  return http_.fetch(req);
}

Result<std::string, ProviderError> AniDbApp::page_get(const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Referer", kReferer});
  req.extra_headers.push_back(
      {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
  req.extra_headers.push_back({"Accept-Language", "en-US,en;q=0.9"});
  req.accept = http::Accept::Any2xx;
  req.fingerprint = fingerprint_;
  auto raw = http_.fetch(req);
  if (!raw.has_value()) return err(raw.error());
  std::string html(raw->begin(), raw->end());
  if (detail::is_challenge(html)) {
    // A challenge interstitial served at 200: refuse as a block so the walk
    // hops instead of reading it as an answer (would stamp a 7-day absence).
    return err(ProviderError::forbidden(403));
  }
  return html;
}

Result<std::vector<Episode>, ProviderError> AniDbApp::fetch_episodes(
    std::string_view site_id) const {
  const std::string url =
      api_ + "/api/frontend/anime/" + std::string(site_id) + "/episodes";
  auto raw = json_get(url);
  if (!raw.has_value()) return err(raw.error());
  return detail::parse_episodes(std::string_view(
      reinterpret_cast<const char*>(raw->data()), raw->size()));
}

Result<std::vector<detail::LanguageRow>, ProviderError> AniDbApp::fetch_languages(
    std::int64_t ep_id) const {
  const std::string url =
      api_ + "/api/frontend/episode/" + std::to_string(ep_id) + "/languages";
  auto raw = json_get(url);
  if (!raw.has_value()) return err(raw.error());
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(raw->begin(), raw->end());
  } catch (const nlohmann::json::exception& e) {
    return err(ProviderError::decode(std::string("languages: ") + e.what()));
  }
  std::vector<detail::LanguageRow> rows;
  if (j.contains("languages") && j["languages"].is_array()) {
    for (const auto& r : j["languages"]) {
      if (!r.is_object()) continue;
      detail::LanguageRow row;
      if (auto it = r.find("code"); it != r.end() && it->is_string()) {
        row.code = it->get<std::string>();
      }
      if (auto it = r.find("embed_url"); it != r.end() && it->is_string()) {
        row.embed_url = it->get<std::string>();
      }
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

Result<bool, ProviderError> AniDbApp::has_dub(std::int64_t ep_id) const {
  auto rows = fetch_languages(ep_id);
  if (!rows.has_value()) return err(rows.error());
  for (const auto& r : *rows) {
    if (r.code.has_value() && *r.code == kCodeDub) return true;
  }
  return false;
}

Result<std::optional<std::size_t>, ProviderError> AniDbApp::dub_prefix(
    const std::vector<Episode>& eps) const {
  if (eps.empty()) return std::optional<std::size_t>(std::nullopt);
  const std::size_t last = eps.size() - 1;
  auto first_dub = has_dub(eps[0].id);
  if (!first_dub.has_value()) return err(first_dub.error());
  if (!*first_dub) return std::optional<std::size_t>(std::nullopt);
  if (last == 0) return std::optional<std::size_t>(last);
  auto last_dub = has_dub(eps[last].id);
  if (!last_dub.has_value()) return err(last_dub.error());
  if (*last_dub) return std::optional<std::size_t>(last);
  // Bisect the sub/dub boundary (dub assumed a prefix).
  std::size_t lo = 0, hi = last;
  while (hi - lo > 1) {
    const std::size_t mid = lo + (hi - lo) / 2;
    auto mid_dub = has_dub(eps[mid].id);
    if (!mid_dub.has_value()) return err(mid_dub.error());
    if (*mid_dub) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return std::optional<std::size_t>(lo);
}

Result<std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>,
       ProviderError>
AniDbApp::probe_ids(std::string_view slug) const {
  const std::string url = api_ + "/anime/" + std::string(slug);
  auto html = page_get(url);
  if (!html.has_value()) return err(html.error());
  return detail::parse_external_ids(*html);
}

std::optional<std::string> AniDbApp::cap_variant(const std::string& master_url,
                                                 Quality quality) const {
  if (!http::guard_fetch_url(master_url).has_value()) return std::nullopt;
  http::Request req;
  req.method = http::Method::Get;
  req.url = master_url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Referer", kReferer});
  req.accept = http::Accept::OkOnly;
  req.fingerprint = fingerprint_;
  auto body = http_.fetch(req);
  if (!body.has_value()) return std::nullopt;
  const std::string text(body->begin(), body->end());
  auto variants = hls::parse_master_playlist(text);
  if (variants.empty()) return std::nullopt;  // media playlist: keep the master.
  std::vector<StreamLink> links;
  for (const auto& v : variants) {
    auto joined = hls::join_url(master_url, v.url);
    if (!joined.has_value()) continue;
    if (!clean_arg(*joined)) continue;
    StreamLink link;
    link.url = *joined;
    link.resolution = v.resolution;
    links.push_back(std::move(link));
  }
  const StreamLink* pick = hls::select_variant(links, quality);
  if (pick == nullptr) return std::nullopt;
  return pick->url;
}

// -- StreamProvider surface --

std::optional<std::string> AniDbApp::canonical_key(const Enrichment& /*show*/) const {
  return std::nullopt;  // no canonical-keyed endpoint; tier-C search binds it.
}

Result<std::vector<SearchHit>, ProviderError> AniDbApp::search(
    std::string_view query, const SearchOptions& opts) const {
  // No paging on this endpoint: page 2 would repeat page 1. Query length is in
  // Unicode scalar values in the Rust; ASCII byte length matches for the API's
  // Latin queries, and the endpoint itself rejects the short case regardless.
  if (opts.page > 1 || query.size() < kMinQueryLen) {
    return std::vector<SearchHit>{};
  }
  const std::string url =
      api_ + "/search/suggestions?q=" + detail::form_urlencode(query);
  auto html = page_get(url);
  if (!html.has_value()) return err(html.error());

  std::vector<SearchHit> hits;
  auto cards = detail::parse_cards(*html);
  std::size_t probed = 0;
  for (auto& card : cards) {
    if (probed >= kMaxProbe) break;
    ++probed;
    std::optional<std::int64_t> anilist_id, mal_id;
    auto ids = probe_ids(card.slug);
    if (!ids.has_value()) {
      // A block is provider-wide: stop and let the walk hop. Other failures
      // cost this card its ids, not the search.
      if (ids.error().kind == ProviderError::Kind::Forbidden) return err(ids.error());
    } else {
      anilist_id = ids->first;
      mal_id = ids->second;
    }
    SearchHit hit;
    hit.provider_id = std::move(card.site_id);
    hit.title = std::move(card.title);
    hit.anilist_id = anilist_id;
    hit.mal_id = mal_id;
    hit.year = card.year;
    hits.push_back(std::move(hit));
  }
  if (hits.size() > opts.limit) hits.resize(opts.limit);
  return hits;
}

Result<std::vector<std::string>, ProviderError> AniDbApp::episodes(
    std::string_view provider_id, Translation translation,
    std::optional<std::uint32_t> /*count_hint*/) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  auto eps = fetch_episodes(provider_id);
  if (!eps.has_value()) return err(eps.error());
  const std::uint32_t offset = detail::base_offset(*eps);

  std::vector<Episode> listed;
  if (translation == Translation::Sub) {
    listed = *eps;
  } else {
    auto pref = dub_prefix(*eps);
    if (!pref.has_value()) return err(pref.error());
    if (pref->has_value()) {
      listed.assign(eps->begin(), eps->begin() + static_cast<std::ptrdiff_t>(**pref) + 1);
    }
    // no dub -> empty (per-track absence).
  }
  std::vector<std::string> out;
  out.reserve(listed.size());
  for (const auto& e : listed) out.push_back(detail::label(e, offset));
  return out;
}

Result<StreamLink, ProviderError> AniDbApp::resolve(std::string_view provider_id,
                                                    std::string_view episode,
                                                    Translation translation,
                                                    Quality quality) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  // Parse the (canonical) episode label. Reject 0, non-numeric, and overflow.
  std::uint32_t want = 0;
  if (episode.empty()) return err(ProviderError::decode("invalid episode"));
  for (const char c : episode) {
    if (c < '0' || c > '9') return err(ProviderError::decode("invalid episode"));
    const std::uint64_t next = static_cast<std::uint64_t>(want) * 10 + (c - '0');
    if (next > UINT32_MAX) return err(ProviderError::decode("invalid episode"));
    want = static_cast<std::uint32_t>(next);
  }
  if (want == 0) return err(ProviderError::decode("invalid episode"));

  auto eps = fetch_episodes(provider_id);
  if (!eps.has_value()) return err(eps.error());
  const std::uint32_t offset = detail::base_offset(*eps);
  const std::uint64_t site_number = static_cast<std::uint64_t>(want) + offset;
  if (site_number > UINT32_MAX) return err(ProviderError::decode("invalid episode"));
  const Episode* ep = nullptr;
  for (const auto& e : *eps) {
    if (e.number == site_number) {
      ep = &e;
      break;
    }
  }
  if (ep == nullptr) return err(ProviderError::decode("no such episode"));

  const char* want_code = (translation == Translation::Sub) ? kCodeSub : kCodeDub;
  auto rows = fetch_languages(ep->id);
  if (!rows.has_value()) return err(rows.error());
  std::optional<std::string> embed;
  for (auto& r : *rows) {
    if (r.code.has_value() && *r.code == want_code && r.embed_url.has_value() &&
        !r.embed_url->empty()) {
      embed = r.embed_url;
      break;
    }
  }
  if (!embed.has_value()) return err(ProviderError::decode("no stream for track"));
  if (!is_absolute_url(*embed) || !clean_arg(*embed) ||
      !http::guard_fetch_url(*embed).has_value()) {
    return err(ProviderError::decode("blocked embed url"));
  }

  auto html = page_get(*embed);
  if (!html.has_value()) return err(html.error());
  auto master = detail::extract_hls(*html);
  if (!master.has_value()) return err(ProviderError::decode("no playable source"));
  if (!detail::stream_url_ok(*master)) {
    return err(ProviderError::decode("bad stream url"));
  }

  // Best leaves mpv on the master ladder; a cap fetches the variants.
  std::string chosen = *master;
  if (quality != Quality::Best) {
    if (auto capped = cap_variant(*master, quality)) chosen = *capped;
  }
  StreamLink link;
  link.url = std::move(chosen);
  link.referer = std::string(kReferer);
  link.user_agent = std::string(kUserAgent);
  // Plain TS named .xls with a spreadsheet content type: mpv must relax its
  // demuxer gate. No decoy prefix, so no stripping proxy.
  link.cloaked_segments = true;
  link.decloak_segments = false;
  return link;
}

Result<CoverRequest, ProviderError> AniDbApp::cover_request(
    std::string_view cover_ref) const {
  if (cover_ref.empty() || cover_ref.size() > kMaxCoverRefLen ||
      !is_absolute_url(cover_ref) || !clean_arg(cover_ref)) {
    return err(ProviderError::decode("invalid cover ref"));
  }
  CoverRequest req;
  req.url = std::string(cover_ref);
  req.referer = std::string(kReferer);
  req.user_agent = std::string(kUserAgent);
  return req;
}

}  // namespace shigoku::anidbapp
