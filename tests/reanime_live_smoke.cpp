// reanime_live_smoke.cpp — resolve against the live site and play the result
// as far as a player would. Not part of the offline ctest suite: run by hand
// or by scripts/provider_canary.sh, which set SHIGOKU_LIVE=1. Skips (exit 0)
// when the variable is absent.
//
// Flow: Frieren (AniList 154587) -> episodes (sub) -> resolve ep 1 sub ->
// the loopback proxy -> the opened master (Japanese audio only) -> the
// smallest variant -> its AES-128 key and first segment, decrypted to
// MPEG-TS. Then resolve ep 1 dub and check the proxied master keeps only the
// English audio.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../src/crypto.hpp"
#include "../src/hls.hpp"
#include "../src/http.hpp"
#include "../src/proxy.hpp"
#include "../src/reanime.hpp"

namespace {

using namespace shigoku;

int fail(const char* what) {
  std::fprintf(stderr, "reanime_live_smoke: FAIL %s\n", what);
  return 1;
}

int fail_provider(const char* what, const ProviderError& e) {
  std::fprintf(stderr, "reanime_live_smoke: FAIL %s kind=%d status=%u detail=%s\n", what,
               static_cast<int>(e.kind), e.status, e.detail.c_str());
  return 1;
}

std::optional<std::string> fetch(const http::Client& client, const std::string& url) {
  http::Request req;
  req.url = url;
  auto body = client.fetch(req);
  if (!body.has_value()) return std::nullopt;
  return std::string(body->begin(), body->end());
}

std::optional<std::string> quoted(std::string_view line, std::string_view key) {
  const auto at = line.find(key);
  if (at == std::string_view::npos) return std::nullopt;
  const auto start = at + key.size();
  const auto end = line.find('"', start);
  if (end == std::string_view::npos) return std::nullopt;
  return std::string(line.substr(start, end - start));
}

std::optional<std::vector<std::uint8_t>> unhex(std::string_view s) {
  if (s.size() != 32) return std::nullopt;
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i < 32; i += 2) {
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = nib(s[i]);
    const int lo = nib(s[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
  }
  return out;
}

}  // namespace

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("reanime_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  auto provider = reanime::ReAnime::create();
  auto client = http::Client::create();
  if (!provider.has_value() || !client.has_value()) return fail("client setup");

  Enrichment show;
  show.anilist_id = 154587;
  const auto key = provider->canonical_key(show);
  if (!key.has_value()) return fail("canonical key");

  auto eps = provider->episodes(*key, Translation::Sub, 28);
  if (!eps.has_value()) return fail_provider("episodes", eps.error());
  if (eps->size() != 28) return fail("episode count");

  auto link = provider->resolve(*key, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) return fail_provider("resolve sub", link.error());
  if (!link->playlist_cipher.has_value() || link->audio_language != "jpn") {
    return fail("sub link shape");
  }

  auto relay = proxy::engage(*link);
  if (!relay.has_value()) return fail("proxy start");
  const auto master = fetch(*client, std::string(relay->url()));
  if (!master.has_value() || master->rfind("#EXTM3U", 0) != 0) return fail("master via proxy");
  if (master->find("LANGUAGE=\"jpn\"") == std::string::npos ||
      master->find("LANGUAGE=\"eng\"") != std::string::npos) {
    return fail("master audio cut (sub)");
  }

  const auto variants = hls::parse_master_playlist(*master);
  if (variants.empty()) return fail("no variants");
  const hls::Variant* smallest = &variants.front();
  for (const auto& v : variants) {
    if (v.resolution.value_or(99999) < smallest->resolution.value_or(99999)) smallest = &v;
  }
  const auto media = fetch(*client, smallest->url);
  if (!media.has_value() || media->rfind("#EXTM3U", 0) != 0) return fail("variant via proxy");

  std::optional<std::string> key_url, iv_hex, segment_url;
  std::size_t pos = 0;
  while (pos < media->size()) {
    const auto nl = media->find('\n', pos);
    const std::string_view line =
        std::string_view(*media).substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (line.starts_with("#EXT-X-KEY:")) {
      key_url = quoted(line, "URI=\"");
      if (const auto at = line.find("IV=0x"); at != std::string_view::npos) {
        iv_hex = std::string(line.substr(at + 5, 32));
      }
    } else if (!line.empty() && line.front() != '#' && !segment_url.has_value()) {
      segment_url = std::string(line);
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (!key_url.has_value() || !iv_hex.has_value() || !segment_url.has_value()) {
    return fail("variant shape (key/iv/segment)");
  }
  const auto key_body = fetch(*client, *key_url);
  const auto iv = unhex(*iv_hex);
  const auto segment = fetch(*client, *segment_url);
  if (!key_body.has_value() || key_body->size() != 16 || !iv.has_value()) return fail("segment key");
  if (!segment.has_value() || segment->empty()) return fail("segment fetch");
  const std::vector<std::uint8_t> aes_key(key_body->begin(), key_body->end());
  auto ts = crypto::aes128cbc_open(aes_key, *iv,
                                   reinterpret_cast<const std::uint8_t*>(segment->data()),
                                   segment->size());
  if (!ts.has_value() || ts->size() < 377 || (*ts)[0] != 0x47 || (*ts)[188] != 0x47 ||
      (*ts)[376] != 0x47) {
    return fail("segment did not decrypt to MPEG-TS");
  }

  auto dub = provider->resolve(*key, "1", Translation::Dub, Quality::Best);
  if (!dub.has_value()) return fail_provider("resolve dub", dub.error());
  if (dub->audio_language != "eng") return fail("dub link shape");
  auto dub_relay = proxy::engage(*dub);
  if (!dub_relay.has_value()) return fail("proxy start (dub)");
  const auto dub_master = fetch(*client, std::string(dub_relay->url()));
  if (!dub_master.has_value() || dub_master->find("LANGUAGE=\"eng\"") == std::string::npos ||
      dub_master->find("LANGUAGE=\"jpn\"") != std::string::npos) {
    return fail("master audio cut (dub)");
  }

  std::printf(
      "reanime_live_smoke: OK %zu episode(s), %zu variant(s), %up segment %zu bytes -> %zu TS "
      "bytes, sub=%s\n",
      eps->size(), variants.size(), smallest->resolution.value_or(0), segment->size(), ts->size(),
      link->sub_url.has_value() ? "yes" : "no");
  return 0;
}
