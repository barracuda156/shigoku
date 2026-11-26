// miruro.cpp — see miruro.hpp.

#include "miruro.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "hls.hpp"
#include "scrape.hpp"
#include "uneval.hpp"

namespace shigoku::miruro {

namespace {

using json = nlohmann::json;

constexpr std::string_view kChallenge = "<title>Just a moment";
constexpr std::string_view kKitStart = "kit.start(";
constexpr std::string_view kResolve = ".resolve(";
constexpr std::size_t kMaxParts = 64;
constexpr int kFindDepth = 4;
constexpr std::size_t kMaxScan = 4096;
constexpr std::size_t kMaxItems = 256;
constexpr std::size_t kMaxEpisodes = kMaxEpisodeHint;
constexpr std::size_t kMaxTracks = 16;
constexpr std::size_t kMaxSources = 64;
constexpr std::size_t kMaxServers = 64;
constexpr std::size_t kMaxStreams = 16;
constexpr std::size_t kMaxSubtitles = 64;
constexpr std::size_t kMaxHeaders = 32;
constexpr std::size_t kMaxRenditions = 256;
constexpr std::size_t kMaxSlugs = 32;
constexpr std::string_view kPlaceholderSlug = "x";

bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool is_challenge(std::string_view html) { return html.find(kChallenge) != std::string_view::npos; }

void skip_spaces(std::string_view s, std::size_t& i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

// Where each value expression starts: the kit.start data, then every
// streamed part, in page order.
std::vector<std::size_t> payload_starts(std::string_view html) {
  std::vector<std::size_t> starts;
  if (const auto kit = html.find(kKitStart); kit != std::string_view::npos) {
    if (const auto d = html.find("data:", kit + kKitStart.size()); d != std::string_view::npos) {
      starts.push_back(d + 5);
    }
  }
  std::size_t parts = 0;
  for (auto at = html.find(kResolve); at != std::string_view::npos && parts < kMaxParts;
       at = html.find(kResolve, at + 1)) {
    std::size_t i = at + kResolve.size();
    const std::size_t digits_from = i;
    while (i < html.size() && is_digit(html[i]) && i - digits_from < 10) ++i;
    if (i == digits_from) continue;
    skip_spaces(html, i);
    if (i >= html.size() || html[i] != ',') continue;
    ++i;
    skip_spaces(html, i);
    if (html.substr(i, 2) != "()") continue;
    i += 2;
    skip_spaces(html, i);
    if (html.substr(i, 2) != "=>") continue;
    starts.push_back(i + 2);
    ++parts;
  }
  std::sort(starts.begin(), starts.end());
  return starts;
}

// Nodes in `v`, counting stops past `budget`.
std::size_t weigh(const json& v, std::size_t budget) {
  std::size_t n = 1;
  if (v.is_structured()) {
    for (const auto& child : v) {
      if (n > budget) break;
      n += weigh(child, budget - n);
    }
  }
  return n;
}

// Every payload that parses, until the page's node budget is spent.
std::vector<json> payloads(std::string_view html) {
  std::vector<json> out;
  std::size_t spent = 0;
  for (const std::size_t at : payload_starts(html)) {
    if (spent >= uneval::kMaxNodes) break;
    auto v = uneval::parse(html.substr(at));
    if (!v.has_value()) continue;
    spent += weigh(*v, uneval::kMaxNodes);
    out.push_back(std::move(*v));
  }
  return out;
}

// The first object, depth first, that holds `key` as an array.
const json* find_key(const json& v, const char* key, int depth) {
  if (v.is_object()) {
    const auto it = v.find(key);
    if (it != v.end() && it->is_array()) return &*it;
  }
  if (depth >= kFindDepth || !v.is_structured()) return nullptr;
  std::size_t n = 0;
  for (const auto& child : v) {
    if (++n > kMaxScan) break;
    if (const json* hit = find_key(child, key, depth + 1)) return hit;
  }
  return nullptr;
}

const json* find_array(const std::vector<json>& all, const char* key) {
  for (const auto& p : all) {
    if (const json* hit = find_key(p, key, 0)) return hit;
  }
  return nullptr;
}

const json* member(const json& obj, const char* key) {
  if (!obj.is_object()) return nullptr;
  const auto it = obj.find(key);
  return it == obj.end() ? nullptr : &*it;
}

std::optional<std::string> str_at(const json& obj, const char* key) {
  const json* v = member(obj, key);
  if (v == nullptr || !v->is_string()) return std::nullopt;
  return v->get<std::string>();
}

std::optional<std::string> nonempty_at(const json& obj, const char* key) {
  auto s = str_at(obj, key);
  if (!s.has_value() || s->empty()) return std::nullopt;
  return s;
}

std::optional<std::int64_t> int_at(const json& obj, const char* key) {
  const json* v = member(obj, key);
  if (v == nullptr) return std::nullopt;
  if (v->is_number_unsigned()) {
    const auto u = v->get<std::uint64_t>();
    if (u > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
    return static_cast<std::int64_t>(u);
  }
  if (v->is_number_integer()) return v->get<std::int64_t>();
  return std::nullopt;
}

std::optional<std::uint32_t> u32_of(std::optional<std::int64_t> n) {
  if (!n.has_value() || *n < 0 || *n > static_cast<std::int64_t>(UINT32_MAX)) return std::nullopt;
  return static_cast<std::uint32_t>(*n);
}

// A positive decimal id, as a string of digits or a number.
std::optional<std::int64_t> positive_id(const json& v) {
  if (v.is_number_integer()) {
    if (v.is_number_unsigned()) {
      const auto u = v.get<std::uint64_t>();
      if (u == 0 || u > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
      return static_cast<std::int64_t>(u);
    }
    const auto n = v.get<std::int64_t>();
    return n > 0 ? std::optional<std::int64_t>(n) : std::nullopt;
  }
  if (!v.is_string()) return std::nullopt;
  const auto& s = v.get_ref<const std::string&>();
  if (s.empty() || s.size() > 18 || s.front() == '0') return std::nullopt;
  std::int64_t n = 0;
  for (const char c : s) {
    if (!is_digit(c)) return std::nullopt;
    n = n * 10 + (c - '0');
  }
  return n;
}

std::optional<std::int64_t> first_id(const json& ids, const char* key) {
  const json* list = member(ids, key);
  if (list == nullptr || !list->is_array() || list->empty()) return std::nullopt;
  return positive_id(list->front());
}

// An episode number in hundredths, the unit labels and ordering use.
std::optional<std::int64_t> episode_cents(double number) {
  if (!std::isfinite(number) || number < 0 || number >= 10000.0) return std::nullopt;
  const double scaled = std::floor(number * 100.0 + 0.5);
  if (scaled >= 1000000.0) return std::nullopt;
  return static_cast<std::int64_t>(scaled);
}

std::string cents_label(std::int64_t cents) {
  std::string out = std::to_string(cents / 100);
  const int frac = static_cast<int>(cents % 100);
  if (frac == 0) return out;
  out.push_back('.');
  out.push_back(static_cast<char>('0' + frac / 10));
  if (frac % 10 != 0) out.push_back(static_cast<char>('0' + frac % 10));
  return out;
}

Result<std::vector<SearchHit>, ProviderError> search_from(const std::vector<json>& all) {
  const json* items = find_array(all, "items");
  if (items == nullptr) return err(ProviderError::decode("search: no items"));
  std::vector<SearchHit> out;
  std::size_t seen = 0;
  for (const auto& it : *items) {
    if (++seen > kMaxItems) break;
    if (!it.is_object()) continue;
    auto id = str_at(it, "id");
    if (!id.has_value() || !detail::valid_show_id(*id)) continue;
    std::optional<std::string> romaji, english, native;
    if (const json* t = member(it, "title"); t != nullptr && t->is_object()) {
      romaji = nonempty_at(*t, "romaji");
      english = nonempty_at(*t, "english");
      native = nonempty_at(*t, "native");
    }
    const std::optional<std::string>& shown =
        romaji.has_value() ? romaji : (english.has_value() ? english : native);
    if (!shown.has_value()) continue;
    SearchHit hit;
    hit.provider_id = std::move(*id);
    hit.title = *shown;
    hit.title_english = std::move(english);
    hit.title_native = std::move(native);
    if (const json* ext = member(it, "external_ids"); ext != nullptr && ext->is_object()) {
      hit.anilist_id = first_id(*ext, "anilist");
      hit.mal_id = first_id(*ext, "mal");
    }
    if (auto n = u32_of(int_at(it, "episode_count")); n.has_value() && *n > 0) {
      hit.total_episodes = n;
    }
    if (const json* c = member(it, "episode_counts"); c != nullptr && c->is_object()) {
      hit.eps_sub = u32_of(int_at(*c, "sub")).value_or(0);
      hit.eps_dub = u32_of(int_at(*c, "dub")).value_or(0);
    }
    if (auto y = u32_of(int_at(it, "season_year")); y.has_value() && *y > 0) hit.year = y;
    out.push_back(std::move(hit));
  }
  return out;
}

Result<std::vector<std::string>, ProviderError> episodes_from(const std::vector<json>& all) {
  const json* eps = find_array(all, "episodes");
  if (eps == nullptr) return err(ProviderError::decode("watch: no episodes"));
  std::vector<std::int64_t> cents;
  std::size_t seen = 0;
  for (const auto& e : *eps) {
    if (++seen > kMaxEpisodes) break;
    const json* n = member(e, "number");
    if (n == nullptr || !n->is_number()) continue;
    if (auto c = episode_cents(n->get<double>()); c.has_value()) cents.push_back(*c);
  }
  std::sort(cents.begin(), cents.end());
  cents.erase(std::unique(cents.begin(), cents.end()), cents.end());
  std::vector<std::string> out;
  out.reserve(cents.size());
  for (const auto c : cents) out.push_back(cents_label(c));
  return out;
}

void read_headers(const json& server, detail::Stream& into) {
  const json* h = member(server, "headers");
  if (h == nullptr || !h->is_object()) return;
  std::size_t n = 0;
  for (auto it = h->begin(); it != h->end() && n < kMaxHeaders; ++it, ++n) {
    if (!it->is_string() || it->get_ref<const std::string&>().empty()) continue;
    const std::string key = lower(it.key());
    if (key == "referer" && !into.referer.has_value()) into.referer = it->get<std::string>();
    if (key == "origin" && !into.origin.has_value()) into.origin = it->get<std::string>();
  }
}

detail::Source read_source(const json& p) {
  detail::Source src;
  src.name = str_at(p, "provider").value_or("");
  if (const json* servers = member(p, "servers"); servers != nullptr && servers->is_array()) {
    std::size_t ns = 0;
    for (const auto& sv : *servers) {
      if (++ns > kMaxServers) break;
      if (!sv.is_object()) continue;
      detail::Stream proto;
      proto.server = str_at(sv, "server").value_or("");
      read_headers(sv, proto);
      const json* streams = member(sv, "streams");
      if (streams == nullptr || !streams->is_array()) continue;
      std::size_t nt = 0;
      for (const auto& st : *streams) {
        if (++nt > kMaxStreams) break;
        auto url = str_at(st, "url");
        if (!url.has_value()) continue;
        detail::Stream s = proto;
        s.url = std::move(*url);
        s.hls = lower(str_at(st, "format").value_or("")) == "hls";
        src.streams.push_back(std::move(s));
      }
    }
  }
  if (const json* subs = member(p, "subtitles"); subs != nullptr && subs->is_array()) {
    std::size_t nb = 0;
    for (const auto& sb : *subs) {
      if (++nb > kMaxSubtitles) break;
      auto file = str_at(sb, "file");
      if (!file.has_value()) continue;
      detail::Subtitle sub;
      sub.url = std::move(*file);
      sub.language = str_at(sb, "language").value_or("");
      sub.label = str_at(sb, "label").value_or("");
      const json* def = member(sb, "default");
      sub.is_default = def != nullptr && def->is_boolean() && def->get<bool>();
      src.subtitles.push_back(std::move(sub));
    }
  }
  return src;
}

Result<std::vector<detail::Track>, ProviderError> tracks_from(const std::vector<json>& all) {
  const json* tracks = find_array(all, "tracks");
  if (tracks == nullptr) return err(ProviderError::decode("watch: no tracks"));
  std::vector<detail::Track> out;
  std::size_t nt = 0;
  for (const auto& t : *tracks) {
    if (++nt > kMaxTracks) break;
    auto kind = str_at(t, "track");
    if (!kind.has_value()) continue;
    detail::Track track;
    track.kind = std::move(*kind);
    if (const json* providers = member(t, "providers"); providers != nullptr && providers->is_array()) {
      std::size_t np = 0;
      for (const auto& p : *providers) {
        if (++np > kMaxSources) break;
        if (!p.is_object()) continue;
        track.sources.push_back(read_source(p));
      }
    }
    out.push_back(std::move(track));
  }
  return out;
}

bool is_english_tag(std::string_view language) {
  const std::string l = lower(language);
  return l == "en" || l == "eng" || l == "english" || l.starts_with("en-");
}

bool fetchable(std::string_view url) {
  return clean_arg(url) && http::guard_fetch_url(url).has_value();
}

bool origin_shaped(std::string_view o) {
  std::string_view rest;
  if (o.starts_with("https://")) {
    rest = o.substr(8);
  } else if (o.starts_with("http://")) {
    rest = o.substr(7);
  } else {
    return false;
  }
  const auto colon = rest.find(':');
  const std::string_view host = rest.substr(0, colon);
  if (host.empty()) return false;
  for (const char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' ||
                    c == '.' || c == '_';
    if (!ok) return false;
  }
  if (colon == std::string_view::npos) return true;
  const std::string_view port = rest.substr(colon + 1);
  if (port.empty() || port.size() > 5) return false;
  return std::all_of(port.begin(), port.end(), is_digit);
}

std::optional<detail::Stream> qualify(const detail::Stream& s) {
  if (!s.url.starts_with("https://") || !fetchable(s.url)) return std::nullopt;
  if (s.referer.has_value() && !clean_arg(*s.referer)) return std::nullopt;
  if (s.origin.has_value() && !clean_arg(*s.origin)) return std::nullopt;
  detail::Stream out = s;
  if (out.origin.has_value() && !origin_shaped(*out.origin)) out.origin.reset();
  return out;
}

std::string_view as_text(const std::vector<std::uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

namespace detail {

bool valid_show_id(std::string_view id) {
  if (id.size() < 8 || id.size() > 64) return false;
  return std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c) || c == '_' || c == '-';
  });
}

bool valid_episode(std::string_view label) {
  const auto dot = label.find('.');
  const std::string_view whole = label.substr(0, dot);
  if (whole.empty() || whole.size() > 4 || !std::all_of(whole.begin(), whole.end(), is_digit)) {
    return false;
  }
  if (dot == std::string_view::npos) return true;
  const std::string_view frac = label.substr(dot + 1);
  return !frac.empty() && frac.size() <= 2 && std::all_of(frac.begin(), frac.end(), is_digit);
}

std::optional<std::string> episode_label(double number) {
  const auto cents = episode_cents(number);
  if (!cents.has_value()) return std::nullopt;
  return cents_label(*cents);
}

Result<std::vector<SearchHit>, ProviderError> parse_search(std::string_view html) {
  if (is_challenge(html)) return err(ProviderError::forbidden(403));
  return search_from(payloads(html));
}

Result<std::vector<std::string>, ProviderError> parse_episodes(std::string_view html) {
  if (is_challenge(html)) return err(ProviderError::forbidden(403));
  return episodes_from(payloads(html));
}

Result<std::vector<Track>, ProviderError> parse_tracks(std::string_view html) {
  if (is_challenge(html)) return err(ProviderError::forbidden(403));
  return tracks_from(payloads(html));
}

std::optional<std::string> pick_subtitle(const std::vector<Subtitle>& subs) {
  const Subtitle* first = nullptr;
  for (const auto& s : subs) {
    const bool english = s.language.empty() ? lower(s.label).starts_with("english")
                                            : is_english_tag(s.language);
    if (!english || !fetchable(s.url)) continue;
    if (s.is_default) return s.url;
    if (first == nullptr) first = &s;
  }
  if (first == nullptr) return std::nullopt;
  return first->url;
}

std::vector<Candidate> candidates(const std::vector<Track>& tracks, Translation translation) {
  std::vector<Candidate> out;
  std::set<std::string> listed;
  auto add = [&](const Source& src, const std::optional<std::string>& sub) {
    for (const auto& s : src.streams) {
      auto q = qualify(s);
      if (!q.has_value() || !listed.insert(q->url).second) continue;
      out.push_back(Candidate{std::move(*q), sub});
    }
  };
  if (translation == Translation::Dub) {
    for (const auto& t : tracks) {
      if (t.kind != "dub") continue;
      for (const auto& src : t.sources) add(src, std::nullopt);
    }
    return out;
  }
  for (const auto& t : tracks) {
    if (t.kind != "ssub") continue;
    for (const auto& src : t.sources) {
      if (auto sub = pick_subtitle(src.subtitles); sub.has_value()) add(src, sub);
    }
  }
  for (const auto& t : tracks) {
    if (t.kind != "sub") continue;
    for (const auto& src : t.sources) add(src, std::nullopt);
  }
  for (const auto& t : tracks) {
    if (t.kind != "ssub") continue;
    for (const auto& src : t.sources) {
      if (!pick_subtitle(src.subtitles).has_value()) add(src, std::nullopt);
    }
  }
  return out;
}

std::optional<std::string> watch_slug(std::string_view url, std::string_view show_id) {
  const auto origin = scrape::origin_of(url);
  if (!origin.has_value()) return std::nullopt;
  std::string_view rest = url.substr(origin->size());
  rest = rest.substr(0, rest.find_first_of("?#"));
  const std::string prefix = "/watch/" + std::string(show_id) + "/";
  if (!rest.starts_with(prefix)) return std::nullopt;
  const std::string_view slug = rest.substr(prefix.size());
  if (slug.empty() || slug.size() > 200) return std::nullopt;
  const bool ok = std::all_of(slug.begin(), slug.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c) || c == '.' ||
           c == '_' || c == '~' || c == '%' || c == '-';
  });
  if (!ok) return std::nullopt;
  return std::string(slug);
}

bool is_playlist(std::string_view body) {
  if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);
  while (!body.empty() && (body.front() == ' ' || body.front() == '\t' || body.front() == '\r' ||
                           body.front() == '\n')) {
    body.remove_prefix(1);
  }
  return body.starts_with("#EXTM3U");
}

std::optional<std::string> dub_audio_language(std::string_view master) {
  bool english = false;
  bool other = false;
  std::size_t n = 0;
  for (const auto& r : hls::parse_audio_renditions(master)) {
    if (++n > kMaxRenditions) break;
    std::string key = lower(r.language.substr(0, r.language.find_first_of("-_")));
    if (key.empty()) continue;
    if (key == "en" || key == "eng") {
      english = true;
    } else {
      other = true;
    }
  }
  if (english && other) return std::string("eng");
  return std::nullopt;
}

}  // namespace detail

// ===========================================================================
// The provider transport.
// ===========================================================================

struct Miruro::State {
  std::atomic<bool> fingerprinted{false};
  // The slugs the site redirected to, newest last.
  std::mutex mu;
  std::vector<std::pair<std::string, std::string>> slugs;

  std::optional<std::string> slug_of(std::string_view show_id) {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [id, slug] : slugs) {
      if (id == show_id) return slug;
    }
    return std::nullopt;
  }

  void remember(std::string_view show_id, std::string slug) {
    std::lock_guard<std::mutex> lock(mu);
    for (auto& [id, known] : slugs) {
      if (id == show_id) {
        known = std::move(slug);
        return;
      }
    }
    if (slugs.size() >= kMaxSlugs) slugs.erase(slugs.begin());
    slugs.emplace_back(std::string(show_id), std::move(slug));
  }
};

Miruro::Miruro(http::Client client, std::string base, std::string stream_origin,
               http::Fingerprint escalate)
    : http_(std::move(client)),
      base_(std::move(base)),
      stream_origin_(std::move(stream_origin)),
      escalate_(escalate),
      state_(std::make_shared<State>()) {}

Result<Miruro, ProviderError> Miruro::create() {
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return Miruro(std::move(*client), kBase, "", http::Fingerprint::Chrome);
}

Result<Miruro, ProviderError> Miruro::with_endpoints(std::string base, std::string stream_origin) {
  while (!base.empty() && base.back() == '/') base.pop_back();
  while (!stream_origin.empty() && stream_origin.back() == '/') stream_origin.pop_back();
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return Miruro(std::move(*client), std::move(base), std::move(stream_origin),
                http::Fingerprint::None);
}

Result<std::string, ProviderError> Miruro::page(const std::string& url,
                                                std::string* landed) const {
  std::string moved;
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.extra_headers.push_back({"Accept", "text/html,application/xhtml+xml,*/*;q=0.8"});
  req.extra_headers.push_back({"Referer", base_ + "/"});
  req.accept = http::Accept::Any2xx;
  req.redirect_target = &moved;
  if (state_->fingerprinted.load(std::memory_order_relaxed)) req.fingerprint = escalate_;

  auto attempt = [&]() -> Result<std::string, ProviderError> {
    auto raw = http_.fetch(req);
    if (!raw.has_value()) return err(raw.error());
    std::string body(as_text(*raw));
    if (is_challenge(body)) return err(ProviderError::forbidden(403));
    return body;
  };
  auto fetch = [&]() -> Result<std::string, ProviderError> {
    auto out = attempt();
    if (!out.has_value() && out.error().kind == ProviderError::Kind::Forbidden &&
        out.error().status == 403 && escalate_ != http::Fingerprint::None &&
        req.fingerprint == http::Fingerprint::None) {
      req.fingerprint = escalate_;
      // A retry that fails leaves the refusal standing: it is what the
      // caller has to hear about, whatever the handshake library then ran
      // into.
      auto again = attempt();
      const bool through = again.has_value() ||
                           (again.error().kind != ProviderError::Kind::Forbidden &&
                            again.error().kind != ProviderError::Kind::Network);
      if (through) {
        state_->fingerprinted.store(true, std::memory_order_relaxed);
        out = std::move(again);
      } else {
        moved.clear();
      }
    }
    return out;
  };

  const auto home = scrape::origin_of(base_);
  auto out = fetch();
  for (int hop = 0; hop < kMaxRedirects; ++hop) {
    const bool redirected = !out.has_value() && out.error().kind == ProviderError::Kind::Http &&
                            out.error().status >= 300 && out.error().status <= 399;
    if (!redirected || moved.empty() || !clean_arg(moved)) break;
    if (!home.has_value() || scrape::origin_of(moved) != home) break;
    req.url = moved;
    out = fetch();
  }
  if (landed != nullptr && out.has_value() && req.url != url) *landed = req.url;
  return out;
}

Result<std::string, ProviderError> Miruro::watch_page(std::string_view show_id,
                                                      std::string_view episode) const {
  const std::string slug = state_->slug_of(show_id).value_or(std::string(kPlaceholderSlug));
  std::string landed;
  auto html = page(base_ + "/watch/" + std::string(show_id) + "/" + slug +
                       "?ep=" + std::string(episode),
                   &landed);
  if (html.has_value() && !landed.empty()) {
    if (auto own = detail::watch_slug(landed, show_id); own.has_value()) {
      state_->remember(show_id, std::move(*own));
    }
  }
  return html;
}

Result<std::string, ProviderError> Miruro::probe(const detail::Stream& stream) const {
  std::string url = stream.url;
  if (!stream_origin_.empty()) {
    if (const auto origin = scrape::origin_of(url); origin.has_value()) {
      url = stream_origin_ + url.substr(origin->size());
    }
  }
  http::Request req;
  req.method = http::Method::Get;
  req.url = std::move(url);
  req.user_agent = http::kBrowserUserAgent;
  req.timeout_secs = kProbeTimeoutSecs;
  if (stream.referer.has_value()) req.extra_headers.push_back({"Referer", *stream.referer});
  if (stream.origin.has_value()) req.extra_headers.push_back({"Origin", *stream.origin});
  req.accept = http::Accept::Any2xx;
  auto raw = http_.fetch(req);
  if (!raw.has_value()) return err(raw.error());
  return std::string(as_text(*raw));
}

std::optional<std::string> Miruro::canonical_key(const Enrichment& /*show*/) const {
  return std::nullopt;
}

Result<std::vector<SearchHit>, ProviderError> Miruro::search(std::string_view query,
                                                             const SearchOptions& opts) const {
  if (opts.page > 1) return std::vector<SearchHit>{};
  auto html = page(base_ + "/search?q=" + scrape::url_encode(query));
  if (!html.has_value()) return err(html.error());
  auto hits = detail::parse_search(*html);
  if (!hits.has_value()) return err(hits.error());
  if (hits->size() > opts.limit) hits->resize(opts.limit);
  return hits;
}

Result<std::vector<std::string>, ProviderError> Miruro::episodes(
    std::string_view provider_id, Translation translation,
    std::optional<std::uint32_t> /*count_hint*/) const {
  if (!detail::valid_show_id(provider_id)) return err(ProviderError::decode("invalid show id"));
  auto html = watch_page(provider_id, "1");
  if (!html.has_value()) return err(html.error());
  const auto all = payloads(*html);
  auto eps = episodes_from(all);
  if (!eps.has_value()) return err(eps.error());
  if (translation == Translation::Dub) {
    const auto tracks = tracks_from(all);
    if (tracks.has_value() && detail::candidates(*tracks, Translation::Dub).empty()) {
      return std::vector<std::string>{};
    }
  }
  return eps;
}

Result<StreamLink, ProviderError> Miruro::resolve(std::string_view provider_id,
                                                  std::string_view episode,
                                                  Translation translation,
                                                  Quality /*quality*/) const {
  if (!detail::valid_show_id(provider_id)) return err(ProviderError::decode("invalid show id"));
  if (!detail::valid_episode(episode)) return err(ProviderError::decode("invalid episode"));
  auto html = watch_page(provider_id, episode);
  if (!html.has_value()) return err(html.error());
  auto tracks = detail::parse_tracks(*html);
  if (!tracks.has_value()) return err(tracks.error());
  const auto list = detail::candidates(*tracks, translation);
  if (list.empty()) return err(ProviderError::decode("no stream for this track"));

  std::optional<ProviderError> last;
  std::size_t probes = 0;
  for (const auto& c : list) {
    std::optional<std::string> master;
    if (c.stream.hls) {
      if (probes >= kMaxProbes) continue;
      ++probes;
      auto body = probe(c.stream);
      if (!body.has_value()) {
        last = body.error();
        continue;
      }
      if (!detail::is_playlist(*body)) {
        last = ProviderError::decode("not a playlist");
        continue;
      }
      master = std::move(*body);
    }
    StreamLink link;
    link.url = c.stream.url;
    link.referer = c.stream.referer;
    link.origin = c.stream.origin;
    link.user_agent = std::string(http::kBrowserUserAgent);
    link.cloaked_segments = c.stream.hls;
    link.sub_url = c.sub_url;
    if (translation == Translation::Dub && master.has_value()) {
      link.audio_language = detail::dub_audio_language(*master);
    }
    return link;
  }
  return err(last.value_or(ProviderError::decode("no stream for this track")));
}

Result<CoverRequest, ProviderError> Miruro::cover_request(std::string_view cover_ref) const {
  if (cover_ref.empty() || cover_ref.size() > kMaxCoverRefLen || !is_absolute_url(cover_ref) ||
      !clean_arg(cover_ref)) {
    return err(ProviderError::decode("invalid cover ref"));
  }
  CoverRequest req;
  req.url = std::string(cover_ref);
  return req;
}

}  // namespace shigoku::miruro
