// config.cpp — ported from sabigoku src/config.rs (06 §2).

#include "config.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

namespace shigoku {

namespace {

using json = nlohmann::json;

// Per-field defaulting — a superset of serde's whole-struct #[serde(default)]:
// one bad field degrades to its own default instead of the whole file
// bailing, which config.rs's blanket try-then-unwrap_or_default() also does
// in practice (any parse failure anywhere -> full defaults) but per-field is
// strictly more resilient. A key present with the wrong JSON type counts as
// bad: json::value() would throw on one, so each read checks the type itself.
std::string field(const json& j, const char* key, const std::string& fallback) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool field(const json& j, const char* key, bool fallback) {
  const auto it = j.find(key);
  return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

// A non-negative whole number that fits; a negative, a fraction or a larger
// one is a bad value like any other.
std::uint32_t field(const json& j, const char* key, std::uint32_t fallback) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number_unsigned()) return fallback;
  const std::uint64_t v = it->get<std::uint64_t>();
  return v > std::numeric_limits<std::uint32_t>::max() ? fallback
                                                         : static_cast<std::uint32_t>(v);
}

Config from_json(const json& j) {
  Config c;
  c.mpv_path = field(j, "mpv_path", c.mpv_path);
  c.default_quality = field(j, "default_quality", c.default_quality);
  c.translation = field(j, "translation", c.translation);
  c.resume_offset_sec = field(j, "resume_offset_sec", c.resume_offset_sec);
  c.skip_mode = field(j, "skip_mode", c.skip_mode);
  c.image_protocol = field(j, "image_protocol", c.image_protocol);
  c.cover_art = field(j, "cover_art", c.cover_art);
  c.kanji_chips = field(j, "kanji_chips", c.kanji_chips);
  c.palette = field(j, "palette", c.palette);
  c.transparent_background = field(j, "transparent_background", c.transparent_background);
  c.landing = field(j, "landing", c.landing);
  c.title_language = field(j, "title_language", c.title_language);
  c.discover_cover_concurrency =
      field(j, "discover_cover_concurrency", c.discover_cover_concurrency);
  c.preferred_provider = field(j, "preferred_provider", c.preferred_provider);
  c.anilist_sync_enabled = field(j, "anilist_sync_enabled", c.anilist_sync_enabled);
  c.check_for_updates = field(j, "check_for_updates", c.check_for_updates);
  c.mal_client_id = field(j, "mal_client_id", c.mal_client_id);
  c.catalog = field(j, "catalog", c.catalog);
  c.download_dir = field(j, "download_dir", c.download_dir);
  c.download_quality = field(j, "download_quality", c.download_quality);
  c.ffmpeg_path = field(j, "ffmpeg_path", c.ffmpeg_path);
  c.senshi_node = field(j, "senshi_node", c.senshi_node);
  c.player = field(j, "player", c.player);
  c.player_path = field(j, "player_path", c.player_path);
  c.cli_picker = field(j, "cli_picker", c.cli_picker);
  c.picker_path = field(j, "picker_path", c.picker_path);
  // A list, so json::value's same-type default does not apply: anything but
  // an array of strings keeps the default, a non-string entry drops alone.
  if (const auto it = j.find("disabled_providers"); it != j.end() && it->is_array()) {
    for (const auto& name : *it) {
      if (name.is_string() && !name.get_ref<const std::string&>().empty()) {
        c.disabled_providers.push_back(name.get<std::string>());
      }
    }
  }
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
      {"catalog", c.catalog},
      {"download_dir", c.download_dir},
      {"download_quality", c.download_quality},
      {"ffmpeg_path", c.ffmpeg_path},
      {"senshi_node", c.senshi_node},
      {"player", c.player},
      {"player_path", c.player_path},
      {"cli_picker", c.cli_picker},
      {"picker_path", c.picker_path},
      {"disabled_providers", c.disabled_providers},
  };
}

std::string with_tmp_suffix(const std::string& path) { return path + ".tmp"; }

}  // namespace

std::uint32_t Config::effective_cover_concurrency() const {
  return std::clamp(discover_cover_concurrency, 1u, 16u);
}

std::string Config::effective_download_quality() const {
  return download_quality.empty() ? default_quality : download_quality;
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
