// ladder.cpp — see ladder.hpp.

#include "ladder.hpp"

#include "proxy.hpp"

namespace shigoku::ladder {

bool is_playlist_url(std::string_view url) {
  std::size_t end = url.size();
  for (std::size_t i = 0; i < url.size(); ++i) {
    if (url[i] == '?' || url[i] == '#') {
      end = i;
      break;
    }
  }
  const std::string_view path = url.substr(0, end);
  const std::size_t dot = path.rfind('.');
  const std::size_t slash = path.rfind('/');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
    return false;
  }
  std::string ext(path.substr(dot + 1));
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return ext == "m3u8" || ext == "m3u";
}

bool is_playlist(const StreamLink& link) {
  return link.cloaked_segments || proxy::needed(link) || is_playlist_url(link.url);
}

std::optional<Master> read(const http::Client& client, const StreamLink& link,
                           std::string_view play_url) {
  http::Request req;
  req.url = std::string(play_url);
  if (link.user_agent) req.user_agent = *link.user_agent;
  if (link.referer) req.extra_headers.push_back({"Referer", *link.referer});
  if (link.origin) req.extra_headers.push_back({"Origin", *link.origin});
  auto body = client.fetch(req);
  if (!body.has_value()) return std::nullopt;
  Master master;
  master.text.assign(reinterpret_cast<const char*>(body->data()), body->size());
  if (hls::parse_master_playlist(master.text).empty()) return std::nullopt;
  master.url = std::string(play_url);
  return master;
}

Result<std::optional<hls::Pair>, GuardError> take(const Master& master, bool relayed,
                                                  const Rung& rung) {
  auto pair = hls::choose_pair(master.text, master.url, rung);
  if (!pair.has_value()) return std::optional<hls::Pair>{};
  if (!relayed) {
    if (auto g = http::guard_fetch_url(pair->video_url); !g.has_value()) return err(g.error());
    if (pair->audio_url) {
      if (auto g = http::guard_fetch_url(*pair->audio_url); !g.has_value()) return err(g.error());
    }
  }
  return std::optional<hls::Pair>(std::move(*pair));
}

}  // namespace shigoku::ladder
