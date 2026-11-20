// settings.cpp — ported from sabigoku tui/view/settings.rs.

#include "settings.hpp"

#include "../mal_catalog.hpp"

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace shigoku::tui {

namespace {

// cycle_preset (settings.rs): unrecognized current value snaps to index 0
// before applying dir, matching "out-of-preset snaps valid" (05 §13).
template <std::size_t N>
std::string_view cycle_preset(const std::array<std::string_view, N>& presets,
                              std::string_view current, int dir) {
  std::size_t idx = 0;
  for (std::size_t i = 0; i < N; ++i) {
    if (presets[i] == current) { idx = i; break; }
  }
  const std::size_t n = N;
  idx = (idx + static_cast<std::size_t>(dir + static_cast<int>(n))) % n;
  return presets[idx];
}

// cycle_provider (settings.rs): unset(0) -> names[0..n] -> unset, wraps. An
// unrecognized non-empty current value (a retired provider) also re-enters
// at unset on the next cycle, matching "unknown re-enters at unset".
std::string cycle_provider(const std::vector<std::string>& names, std::string_view current,
                           int dir) {
  if (names.empty()) return std::string{};
  // Position 0 = unset, 1..n = names[0..n-1].
  std::size_t pos = 0;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (names[i] == current) { pos = i + 1; break; }
  }
  const std::size_t n = names.size() + 1;
  pos = (pos + static_cast<std::size_t>(dir + static_cast<int>(n))) % n;
  if (pos == 0) return std::string{};
  return names[pos - 1];
}

std::uint32_t cycle_resume_offset(std::uint32_t current, int dir) {
  std::size_t idx = 0;
  for (std::size_t i = 0; i < kResumeOffsetPresets.size(); ++i) {
    if (kResumeOffsetPresets[i] == current) { idx = i; break; }
  }
  const std::size_t n = kResumeOffsetPresets.size();
  idx = (idx + static_cast<std::size_t>(dir + static_cast<int>(n))) % n;
  return kResumeOffsetPresets[idx];
}

// The backends that read player_path. mpv, and any spelling the player
// plays as mpv, reads mpv_path instead.
bool alternate_player(const Config& config) {
  return config.player == "mplayer" || config.player == "qmplay2";
}

void cycle(Config& config, SettingsRowId id, int dir, const std::vector<std::string>& providers) {
  switch (id) {
    case SettingsRowId::Quality:
      config.default_quality = std::string(cycle_preset(kQualityPresets, config.default_quality, dir));
      return;
    case SettingsRowId::Translation:
      config.translation = std::string(cycle_preset(kTranslationPresets, config.translation, dir));
      return;
    case SettingsRowId::ResumeOffset:
      config.resume_offset_sec = cycle_resume_offset(config.resume_offset_sec, dir);
      return;
    case SettingsRowId::SkipMode:
      config.skip_mode = std::string(cycle_preset(kSkipPresets, config.skip_mode, dir));
      return;
    case SettingsRowId::Provider:
      // Empty registry: inert, never erase a configured pin (settings.rs guards
      // the whole arm on !providers.is_empty()).
      if (!providers.empty()) {
        config.preferred_provider = cycle_provider(providers, config.preferred_provider, dir);
      }
      return;
    case SettingsRowId::Palette:
      config.palette = std::string(cycle_preset(kPalettePresets, config.palette, dir));
      return;
    case SettingsRowId::Landing:
      config.landing = std::string(cycle_preset(kLandingPresets, config.landing, dir));
      return;
    case SettingsRowId::TitleLanguage:
      config.title_language = std::string(cycle_preset(kTitleLanguagePresets, config.title_language, dir));
      return;
    case SettingsRowId::Player:
      // Unknown stored value snaps to index 0 = mpv before the step — the
      // wheel-side half of "unknown re-enters at mpv" (§9 P39 slice 2;
      // player::parse_backend is the play-side half).
      config.player = std::string(cycle_preset(kPlayerPresets, config.player, dir));
      return;
    case SettingsRowId::Catalog:
      config.catalog = std::string(cycle_preset(kCatalogPresets, config.catalog, dir));
      return;
    case SettingsRowId::DownloadQuality:
      config.download_quality =
          std::string(cycle_preset(kDownloadQualityPresets, config.download_quality, dir));
      return;
    case SettingsRowId::CoverArt:
    case SettingsRowId::KanjiChips:
    case SettingsRowId::TransparentBg:
    case SettingsRowId::Connect:
    case SettingsRowId::Sync:
    case SettingsRowId::CheckUpdates:
    case SettingsRowId::MalClientId:
    case SettingsRowId::MalConnect:
    case SettingsRowId::DownloadDir:
    case SettingsRowId::FfmpegPath:
    case SettingsRowId::PlayerPath:
      return;  // not a Cycle row.
  }
}

void toggle(Config& config, SettingsRowId id) {
  switch (id) {
    case SettingsRowId::CoverArt: config.cover_art = !config.cover_art; return;
    case SettingsRowId::KanjiChips: config.kanji_chips = !config.kanji_chips; return;
    case SettingsRowId::TransparentBg:
      config.transparent_background = !config.transparent_background;
      return;
    case SettingsRowId::Sync: config.anilist_sync_enabled = !config.anilist_sync_enabled; return;
    case SettingsRowId::CheckUpdates: config.check_for_updates = !config.check_for_updates; return;
    case SettingsRowId::Quality:
    case SettingsRowId::Translation:
    case SettingsRowId::ResumeOffset:
    case SettingsRowId::SkipMode:
    case SettingsRowId::Provider:
    case SettingsRowId::Palette:
    case SettingsRowId::Landing:
    case SettingsRowId::TitleLanguage:
    case SettingsRowId::Connect:
    case SettingsRowId::MalClientId:
    case SettingsRowId::MalConnect:
    case SettingsRowId::DownloadDir:
    case SettingsRowId::FfmpegPath:
    case SettingsRowId::Player:
    case SettingsRowId::Catalog:
    case SettingsRowId::PlayerPath:
    case SettingsRowId::DownloadQuality:
      return;  // not a Toggle row.
  }
}

// The config string a Text row edits; nullptr for any other row.
std::string* text_field(Config& config, SettingsRowId id) {
  switch (id) {
    case SettingsRowId::PlayerPath:
      return alternate_player(config) ? &config.player_path : &config.mpv_path;
    case SettingsRowId::MalClientId: return &config.mal_client_id;
    case SettingsRowId::DownloadDir: return &config.download_dir;
    case SettingsRowId::FfmpegPath: return &config.ffmpeg_path;
    case SettingsRowId::Player:
    case SettingsRowId::Quality:
    case SettingsRowId::Translation:
    case SettingsRowId::ResumeOffset:
    case SettingsRowId::SkipMode:
    case SettingsRowId::Catalog:
    case SettingsRowId::Provider:
    case SettingsRowId::DownloadQuality:
    case SettingsRowId::CoverArt:
    case SettingsRowId::KanjiChips:
    case SettingsRowId::Palette:
    case SettingsRowId::TransparentBg:
    case SettingsRowId::Landing:
    case SettingsRowId::TitleLanguage:
    case SettingsRowId::Connect:
    case SettingsRowId::Sync:
    case SettingsRowId::MalConnect:
    case SettingsRowId::CheckUpdates:
      return nullptr;  // not a Text row.
  }
  return nullptr;  // unreachable (closed enum).
}

// Text rows where blank is a meaningful value an empty commit may set:
// mal_client_id (the built-in registration), download_dir (the default dir),
// player_path (the alternate backend's default binary name). mpv_path and
// ffmpeg_path never blank — an empty binary path would break every spawn.
bool blank_allowed(const Config& config, SettingsRowId id) {
  return id == SettingsRowId::MalClientId || id == SettingsRowId::DownloadDir ||
         (id == SettingsRowId::PlayerPath && alternate_player(config));
}

}  // namespace

SettingsOutcome SettingsState::on_key(const KeyEvent& key, Config& config,
                                      const std::vector<std::string>& providers) {
  if (editing_) return on_edit_key(key, config);

  using S = KeyEvent::Special;
  const SettingsRow& row = kSettingsRows[cursor_];

  if (key.special == S::None && !key.ctrl) {
    switch (key.codepoint) {
      case U'j':
        if (cursor_ + 1 < kSettingsRows.size()) ++cursor_;
        return SettingsOutcome::Consumed;
      case U'k':
        if (cursor_ > 0) --cursor_;
        return SettingsOutcome::Consumed;
      case U'h':
      case U'l':
        if (row.kind == SettingsRowKind::Cycle) {
          // Provider wheel with an empty registry is inert: no change, no
          // dirty (settings.rs guards the arm on !providers.is_empty()).
          if (row.id == SettingsRowId::Provider && providers.empty()) {
            return SettingsOutcome::Consumed;
          }
          cycle(config, row.id, key.codepoint == U'l' ? 1 : -1, providers);
          dirty_ = true;
          return SettingsOutcome::ConfigChanged;
        }
        return SettingsOutcome::Consumed;
      case U' ':
        if (row.kind == SettingsRowKind::Toggle) {
          toggle(config, row.id);
          dirty_ = true;
          return SettingsOutcome::ConfigChanged;
        }
        return SettingsOutcome::Consumed;
      default: break;
    }
    return SettingsOutcome::Ignored;
  }

  if (key.special == S::Down) {
    if (cursor_ + 1 < kSettingsRows.size()) ++cursor_;
    return SettingsOutcome::Consumed;
  }
  if (key.special == S::Up) {
    if (cursor_ > 0) --cursor_;
    return SettingsOutcome::Consumed;
  }
  if (key.special == S::Left || key.special == S::Right) {
    if (row.kind == SettingsRowKind::Cycle) {
      cycle(config, row.id, key.special == S::Right ? 1 : -1, providers);
      dirty_ = true;
      return SettingsOutcome::ConfigChanged;
    }
    return SettingsOutcome::Consumed;
  }
  if (key.special == S::Enter) {
    if (row.kind == SettingsRowKind::Text) {
      const std::string* field = text_field(config, row.id);
      edit_ = field != nullptr ? *field : std::string{};
      editing_ = true;
      return SettingsOutcome::Consumed;
    }
    if (row.kind == SettingsRowKind::Action) {
      return row.id == SettingsRowId::MalConnect ? SettingsOutcome::MalConnectRequested
                                                  : SettingsOutcome::ConnectRequested;
    }
    return SettingsOutcome::Consumed;
  }

  return SettingsOutcome::Ignored;
}

SettingsOutcome SettingsState::on_edit_key(const KeyEvent& key, Config& config) {
  using S = KeyEvent::Special;
  if (key.special == S::Escape) {
    editing_ = false;
    edit_.clear();
    return SettingsOutcome::Consumed;
  }
  if (key.special == S::Enter) {
    editing_ = false;
    const SettingsRowId id = kSettingsRows[cursor_].id;
    std::string* field = text_field(config, id);
    std::string committed = std::move(edit_);
    edit_.clear();
    if (field == nullptr) return SettingsOutcome::Consumed;
    if (!committed.empty()) {
      *field = std::move(committed);
      dirty_ = true;
      return SettingsOutcome::ConfigChanged;
    }
    if (blank_allowed(config, id) && !field->empty()) {
      field->clear();
      dirty_ = true;
      return SettingsOutcome::ConfigChanged;
    }
    return SettingsOutcome::Consumed;
  }
  if (key.special == S::Backspace) {
    if (!edit_.empty()) {
      while (!edit_.empty() && (static_cast<unsigned char>(edit_.back()) & 0xC0) == 0x80) {
        edit_.pop_back();
      }
      if (!edit_.empty()) edit_.pop_back();
    }
    return SettingsOutcome::Consumed;
  }
  if (key.special == S::None && !key.ctrl && key.codepoint >= 0x20 && key.codepoint < 0x7F) {
    if (edit_.size() < kSettingsEditMax) {
      edit_.push_back(static_cast<char>(key.codepoint));
    }
    return SettingsOutcome::Consumed;
  }
  return SettingsOutcome::Consumed;  // edit mode swallows everything else (05 §13).
}

void SettingsState::select(std::size_t row) {
  if (editing_ || row >= kSettingsRows.size()) return;
  cursor_ = row;
}

std::string settings_value(const Config& config, SettingsRowId id,
                           const std::vector<std::string>& providers) {
  auto onoff = [](bool v) { return std::string(v ? "on" : "off"); };
  switch (id) {
    case SettingsRowId::Quality: return config.default_quality;
    case SettingsRowId::Translation: return config.translation;
    case SettingsRowId::ResumeOffset: return std::to_string(config.resume_offset_sec) + "s";
    case SettingsRowId::SkipMode: return config.skip_mode;
    case SettingsRowId::Provider:
      // Unset shows the leader tagged so blank is never confused with an
      // explicit pin to the same provider.
      if (!config.preferred_provider.empty()) return config.preferred_provider;
      if (!providers.empty()) return providers.front() + " (default)";
      return {};
    case SettingsRowId::CoverArt: return onoff(config.cover_art);
    case SettingsRowId::KanjiChips: return onoff(config.kanji_chips);
    case SettingsRowId::Palette: return config.palette;
    case SettingsRowId::TransparentBg: return onoff(config.transparent_background);
    case SettingsRowId::Landing: return config.landing;
    case SettingsRowId::TitleLanguage: return config.title_language;
    case SettingsRowId::Sync: return onoff(config.anilist_sync_enabled);
    case SettingsRowId::Connect: return {};
    case SettingsRowId::CheckUpdates: return onoff(config.check_for_updates);
    case SettingsRowId::MalClientId:
      // Blank shows that the app's own registration is in use (the
      // DownloadDir "(default)" convention); an explicit id overrides it.
      if (!config.mal_client_id.empty()) return config.mal_client_id;
      return mal_catalog::kClientId[0] != '\0' ? "(built-in)" : "";
    case SettingsRowId::MalConnect: return {};
    case SettingsRowId::DownloadDir:
      // Blank shows the resolved default tagged, so "unset" is never confused
      // with an explicit dir (the Provider row's convention).
      if (!config.download_dir.empty()) return config.download_dir;
      return "(default)";
    case SettingsRowId::FfmpegPath: return config.ffmpeg_path;
    case SettingsRowId::Player: return config.player;
    case SettingsRowId::Catalog: return config.catalog;
    case SettingsRowId::PlayerPath:
      // An alternate backend's blank path = its default binary name (the
      // DownloadDir convention); mpv's path is never blank.
      if (!alternate_player(config)) return config.mpv_path;
      if (!config.player_path.empty()) return config.player_path;
      return "(default)";
    case SettingsRowId::DownloadQuality:
      if (!config.download_quality.empty()) return config.download_quality;
      return "same as playback";
  }
  return {};  // unreachable (closed enum).
}

std::vector<SettingsLine> settings_layout(const SettingsEnv& env) {
  std::vector<SettingsLine> out;
  auto header = [&](std::string_view h) {
    out.push_back(SettingsLine{SettingsLine::Kind::Header, h, 0, {}, {}});
    out.push_back(SettingsLine{SettingsLine::Kind::Rule, {}, 0, {}, {}});
  };
  auto rows = [&](std::initializer_list<SettingsRowId> ids) {
    for (const SettingsRowId id : ids) {
      out.push_back(SettingsLine{SettingsLine::Kind::Row, {}, settings_row_index(id), {}, {}});
    }
  };
  auto inert = [&](std::string_view label, std::string_view value) {
    out.push_back(
        SettingsLine{SettingsLine::Kind::Inert, {}, 0, label, std::string(value)});
  };
  auto blank = [&]() { out.push_back(SettingsLine{SettingsLine::Kind::Blank, {}, 0, {}, {}}); };

  using R = SettingsRowId;
  header("Playback");
  rows({R::Player, R::PlayerPath, R::Quality, R::Translation, R::ResumeOffset, R::SkipMode});
  blank();

  header("Sources");
  rows({R::Catalog, R::Provider});
  inert("metadata refresh", "automatic");
  inert("cover art cache", env.covers_dir);
  blank();

  header("Downloads");
  rows({R::DownloadDir, R::DownloadQuality, R::FfmpegPath});
  blank();

  header("Interface");
  rows({R::CoverArt, R::KanjiChips, R::Palette, R::TransparentBg, R::Landing,
        R::TitleLanguage});
  blank();

  header("AniList");
  inert("account", env.account);
  rows({R::Connect, R::Sync});
  blank();

  header("MyAnimeList");
  inert("account", env.mal_account);
  rows({R::MalClientId, R::MalConnect});
  blank();

  header("Updates");
  inert("version", env.version);
  rows({R::CheckUpdates});

  return out;
}

}  // namespace shigoku::tui
