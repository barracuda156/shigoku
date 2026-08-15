// mconfig.cpp — manga config-lite. The config.cpp mechanics in miniature:
// nlohmann parse, json::value per-field defaulting, tmp+rename save.

#include "mconfig.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace shigoku::manga {

namespace {

using json = nlohmann::json;

inline constexpr std::uint64_t kMaxBytes = 64 * 1024;  // oversized -> defaults.

MangaConfig from_json(const json& j) {
  const MangaConfig d;
  MangaConfig c;
  // json::value(key, default) never throws on a MISSING key, but an
  // ill-TYPED one throws type_error — catch per field so one bad key can't
  // zero its neighbors (the config.cpp per-field defaulting law).
  auto get_str = [&](const char* key, const std::string& dflt) {
    try {
      return j.value(key, dflt);
    } catch (const json::type_error&) {
      return dflt;
    }
  };
  auto get_bool = [&](const char* key, bool dflt) {
    try {
      return j.value(key, dflt);
    } catch (const json::type_error&) {
      return dflt;
    }
  };
  c.viewer = get_str("viewer", d.viewer);
  c.viewer_path = get_str("viewer_path", d.viewer_path);
  c.rtl = get_bool("rtl", d.rtl);
  c.data_saver = get_bool("data_saver", d.data_saver);
  c.chapter_lang = get_str("chapter_lang", d.chapter_lang);
  if (c.chapter_lang.empty()) c.chapter_lang = d.chapter_lang;
  c.source = get_str("source", d.source);
  if (c.source.empty()) c.source = d.source;
  c.nsfw_sources = get_bool("nsfw_sources", d.nsfw_sources);
  c.anilist_sync = get_bool("anilist_sync", d.anilist_sync);
  return c;
}

json to_json(const MangaConfig& c) {
  return json{
      {"viewer", c.viewer},           {"viewer_path", c.viewer_path},
      {"rtl", c.rtl},                 {"data_saver", c.data_saver},
      {"chapter_lang", c.chapter_lang}, {"source", c.source},
      {"nsfw_sources", c.nsfw_sources}, {"anilist_sync", c.anilist_sync},
  };
}

}  // namespace

MangaConfig parse_manga_config(const std::string& raw) {
  if (raw.size() > kMaxBytes) return MangaConfig{};
  json parsed;
  try {
    parsed = json::parse(raw);
  } catch (const json::parse_error&) {
    return MangaConfig{};
  }
  if (!parsed.is_object()) return MangaConfig{};
  return from_json(parsed);
}

MangaConfig load_manga_config(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return MangaConfig{};
  std::ostringstream buf;
  buf << in.rdbuf();
  if (!in.good() && !in.eof()) return MangaConfig{};
  return parse_manga_config(buf.str());
}

Result<Unit, std::string> save_manga_config(const std::string& path,
                                            const MangaConfig& c) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return err(std::string("open failed: ") + tmp);
    out << to_json(c).dump(2);
    out.flush();
    if (!out.good()) return err(std::string("write failed: ") + tmp);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return err(std::string("rename failed: ") + path);
  }
  return Unit{};
}

void save_default_if_missing(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0) return;
  (void)save_manga_config(path, MangaConfig{});
}

}  // namespace shigoku::manga
