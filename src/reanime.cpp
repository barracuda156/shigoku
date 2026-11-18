// reanime.cpp — see reanime.hpp.

#include "reanime.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "crypto.hpp"
#include "scrape.hpp"
#include "wasm.hpp"

namespace shigoku::reanime {

namespace {

using json = nlohmann::json;

// Where the page's JS lays the three key fragments out in the module's
// memory, and the key schedule's PBKDF2 round count.
constexpr std::uint32_t kScratch = 1000;
constexpr std::uint32_t kPbkdf2Rounds = 1000;
constexpr std::size_t kMaxFragment = 256;
constexpr std::size_t kMaxModule = 64 * 1024;
constexpr std::size_t kMaxToken = 128;
constexpr std::size_t kPlaylistKeyLen = 32;

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
  return to_lower(hay).find(needle) != std::string::npos;
}

bool token_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-';
}

bool hex_char(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

using scrape::origin_of;

std::optional<json> parse_json(std::string_view raw) {
  try {
    return json::parse(raw.begin(), raw.end());
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

std::optional<std::vector<std::uint8_t>> b64_field(const json* v, std::size_t max_len) {
  if (v == nullptr || !v->is_string()) return std::nullopt;
  const auto& s = v->get_ref<const std::string&>();
  if (s.empty() || s.size() > max_len * 2) return std::nullopt;
  auto bytes = crypto::base64_decode(s);
  if (!bytes.has_value() || bytes->empty() || bytes->size() > max_len) return std::nullopt;
  return bytes;
}

// A devalue-encoded node: every object value and array element is an index
// into the flat `data` array; negative indices are undefined/holes.
class Devalue {
 public:
  explicit Devalue(const json& data) : data_(data) {}

  [[nodiscard]] const json* root() const { return data_.empty() ? nullptr : &data_[0]; }

  [[nodiscard]] const json* at(const json& index) const {
    if (!index.is_number_integer()) return nullptr;
    const auto i = index.get<std::int64_t>();
    if (i < 0 || static_cast<std::uint64_t>(i) >= data_.size()) return nullptr;
    return &data_[static_cast<std::size_t>(i)];
  }

  [[nodiscard]] const json* field(const json* obj, const std::string& key) const {
    if (obj == nullptr || !obj->is_object()) return nullptr;
    const auto it = obj->find(key);
    return it == obj->end() ? nullptr : at(*it);
  }

  [[nodiscard]] const json* element(const json* arr, std::size_t i) const {
    if (arr == nullptr || !arr->is_array() || i >= arr->size()) return nullptr;
    return at((*arr)[i]);
  }

  [[nodiscard]] std::optional<std::string> str(const json* obj, const std::string& key) const {
    const json* v = field(obj, key);
    if (v == nullptr || !v->is_string()) return std::nullopt;
    return v->get<std::string>();
  }

 private:
  const json& data_;
};

// The 64-hex-char SHA-256 of `s`, or "" (and every derived name empty).
std::string hex_digest(const std::string& s) {
  std::string h = crypto::sha256_hex(s);
  return h.size() == 64 ? h : std::string();
}

}  // namespace

namespace detail {

Result<std::vector<Server>, ProviderError> parse_servers(std::string_view raw_json) {
  auto j = parse_json(raw_json);
  if (!j.has_value() || !j->is_object()) return err(ProviderError::decode("flix: not an object"));
  const auto it = j->find("servers");
  if (it == j->end() || !it->is_array()) return err(ProviderError::decode("flix: no servers"));
  std::vector<Server> out;
  for (const auto& e : *it) {
    if (!e.is_object()) continue;
    const auto name = e.find("serverName");
    const auto link = e.find("dataLink");
    const auto type = e.find("dataType");
    if (name == e.end() || link == e.end() || type == e.end() || !name->is_string() ||
        !link->is_string() || !type->is_string()) {
      continue;
    }
    Server s;
    const auto& t = type->get_ref<const std::string&>();
    if (t == "sub") {
      s.track = Translation::Sub;
    } else if (t == "dub") {
      s.track = Translation::Dub;
    } else {
      continue;
    }
    s.name = name->get<std::string>();
    s.link = link->get<std::string>();
    if (!is_absolute_url(s.link) || !clean_arg(s.link)) continue;
    const auto hard = e.find("hardsub");
    s.hardsub = hard != e.end() && hard->is_boolean() && hard->get<bool>();
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<Server> servers_for(const std::vector<Server>& all, Translation track) {
  std::vector<Server> out;
  for (const auto& s : all) {
    if (s.track == track) out.push_back(s);
  }
  auto rank = [](const Server& s) { return s.name == "HD-2" ? 0 : (s.name == "HD-1" ? 1 : 2); };
  std::stable_sort(out.begin(), out.end(),
                   [&](const Server& a, const Server& b) { return rank(a) < rank(b); });
  return out;
}

std::optional<std::string> data_url(std::string_view embed_link) {
  if (!embed_link.starts_with("https://") || !clean_arg(embed_link) ||
      !http::guard_fetch_url(embed_link).has_value()) {
    return std::nullopt;
  }
  auto origin = origin_of(embed_link);
  if (!origin.has_value()) return std::nullopt;
  std::string_view rest = embed_link.substr(origin->size());
  if (const auto hash = rest.find('#'); hash != std::string_view::npos) rest = rest.substr(0, hash);
  if (!rest.starts_with("/e/")) return std::nullopt;
  rest.remove_prefix(3);
  const auto q = rest.find('?');
  const std::string_view id = rest.substr(0, q);
  if (id.empty() || id.size() > 64 || !std::all_of(id.begin(), id.end(), token_char)) {
    return std::nullopt;
  }
  std::string out = *origin + "/e/" + std::string(id) + "/__data.json";
  if (q != std::string_view::npos && q + 1 < rest.size()) {
    out += "?" + std::string(rest.substr(q + 1));
  }
  return out;
}

FieldNames field_names(std::string_view seed) {
  auto chain = [](std::string v) {
    for (int i = 0; i < 3 && !v.empty(); ++i) v = hex_digest(v + std::to_string(i));
    return v;
  };
  const std::string e = seed.empty() ? std::string() : chain(std::string(seed));
  const std::string a = e.empty() ? std::string() : chain(e);
  if (e.empty() || a.empty()) return {};
  FieldNames n;
  n.key_frag = "kf_" + e.substr(8, 8);
  n.iv = "ivf_" + e.substr(16, 8);
  n.container = "cd_" + e.substr(24, 8);
  n.array = "ad_" + e.substr(32, 8);
  n.object = "od_" + e.substr(40, 8);
  n.token = e.substr(48, 16) + "_" + e.substr(56, 8);
  n.key_frag2 = a.substr(0, 16) + "_" + a.substr(16, 8);
  return n;
}

Result<EmbedPage, ProviderError> parse_embed(std::string_view data_json) {
  auto j = parse_json(data_json);
  if (!j.has_value() || !j->is_object()) return err(ProviderError::decode("embed: not an object"));
  const auto nodes = j->find("nodes");
  if (nodes == j->end() || !nodes->is_array()) return err(ProviderError::decode("embed: no nodes"));

  const json* data = nullptr;
  for (const auto& node : *nodes) {
    if (!node.is_object()) continue;
    const auto d = node.find("data");
    if (d == node.end() || !d->is_array() || d->empty() || !(*d)[0].is_object()) continue;
    if ((*d)[0].contains("obfuscation_seed")) data = &*d;
  }
  if (data == nullptr) return err(ProviderError::decode("embed: no player data"));
  const Devalue dv(*data);
  const json* root = dv.root();

  EmbedPage page;
  auto seed = dv.str(root, "obfuscation_seed");
  if (!seed.has_value() || seed->size() < 8 || seed->size() > 64 ||
      !std::all_of(seed->begin(), seed->end(), hex_char)) {
    return err(ProviderError::decode("embed: bad seed"));
  }
  page.seed = std::move(*seed);
  const FieldNames names = field_names(page.seed);
  if (names.token.empty()) return err(ProviderError::decode("embed: bad seed"));

  auto wasm = b64_field(dv.field(root, "w_payload"), kMaxModule);
  if (!wasm.has_value()) return err(ProviderError::decode("embed: no player module"));
  page.wasm = std::move(*wasm);

  const json* box = dv.field(root, "obfuscated_crypto_data");
  box = dv.field(box, names.container);
  box = dv.field(box, names.array);
  box = dv.element(box, 0);
  box = dv.field(box, names.object);
  auto frag1 = b64_field(dv.field(box, names.key_frag), kMaxFragment);
  auto iv = b64_field(dv.field(box, names.iv), 16);
  auto frag2 = b64_field(dv.field(root, names.key_frag2), kMaxFragment);
  if (!frag1.has_value() || !frag2.has_value() || !iv.has_value() || iv->size() != 16) {
    return err(ProviderError::decode("embed: key fields missing"));
  }
  page.frag1 = std::move(*frag1);
  page.frag2 = std::move(*frag2);
  page.iv = std::move(*iv);

  auto token = dv.str(root, names.token);
  if (!token.has_value() || token->empty() || token->size() > kMaxToken ||
      !std::all_of(token->begin(), token->end(), token_char)) {
    return err(ProviderError::decode("embed: no token"));
  }
  page.token = std::move(*token);
  page.audio_type = dv.str(root, "audio_type").value_or("");

  const json* subs = dv.field(root, "subtitles");
  for (std::size_t i = 0; subs != nullptr && subs->is_array() && i < subs->size() && i < 32; ++i) {
    const json* s = dv.element(subs, i);
    auto url = dv.str(s, "url");
    if (!url.has_value()) continue;
    Subtitle sub;
    sub.url = std::move(*url);
    sub.label = dv.str(s, "language").value_or("");
    const json* def = dv.field(s, "default");
    sub.is_default = def != nullptr && def->is_boolean() && def->get<bool>();
    page.subtitles.push_back(std::move(sub));
  }
  return page;
}

Result<TokenReply, ProviderError> parse_token_reply(std::string_view raw_json,
                                                    std::string_view token) {
  auto j = parse_json(raw_json);
  if (!j.has_value() || !j->is_object()) return err(ProviderError::decode("token: not an object"));
  const std::string vid = hex_digest(std::string(token) + "vid");
  const std::string key = hex_digest(std::string(token) + "key");
  if (vid.empty() || key.empty()) return err(ProviderError::decode("token: digest"));
  auto lookup = [&](const std::string& k) -> const json* {
    const auto it = j->find(k.substr(0, 10));
    return it == j->end() ? nullptr : &*it;
  };
  auto ct = b64_field(lookup(vid), 4096);
  auto frag3 = b64_field(lookup(key), kMaxFragment);
  if (!ct.has_value() || !frag3.has_value()) return err(ProviderError::decode("token: fields missing"));
  return TokenReply{std::move(*ct), std::move(*frag3)};
}

Result<Unlocked, ProviderError> unlock(const EmbedPage& page, const TokenReply& reply) {
  if (page.seed.size() < 8 || !std::all_of(page.seed.begin(), page.seed.end(), hex_char) ||
      page.frag1.empty() || page.frag1.size() > kMaxFragment || page.frag2.size() > kMaxFragment ||
      reply.frag3.size() > kMaxFragment) {
    return err(ProviderError::decode("embed: bad key fields"));
  }
  auto inst = wasm::Instance::load(page.wasm.data(), page.wasm.size());
  if (!inst.has_value() || !inst->ensure_pages(1)) {
    return err(ProviderError::decode("player module"));
  }
  const auto len = static_cast<std::uint32_t>(page.frag1.size());
  const std::uint32_t a = kScratch + len;
  const std::uint32_t b = a + len;
  const std::uint32_t out = b + len;
  // The page's own write order: a longer later fragment overwrites the next
  // slot, exactly as its Uint8Array.set calls do.
  if (!inst->write(kScratch, page.frag1.data(), page.frag1.size()) ||
      !inst->write(a, page.frag2.data(), page.frag2.size()) ||
      !inst->write(b, reply.frag3.data(), reply.frag3.size())) {
    return err(ProviderError::decode("player module"));
  }
  std::uint32_t g = 0;
  for (int i = 0; i < 8; ++i) {
    const char c = page.seed[static_cast<std::size_t>(i)];
    const std::uint32_t v = (c >= '0' && c <= '9') ? static_cast<std::uint32_t>(c - '0')
                            : (c >= 'a' && c <= 'f') ? static_cast<std::uint32_t>(c - 'a' + 10)
                                                     : static_cast<std::uint32_t>(c - 'A' + 10);
    g = (g << 4) | v;
  }
  if (!inst->call("_s", {g}).has_value() || !inst->call("_r", {kScratch, a, b, out, len}).has_value()) {
    return err(ProviderError::decode("player module"));
  }
  auto mixed = inst->read(out, len);
  auto pk_at = inst->call("_c", {});
  if (!mixed.has_value() || !pk_at.has_value() || pk_at->size() != 1) {
    return err(ProviderError::decode("player module"));
  }
  auto playlist_key = inst->read((*pk_at)[0], kPlaylistKeyLen);
  if (!playlist_key.has_value()) return err(ProviderError::decode("player module"));

  auto stretched = crypto::pbkdf2_sha256(*mixed, page.seed, kPbkdf2Rounds, 32);
  if (!stretched.has_value()) return err(ProviderError::decode("key schedule"));
  for (std::size_t i = 0; i < stretched->size(); ++i) {
    (*stretched)[i] ^= static_cast<std::uint8_t>(page.seed[i % page.seed.size()]);
  }
  auto key = crypto::sha256(stretched->data(), stretched->size());
  if (!key.has_value()) return err(ProviderError::decode("key schedule"));
  auto plain = crypto::aes256cbc_open(*key, page.iv, reply.ciphertext.data(), reply.ciphertext.size());
  if (!plain.has_value()) return err(ProviderError::decode("bad envelope"));

  std::string master(plain->begin(), plain->end());
  while (!master.empty() && (master.back() == '\n' || master.back() == '\r' || master.back() == ' ')) {
    master.pop_back();
  }
  if (!is_absolute_url(master) || !clean_arg(master) || !http::guard_fetch_url(master).has_value()) {
    return err(ProviderError::decode("bad stream url"));
  }
  return Unlocked{std::move(master), std::move(*playlist_key)};
}

std::optional<std::string> pick_subtitle(const std::vector<Subtitle>& subs, Translation track) {
  std::vector<const Subtitle*> english;
  for (const auto& s : subs) {
    if (!to_lower(s.label).starts_with("english")) continue;
    if (!is_absolute_url(s.url) || !clean_arg(s.url) || !http::guard_fetch_url(s.url).has_value()) {
      continue;
    }
    english.push_back(&s);
  }
  auto signs = [](const Subtitle* s) { return contains_ci(s->label, "sign"); };
  auto dubtitle = [](const Subtitle* s) { return contains_ci(s->label, "dubtitle"); };
  if (track == Translation::Dub) {
    for (const Subtitle* s : english) {
      if (signs(s)) return s->url;
    }
    return std::nullopt;
  }
  for (const Subtitle* s : english) {
    if (s->is_default) return s->url;
  }
  for (const Subtitle* s : english) {
    if (contains_ci(s->label, "full")) return s->url;
  }
  for (const Subtitle* s : english) {
    if (!signs(s) && !dubtitle(s)) return s->url;
  }
  return std::nullopt;
}

bool valid_show_id(std::string_view id) {
  return !id.empty() && id.size() <= 10 && id.front() != '0' &&
         std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string> labels(std::uint32_t n) { return scrape::numbered_labels(n); }

}  // namespace detail

// ===========================================================================
// The provider transport.
// ===========================================================================

Result<ReAnime, ProviderError> ReAnime::create() { return with_endpoints(kBase, ""); }

Result<ReAnime, ProviderError> ReAnime::with_endpoints(std::string base, std::string embed_origin) {
  while (!base.empty() && base.back() == '/') base.pop_back();
  while (!embed_origin.empty() && embed_origin.back() == '/') embed_origin.pop_back();
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return ReAnime(std::move(*client), std::move(base), std::move(embed_origin));
}

Result<std::vector<std::uint8_t>, ProviderError> ReAnime::get(const std::string& url,
                                                              const std::string& referer,
                                                              bool json) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.extra_headers.push_back({"Referer", referer});
  req.extra_headers.push_back({"Accept", json ? "application/json" : "text/html,*/*;q=0.8"});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
}

std::string ReAnime::embed_fetch_url(const std::string& url) const {
  if (embed_origin_.empty()) return url;
  const auto origin = origin_of(url);
  if (!origin.has_value()) return url;
  return embed_origin_ + url.substr(origin->size());
}

Result<std::vector<detail::Server>, ProviderError> ReAnime::fetch_servers(
    std::string_view anilist_id, std::uint32_t episode) const {
  auto raw = get(base_ + "/api/flix/" + std::string(anilist_id) + "/" + std::to_string(episode),
                 kReferer, true);
  if (!raw.has_value()) return err(raw.error());
  return detail::parse_servers(
      std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
}

Result<StreamLink, ProviderError> ReAnime::resolve_on(const detail::Server& server,
                                                      Translation translation) const {
  const auto data = detail::data_url(server.link);
  const auto origin = origin_of(server.link);
  if (!data.has_value() || !origin.has_value()) {
    return err(ProviderError::decode("blocked embed url"));
  }
  auto raw = get(embed_fetch_url(*data), kReferer, true);
  if (!raw.has_value()) return err(raw.error());
  auto page = detail::parse_embed(
      std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
  if (!page.has_value()) return err(page.error());
  const bool dual = page->audio_type == "dual";
  if (translation == Translation::Dub && !dual) return err(ProviderError::decode("no dub audio"));

  auto reply_raw = get(embed_fetch_url(*origin + "/api/m3u8/" + page->token), server.link, true);
  if (!reply_raw.has_value()) return err(reply_raw.error());
  auto reply = detail::parse_token_reply(
      std::string_view(reinterpret_cast<const char*>(reply_raw->data()), reply_raw->size()),
      page->token);
  if (!reply.has_value()) return err(reply.error());
  auto unlocked = detail::unlock(*page, *reply);
  if (!unlocked.has_value()) return err(unlocked.error());

  StreamLink link;
  link.url = std::move(unlocked->master);
  link.referer = *origin + "/";
  link.user_agent = std::string(http::kBrowserUserAgent);
  // Segments are disguised as .png/.webp upstream; like megaplay's, the link
  // keeps the player's extension gate relaxed behind the proxy too.
  link.cloaked_segments = true;
  StreamLink::PlaylistCipher cipher;
  cipher.key = std::move(unlocked->playlist_key);
  cipher.kind = StreamLink::PlaylistCipher::Kind::Xor;
  link.playlist_cipher = std::move(cipher);
  if (dual) link.audio_language = translation == Translation::Dub ? "eng" : "jpn";
  if (!server.hardsub) link.sub_url = detail::pick_subtitle(page->subtitles, translation);
  return link;
}

std::optional<std::string> ReAnime::canonical_key(const Enrichment& show) const {
  if (show.anilist_id <= 0) return std::nullopt;
  return std::to_string(show.anilist_id);
}

Result<std::vector<SearchHit>, ProviderError> ReAnime::search(std::string_view /*query*/,
                                                              const SearchOptions& /*opts*/) const {
  return err(ProviderError::unsupported());
}

Result<std::vector<std::string>, ProviderError> ReAnime::episodes(
    std::string_view provider_id, Translation translation,
    std::optional<std::uint32_t> count_hint) const {
  if (!detail::valid_show_id(provider_id)) return err(ProviderError::decode("invalid show id"));
  auto servers = fetch_servers(provider_id, 1);
  if (!servers.has_value()) return err(servers.error());
  if (detail::servers_for(*servers, translation).empty()) return std::vector<std::string>{};
  return detail::labels(count_hint.value_or(1));
}

Result<StreamLink, ProviderError> ReAnime::resolve(std::string_view provider_id,
                                                   std::string_view episode,
                                                   Translation translation,
                                                   Quality /*quality*/) const {
  if (!detail::valid_show_id(provider_id)) return err(ProviderError::decode("invalid show id"));
  std::uint32_t n = 0;
  if (episode.empty() || episode.size() > 9) return err(ProviderError::decode("invalid episode"));
  for (const char c : episode) {
    if (c < '0' || c > '9') return err(ProviderError::decode("invalid episode"));
    n = n * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (n == 0) return err(ProviderError::decode("invalid episode"));

  auto servers = fetch_servers(provider_id, n);
  if (!servers.has_value()) return err(servers.error());
  const auto candidates = detail::servers_for(*servers, translation);
  if (candidates.empty()) return err(ProviderError::decode("no such episode"));
  std::optional<ProviderError> last;
  for (const auto& server : candidates) {
    auto link = resolve_on(server, translation);
    if (link.has_value()) return link;
    last = link.error();
  }
  return err(*last);
}

Result<CoverRequest, ProviderError> ReAnime::cover_request(std::string_view cover_ref) const {
  if (cover_ref.empty() || cover_ref.size() > kMaxCoverRefLen || !is_absolute_url(cover_ref) ||
      !clean_arg(cover_ref)) {
    return err(ProviderError::decode("invalid cover ref"));
  }
  CoverRequest req;
  req.url = std::string(cover_ref);
  return req;
}

}  // namespace shigoku::reanime
