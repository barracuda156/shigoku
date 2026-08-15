// settings_tests.cpp — P18 Settings view state tests.
//
// Ports the sabigoku tui/view/settings.rs #[test] cases 1:1 (§8: golden tests
// are the port contract) for the state-machine half (SettingsState::on_key,
// cycling, edit mode). The two render/formatting tests (value_forms_match_
// the_mock, tilde_collapses_only_the_home_prefix) port alongside draw_settings
// in views_tests / a dedicated settings-render test once that lands.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "../src/tui/settings.hpp"

using namespace shigoku;
using namespace shigoku::tui;

namespace {

const std::vector<std::string> kProviders = {"megaplay", "senshi", "allanime"};

KeyEvent ch(char32_t c) { return KeyEvent{c, KeyEvent::Special::None, false}; }
KeyEvent special(KeyEvent::Special s) { return KeyEvent{0, s, false}; }

SettingsOutcome press(SettingsState& s, Config& c, std::initializer_list<KeyEvent> keys) {
  SettingsOutcome last = SettingsOutcome::Ignored;
  for (const auto& k : keys) last = s.on_key(k, c, kProviders);
  return last;
}

}  // namespace

TEST_CASE("navigation clamps over interactive rows only") {
  SettingsState s;
  Config c;
  press(s, c, {ch(U'k')});
  CHECK(s.cursor() == 0);
  for (int i = 0; i < 20; ++i) press(s, c, {ch(U'j')});
  CHECK(s.cursor() == kSettingsRows.size() - 1);
}

TEST_CASE("cycle rows step their wheels both ways") {
  SettingsState s;
  Config c;
  press(s, c, {ch(U'j')});  // default quality
  CHECK(press(s, c, {ch(U'h')}) == SettingsOutcome::ConfigChanged);
  CHECK(c.default_quality == "1080");  // best wraps back to 1080
  press(s, c, {ch(U'l')});
  CHECK(c.default_quality == "best");

  press(s, c, {ch(U'j')});  // translation
  press(s, c, {ch(U'l')});
  CHECK(c.translation == "dub");

  press(s, c, {ch(U'j')});  // resume offset
  press(s, c, {ch(U'l')});
  CHECK(c.resume_offset_sec == 10);  // 5 steps to 10
  press(s, c, {ch(U'h'), ch(U'h')});
  CHECK(c.resume_offset_sec == 3);
  CHECK(s.dirty());
}

TEST_CASE("unrecognized stored value cycles from index zero") {
  SettingsState s;
  Config c;
  c.default_quality = "hand-edited";
  press(s, c, {ch(U'j'), ch(U'l')});
  CHECK(c.default_quality == "480");  // unknown reads as index 0
}

TEST_CASE("provider wheel walks unset then names then unset") {
  SettingsState s;
  Config c;
  for (int i = 0; i < 5; ++i) press(s, c, {ch(U'j')});
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::Provider);
  std::vector<std::string> seen;
  for (int i = 0; i < 4; ++i) {
    press(s, c, {ch(U'l')});
    seen.push_back(c.preferred_provider);
  }
  CHECK(seen == std::vector<std::string>{"megaplay", "senshi", "allanime", ""});
  // And backwards off unset lands on the tail.
  press(s, c, {ch(U'h')});
  CHECK(c.preferred_provider == "allanime");
}

TEST_CASE("provider wheel with an empty registry is inert") {
  // settings.rs guards the Provider arm on !providers.is_empty(): cycling with
  // no registry must never erase a configured pin.
  SettingsState s;
  Config c;
  c.preferred_provider = "megaplay";
  for (int i = 0; i < 5; ++i) {
    (void)s.on_key(ch(U'j'), c, {});
  }
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::Provider);
  CHECK(s.on_key(ch(U'l'), c, {}) == SettingsOutcome::Consumed);
  CHECK(c.preferred_provider == "megaplay");
  CHECK(s.on_key(ch(U'h'), c, {}) == SettingsOutcome::Consumed);
  CHECK(c.preferred_provider == "megaplay");
  CHECK(!s.dirty());
}

TEST_CASE("toggles flip with space and report change") {
  SettingsState s;
  Config c;
  for (int i = 0; i < 6; ++i) press(s, c, {ch(U'j')});
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::CoverArt);
  CHECK(press(s, c, {ch(U' ')}) == SettingsOutcome::ConfigChanged);
  CHECK(!c.cover_art);
  // Space on a non-toggle row is consumed, never a fallthrough.
  press(s, c, {ch(U'k')});
  CHECK(press(s, c, {ch(U' ')}) == SettingsOutcome::Consumed);
}

TEST_CASE("transparent bg row flips the config key") {
  SettingsState s;
  Config c;
  for (int i = 0; i < 9; ++i) press(s, c, {ch(U'j')});
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::TransparentBg);
  CHECK(press(s, c, {ch(U' ')}) == SettingsOutcome::ConfigChanged);
  CHECK(c.transparent_background);
  CHECK(s.dirty());
}

TEST_CASE("edit mode prefills appends commits and cancels") {
  SettingsState s;
  Config c;
  press(s, c, {special(KeyEvent::Special::Enter)});
  CHECK(s.editing());
  // Prefilled with the current value; append + backspace.
  press(s, c, {ch(U'-'), ch(U'x'), special(KeyEvent::Special::Backspace)});
  press(s, c, {special(KeyEvent::Special::Enter)});
  CHECK(!s.editing());
  CHECK(c.mpv_path == "mpv-");
  CHECK(s.dirty());

  // Esc discards.
  press(s, c,
        {special(KeyEvent::Special::Enter), ch(U'z'), special(KeyEvent::Special::Escape)});
  CHECK(c.mpv_path == "mpv-");
  CHECK(!s.editing());
}

TEST_CASE("empty commit never blanks the mpv path") {
  SettingsState s;
  Config c;
  press(s, c, {special(KeyEvent::Special::Enter)});
  for (int i = 0; i < 10; ++i) press(s, c, {special(KeyEvent::Special::Backspace)});
  press(s, c, {special(KeyEvent::Special::Enter)});
  CHECK(c.mpv_path == "mpv");  // empty commit is a no-op
}

TEST_CASE("edit mode swallows globals and rejects control chars") {
  SettingsState s;
  Config c;
  press(s, c, {special(KeyEvent::Special::Enter)});
  // q/B/F-key letters are text while editing, never navigation.
  CHECK(press(s, c, {ch(U'q')}) == SettingsOutcome::Consumed);
  CHECK(press(s, c, {ch(0x1b)}) == SettingsOutcome::Consumed);
  press(s, c, {special(KeyEvent::Special::Enter)});
  CHECK(c.mpv_path == "mpvq");  // printable kept, control dropped
}

TEST_CASE("q and view letters fall through when not editing") {
  SettingsState s;
  Config c;
  CHECK(press(s, c, {ch(U'q')}) == SettingsOutcome::Ignored);
  CHECK(press(s, c, {ch(U'B')}) == SettingsOutcome::Ignored);
  CHECK(press(s, c, {special(KeyEvent::Special::Escape)}) == SettingsOutcome::Ignored);
}

TEST_CASE("connect row reports the action") {
  SettingsState s;
  Config c;
  for (int i = 0; i < 12; ++i) press(s, c, {ch(U'j')});
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::Connect);
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConnectRequested);
  CHECK(!s.dirty());  // the action row never dirties the tab
}

TEST_CASE("check updates row toggles the boot gate") {
  SettingsState s;
  Config c;
  for (int i = 0; i < 14; ++i) press(s, c, {ch(U'j')});
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::CheckUpdates);
  CHECK(press(s, c, {ch(U' ')}) == SettingsOutcome::ConfigChanged);
  CHECK(!c.check_for_updates);
  CHECK(s.dirty());
}

TEST_CASE("value forms match the mock") {
  Config c;
  CHECK(settings_value(c, SettingsRowId::ResumeOffset, kProviders) == "5s");
  CHECK(settings_value(c, SettingsRowId::Provider, kProviders) == "megaplay (default)");

  Config pinned;
  pinned.preferred_provider = "senshi";
  CHECK(settings_value(pinned, SettingsRowId::Provider, kProviders) == "senshi");
  CHECK(settings_value(c, SettingsRowId::Connect, kProviders) == "");
}

TEST_CASE("download rows: dir edits and clears, ffmpeg path never blanks (P35)") {
  SettingsState s;
  Config c;
  // Blank download_dir shows the tagged default, never an empty cell.
  CHECK(settings_value(c, SettingsRowId::DownloadDir, kProviders) == "(default)");
  CHECK(settings_value(c, SettingsRowId::FfmpegPath, kProviders) == "ffmpeg");

  s.select(17);  // download dir (the [17, 19) Downloads section).
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::DownloadDir);
  press(s, c, {special(KeyEvent::Special::Enter), ch(U'/'), ch(U'd'), ch(U'l')});
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConfigChanged);
  CHECK(c.download_dir == "/dl");
  // Empty commit clears back to the default (the mal_client_id shape).
  press(s, c, {special(KeyEvent::Special::Enter)});
  for (int i = 0; i < 8; ++i) press(s, c, {special(KeyEvent::Special::Backspace)});
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConfigChanged);
  CHECK(c.download_dir.empty());

  s.select(18);  // ffmpeg path.
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::FfmpegPath);
  press(s, c, {special(KeyEvent::Special::Enter)});
  for (int i = 0; i < 16; ++i) press(s, c, {special(KeyEvent::Special::Backspace)});
  press(s, c, {special(KeyEvent::Special::Enter)});
  CHECK(c.ffmpeg_path == "ffmpeg");  // empty commit never blanks a binary path.
}

TEST_CASE("player wheel cycles the three backends and snaps unknown to mpv (P39)") {
  SettingsState s;
  Config c;
  s.select(19);  // player wheel (the [19, 21) Player backend section).
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::Player);
  CHECK(press(s, c, {ch(U'l')}) == SettingsOutcome::ConfigChanged);
  CHECK(c.player == "mplayer");
  press(s, c, {ch(U'l')});
  CHECK(c.player == "qmplay2");
  press(s, c, {ch(U'l')});
  CHECK(c.player == "mpv");  // full wheel.
  press(s, c, {ch(U'h')});
  CHECK(c.player == "qmplay2");  // and backwards.
  // Unknown stored value (a newer build's backend, a hand-edit) re-enters
  // the wheel at mpv: snap to index 0, then step.
  c.player = "vlc";
  press(s, c, {ch(U'l')});
  CHECK(c.player == "mplayer");
}

TEST_CASE("player path row edits and clears back to the default (P39)") {
  SettingsState s;
  Config c;
  CHECK(settings_value(c, SettingsRowId::Player, kProviders) == "mpv");
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "(default)");
  s.select(20);
  CHECK(kSettingsRows[s.cursor()].id == SettingsRowId::PlayerPath);
  press(s, c, {special(KeyEvent::Special::Enter), ch(U'/'), ch(U'm'), ch(U'p')});
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConfigChanged);
  CHECK(c.player_path == "/mp");
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "/mp");
  // Empty commit clears back to the kind's default binary name — blank is a
  // meaningful state here (the download_dir shape), NOT a guarded binary
  // path like mpv_path/ffmpeg_path: mpv never reads this row.
  press(s, c, {special(KeyEvent::Special::Enter)});
  for (int i = 0; i < 8; ++i) press(s, c, {special(KeyEvent::Special::Backspace)});
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConfigChanged);
  CHECK(c.player_path.empty());
}
