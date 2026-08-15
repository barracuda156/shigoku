// megaplay.cpp — P13. Ported from sabigoku src/providers/megaplay.rs.
//
// Two GETs, no decryption: embed (scrape data-id, the sub/dub fork) then
// getSources (cleartext JSON: master m3u8 + softsub tracks). MAL-keyed like
// senshi, so it needs zero resolve-walk changes. The one megaplay-specific
// machinery is the cue-count softsub refinement (ROD-377): with >=2 English
// tracks, probe each vtt's " --> " count and upgrade to the highest, never
// below the metadata pick.
//
// cap_variant does not apply here: megaplay.rs never calls hls::cap_variant
// (verified in source, P23) — resolve() always takes the master URL
// regardless of Quality, same as the Rust. Not a deferred-scope gap.
//
// The decloak wiring is the reason this provider exists in M2: its links carry
// decloak_segments=true, so the player routes them through the P12 proxy. That
// seam is already live (proxy.cpp / player.cpp); here the provider just sets the
// flag, same as the Rust.

#include "megaplay.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <nlohmann/json.hpp>

#include "provider.hpp"

namespace shigoku::megaplay {

namespace {

using json = nlohmann::json;

// serde `Option<String>` on a struct field: absent or JSON null -> nullopt; a
// present string -> the string; a present NON-string is a hard type error
// (serde_json rejects the whole struct). Signalled by the bool out-param `ok`.
std::optional<std::string> strict_opt_str(const json& obj, const char* key, bool& ok) {
  if (!obj.contains(key) || obj.at(key).is_null()) return std::nullopt;
  if (!obj.at(key).is_string()) {
    ok = false;
    return std::nullopt;
  }
  return obj.at(key).get<std::string>();
}

// serde `#[serde(default)] bool`: absent -> false; present bool -> its value; a
// present non-bool is a hard type error (matches serde_json).
bool strict_default_bool(const json& obj, const char* key, bool& ok) {
  if (!obj.contains(key) || obj.at(key).is_null()) return false;
  if (!obj.at(key).is_boolean()) {
    ok = false;
    return false;
  }
  return obj.at(key).get<bool>();
}

std::string to_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

// starts_with over an already-lowercased needle (label match helpers).
bool lower_starts_with(std::string_view s, std::string_view prefix) {
  return to_lower(s).rfind(prefix, 0) == 0;
}

}  // namespace

namespace detail {

bool is_subtitle_track(const Track& t) {
  // `captions` or kind-less counts as a subtitle. `thumbnails` and any unknown
  // kind do not (never a wrong --sub-file).
  if (!t.kind.has_value()) return true;
  return *t.kind == "captions";
}

std::optional<std::size_t> pick_subtitle(const std::vector<Track>& tracks) {
  std::optional<std::size_t> english;
  std::optional<std::size_t> first;
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    const Track& t = tracks[i];
    if (!is_subtitle_track(t)) continue;
    if (t.is_default) return i;
    if (!english.has_value() && t.label.has_value() &&
        lower_starts_with(*t.label, "english")) {
      english = i;
    }
    if (!first.has_value()) first = i;
  }
  return english.has_value() ? english : first;
}

std::vector<std::string_view> english_captions(const std::vector<Track>& tracks) {
  std::vector<std::string_view> out;
  for (const Track& t : tracks) {
    if (out.size() >= kMaxSubtitleProbes) break;
    if (!is_subtitle_track(t)) continue;
    if (!t.label.has_value()) continue;
    if (!lower_starts_with(*t.label, "english")) continue;
    out.push_back(t.file);
  }
  return out;
}

std::string embed_url(std::string_view host, std::string_view mal_id, std::string_view ep_label,
                      Translation tt) {
  std::string out;
  out.append(host);
  out.append("/stream/mal/");
  out.append(mal_id);
  out.push_back('/');
  out.append(ep_label);
  out.push_back('/');
  out.append(to_string(tt));  // "sub" | "dub"
  return out;
}

std::optional<std::string_view> parse_data_id(std::string_view html) {
  const std::string_view needle = "data-id=";
  std::size_t from = 0;
  while (true) {
    const auto rel = html.substr(from).find(needle);
    if (rel == std::string_view::npos) return std::nullopt;
    const std::size_t at = from + rel;
    std::size_t i = at + needle.size();
    if (i < html.size() && (html[i] == '"' || html[i] == '\'')) ++i;
    const std::size_t start = i;
    while (i < html.size() && html[i] >= '0' && html[i] <= '9') ++i;
    if (i > start && (i - start) <= kMaxDataIdLen) {
      return html.substr(start, i - start);
    }
    from = at + needle.size();
  }
}

std::vector<std::string> labels(std::uint32_t n) {
  const std::uint32_t count = std::clamp<std::uint32_t>(n, 1, kMaxEpisodeHint);
  std::vector<std::string> out;
  out.reserve(count);
  for (std::uint32_t i = 1; i <= count; ++i) out.push_back(std::to_string(i));
  return out;
}

Result<Sources, ProviderError> map_sources(std::string_view raw, Translation tt) {
  json resp;
  try {
    resp = json::parse(raw.begin(), raw.end());
  } catch (const json::parse_error& e) {
    return err(ProviderError::decode(std::string("getSources: ") + e.what()));
  }

  // The DTO walk below mirrors serde_json's strictness: a present-but-wrong-
  // typed KNOWN field (sources, file, tracks, label, kind, default) is a hard
  // Decode error, exactly as `#[derive(Deserialize)]` would reject the struct
  // (SourcesResp/RawSources/RawTrack). Unknown fields are ignored (serde drops
  // them); a missing/null optional degrades to none (serde default). `ok` trips
  // to false on any type mismatch.
  bool ok = true;

  // sources: Option<RawSources>. Absent/null -> no source (Decode "no stream
  // source"); a present non-object is a type error.
  const bool has_sources = resp.is_object() && resp.contains("sources") &&
                           !resp.at("sources").is_null();
  if (has_sources && !resp.at("sources").is_object()) {
    return err(ProviderError::decode("getSources: sources not an object"));
  }
  std::optional<std::string> file;
  if (has_sources) file = strict_opt_str(resp.at("sources"), "file", ok);
  if (!ok) return err(ProviderError::decode("getSources: bad field type"));
  if (!file.has_value()) return err(ProviderError::decode("no stream source"));
  // Host data -> mpv argv: absolute http(s) + clean argv bytes only.
  if (!is_absolute_url(*file) || !clean_arg(*file)) {
    return err(ProviderError::decode("bad stream url"));
  }

  // tracks: Vec<RawTrack> (#[serde(default)]). Absent/null -> empty; a present
  // non-array, or a non-object element, is a type error.
  std::vector<Track> tracks;
  const bool has_tracks = resp.is_object() && resp.contains("tracks") &&
                          !resp.at("tracks").is_null();
  if (has_tracks && !resp.at("tracks").is_array()) {
    return err(ProviderError::decode("getSources: tracks not an array"));
  }
  if (has_tracks) {
    for (const auto& t : resp.at("tracks")) {
      if (!t.is_object()) return err(ProviderError::decode("getSources: track not an object"));
      const std::optional<std::string> f = strict_opt_str(t, "file", ok);
      const std::optional<std::string> label = strict_opt_str(t, "label", ok);
      const std::optional<std::string> kind = strict_opt_str(t, "kind", ok);
      const bool is_default = strict_default_bool(t, "default", ok);
      if (!ok) return err(ProviderError::decode("getSources: bad track field type"));
      if (!f.has_value()) continue;  // a file-less ghost drops (file is Option)
      // Drop an unsafe track, never fail the stream over it.
      if (!is_absolute_url(*f) || !clean_arg(*f)) continue;
      Track tk;
      tk.file = *f;
      tk.label = label;
      tk.kind = kind;
      tk.is_default = is_default;
      tracks.push_back(std::move(tk));
    }
  }

  // The picked sub_url goes straight to mpv --sub-file, bypassing the proxy that
  // SSRF-guards the stream. Guard it here or drop it (play raw): argv-vet alone
  // is host-blind, so a track aimed at loopback/metadata would otherwise reach
  // mpv. Deviation past freeze, ratified ROD-445.
  std::optional<std::string> sub_url;
  if (tt == Translation::Sub) {
    if (const auto pick = pick_subtitle(tracks); pick.has_value()) {
      const std::string& cand = tracks[*pick].file;
      if (http::guard_fetch_url(cand).has_value()) sub_url = cand;
    }
  }

  Sources s;
  s.link.url = *file;
  s.link.resolution = std::nullopt;
  s.link.referer = std::string(kStreamReferer);
  s.link.user_agent = std::string(kUserAgent);
  // Segment CDN serves .ts as .jpg AND prepends a decoy PNG header: mpv needs
  // the relaxed demuxer and the proxy.
  s.link.cloaked_segments = true;
  s.link.decloak_segments = true;
  s.link.sub_url = std::move(sub_url);
  s.tracks = std::move(tracks);
  return s;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The provider.
// ---------------------------------------------------------------------------

Result<MegaPlay, ProviderError> MegaPlay::create() { return with_host(kHost); }

Result<MegaPlay, ProviderError> MegaPlay::with_host(std::string host) {
  // Trim a trailing '/' so the URL splices ("{host}/stream/...") stay single-
  // slash regardless of how the fixture server hands back its base.
  while (!host.empty() && host.back() == '/') host.pop_back();
  auto client = http::Client::create();
  if (!client.has_value()) return err(ProviderError::network());
  return MegaPlay(std::move(*client), std::move(host));
}

std::optional<std::string> MegaPlay::canonical_key(const Enrichment& show) const {
  if (!show.mal_id.has_value()) return std::nullopt;
  return std::to_string(*show.mal_id);
}

Result<std::vector<SearchHit>, ProviderError> MegaPlay::search(
    std::string_view /*query*/, const SearchOptions& /*opts*/) const {
  return err(ProviderError::unsupported());
}

Result<std::vector<std::uint8_t>, ProviderError> MegaPlay::embed_get(const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Referer", kStreamReferer});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
}

Result<std::vector<std::uint8_t>, ProviderError> MegaPlay::xhr_get(const std::string& url) const {
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Referer", kStreamReferer});
  req.extra_headers.push_back({"X-Requested-With", "XMLHttpRequest"});
  req.extra_headers.push_back({"Accept", "application/json"});
  req.accept = http::Accept::Any2xx;
  return http_.fetch(req);
}

std::optional<std::size_t> MegaPlay::probe_cues(const std::string& url) const {
  if (!http::guard_fetch_url(url).has_value()) return std::nullopt;
  http::Request req;
  req.method = http::Method::Get;
  req.url = url;
  req.user_agent = kUserAgent;
  req.extra_headers.push_back({"Referer", kStreamReferer});
  req.accept = http::Accept::Any2xx;
  auto body = http_.fetch(req);
  if (!body.has_value()) return std::nullopt;
  // Count " --> " (5-byte) windows. The body is counted only, never argv.
  const std::string_view needle = " --> ";
  std::size_t count = 0;
  if (body->size() >= needle.size()) {
    for (std::size_t i = 0; i + needle.size() <= body->size(); ++i) {
      if (std::memcmp(body->data() + i, needle.data(), needle.size()) == 0) ++count;
    }
  }
  return count;
}

std::optional<std::string> MegaPlay::refine_subtitle_by_cues(
    const std::vector<std::string_view>& candidates, const std::string& baseline) const {
  const auto base_cues = probe_cues(baseline);
  if (!base_cues.has_value()) return std::nullopt;  // failed baseline keeps metadata
  std::string best = baseline;
  std::size_t best_cues = *base_cues;
  for (const std::string_view cand : candidates) {
    if (cand == baseline) continue;
    const std::string cand_url(cand);
    const auto cues = probe_cues(cand_url);
    if (!cues.has_value()) continue;  // a failed candidate drops, not the decision
    if (*cues > best_cues) {
      best = cand_url;
      best_cues = *cues;
    }
  }
  if (best == baseline) return std::nullopt;
  return best;
}

Result<std::vector<std::string>, ProviderError> MegaPlay::episodes(
    std::string_view provider_id, Translation /*translation*/,
    std::optional<std::uint32_t> count_hint) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  // Probe ep 1 (sub, track-agnostic) for existence. tt does not filter here.
  const std::string url = detail::embed_url(host_, provider_id, "1", Translation::Sub);
  auto html = embed_get(url);
  if (!html.has_value()) return err(html.error());
  const std::string_view text(reinterpret_cast<const char*>(html->data()), html->size());
  // 200 with no data-id = authoritative not stocked (03 §8.1).
  if (!detail::parse_data_id(text).has_value()) return std::vector<std::string>{};
  return detail::labels(count_hint.value_or(1));
}

Result<StreamLink, ProviderError> MegaPlay::resolve(std::string_view provider_id,
                                                    std::string_view episode, Translation tt,
                                                    Quality /*quality*/) const {
  if (auto g = guard_show_id(provider_id); !g.has_value()) return err(g.error());
  // Own mint is 1-based integers. A foreign fractional label ("13.5") has no
  // MAL-route address; a u32 parse rejects it before any network. Splice the
  // parsed `n` back (canonical form), not the raw label.
  std::uint32_t n = 0;
  if (episode.empty()) return err(ProviderError::decode("invalid episode"));
  for (const char c : episode) {
    if (c < '0' || c > '9') return err(ProviderError::decode("invalid episode"));
    if (n > (0xFFFFFFFFu - 9) / 10) return err(ProviderError::decode("invalid episode"));
    n = n * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (n == 0) return err(ProviderError::decode("invalid episode"));

  const std::string embed = detail::embed_url(host_, provider_id, std::to_string(n), tt);
  auto html = embed_get(embed);
  if (!html.has_value()) return err(html.error());
  const std::string_view text(reinterpret_cast<const char*>(html->data()), html->size());
  // Past-end or missing track (the sub/dub fork lives here), not a transport
  // failure.
  const auto data_id = detail::parse_data_id(text);
  if (!data_id.has_value()) return err(ProviderError::decode("no data-id"));

  const std::string src_url =
      host_ + "/stream/getSources?id=" + std::string(*data_id);
  auto raw = xhr_get(src_url);
  if (!raw.has_value()) return err(raw.error());
  const std::string_view raw_view(reinterpret_cast<const char*>(raw->data()), raw->size());
  auto sources = detail::map_sources(raw_view, tt);
  if (!sources.has_value()) return err(sources.error());

  // Softsub pick (ROD-377): the host `default` sometimes marks a signs-only
  // track over dialogue; with >=2 English tracks, upgrade to highest-cue by
  // content. Never downgrades below the metadata pick.
  if (tt == Translation::Sub && sources->link.sub_url.has_value()) {
    const std::string baseline = *sources->link.sub_url;
    const auto candidates = detail::english_captions(sources->tracks);
    if (candidates.size() >= 2) {
      if (auto best = refine_subtitle_by_cues(candidates, baseline); best.has_value()) {
        sources->link.sub_url = std::move(*best);
      }
    }
  }
  return sources->link;
}

Result<CoverRequest, ProviderError> MegaPlay::cover_request(std::string_view cover_ref) const {
  // No covers of its own: an absolute AniList/MAL CDN ref passes through, a
  // relative or unsafe ref is rejected (never "sanitized" and fetched).
  if (cover_ref.empty() || !is_absolute_url(cover_ref) || !clean_arg(cover_ref)) {
    return err(ProviderError::decode("invalid cover ref"));
  }
  CoverRequest cr;
  cr.url = std::string(cover_ref);
  return cr;
}

}  // namespace shigoku::megaplay
