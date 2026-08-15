// settings.hpp — the Settings view state (P18, DESIGN §5.5 / 05 §13). Ported
// 1:1 from sabigoku tui/view/settings.rs (822 lines): the 15-row model, the
// cursor/edit/dirty state machine, and the preset cycling tables. Rendering
// lives in views.cpp (draw_settings); this header owns only state so it can
// join shigoku_tui_render (pure, no curl/store — the app.hpp rule).
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
  MpvPath,
  Quality,
  Translation,
  ResumeOffset,
  SkipMode,
  Provider,
  CoverArt,
  KanjiChips,
  Palette,
  TransparentBg,
  Landing,
  TitleLanguage,
  Connect,
  Sync,
  CheckUpdates,
  // --- MyAnimeList mirror (P31 §9.1 slice 2) ---
  MalClientId,
  MalConnect,
  // --- Downloads (P35 slice 3) ---
  DownloadDir,
  FfmpegPath,
  // --- Player backend (P39 slice 2) ---
  Player,
  PlayerPath,
};

enum class SettingsRowKind { Text, Cycle, Toggle, Action };

struct SettingsRow {
  SettingsRowId id;
  std::string_view label;
  SettingsRowKind kind;
  std::string_view hint;
};

// Fixed order (DESIGN §5.5 mock); section boundaries below are load-bearing —
// a row insertion that shifts a section must update the static_asserts too.
// The two MyAnimeList rows are appended at the tail (not interleaved into
// the existing sections) specifically so every prior index/static_assert
// stays byte-stable — P31 is a post-parity addition (§8), never a reshape.
inline constexpr std::array<SettingsRow, 21> kSettingsRows = {{
    // --- Player [0, 5) ---
    {SettingsRowId::MpvPath, "mpv path", SettingsRowKind::Text, "enter to edit"},
    {SettingsRowId::Quality, "default quality", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::Translation, "translation", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::ResumeOffset, "resume offset", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::SkipMode, "skip mode", SettingsRowKind::Cycle, "h/l cycle"},
    // --- Catalog [5, 6) ---
    {SettingsRowId::Provider, "preferred provider", SettingsRowKind::Cycle, "h/l cycle"},
    // --- Interface [6, 12) ---
    {SettingsRowId::CoverArt, "cover art", SettingsRowKind::Toggle, "space toggle"},
    {SettingsRowId::KanjiChips, "kanji chips", SettingsRowKind::Toggle, "space toggle"},
    {SettingsRowId::Palette, "palette", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::TransparentBg, "transparent background", SettingsRowKind::Toggle, "space toggle"},
    {SettingsRowId::Landing, "landing view", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::TitleLanguage, "title language", SettingsRowKind::Cycle, "h/l cycle"},
    // --- AniList Sync [12, 14) ---
    {SettingsRowId::Connect, "connect", SettingsRowKind::Action, "enter"},
    {SettingsRowId::Sync, "sync enabled", SettingsRowKind::Toggle, "space toggle"},
    // --- Updates [14, 15) ---
    {SettingsRowId::CheckUpdates, "check for updates", SettingsRowKind::Toggle, "space toggle"},
    // --- MyAnimeList [15, 17) (P31 §9.1 slice 2) ---
    {SettingsRowId::MalClientId, "mal client id", SettingsRowKind::Text, "enter to edit"},
    {SettingsRowId::MalConnect, "connect", SettingsRowKind::Action, "enter"},
    // --- Downloads [17, 19) (P35 slice 3; appended at the tail, same
    //     post-parity byte-stability rationale as the MAL rows above) ---
    {SettingsRowId::DownloadDir, "download dir", SettingsRowKind::Text, "enter to edit"},
    {SettingsRowId::FfmpegPath, "ffmpeg path", SettingsRowKind::Text, "enter to edit"},
    // --- Player backend [19, 21) (P39 slice 2; appended at the tail, same
    //     post-parity byte-stability rationale as the MAL rows above) ---
    {SettingsRowId::Player, "player", SettingsRowKind::Cycle, "h/l cycle"},
    {SettingsRowId::PlayerPath, "player path", SettingsRowKind::Text, "enter to edit"},
}};

static_assert(kSettingsRows.size() == 21);
static_assert(kSettingsRows[0].id == SettingsRowId::MpvPath);      // Player starts at 0
static_assert(kSettingsRows[5].id == SettingsRowId::Provider);     // Catalog starts at 5
static_assert(kSettingsRows[6].id == SettingsRowId::CoverArt);     // Interface starts at 6
static_assert(kSettingsRows[12].id == SettingsRowId::Connect);     // AniList Sync starts at 12
static_assert(kSettingsRows[14].id == SettingsRowId::CheckUpdates);// Updates starts at 14
static_assert(kSettingsRows[15].id == SettingsRowId::MalClientId);// MyAnimeList starts at 15
static_assert(kSettingsRows[17].id == SettingsRowId::DownloadDir);// Downloads starts at 17
static_assert(kSettingsRows[19].id == SettingsRowId::Player);     // Player backend starts at 19

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
// P39 slice 2: the backend wheel. Order is the §9 tier order (mpv first =
// the default AND the snap target for an unknown stored value).
inline constexpr std::array<std::string_view, 3> kPlayerPresets = {
    "mpv", "mplayer", "qmplay2"};

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

// The Settings view's whole state (settings.rs SettingsState). `cursor`
// indexes kSettingsRows directly (all 15 rows are interactive or grouped by
// row index — no separate inert-row skip list needed since inert rows
// (account/version/catalog status) are never part of kSettingsRows; layout()
// interleaves them for draw only).
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
  std::string_view covers_dir;  // Catalog's "cover art cache" row, ~-collapsed.
  std::string_view account;     // AniList Sync's "account" row.
  std::string_view version;     // Updates' "version" row.
  std::string_view mal_account; // MyAnimeList's "account" row (P31 slice 2).
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

// The DESIGN §5.5 line layout: 5 sections, headers/rules/rows/inert lines,
// one blank between sections (settings.rs layout()).
[[nodiscard]] std::vector<SettingsLine> settings_layout(const SettingsEnv& env);

// value() (settings.rs): the display string for a row's current value —
// text/cycle rows show the raw config string, Provider shows the pinned name
// or "{leader} (default)", toggles show "on"/"off", Connect is always "".
[[nodiscard]] std::string settings_value(const Config& config, SettingsRowId id,
                                         const std::vector<std::string>& providers);

}  // namespace shigoku::tui
