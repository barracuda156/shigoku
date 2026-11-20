// download_library.cpp — the download tree read back; see download_library.hpp.

#include "download_library.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

namespace shigoku::download {

namespace {

constexpr std::string_view kPartSuffix = ".part";

std::vector<std::string> list_names(const std::string& dir) {
  std::vector<std::string> names;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return names;
  while (const struct dirent* e = ::readdir(d)) {
    const std::string_view name = e->d_name;
    if (name != "." && name != "..") names.emplace_back(name);
  }
  ::closedir(d);
  return names;
}

std::optional<std::int64_t> parse_show_id(std::string_view s) {
  const bool negative = !s.empty() && s.front() == '-';
  if (negative) s.remove_prefix(1);
  if (s.empty()) return std::nullopt;
  const std::uint64_t limit = negative ? std::uint64_t{1} << 63 : (std::uint64_t{1} << 63) - 1;
  std::uint64_t magnitude = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (magnitude > (limit - digit) / 10) return std::nullopt;
    magnitude = magnitude * 10 + digit;
  }
  if (!negative) return static_cast<std::int64_t>(magnitude);
  if (magnitude == limit) return std::numeric_limits<std::int64_t>::min();
  return -static_cast<std::int64_t>(magnitude);
}

// The stem of a finished file's name `<stem>.<ext>`, split at the last dot.
std::optional<std::string_view> finished_stem(std::string_view name) {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) return std::nullopt;
  if (name.substr(dot + 1) == "part") return std::nullopt;
  return name.substr(0, dot);
}

bool in_episode_order(const LocalEpisode& a, const LocalEpisode& b) {
  if (const int c = episode_label_cmp(a.label, b.label); c != 0) return c < 0;
  if (a.label != b.label) return a.label < b.label;
  return a.path < b.path;
}

std::vector<LocalEpisode> scan_track(const std::string& dir, Translation translation) {
  std::map<std::string, LocalEpisode> finished;  // by label.
  std::vector<LocalEpisode> partials;
  for (const std::string& name : list_names(dir)) {
    std::string_view base = name;
    const bool partial = base.ends_with(kPartSuffix);
    if (partial) base.remove_suffix(kPartSuffix.size());
    const std::optional<std::string_view> stem = finished_stem(base);
    if (!stem.has_value()) continue;

    std::string path = dir + '/' + name;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

    LocalEpisode ep;
    ep.label = std::string(*stem);
    ep.translation = translation;
    ep.path = std::move(path);
    ep.bytes = static_cast<std::uint64_t>(st.st_size);
    ep.partial = partial;
    if (partial) {
      partials.push_back(std::move(ep));
      continue;
    }
    const auto it = finished.find(ep.label);
    if (it == finished.end()) {
      std::string label = ep.label;
      finished.emplace(std::move(label), std::move(ep));
    } else if (ep.path < it->second.path) {
      it->second = std::move(ep);
    }
  }

  std::vector<LocalEpisode> out;
  out.reserve(finished.size() + partials.size());
  for (LocalEpisode& ep : partials) {
    if (!finished.contains(ep.label)) out.push_back(std::move(ep));
  }
  for (auto& entry : finished) out.push_back(std::move(entry.second));
  std::sort(out.begin(), out.end(), in_episode_order);
  return out;
}

}  // namespace

std::vector<LocalShow> scan_library(std::string_view download_dir) {
  std::vector<LocalShow> shows;
  if (download_dir.empty()) return shows;
  std::string root(download_dir);
  if (root.back() != '/') root.push_back('/');

  std::vector<std::string> names = list_names(root);
  std::sort(names.begin(), names.end());
  for (std::string& name : names) {
    if (name.front() == '.') continue;
    const std::string show_dir = root + name;
    struct stat st{};
    if (::stat(show_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

    LocalShow show;
    for (const Translation t : {Translation::Sub, Translation::Dub}) {
      std::vector<LocalEpisode> track = scan_track(show_dir + '/' + std::string(to_string(t)), t);
      show.episodes.insert(show.episodes.end(), std::make_move_iterator(track.begin()),
                           std::make_move_iterator(track.end()));
    }
    if (show.episodes.empty()) continue;
    show.anilist_id = parse_show_id(name);
    show.folder = std::move(name);
    shows.push_back(std::move(show));
  }
  return shows;
}

bool remove_local_episode(const LocalEpisode& episode) {
  const std::string& path = episode.path;
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) return false;

  const std::size_t last = path.rfind('/');
  if (last == std::string::npos || last == 0) return true;
  const std::string track_dir = path.substr(0, last);
  (void)::rmdir(track_dir.c_str());  // fails, harmlessly, while anything is left.

  const std::size_t prev = path.rfind('/', last - 1);
  if (prev == std::string::npos || prev == 0) return true;
  const std::string show_dir = path.substr(0, prev);
  (void)::rmdir(show_dir.c_str());
  return true;
}

}  // namespace shigoku::download
