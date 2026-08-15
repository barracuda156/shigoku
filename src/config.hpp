// config.hpp — user config per 06 §2 (P18). Ported from sabigoku src/config.rs
// (210 lines): domain enums land later, so keys stay strings here and degrade
// at the call site (a `skip_mode` typo must not disable skipping).
//
// Format: sabigoku's own doc left this open ("TOML or JSON; decide in 08").
// shigoku vendors nlohmann/json (already used for AniList + mpv IPC) and no
// TOML library, so config.json via nlohmann is the natural choice — same
// total-load / atomic-save contract as the Rust reference, only the
// serializer differs.

#pragma once

#include <cstdint>
#include <string>

#include "result.hpp"

namespace shigoku {

// 06 §2.1: oversized file loads as defaults.
inline constexpr std::uint64_t kMaxConfigBytes = 64 * 1024;

// All 06 §2.2 keys. Unknown enum strings are kept verbatim and degrade to
// safe defaults at the call site (domain.hpp's parse_translation/
// parse_quality/parse_title_language already implement that degrade).
struct Config {
  std::string mpv_path = "mpv";
  std::string default_quality = "best";
  std::string translation = "sub";
  std::uint32_t resume_offset_sec = 5;
  std::string skip_mode = "both";
  std::string image_protocol = "auto";
  bool cover_art = true;
  bool kanji_chips = true;
  std::string palette = "terminal_ghost";
  bool transparent_background = false;
  std::string landing = "last_watched";
  std::string title_language = "romaji";
  // Unclamped as stored; read through effective_cover_concurrency().
  std::uint32_t discover_cover_concurrency = 4;
  std::string preferred_provider;  // "" = unset/follow leader.
  bool anilist_sync_enabled = true;
  bool check_for_updates = true;
  // P31 §9.1 slice 2: the user's own MAL app registration (client_id only,
  // no secret — MAL app type "other"). "" = MAL connect refuses cleanly
  // (mal_login::MalConnectResult::Kind::NoClientId) rather than sending a
  // request MAL would reject.
  std::string mal_client_id;
  // P35 slice 3: where episode downloads land. "" = the default,
  // <data>/downloads, resolved at boot (main.cpp) — the config value stays
  // empty so a moved data dir follows automatically.
  std::string download_dir;
  // P35 slice 3: the HLS download muxer, spawned like mpv (runtime-only dep;
  // absent at spawn = the "install ffmpeg" toast, never a config error).
  std::string ffmpeg_path = "ffmpeg";
  // P39 slice 2: which player backend runs playback ("mpv" / "mplayer" /
  // "qmplay2"); any unknown value plays as mpv (player::parse_backend).
  // player_path is the ALTERNATE backends' binary ("" = the kind's default
  // name); mpv keeps reading mpv_path — the two never mix (§9 P39).
  std::string player = "mpv";
  std::string player_path;

  friend bool operator==(const Config&, const Config&) = default;

  // 06 §2.2: clamp to [1, 16] at read.
  [[nodiscard]] std::uint32_t effective_cover_concurrency() const;

  // Total load (06 §2.1): missing, unreadable, oversized, or corrupt files
  // all yield defaults. Startup never wedges on config.
  [[nodiscard]] static Config load(const std::string& path);

  // Save surfaces errors (06 §2.1): Settings and the quit report need them.
  // Write-to-temp + rename: a crash mid-write must never leave a torn file
  // that silently loads as defaults.
  [[nodiscard]] Result<Unit, std::string> save(const std::string& path) const;
};

}  // namespace shigoku
