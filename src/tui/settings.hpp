// settings.hpp — the Settings view state: the row table, the cursor/edit/
// dirty state machine, and the preset cycling tables. Rendering lives in
// views.cpp (draw_settings); this header owns only state so it can join
// shigoku_tui_render (pure, no curl/store — the app.hpp rule).
//
// `Config` is the mutated target (config.hpp); SettingsState never owns one,
// mirroring how DiscoverState is handed the store pointer per-call rather
// than owning it.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "../config.hpp"
#include "../event.hpp"  // KeyEvent.

namespace shigoku::tui {

enum class SettingsRowId {
  Player,
  PlayerPath,
  Quality,
  Translation,
  ResumeOffset,
  SkipMode,
  Catalog,
  Provider,
  DownloadDir,
  DownloadQuality,
  FfmpegPath,
  CoverArt,
  KanjiChips,
  Palette,
  TransparentBg,
  Landing,
  TitleLanguage,
  Connect,
  Sync,
  MalClientId,
  MalConnect,
  CheckUpdates,
};

enum class SettingsRowKind { Text, Cycle, Toggle, Action };

struct SettingsRow {
  SettingsRowId id;
  std::string_view label;
  SettingsRowKind kind;
  std::string_view hint;
};

// Table order is the j/k order and the on-screen order: settings_layout()
// emits every row once, in this order, under its section header. The
// static_asserts below pin where each section starts.
inline constexpr std::array<SettingsRow, 22> kSettingsRows = {{
    // --- Playback [0, 6) ---
    {SettingsRowId::Player, "player", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::PlayerPath, "player path", SettingsRowKind::Text, "enter to edit"},
    {SettingsRowId::Quality, "default quality", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::Translation, "translation", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::ResumeOffset, "resume offset", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::SkipMode, "skip mode", SettingsRowKind::Cycle, "h/l cycle"},
    // --- Sources [6, 8) ---
    {SettingsRowId::Catalog, "catalog", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::Provider, "preferred provider", SettingsRowKind::Cycle, "h/l cycle"},
    // --- Downloads [8, 11) ---
    {SettingsRowId::DownloadDir, "download dir", SettingsRowKind::Text, "enter to edit"},
    {SettingsRowId::DownloadQuality, "download quality", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::FfmpegPath, "ffmpeg path", SettingsRowKind::Text, "enter to edit"},
    // --- Interface [11, 17) ---
    {SettingsRowId::CoverArt, "cover art", SettingsRowKind::Toggle, "space toggle"},
    {SettingsRowId::KanjiChips, "kanji chips", SettingsRowKind::Toggle, "space toggle"},
    {SettingsRowId::Palette, "palette", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::TransparentBg, "transparent background", SettingsRowKind::Toggle, "space toggle"},
    {SettingsRowId::Landing, "landing view", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::TitleLanguage, "title language", SettingsRowKind::Cycle, "h/l cycle"},
    // --- AniList [17, 19) ---
    {SettingsRowId::Connect, "connect", SettingsRowKind::Action, "enter"},
    {SettingsRowId::Sync, "sync enabled", SettingsRowKind::Toggle, "space toggle"},
    // --- MyAnimeList [19, 21) ---
    {SettingsRowId::MalClientId, "mal client id", SettingsRowKind::Text, "enter to edit"},
    {SettingsRowId::MalConnect, "connect", SettingsRowKind::Action, "enter"},
    // --- Updates [21, 22) ---
    {SettingsRowId::CheckUpdates, "check for updates", SettingsRowKind::Toggle, "space toggle"},
}};

static_assert(kSettingsRows.size() == 22);
static_assert(kSettingsRows[0].id == SettingsRowId::Player);         // Playback starts at 0
static_assert(kSettingsRows[6].id == SettingsRowId::Catalog);        // Sources starts at 6
static_assert(kSettingsRows[8].id == SettingsRowId::DownloadDir);    // Downloads starts at 8
static_assert(kSettingsRows[11].id == SettingsRowId::CoverArt);      // Interface starts at 11
static_assert(kSettingsRows[17].id == SettingsRowId::Connect);       // AniList starts at 17
static_assert(kSettingsRows[19].id == SettingsRowId::MalClientId);   // MyAnimeList starts at 19
static_assert(kSettingsRows[21].id == SettingsRowId::CheckUpdates);  // Updates starts at 21

// The kSettingsRows index of `id`; kSettingsRows.size() when the table has
// no such row.
[[nodiscard]] constexpr std::size_t settings_row_index(SettingsRowId id) {
  for (std::size_t i = 0; i < kSettingsRows.size(); ++i) {
    if (kSettingsRows[i].id == id) return i;
  }
  return kSettingsRows.size();
}

// Preset cycle tables (05 §13: "out-of-preset snaps valid"; DESIGN §5.5).
inline constexpr std::array<std::string_view, 5> kQualityPresets = {
    "worst", "480", "720", "1080", "best"};
inline constexpr std::array<std::string_view, 2> kTranslationPresets = {"sub", "dub"};
inline constexpr std::array<std::string_view, 4> kSkipPresets = {
    "none", "intro", "outro", "both"};
inline constexpr std::array<std::uint32_t, 6> kResumeOffsetPresets = {0, 3, 5, 10, 15, 30};
// Only "terminal_ghost" actually renders in shigoku today (theme.hpp is fixed
// constants, no per-instance Palette resolver) — the other three persist and
// round-trip correctly but have no visual effect until that lands (§1 scope
// table, P18_PLAN.md).
inline constexpr std::array<std::string_view, 4> kPalettePresets = {
    "terminal_ghost", "phosphor", "nord", "tokyonight"};
inline constexpr std::array<std::string_view, 3> kLandingPresets = {
    "history", "browse", "last_watched"};
inline constexpr std::array<std::string_view, 3> kTitleLanguagePresets = {
    "romaji", "english", "native"};
// The backend wheel: mpv first (the default AND the snap target for an
// unknown stored value), then the alternate backends.
inline constexpr std::array<std::string_view, 3> kPlayerPresets = {
    "mpv", "mplayer", "qmplay2"};
// The catalog wheel: auto first (the default AND the snap target for an
// unknown stored value), then the two fixed sources.
inline constexpr std::array<std::string_view, 3> kCatalogPresets = {
    "auto", "anilist", "mal"};
// The download quality wheel: "" = follow default_quality first (the default
// AND the snap target for an unknown stored value), then the quality presets.
inline constexpr std::array<std::string_view, 6> kDownloadQualityPresets = {
    "", "worst", "480", "720", "1080", "best"};

inline constexpr std::size_t kSettingsEditMax = 256;

enum class SettingsOutcome {
  Ignored,
  Consumed,
  ConfigChanged,
  ConnectRequested,
  // P31 §9.1 slice 2: the MyAnimeList "connect" row's Action (distinct from
  // ConnectRequested, which is AniList-only) — app.cpp opens the MAL connect
  // session instead of AniList's.
  MalConnectRequested,
};

// The Settings view's whole state. `cursor` indexes kSettingsRows directly:
// every row in the table is interactive, and the inert lines (account/
// version/cache status) are never part of it — settings_layout() interleaves
// them for draw only.
class SettingsState {
 public:
  [[nodiscard]] std::size_t cursor() const { return cursor_; }
  [[nodiscard]] bool editing() const { return editing_; }
  [[nodiscard]] const std::string& edit_buffer() const { return edit_; }
  [[nodiscard]] bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }

  // Dispatch one key in normal mode; edit mode is handled internally
  // (settings.rs on_key / on_edit_key). `providers` is the registry's names
  // in construction order (app.cpp supplies this per-call so this header
  // never touches provider.hpp).
  [[nodiscard]] SettingsOutcome on_key(const KeyEvent& key, Config& config,
                                       const std::vector<std::string>& providers);

  // Click-to-select (P32, §9 — no Rust precedent): move the cursor to `row`
  // (a kSettingsRows index). Inert while editing — the edit buffer owns
  // input until Enter/Esc — and on an out-of-range index.
  void select(std::size_t row);

 private:
  [[nodiscard]] SettingsOutcome on_edit_key(const KeyEvent& key, Config& config);

  std::size_t cursor_ = 0;
  bool editing_ = false;
  std::string edit_;
  bool dirty_ = false;
};

// Runtime facts the read-only rows display (settings.rs SettingsEnv);
// borrowed per draw. `covers_dir`/`account`/`version` are pre-formatted
// strings so this header never touches paths.hpp / auth (not built yet).
struct SettingsEnv {
  std::string_view covers_dir;  // Sources' "cover art cache" row, ~-collapsed.
  std::string_view account;     // AniList's "account" row.
  std::string_view version;     // Updates' "version" row.
  std::string_view mal_account; // MyAnimeList's "account" row.
};

// One display line of the Settings layout (mirrors HistoryLine's role: the
// single source both nav and draw consume). Inert lines (account/version/
// catalog status) carry their pre-formatted value; Row lines index
// kSettingsRows (the row's own live value is read at draw time from Config,
// since it can change every frame the cursor/edit state does).
struct SettingsLine {
  enum class Kind { Header, Rule, Row, Inert, Blank } kind = Kind::Blank;
  std::string_view header;         // Header only.
  std::size_t row = 0;             // Row only: index into kSettingsRows.
  std::string_view inert_label;    // Inert only.
  std::string inert_value;         // Inert only.
};

// The line layout: 7 sections (Playback, Sources, Downloads, Interface,
// AniList, MyAnimeList, Updates), each a header + rule + its rows and inert
// lines, one blank between sections. Every kSettingsRows index appears as
// exactly one Row line, in table order.
[[nodiscard]] std::vector<SettingsLine> settings_layout(const SettingsEnv& env);

// The display string for a row's current value — text/cycle rows show the
// raw config string, Provider shows the pinned name or "{leader} (default)",
// toggles show "on"/"off", the connect rows are always "". The player path
// row shows the selected backend's binary (mpv_path for mpv and any unknown
// backend, player_path or "(default)" for mplayer/qmplay2); a blank download
// quality shows "same as playback".
[[nodiscard]] std::string settings_value(const Config& config, SettingsRowId id,
                                         const std::vector<std::string>& providers);

}  // namespace shigoku::tui
