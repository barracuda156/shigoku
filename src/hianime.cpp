// hianime.cpp — hianime.at provider (P47).

#include "hianime.hpp"

#include <algorithm>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "crypto.hpp"
#include "provider.hpp"  // clean_arg, guard_show_id (shared provider guards).

namespace shigoku::hianime {
namespace detail {

namespace {

// Value of `name="..."` anywhere in `block` (nullopt if absent/unterminated).
// Whitespace-agnostic by construction — attributes on their own line (this
// site's style) match the same as inline ones.
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

std::string_view trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// The `html` field of a `{"status":..., "html": "..."}` envelope. Err(Decode)
// when the body isn't that JSON shape (e.g. an HTML error page).
Result<std::string, ProviderError> unwrap_html_envelope(std::string_view raw_json) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(raw_json);
  } catch (const nlohmann::json::exception& e) {
    return err(ProviderError::decode(std::string("envelope: ") + e.what()));
  }
  if (!j.is_object() || !j.contains("html") || !j["html"].is_string()) {
    return err(ProviderError::decode("envelope: no html field"));
  }
  return j["html"].get<std::string>();
}

}  // namespace

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

std::optional<std::string> trailing_id(std::string_view href) {
  const auto slash = href.find_last_of('/');
  std::string_view seg = (slash == std::string_view::npos) ? href : href.substr(slash + 1);
  const auto qh = seg.find_first_of("?#");
  if (qh != std::string_view::npos) seg = seg.substr(0, qh);
  const auto dash = seg.find_last_of('-');
  if (dash == std::string_view::npos) return std::nullopt;
  std::string_view id = seg.substr(dash + 1);
  if (id.empty()) return std::nullopt;
  for (const char c : id) {
    if (c < '0' || c > '9') return std::nullopt;
  }
  return std::string(id);
}

std::vector<Card> parse_search_cards(std::string_view html) {
  const auto cut = html.find("id=\"main-sidebar\"");
  const std::string_view scoped = (cut == std::string_view::npos) ? html : html.substr(0, cut);

  std::vector<Card> out;
  std::string_view rest = scoped;
  while (true) {
    const auto at = rest.find("class=\"film-name\"");
    if (at == std::string_view::npos) break;
    std::string_view after = rest.substr(at);
    const auto a_at = after.find("<a ");
    if (a_at == std::string_view::npos) break;
    std::string_view block_start = after.substr(a_at + 3);  // len("<a ")
    const auto close = block_start.find("</a>");
    std::string_view block =
        (close == std::string_view::npos) ? block_start : block_start.substr(0, close);
    rest = (close == std::string_view::npos) ? std::string_view()
                                             : block_start.substr(close + 4);

    auto href = attr_value(block, "href");
    if (!href) continue;
    auto id = trailing_id(*href);
    if (!id) continue;
    const auto gt = block.find('>');  // end of the <a ...> opening tag.
    if (gt == std::string_view::npos) continue;
    std::string title = decode_entities(trim(block.substr(gt + 1)));
    if (title.empty()) continue;
    out.push_back(Card{std::move(*id), std::move(title)});
  }
  return out;
}

Result<std::vector<EpisodeRow>, ProviderError> parse_episode_list(std::string_view raw_json) {
  auto html = unwrap_html_envelope(raw_json);
  if (!html.has_value()) return err(html.error());

  std::vector<EpisodeRow> rows;
  std::string_view rest = *html;
  while (true) {
    const auto at = rest.find("<a ");
    if (at == std::string_view::npos) break;
    std::string_view after = rest.substr(at + 3);
    const auto close = after.find("</a>");
    std::string_view block = (close == std::string_view::npos) ? after : after.substr(0, close);
    rest = (close == std::string_view::npos) ? std::string_view() : after.substr(close + 4);

    auto number_s = attr_value(block, "data-number");
    auto id = attr_value(block, "data-id");
    if (!number_s || !id || number_s->empty() || id->empty()) continue;
    std::uint64_t number = 0;
    bool ok = true;
    for (const char c : *number_s) {
      if (c < '0' || c > '9') {
        ok = false;
        break;
      }
      number = number * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (!ok || number < 1 || number > UINT32_MAX) continue;
    rows.push_back(EpisodeRow{static_cast<std::uint32_t>(number), std::move(*id)});
  }
  std::stable_sort(rows.begin(), rows.end(),
                   [](const EpisodeRow& a, const EpisodeRow& b) { return a.number < b.number; });
  rows.erase(std::unique(rows.begin(), rows.end(),
                        [](const EpisodeRow& a, const EpisodeRow& b) {
                          return a.number == b.number;
                        }),
            rows.end());
  return rows;
}

Result<std::optional<std::string>, ProviderError> find_zokoanime_hash(
    std::string_view raw_json, Translation translation) {
  auto html = unwrap_html_envelope(raw_json);
  if (!html.has_value()) return err(html.error());
  const std::string_view want_type = (translation == Translation::Sub) ? "sub" : "dub";

  const std::string_view marker = "class=\"item server-item\"";
  std::string_view rest = *html;
  while (true) {
    const auto at = rest.find(marker);
    if (at == std::string_view::npos) break;
    std::string_view after = rest.substr(at + marker.size());
    const auto gt = after.find('>');
    if (gt == std::string_view::npos) break;
    std::string_view tag = after.substr(0, gt);  // the rest of this div's opening tag.
    rest = after.substr(gt + 1);

    auto type = attr_value(tag, "data-type");
    auto server = attr_value(tag, "data-server-name");
    auto hash = attr_value(tag, "data-hash");
    if (type && server && hash && *type == want_type && *server == "ZokoAnime" &&
        !hash->empty()) {
      return std::optional<std::string>(std::move(*hash));
    }
  }
  return std::optional<std::string>(std::nullopt);
}

std::optional<std::string> decode_hash(std::string_view hash_b64) {
  auto bytes = crypto::base64_decode(hash_b64);
  if (!bytes.has_value() || bytes->empty()) return std::nullopt;
  std::string url(bytes->begin(), bytes->end());
  if (!is_absolute_url(url)) return std::nullopt;
  return url;
}

std::optional<std::string_view> extract_p_blob(std::string_view html) {
  const std::string_view marker = "window.__P=\"";
  const auto at = html.find(marker);
  if (at == std::string_view::npos) return std::nullopt;
  std::string_view rest = html.substr(at + marker.size());
  const auto end = rest.find('"');
  if (end == std::string_view::npos) return std::nullopt;
  return rest.substr(0, end);
}

std::vector<std::uint8_t> xor_repeat(const std::vector<std::uint8_t>& data,
                                     std::string_view key) {
  std::vector<std::uint8_t> out(data.size());
  if (key.empty()) return data;
  for (std::size_t i = 0; i < data.size(); ++i) {
    out[i] = data[i] ^ static_cast<std::uint8_t>(key[i % key.size()]);
  }
  return out;
}

Result<Embed, ProviderError> parse_embed(std::string_view plaintext) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(plaintext);
  } catch (const nlohmann::json::exception& e) {
    return err(ProviderError::decode(std::string("embed: ") + e.what()));
  }
  if (!j.is_object()) return err(ProviderError::decode("embed: not an object"));

  Embed out;
  if (auto it = j.find("src"); it != j.end() && it->is_string()) {
    out.src = it->get<std::string>();
  }
  if (auto it = j.find("subtitles"); it != j.end() && it->is_array()) {
    for (const auto& row : *it) {
      if (!row.is_object()) continue;
      Subtitle sub;
      if (auto f = row.find("lang"); f != row.end() && f->is_string()) {
        sub.lang = f->get<std::string>();
      }
      if (auto f = row.find("label"); f != row.end() && f->is_string()) {
        sub.label = f->get<std::string>();
      }
      if (auto f = row.find("default"); f != row.end() && f->is_boolean()) {
        sub.is_default = f->get<bool>();
      }
      if (auto f = row.find("src"); f != row.end() && f->is_string()) {
        sub.src = f->get<std::string>();
      }
      if (!sub.src.empty()) out.subtitles.push_back(std::move(sub));
    }
  }
  return out;
}

bool stream_url_ok(std::string_view url) {
  return is_absolute_url(url) && clean_arg(url) && http::guard_fetch_url(url).has_value();
}

std::string url_encode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                            c == '~';
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

std::optional<std::string> origin_of(std::string_view absolute_url) {
  const auto scheme_end = absolute_url.find("://");
  if (scheme_end == std::string_view::npos) return std::nullopt;
  const auto path_start = absolute_url.find('/', scheme_end + 3);
  std::string_view origin =
      (path_start == std::string_view::npos) ? absolute_url : absolute_url.substr(0, path_start);
  return std::string(origin) + "/";
}

}  // namespace detail

// ===========================================================================
// The provider transport.
// ===========================================================================

using detail::EpisodeRow;

Result<Hianime, ProviderError> Hianime::create() {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return Hianime(std::move(*client), kApi);
}

Result<Hianime, ProviderError> Hianime::with_endpoint(std::string api) {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return Hianime(std::move(*client), std::move(api));
}

Result<std::vector<std::uint8_t>, ProviderError> Hianime::json_get(
    const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.extra_headers.push_back({"Referer", kReferer});
  req.extra_headers.push_back({"Accept", "application/json, text/javascript, */*; q=0.01"});
  req.extra_headers.push_back({"X-Requested-With", "XMLHttpRequest"});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
}

Result<std::string, ProviderError> Hianime::page_get(const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.extra_headers.push_back({"Referer", kReferer});
  req.extra_headers.push_back(
      {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
  req.accept = http::Accept::Any2xx;
  auto raw = http_.fetch(req);
  if (!raw.has_value()) return err(raw.error());
  return std::string(raw->begin(), raw->end());
}

Result<std::vector<EpisodeRow>, ProviderError> Hianime::fetch_episodes(
    std::string_view id) const {
  const std::string url = api_ + "/api/theme/episode/list/" + std::string(id);
  auto raw = json_get(url);
  if (!raw.has_value()) return err(raw.error());
  return detail::parse_episode_list(
      std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
}

Result<bool, ProviderError> Hianime::has_dub(const std::string& episode_id) const {
  const std::string url = api_ + "/api/theme/episode/servers?episodeId=" + episode_id;
  auto raw = json_get(url);
  if (!raw.has_value()) return err(raw.error());
  auto hash = detail::find_zokoanime_hash(
      std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()),
      Translation::Dub);
  if (!hash.has_value()) return err(hash.error());
  return hash->has_value();
}

Result<std::optional<std::size_t>, ProviderError> Hianime::dub_prefix(
    const std::vector<EpisodeRow>& eps) const {
  if (eps.empty()) return std::optional<std::size_t>(std::nullopt);
  const std::size_t last = eps.size() - 1;
  auto first_dub = has_dub(eps[0].id);
  if (!first_dub.has_value()) return err(first_dub.error());
  if (!*first_dub) return std::optional<std::size_t>(std::nullopt);
  if (last == 0) return std::optional<std::size_t>(last);
  auto last_dub = has_dub(eps[last].id);
  if (!last_dub.has_value()) return err(last_dub.error());
  if (*last_dub) return std::optional<std::size_t>(last);
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

// -- StreamProvider surface --

std::optional<std::string> Hianime::canonical_key(const Enrichment& /*show*/) const {
  return std::nullopt;  // no canonical-keyed endpoint; tier-C search binds it.
}

Result<std::vector<SearchHit>, ProviderError> Hianime::search(std::string_view query,
                                                              const SearchOptions& opts) const {
  std::string url = api_ + "/search?keyword=" + detail::url_encode(query);
  if (opts.page > 1) url += "&page=" + std::to_string(opts.page);
  auto html = page_get(url);
  if (!html.has_value()) return err(html.error());

  auto cards = detail::parse_search_cards(*html);
  std::vector<SearchHit> hits;
  hits.reserve(cards.size());
  for (auto& c : cards) {
    SearchHit hit;
    hit.provider_id = std::move(c.id);
    hit.title = std::move(c.title);
    hits.push_back(std::move(hit));
  }
  if (hits.size() > opts.limit) hits.resize(opts.limit);
  return hits;
}

Result<std::vector<std::string>, ProviderError> Hianime::episodes(
    std::string_view provider_id, Translation translation,
    std::optional<std::uint32_t> /*count_hint*/) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  auto eps = fetch_episodes(provider_id);
  if (!eps.has_value()) return err(eps.error());

  std::vector<std::string> out;
  if (translation == Translation::Sub) {
    out.reserve(eps->size());
    for (const auto& e : *eps) out.push_back(std::to_string(e.number));
    return out;
  }
  auto pref = dub_prefix(*eps);
  if (!pref.has_value()) return err(pref.error());
  if (!pref->has_value()) return out;  // no dub at all -> authoritative empty.
  for (std::size_t i = 0; i <= **pref; ++i) out.push_back(std::to_string((*eps)[i].number));
  return out;
}

Result<StreamLink, ProviderError> Hianime::resolve(std::string_view provider_id,
                                                    std::string_view episode,
                                                    Translation translation,
                                                    Quality /*quality*/) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
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
  const EpisodeRow* ep = nullptr;
  for (const auto& e : *eps) {
    if (e.number == want) {
      ep = &e;
      break;
    }
  }
  if (ep == nullptr) return err(ProviderError::decode("no such episode"));

  const std::string servers_url = api_ + "/api/theme/episode/servers?episodeId=" + ep->id;
  auto raw = json_get(servers_url);
  if (!raw.has_value()) return err(raw.error());
  auto hash = detail::find_zokoanime_hash(
      std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()), translation);
  if (!hash.has_value()) return err(hash.error());
  if (!hash->has_value()) return err(ProviderError::decode("no stream for track"));

  auto embed_url = detail::decode_hash(**hash);
  if (!embed_url.has_value() || !detail::stream_url_ok(*embed_url)) {
    return err(ProviderError::decode("blocked embed url"));
  }

  auto html = page_get(*embed_url);
  if (!html.has_value()) return err(html.error());
  auto blob = detail::extract_p_blob(*html);
  if (!blob.has_value()) return err(ProviderError::decode("no playable source"));
  auto raw_bytes = crypto::base64_decode(*blob);
  if (!raw_bytes.has_value()) return err(ProviderError::decode("bad embed payload"));
  auto plain = detail::xor_repeat(*raw_bytes, kEmbedKey);
  auto embed = detail::parse_embed(
      std::string_view(reinterpret_cast<const char*>(plain.data()), plain.size()));
  if (!embed.has_value()) return err(embed.error());
  if (!embed->src.has_value() || !detail::stream_url_ok(*embed->src)) {
    return err(ProviderError::decode("no playable source"));
  }

  StreamLink link;
  link.url = *embed->src;
  link.referer = detail::origin_of(*embed_url).value_or(std::string(kReferer));
  link.user_agent = std::string(http::kBrowserUserAgent);
  for (const auto& sub : embed->subtitles) {
    if (sub.is_default && detail::stream_url_ok(sub.src)) {
      link.sub_url = sub.src;
      break;
    }
  }
  return link;
}

Result<CoverRequest, ProviderError> Hianime::cover_request(std::string_view cover_ref) const {
  if (cover_ref.empty() || cover_ref.size() > kMaxCoverRefLen || !is_absolute_url(cover_ref) ||
      !clean_arg(cover_ref)) {
    return err(ProviderError::decode("invalid cover ref"));
  }
  CoverRequest req;
  req.url = std::string(cover_ref);
  req.referer = std::string(kReferer);
  req.user_agent = std::string(http::kBrowserUserAgent);
  return req;
}

}  // namespace shigoku::hianime
