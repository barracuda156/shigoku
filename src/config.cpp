// config.cpp — ported from sabigoku src/config.rs (06 §2).

#include "config.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace shigoku {

namespace {

using json = nlohmann::json;

// Per-field defaulting (json::value(key, default) never throws on a missing
// or wrong-typed key) — a superset of serde's whole-struct #[serde(default)]:
// one bad field degrades to its own default instead of the whole file
// bailing, which config.rs's blanket try-then-unwrap_or_default() also does
// in practice (any parse failure anywhere -> full defaults) but per-field is
// strictly more resilient and free to implement with nlohmann.
Config from_json(const json& j) {
  Config c;
  c.mpv_path = j.value("mpv_path", c.mpv_path);
  c.default_quality = j.value("default_quality", c.default_quality);
  c.translation = j.value("translation", c.translation);
  c.resume_offset_sec = j.value("resume_offset_sec", c.resume_offset_sec);
  c.skip_mode = j.value("skip_mode", c.skip_mode);
  c.image_protocol = j.value("image_protocol", c.image_protocol);
  c.cover_art = j.value("cover_art", c.cover_art);
  c.kanji_chips = j.value("kanji_chips", c.kanji_chips);
  c.palette = j.value("palette", c.palette);
  c.transparent_background = j.value("transparent_background", c.transparent_background);
  c.landing = j.value("landing", c.landing);
  c.title_language = j.value("title_language", c.title_language);
  c.discover_cover_concurrency =
      j.value("discover_cover_concurrency", c.discover_cover_concurrency);
  c.preferred_provider = j.value("preferred_provider", c.preferred_provider);
  c.anilist_sync_enabled = j.value("anilist_sync_enabled", c.anilist_sync_enabled);
  c.check_for_updates = j.value("check_for_updates", c.check_for_updates);
  c.mal_client_id = j.value("mal_client_id", c.mal_client_id);
  c.download_dir = j.value("download_dir", c.download_dir);
  c.ffmpeg_path = j.value("ffmpeg_path", c.ffmpeg_path);
  c.player = j.value("player", c.player);
  c.player_path = j.value("player_path", c.player_path);
  return c;
}

json to_json(const Config& c) {
  return json{
      {"mpv_path", c.mpv_path},
      {"default_quality", c.default_quality},
      {"translation", c.translation},
      {"resume_offset_sec", c.resume_offset_sec},
      {"skip_mode", c.skip_mode},
      {"image_protocol", c.image_protocol},
      {"cover_art", c.cover_art},
      {"kanji_chips", c.kanji_chips},
      {"palette", c.palette},
      {"transparent_background", c.transparent_background},
      {"landing", c.landing},
      {"title_language", c.title_language},
      {"discover_cover_concurrency", c.discover_cover_concurrency},
      {"preferred_provider", c.preferred_provider},
      {"anilist_sync_enabled", c.anilist_sync_enabled},
      {"check_for_updates", c.check_for_updates},
      {"mal_client_id", c.mal_client_id},
      {"download_dir", c.download_dir},
      {"ffmpeg_path", c.ffmpeg_path},
      {"player", c.player},
      {"player_path", c.player_path},
  };
}

std::string with_tmp_suffix(const std::string& path) { return path + ".tmp"; }

}  // namespace

std::uint32_t Config::effective_cover_concurrency() const {
  return std::clamp(discover_cover_concurrency, 1u, 16u);
}

Config Config::load(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return Config{};
  // Regular files only: a FIFO/device would block the read forever and wedge
  // startup (06 §2.1 total load). A non-regular or oversized file is defaults.
  if (!S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) > kMaxConfigBytes) {
    return Config{};
  }

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return Config{};
  std::ostringstream buf;
  buf << in.rdbuf();
  if (!in.good() && !in.eof()) return Config{};

  json parsed;
  try {
    parsed = json::parse(buf.str());
  } catch (const json::parse_error&) {
    return Config{};
  }
  if (!parsed.is_object()) return Config{};
  return from_json(parsed);
}

Result<Unit, std::string> Config::save(const std::string& path) const {
  const std::string tmp = with_tmp_suffix(path);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return err(std::string("open failed: ") + tmp);
    }
    out << to_json(*this).dump(2);
    out.flush();
    if (!out.good()) {
      return err(std::string("write failed: ") + tmp);
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return err(std::string("rename failed: ") + path);
  }
  return Unit{};
}

}  // namespace shigoku
