// animehub.cpp — 123animehub.cc provider.

#include "animehub.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "provider.hpp"  // clean_arg.

namespace shigoku::animehub {
namespace detail {

namespace {

// Value of `name="..."` inside one tag (nullopt if absent/unterminated). The
// match must start the attribute, so `data-name=` never answers for `name=`.
std::optional<std::string> attr_value(std::string_view tag, std::string_view name) {
  const std::string needle = std::string(name) + "=\"";
  for (auto at = tag.find(needle); at != std::string_view::npos;
       at = tag.find(needle, at + 1)) {
    if (at > 0 && tag[at - 1] != ' ' && tag[at - 1] != '\t' && tag[at - 1] != '\n' &&
        tag[at - 1] != '\r') {
      continue;
    }
    std::string_view rest = tag.substr(at + needle.size());
    const auto end = rest.find('"');
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(rest.substr(0, end));
  }
  return std::nullopt;
}

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

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto ca = static_cast<unsigned char>(a[i]);
    const auto cb = static_cast<unsigned char>(b[i]);
    const auto la = (ca >= 'A' && ca <= 'Z') ? ca + 32 : ca;
    const auto lb = (cb >= 'A' && cb <= 'Z') ? cb + 32 : cb;
    if (la != lb) return false;
  }
  return true;
}

// The slug of an `/anime/<slug>` href, site-relative or absolute.
std::optional<std::string> href_slug(std::string_view href) {
  if (const auto scheme = href.find("://"); scheme != std::string_view::npos) {
    const auto path = href.find('/', scheme + 3);
    if (path == std::string_view::npos) return std::nullopt;
    href = href.substr(path);
  }
  constexpr std::string_view kPrefix = "/anime/";
  if (href.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
  std::string_view slug = href.substr(kPrefix.size());
  if (const auto end = slug.find_first_of("/?#"); end != std::string_view::npos) {
    slug = slug.substr(0, end);
  }
  if (!valid_slug(slug)) return std::nullopt;
  return std::string(slug);
}

// "Ep 10" -> 10; anything else -> 0.
std::uint32_t ep_count(std::string_view text) {
  text = trim(text);
  if (text.substr(0, 3) != "Ep ") return 0;
  text = trim(text.substr(3));
  if (text.empty() || text.size() > 6) return 0;
  std::uint32_t n = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return 0;
    n = n * 10 + static_cast<std::uint32_t>(c - '0');
  }
  return n;
}

// A dub card's name carries the track as a suffix: " (Dub)" or " Dub".
std::string strip_dub_suffix(std::string name) {
  for (const std::string_view suffix : {std::string_view(" (Dub)"), std::string_view(" Dub")}) {
    if (ends_with(name, suffix)) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name;
}

Card parse_card(std::string_view block) {
  Card card;
  const auto name_at = block.find("class=\"name\"");
  if (name_at == std::string_view::npos) return card;
  const auto tag_start = block.rfind("<a", name_at);
  const auto tag_end = block.find('>', name_at);
  if (tag_start == std::string_view::npos || tag_end == std::string_view::npos) return card;
  const std::string_view tag = block.substr(tag_start, tag_end - tag_start);
  auto href = attr_value(tag, "href");
  if (!href) return card;
  auto slug = href_slug(*href);
  if (!slug) return card;
  std::string_view text = block.substr(tag_end + 1);
  if (const auto close = text.find("</a>"); close != std::string_view::npos) {
    text = text.substr(0, close);
  }
  card.name = decode_entities(trim(text));
  if (card.name.empty()) return card;
  card.slug = std::move(*slug);
  if (block.find("<span class=\"dub\">") != std::string_view::npos) {
    card.track = Translation::Dub;
  }
  constexpr std::string_view kEp = "<div class=\"ep\">";
  if (const auto ep = block.find(kEp); ep != std::string_view::npos) {
    std::string_view ep_text = block.substr(ep + kEp.size());
    ep_text = ep_text.substr(0, ep_text.find('<'));
    card.episodes = ep_count(ep_text);
  }
  return card;
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

std::vector<Card> parse_search_cards(std::string_view html) {
  const auto sidebar = html.find("id=\"sidebar\"");
  const auto list = html.find("class=\"film-list\"");
  std::string_view scoped = html.substr(0, sidebar);
  if (list != std::string_view::npos && (sidebar == std::string_view::npos || list < sidebar)) {
    scoped = scoped.substr(list);
    if (const auto paging = scoped.find("class=\"paging-wrapper\"");
        paging != std::string_view::npos) {
      scoped = scoped.substr(0, paging);
    }
  }

  constexpr std::string_view kItem = "<div class=\"item\">";
  std::vector<Card> out;
  for (auto at = scoped.find(kItem); at != std::string_view::npos;) {
    const auto next = scoped.find(kItem, at + kItem.size());
    const std::string_view block = scoped.substr(at, next == std::string_view::npos
                                                         ? std::string_view::npos
                                                         : next - at);
    Card card = parse_card(block);
    if (!card.slug.empty()) out.push_back(std::move(card));
    at = next;
  }
  return out;
}

std::vector<SearchHit> merge_cards(const std::vector<Card>& cards) {
  struct Folded {
    SearchHit hit;
    std::string sub_name;
    std::string dub_name;
  };
  std::vector<Folded> folded;
  for (const Card& c : cards) {
    std::string base = c.slug;
    if (c.track == Translation::Dub && ends_with(base, kDubSuffix)) {
      base.resize(base.size() - kDubSuffix.size());
    }
    if (!valid_slug(base)) continue;

    auto it = std::find_if(folded.begin(), folded.end(),
                           [&](const Folded& f) { return f.hit.provider_id == base; });
    if (it == folded.end()) {
      Folded f;
      f.hit.provider_id = base;
      f.hit.title = slug_title(base);
      folded.push_back(std::move(f));
      it = folded.end() - 1;
    }
    if (c.track == Translation::Sub) {
      it->hit.eps_sub = std::max(it->hit.eps_sub, c.episodes);
      if (it->sub_name.empty()) it->sub_name = c.name;
    } else {
      it->hit.eps_dub = std::max(it->hit.eps_dub, c.episodes);
      if (it->dub_name.empty()) it->dub_name = strip_dub_suffix(c.name);
    }
  }

  std::vector<SearchHit> hits;
  hits.reserve(folded.size());
  for (auto& f : folded) {
    std::string& name = f.sub_name.empty() ? f.dub_name : f.sub_name;
    if (!name.empty() && !iequals(name, f.hit.title)) f.hit.title_english = std::move(name);
    hits.push_back(std::move(f.hit));
  }
  return hits;
}

std::string search_query(std::string_view query) {
  std::string out;
  out.reserve(query.size());
  bool pending_space = false;
  for (const char ch : query) {
    const auto c = static_cast<unsigned char>(ch);
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-';
    if (!keep) {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty()) out.push_back(' ');
    pending_space = false;
    out.push_back(ch);
  }
  return out;
}

bool valid_slug(std::string_view slug) {
  if (slug.empty() || slug.size() > kMaxSlugLen) return false;
  if (slug.front() == '-' || slug.back() == '-') return false;
  for (const char c : slug) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string slug_title(std::string_view slug) {
  std::string out;
  out.reserve(slug.size());
  bool word_start = true;
  for (const char c : slug) {
    if (c == '-') {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      word_start = true;
      continue;
    }
    out.push_back(word_start && c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c);
    word_start = false;
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string track_slug(std::string_view slug, Translation translation) {
  std::string out(slug);
  if (translation == Translation::Dub) out += kDubSuffix;
  return out;
}

Result<Film, ProviderError> parse_film(std::string_view raw_json, std::string_view track_slug) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(raw_json);
  } catch (const nlohmann::json::exception& e) {
    return err(ProviderError::decode(std::string("episode grid: ") + e.what()));
  }
  if (!j.is_object() || !j.contains("html") || !j["html"].is_string()) {
    return err(ProviderError::decode("episode grid: no html field"));
  }
  const std::string html = j["html"].get<std::string>();

  Film film;
  const std::string needle = "data-id=\"" + std::string(track_slug) + "/";
  for (auto at = html.find(needle); at != std::string::npos;
       at = html.find(needle, at + needle.size())) {
    const auto start = at + needle.size();
    const auto end = html.find('"', start);
    if (end == std::string::npos) break;
    std::string label = html.substr(start, end - start);
    if (valid_label(label)) film.episodes.push_back(std::move(label));
  }
  std::stable_sort(film.episodes.begin(), film.episodes.end(),
                   [](const std::string& a, const std::string& b) { return label_less(a, b); });
  film.episodes.erase(std::unique(film.episodes.begin(), film.episodes.end()),
                      film.episodes.end());

  constexpr std::string_view kTab = "class=\"tip tab";
  for (auto at = html.find(kTab); at != std::string::npos; at = html.find(kTab, at + 1)) {
    const auto tag_start = html.rfind('<', at);
    const auto tag_end = html.find('>', at);
    if (tag_start == std::string::npos || tag_end == std::string::npos) break;
    auto id = attr_value(std::string_view(html).substr(tag_start, tag_end - tag_start),
                         "data-name");
    if (!id || id->empty() || id->size() > 8) continue;
    if (!std::all_of(id->begin(), id->end(), [](char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    if (std::find(film.servers.begin(), film.servers.end(), *id) == film.servers.end()) {
      film.servers.push_back(std::move(*id));
    }
  }
  return film;
}

bool valid_label(std::string_view label) {
  if (label.empty() || label.size() > 16) return false;
  bool dot = false;
  bool digit_since_dot = false;
  for (const char c : label) {
    if (c == '.') {
      if (dot || !digit_since_dot) return false;
      dot = true;
      digit_since_dot = false;
      continue;
    }
    if (c < '0' || c > '9') return false;
    digit_since_dot = true;
  }
  return digit_since_dot;
}

bool label_less(std::string_view a, std::string_view b) {
  auto split = [](std::string_view s) {
    const auto dot = s.find('.');
    std::string_view whole = s.substr(0, dot);
    std::string_view frac = dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);
    while (whole.size() > 1 && whole.front() == '0') whole.remove_prefix(1);
    return std::pair<std::string_view, std::string_view>(whole, frac);
  };
  const auto [wa, fa] = split(a);
  const auto [wb, fb] = split(b);
  if (wa.size() != wb.size()) return wa.size() < wb.size();
  if (wa != wb) return wa < wb;
  const std::size_t n = std::max(fa.size(), fb.size());
  for (std::size_t i = 0; i < n; ++i) {
    const char ca = i < fa.size() ? fa[i] : '0';
    const char cb = i < fb.size() ? fb[i] : '0';
    if (ca != cb) return ca < cb;
  }
  return false;
}

std::optional<std::string> parse_target(std::string_view raw_json) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(raw_json);
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
  if (!j.is_object()) return std::nullopt;
  const auto it = j.find("target");
  if (it == j.end() || !it->is_string()) return std::nullopt;
  std::string target = it->get<std::string>();
  if (target.empty()) return std::nullopt;
  return target;
}

std::optional<std::string> extract_zrpart2(std::string_view html) {
  constexpr std::string_view kVar = "zrpart2";
  for (auto at = html.find(kVar); at != std::string_view::npos;
       at = html.find(kVar, at + kVar.size())) {
    std::string_view rest = html.substr(at + kVar.size());
    rest = rest.substr(std::min(rest.find_first_not_of(" \t"), rest.size()));
    if (rest.empty() || rest.front() != '=') continue;
    rest = rest.substr(1);
    rest = rest.substr(std::min(rest.find_first_not_of(" \t"), rest.size()));
    if (rest.empty() || (rest.front() != '\'' && rest.front() != '"')) continue;
    const char quote = rest.front();
    rest = rest.substr(1);
    const auto end = rest.find(quote);
    if (end == std::string_view::npos || end == 0 || end > 4096) return std::nullopt;
    const std::string_view token = rest.substr(0, end);
    const bool ok = std::all_of(token.begin(), token.end(), [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
             c == '+' || c == '/' || c == '=' || c == '_' || c == '-';
    });
    if (!ok) return std::nullopt;
    return std::string(token);
  }
  return std::nullopt;
}

std::optional<std::string> parse_sources(std::string_view html) {
  const auto at = html.find("id=\"sources\"");
  if (at == std::string_view::npos) return std::nullopt;
  const auto open_end = html.find('>', at);
  if (open_end == std::string_view::npos) return std::nullopt;
  std::string_view body = html.substr(open_end + 1);
  const auto close = body.find("</div>");
  if (close == std::string_view::npos) return std::nullopt;
  body = trim(body.substr(0, close));

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
  if (!j.is_object()) return std::nullopt;
  const auto it = j.find("sources");
  if (it == j.end()) return std::nullopt;
  const nlohmann::json* src = &*it;
  if (src->is_array()) {
    if (src->empty()) return std::nullopt;
    src = &(*src)[0];
  }
  if (src->is_object()) {
    const auto file = src->find("file");
    if (file == src->end()) return std::nullopt;
    src = &*file;
  }
  if (!src->is_string()) return std::nullopt;
  std::string url = src->get<std::string>();
  if (url.empty()) return std::nullopt;
  return url;
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
  const auto path_start = absolute_url.find_first_of("/?#", scheme_end + 3);
  const std::string_view origin = absolute_url.substr(0, path_start);
  if (origin.size() == scheme_end + 3) return std::nullopt;
  return std::string(origin);
}

}  // namespace detail

// ===========================================================================
// The provider transport.
// ===========================================================================

Result<AnimeHub, ProviderError> AnimeHub::create() {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return AnimeHub(std::move(*client), kBase);
}

Result<AnimeHub, ProviderError> AnimeHub::with_endpoint(std::string base) {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return AnimeHub(std::move(*client), std::move(base));
}

Result<std::vector<std::uint8_t>, ProviderError> AnimeHub::json_get(
    const std::string& url, const std::string& referer) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.extra_headers.push_back({"Referer", referer});
  req.extra_headers.push_back({"Accept", "application/json, text/javascript, */*; q=0.01"});
  req.extra_headers.push_back({"X-Requested-With", "XMLHttpRequest"});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
}

Result<std::string, ProviderError> AnimeHub::page_get(const std::string& url,
                                                      const std::string& referer) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.extra_headers.push_back({"Referer", referer});
  req.extra_headers.push_back(
      {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"});
  req.accept = http::Accept::Any2xx;
  auto raw = http_.fetch(req);
  if (!raw.has_value()) return err(raw.error());
  return std::string(raw->begin(), raw->end());
}

std::string AnimeHub::show_url(std::string_view track_slug) const {
  return base_ + "/anime/" + std::string(track_slug);
}

Result<detail::Film, ProviderError> AnimeHub::fetch_film(const std::string& track_slug) const {
  auto raw = json_get(base_ + "/ajax/film/sv?id=" + track_slug, show_url(track_slug));
  if (!raw.has_value()) return err(raw.error());
  return detail::parse_film(
      std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()), track_slug);
}

Result<StreamLink, ProviderError> AnimeHub::resolve_on(const std::string& track_slug,
                                                       std::string_view episode,
                                                       const std::string& server) const {
  const std::string show = show_url(track_slug);
  auto info = json_get(base_ + "/ajax/episode/info?epr=" + track_slug + "/" +
                           std::string(episode) + "/" + server,
                       show);
  if (!info.has_value()) return err(info.error());
  auto target = detail::parse_target(
      std::string_view(reinterpret_cast<const char*>(info->data()), info->size()));
  if (!target.has_value()) return err(ProviderError::decode("no embed"));
  const auto origin = detail::origin_of(*target);
  if (!origin.has_value() || !detail::stream_url_ok(*target)) {
    return err(ProviderError::decode("blocked embed url"));
  }

  auto embed = page_get(*target, show);
  if (!embed.has_value()) return err(embed.error());
  auto token = detail::extract_zrpart2(*embed);
  if (!token.has_value()) return err(ProviderError::decode("no playable source"));

  auto player = page_get(*origin + "/hs/" + *token + "?pl_usn=1", *target);
  if (!player.has_value()) return err(player.error());
  auto master = detail::parse_sources(*player);
  if (!master.has_value() || !detail::stream_url_ok(*master)) {
    return err(ProviderError::decode("no playable source"));
  }

  StreamLink link;
  link.url = std::move(*master);
  link.referer = *origin + "/";
  link.user_agent = std::string(http::kBrowserUserAgent);
  link.cloaked_segments = true;
  return link;
}

// -- StreamProvider surface --

std::optional<std::string> AnimeHub::canonical_key(const Enrichment& /*show*/) const {
  return std::nullopt;
}

Result<std::vector<SearchHit>, ProviderError> AnimeHub::search(std::string_view query,
                                                               const SearchOptions& opts) const {
  const std::string keyword = detail::search_query(query);
  if (keyword.empty()) return std::vector<SearchHit>{};
  std::string url = base_ + "/search?keyword=" + detail::url_encode(keyword);
  if (opts.page > 1) url += "&page=" + std::to_string(opts.page);
  auto html = page_get(url, kReferer);
  if (!html.has_value()) return err(html.error());

  auto hits = detail::merge_cards(detail::parse_search_cards(*html));
  if (hits.size() > opts.limit) hits.resize(opts.limit);
  return hits;
}

Result<std::vector<std::string>, ProviderError> AnimeHub::episodes(
    std::string_view provider_id, Translation translation,
    std::optional<std::uint32_t> /*count_hint*/) const {
  if (!detail::valid_slug(provider_id)) return err(ProviderError::decode("invalid show id"));
  const std::string slug = detail::track_slug(provider_id, translation);
  auto film = fetch_film(slug);
  if (film.has_value()) return std::move(film->episodes);

  if (translation == Translation::Dub && film.error().kind == ProviderError::Kind::Server) {
    auto page = page_get(show_url(slug), kReferer);
    if (!page.has_value() && page.error().kind == ProviderError::Kind::Http &&
        page.error().status == 404) {
      return std::vector<std::string>{};
    }
  }
  return err(film.error());
}

Result<StreamLink, ProviderError> AnimeHub::resolve(std::string_view provider_id,
                                                    std::string_view episode,
                                                    Translation translation,
                                                    Quality /*quality*/) const {
  if (!detail::valid_slug(provider_id)) return err(ProviderError::decode("invalid show id"));
  if (!detail::valid_label(episode)) return err(ProviderError::decode("invalid episode"));
  const std::string slug = detail::track_slug(provider_id, translation);

  auto film = fetch_film(slug);
  if (!film.has_value()) return err(film.error());
  if (std::find(film->episodes.begin(), film->episodes.end(), episode) ==
      film->episodes.end()) {
    return err(ProviderError::decode("no such episode"));
  }

  std::vector<std::string> servers = film->servers;
  if (servers.empty()) servers.emplace_back(kDefaultServer);
  std::optional<ProviderError> last;
  for (const auto& server : servers) {
    auto link = resolve_on(slug, episode, server);
    if (link.has_value()) return link;
    last = link.error();
  }
  return err(*last);
}

Result<CoverRequest, ProviderError> AnimeHub::cover_request(std::string_view cover_ref) const {
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

}  // namespace shigoku::animehub
