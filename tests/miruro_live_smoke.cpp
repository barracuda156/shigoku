// miruro_live_smoke.cpp — search, list and resolve against the live site,
// then fetch the stream as a player would, as far as the first bytes of its
// first segment. Not part of the offline ctest suite: run by hand or by
// scripts/provider_canary.sh, which set SHIGOKU_LIVE=1. Skips (exit 0) when
// the variable is absent.
//
// Flow: search "Frieren" -> the hit naming AniList 154587 -> episodes (sub)
// -> resolve episode 1 sub -> the master with the link's Referer, Origin and
// user agent -> its lowest variant (or the playlist itself when it is
// a media playlist) -> the first segment's first bytes.
//
// Exit 77 when the very first request is refused (Cloudflare challenges
// some addresses whatever the client), so a run can tell that apart from
// a failure.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../src/hls.hpp"
#include "../src/http.hpp"
#include "../src/miruro.hpp"

namespace {

using namespace shigoku;

constexpr std::int64_t kFrierenAnilist = 154587;
constexpr std::size_t kSegmentProbeBytes = 4096;

int fail(const char* what) {
  std::fprintf(stderr, "miruro_live_smoke: FAIL %s\n", what);
  return 1;
}

int fail_provider(const char* what, const ProviderError& e) {
  std::fprintf(stderr, "miruro_live_smoke: FAIL %s kind=%d status=%u detail=%s\n", what,
               static_cast<int>(e.kind), e.status, e.detail.c_str());
  return 1;
}

http::Request stream_request(const std::string& url, const StreamLink& link) {
  http::Request req;
  req.url = url;
  if (link.user_agent.has_value()) req.user_agent = *link.user_agent;
  if (link.referer.has_value()) req.extra_headers.push_back({"Referer", *link.referer});
  if (link.origin.has_value()) req.extra_headers.push_back({"Origin", *link.origin});
  return req;
}

Result<std::string, ProviderError> fetch_text(const http::Client& client, const std::string& url,
                                              const StreamLink& link) {
  if (!http::guard_fetch_url(url).has_value()) return err(ProviderError::decode("unsafe url"));
  auto body = client.fetch(stream_request(url, link));
  if (!body.has_value()) return err(body.error());
  return std::string(body->begin(), body->end());
}

std::optional<std::string> first_segment(std::string_view media) {
  std::size_t pos = 0;
  while (pos < media.size()) {
    const auto nl = media.find('\n', pos);
    std::string_view line =
        media.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (!line.empty() && line.front() != '#') return std::string(line);
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return std::nullopt;
}

}  // namespace

int main() {
  if (std::getenv("SHIGOKU_LIVE") == nullptr) {
    std::printf("miruro_live_smoke: SKIP (SHIGOKU_LIVE not set)\n");
    return 0;
  }

  auto provider = miruro::Miruro::create();
  auto client = http::Client::create();
  if (!provider.has_value() || !client.has_value()) return fail("client setup");

  auto hits = provider->search("Frieren", SearchOptions{});
  if (!hits.has_value()) {
    if (hits.error().kind == ProviderError::Kind::Forbidden) {
      std::printf("miruro_live_smoke: SKIP Cloudflare challenges this address\n");
      return 77;
    }
    return fail_provider("search", hits.error());
  }
  const SearchHit* pick = nullptr;
  for (const auto& h : *hits) {
    if (h.anilist_id == kFrierenAnilist) {
      pick = &h;
      break;
    }
  }
  if (pick == nullptr) return fail("search: no hit names AniList 154587");
  std::printf("miruro_live_smoke: search %zu hit(s), using id=%s title='%s'\n", hits->size(),
              pick->provider_id.c_str(), pick->title.c_str());

  auto eps = provider->episodes(pick->provider_id, Translation::Sub, std::nullopt);
  if (!eps.has_value()) return fail_provider("episodes", eps.error());
  if (eps->empty()) return fail("episodes: empty listing");

  auto link = provider->resolve(pick->provider_id, "1", Translation::Sub, Quality::Best);
  if (!link.has_value()) return fail_provider("resolve", link.error());

  auto master = fetch_text(*client, link->url, *link);
  if (!master.has_value()) return fail_provider("master fetch", master.error());
  if (!miruro::detail::is_playlist(*master)) return fail("master is not a playlist");

  std::string media_url = link->url;
  std::string media = *master;
  const auto variants = hls::parse_master_playlist(*master);
  if (!variants.empty()) {
    const hls::Variant* lowest = &variants.front();
    for (const auto& v : variants) {
      if (v.resolution.value_or(99999) < lowest->resolution.value_or(99999)) lowest = &v;
    }
    const auto joined = hls::join_url(link->url, lowest->url);
    if (!joined.has_value()) return fail("variant url does not join");
    media_url = *joined;
    auto body = fetch_text(*client, media_url, *link);
    if (!body.has_value()) return fail_provider("variant fetch", body.error());
    if (!miruro::detail::is_playlist(*body)) return fail("variant is not a playlist");
    media = std::move(*body);
  }

  const auto segment = first_segment(media);
  if (!segment.has_value()) return fail("media playlist names no segment");
  const auto segment_url = hls::join_url(media_url, *segment);
  if (!segment_url.has_value() || !http::guard_fetch_url(*segment_url).has_value()) {
    return fail("segment url");
  }
  http::Request req = stream_request(*segment_url, *link);
  req.timeout_secs = 20;
  std::vector<std::uint8_t> head;
  const auto got = client->fetch_to_sink(
      req, [&](const http::StreamMeta&, const std::uint8_t* data, std::size_t n) {
        head.insert(head.end(), data, data + n);
        return head.size() < kSegmentProbeBytes;
      });
  if (head.empty()) {
    if (!got.has_value()) return fail_provider("segment fetch", got.error());
    return fail("segment fetch: empty body");
  }

  std::printf(
      "miruro_live_smoke: OK %zu episode(s), %zu variant(s), segment first %zu byte(s) "
      "(0x%02x), origin=%s sub=%s\n",
      eps->size(), variants.size(), head.size(), static_cast<unsigned>(head[0]),
      link->origin.has_value() ? link->origin->c_str() : "-", link->sub_url.has_value() ? "yes" : "no");
  return 0;
}
