// settings.cpp — ported from sabigoku tui/view/settings.rs.

#include "settings.hpp"

#include <algorithm>

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
    case SettingsRowId::MpvPath:
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
    case SettingsRowId::MpvPath:
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
    case SettingsRowId::PlayerPath:
      return;  // not a Toggle row.
  }
}

std::string_view current_text(const Config& config, SettingsRowId id) {
  switch (id) {
    case SettingsRowId::MpvPath: return config.mpv_path;
    case SettingsRowId::MalClientId: return config.mal_client_id;
    case SettingsRowId::DownloadDir: return config.download_dir;
    case SettingsRowId::FfmpegPath: return config.ffmpeg_path;
    case SettingsRowId::PlayerPath: return config.player_path;
    case SettingsRowId::Quality:
    case SettingsRowId::Translation:
    case SettingsRowId::ResumeOffset:
    case SettingsRowId::SkipMode:
    case SettingsRowId::Provider:
    case SettingsRowId::CoverArt:
    case SettingsRowId::KanjiChips:
    case SettingsRowId::Palette:
    case SettingsRowId::TransparentBg:
    case SettingsRowId::Landing:
    case SettingsRowId::TitleLanguage:
    case SettingsRowId::Connect:
    case SettingsRowId::Sync:
    case SettingsRowId::CheckUpdates:
    case SettingsRowId::MalConnect:
    case SettingsRowId::Player:
      return {};  // not a Text row.
  }
  return {};  // unreachable (closed enum).
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
      edit_ = std::string(current_text(config, row.id));
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
    if (!edit_.empty()) {
      // mpv_path/ffmpeg_path: empty commit never blanks them (an empty
      // binary path would break every future spawn). mal_client_id and
      // download_dir are the opposite: blank is a valid, meaningful state
      // ("not configured" / "use the default dir" — see the empty-commit
      // branch below), so a NON-empty commit just sets them here same as any
      // other Text row.
      if (id == SettingsRowId::MalClientId) {
        config.mal_client_id = edit_;
      } else if (id == SettingsRowId::DownloadDir) {
        config.download_dir = edit_;
      } else if (id == SettingsRowId::FfmpegPath) {
        config.ffmpeg_path = edit_;
      } else if (id == SettingsRowId::PlayerPath) {
        config.player_path = edit_;
      } else {
        config.mpv_path = edit_;
      }
      edit_.clear();
      dirty_ = true;
      return SettingsOutcome::ConfigChanged;
    }
    // Empty commit: mal_client_id may be intentionally cleared (the user
    // backspaced everything then hit Enter to disconnect the client
    // registration), download_dir likewise (back to the default dir);
    // mpv_path/ffmpeg_path never blank on empty commit (unreachable
    // fallback path for the spawns).
    if (id == SettingsRowId::MalClientId && !config.mal_client_id.empty()) {
      config.mal_client_id.clear();
      dirty_ = true;
      return SettingsOutcome::ConfigChanged;
    }
    if (id == SettingsRowId::DownloadDir && !config.download_dir.empty()) {
      config.download_dir.clear();
      dirty_ = true;
      return SettingsOutcome::ConfigChanged;
    }
    // player_path blank is meaningful too: "spawn the kind's default binary
    // name" (PlayOpts::player_path doc) — same clearable shape as
    // download_dir, unlike mpv_path/ffmpeg_path which never blank.
    if (id == SettingsRowId::PlayerPath && !config.player_path.empty()) {
      config.player_path.clear();
      dirty_ = true;
      return SettingsOutcome::ConfigChanged;
    }
    edit_.clear();
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
    case SettingsRowId::MpvPath: return config.mpv_path;
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
    case SettingsRowId::MalClientId: return config.mal_client_id;
    case SettingsRowId::MalConnect: return {};
    case SettingsRowId::DownloadDir:
      // Blank shows the resolved default tagged, so "unset" is never confused
      // with an explicit dir (the Provider row's convention).
      if (!config.download_dir.empty()) return config.download_dir;
      return "(default)";
    case SettingsRowId::FfmpegPath: return config.ffmpeg_path;
    case SettingsRowId::Player: return config.player;
    case SettingsRowId::PlayerPath:
      // Blank = the selected kind's default binary name (the DownloadDir
      // convention; mpv ignores this row entirely — it keeps mpv_path).
      if (!config.player_path.empty()) return config.player_path;
      return "(default)";
  }
  return {};  // unreachable (closed enum).
}

std::vector<SettingsLine> settings_layout(const SettingsEnv& env) {
  std::vector<SettingsLine> out;
  auto header = [&](std::string_view h) {
    out.push_back(SettingsLine{SettingsLine::Kind::Header, h, 0, {}, {}});
    out.push_back(SettingsLine{SettingsLine::Kind::Rule, {}, 0, {}, {}});
  };
  auto row = [&](std::size_t i) {
    out.push_back(SettingsLine{SettingsLine::Kind::Row, {}, i, {}, {}});
  };
  auto inert = [&](std::string_view label, std::string_view value) {
    out.push_back(
        SettingsLine{SettingsLine::Kind::Inert, {}, 0, label, std::string(value)});
  };
  auto blank = [&]() { out.push_back(SettingsLine{SettingsLine::Kind::Blank, {}, 0, {}, {}}); };

  header("Player");
  for (std::size_t i = 0; i < 5; ++i) row(i);
  blank();

  header("Catalog");
  inert("metadata refresh", "automatic");
  inert("cover art cache", env.covers_dir);
  row(5);
  blank();

  header("Interface");
  for (std::size_t i = 6; i < 12; ++i) row(i);
  blank();

  header("AniList Sync");
  inert("account", env.account);
  row(12);
  row(13);
  blank();

  header("Updates");
  inert("version", env.version);
  row(14);
  blank();

  header("MyAnimeList");
  inert("account", env.mal_account);
  row(15);
  row(16);
  blank();

  header("Downloads");
  row(17);
  row(18);
  blank();

  header("Player backend");
  row(19);
  row(20);

  return out;
}

}  // namespace shigoku::tui
