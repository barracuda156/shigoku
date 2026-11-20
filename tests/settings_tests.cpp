// settings_tests.cpp — the Settings view state: row navigation, preset
// cycling, toggles, edit mode, the value strings, and the line layout the
// draw and the mouse hit-test both read.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <utility>
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

// Park the cursor on the row `id`, wherever the table puts it.
void at(SettingsState& s, SettingsRowId id) {
  const std::size_t ix = settings_row_index(id);
  REQUIRE(ix < kSettingsRows.size());
  s.select(ix);
  REQUIRE(kSettingsRows[s.cursor()].id == id);
}

// Enter, `n` backspaces, Enter: commit an emptied edit buffer.
SettingsOutcome commit_empty(SettingsState& s, Config& c, int n = 32) {
  press(s, c, {special(KeyEvent::Special::Enter)});
  for (int i = 0; i < n; ++i) press(s, c, {special(KeyEvent::Special::Backspace)});
  return press(s, c, {special(KeyEvent::Special::Enter)});
}

}  // namespace

TEST_CASE("navigation clamps over interactive rows only") {
  SettingsState s;
  Config c;
  press(s, c, {ch(U'k')});
  CHECK(s.cursor() == 0);
  press(s, c, {ch(U'j'), ch(U'j'), ch(U'k')});
  CHECK(s.cursor() == 1);
  for (std::size_t i = 0; i < kSettingsRows.size(); ++i) press(s, c, {ch(U'j')});
  CHECK(s.cursor() == kSettingsRows.size() - 1);
}

TEST_CASE("cycle rows step their wheels both ways") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::Quality);
  CHECK(press(s, c, {ch(U'h')}) == SettingsOutcome::ConfigChanged);
  CHECK(c.default_quality == "1080");  // best wraps back to 1080
  press(s, c, {ch(U'l')});
  CHECK(c.default_quality == "best");

  at(s, SettingsRowId::Translation);
  press(s, c, {ch(U'l')});
  CHECK(c.translation == "dub");

  at(s, SettingsRowId::ResumeOffset);
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
  at(s, SettingsRowId::Quality);
  press(s, c, {ch(U'l')});
  CHECK(c.default_quality == "480");  // unknown reads as index 0
}

TEST_CASE("provider wheel walks unset then names then unset") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::Provider);
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
  // Cycling with no registry must never erase a configured pin.
  SettingsState s;
  Config c;
  c.preferred_provider = "megaplay";
  at(s, SettingsRowId::Provider);
  CHECK(s.on_key(ch(U'l'), c, {}) == SettingsOutcome::Consumed);
  CHECK(c.preferred_provider == "megaplay");
  CHECK(s.on_key(ch(U'h'), c, {}) == SettingsOutcome::Consumed);
  CHECK(c.preferred_provider == "megaplay");
  CHECK(!s.dirty());
}

TEST_CASE("toggles flip with space and report change") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::CoverArt);
  CHECK(press(s, c, {ch(U' ')}) == SettingsOutcome::ConfigChanged);
  CHECK(!c.cover_art);
  // Space on a non-toggle row is consumed, never a fallthrough.
  at(s, SettingsRowId::Palette);
  CHECK(press(s, c, {ch(U' ')}) == SettingsOutcome::Consumed);
}

TEST_CASE("transparent bg row flips the config key") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::TransparentBg);
  CHECK(press(s, c, {ch(U' ')}) == SettingsOutcome::ConfigChanged);
  CHECK(c.transparent_background);
  CHECK(s.dirty());
}

TEST_CASE("edit mode prefills appends commits and cancels") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::PlayerPath);
  press(s, c, {special(KeyEvent::Special::Enter)});
  CHECK(s.editing());
  CHECK(s.edit_buffer() == "mpv");  // prefilled with the current value
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
  at(s, SettingsRowId::PlayerPath);
  CHECK(commit_empty(s, c, 10) == SettingsOutcome::Consumed);
  CHECK(c.mpv_path == "mpv");  // empty commit is a no-op
  CHECK(!s.dirty());
}

TEST_CASE("edit mode swallows globals and rejects control chars") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::PlayerPath);
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

TEST_CASE("connect rows report their actions") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::Connect);
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConnectRequested);
  at(s, SettingsRowId::MalConnect);
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) ==
        SettingsOutcome::MalConnectRequested);
  CHECK(!s.dirty());  // the action rows never dirty the tab
}

TEST_CASE("check updates row toggles the boot gate") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::CheckUpdates);
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

TEST_CASE("download rows: dir edits and clears, ffmpeg path never blanks") {
  SettingsState s;
  Config c;
  // Blank download_dir shows the tagged default, never an empty cell.
  CHECK(settings_value(c, SettingsRowId::DownloadDir, kProviders) == "(default)");
  CHECK(settings_value(c, SettingsRowId::FfmpegPath, kProviders) == "ffmpeg");

  at(s, SettingsRowId::DownloadDir);
  press(s, c, {special(KeyEvent::Special::Enter), ch(U'/'), ch(U'd'), ch(U'l')});
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConfigChanged);
  CHECK(c.download_dir == "/dl");
  // Empty commit clears back to the default (the mal_client_id shape).
  CHECK(commit_empty(s, c, 8) == SettingsOutcome::ConfigChanged);
  CHECK(c.download_dir.empty());

  at(s, SettingsRowId::FfmpegPath);
  commit_empty(s, c, 16);
  CHECK(c.ffmpeg_path == "ffmpeg");  // empty commit never blanks a binary path.
}

TEST_CASE("download quality wheel follows playback first, then the presets") {
  SettingsState s;
  Config c;
  CHECK(settings_value(c, SettingsRowId::DownloadQuality, kProviders) == "same as playback");
  at(s, SettingsRowId::DownloadQuality);
  std::vector<std::string> seen;
  for (int i = 0; i < 6; ++i) {
    CHECK(press(s, c, {ch(U'l')}) == SettingsOutcome::ConfigChanged);
    seen.push_back(c.download_quality);
  }
  CHECK(seen == std::vector<std::string>{"worst", "480", "720", "1080", "best", ""});
  CHECK(s.dirty());
  // And backwards off "same as playback" lands on best.
  press(s, c, {ch(U'h')});
  CHECK(c.download_quality == "best");
  CHECK(settings_value(c, SettingsRowId::DownloadQuality, kProviders) == "best");
  press(s, c, {ch(U'h')});
  CHECK(c.download_quality == "1080");
  // An unknown stored value snaps to "same as playback" before the step.
  c.download_quality = "4k";
  press(s, c, {ch(U'l')});
  CHECK(c.download_quality == "worst");
  c.download_quality = "4k";
  press(s, c, {ch(U'h')});
  CHECK(c.download_quality == "best");
  // The wheel never touches the playback quality.
  CHECK(c.default_quality == "best");
}

TEST_CASE("player wheel cycles the three backends and snaps unknown to mpv") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::Player);
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

TEST_CASE("player path row edits mpv_path while mpv is the player") {
  SettingsState s;
  Config c;
  CHECK(settings_value(c, SettingsRowId::Player, kProviders) == "mpv");
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "mpv");
  at(s, SettingsRowId::PlayerPath);
  press(s, c, {special(KeyEvent::Special::Enter)});
  CHECK(s.edit_buffer() == "mpv");
  for (int i = 0; i < 3; ++i) press(s, c, {special(KeyEvent::Special::Backspace)});
  press(s, c, {ch(U'/'), ch(U'm'), ch(U'v')});
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConfigChanged);
  CHECK(c.mpv_path == "/mv");
  CHECK(c.player_path.empty());  // the alternate backends' path is untouched.
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "/mv");
  // Blank is never an mpv path.
  CHECK(commit_empty(s, c) == SettingsOutcome::Consumed);
  CHECK(c.mpv_path == "/mv");
}

TEST_CASE("player path row edits player_path while an alternate backend is the player") {
  SettingsState s;
  Config c;
  c.player = "mplayer";
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "(default)");
  at(s, SettingsRowId::PlayerPath);
  press(s, c, {special(KeyEvent::Special::Enter)});
  CHECK(s.edit_buffer().empty());  // prefilled from player_path, not mpv_path.
  press(s, c, {ch(U'/'), ch(U'm'), ch(U'p')});
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConfigChanged);
  CHECK(c.player_path == "/mp");
  CHECK(c.mpv_path == "mpv");  // mpv's own path is untouched.
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "/mp");
  // Empty commit clears back to the backend's default binary name — blank
  // is a meaningful state here (the download_dir shape).
  CHECK(commit_empty(s, c, 8) == SettingsOutcome::ConfigChanged);
  CHECK(c.player_path.empty());
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "(default)");

  // qmplay2 reads the same field.
  c.player = "qmplay2";
  c.player_path = "/opt/qmplay2";
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "/opt/qmplay2");
}

TEST_CASE("player path row follows the player wheel") {
  SettingsState s;
  Config c;
  c.mpv_path = "/opt/mpv";
  c.player_path = "/opt/mplayer";
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "/opt/mpv");
  at(s, SettingsRowId::Player);
  press(s, c, {ch(U'l')});
  REQUIRE(c.player == "mplayer");
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "/opt/mplayer");
  press(s, c, {ch(U'h')});
  REQUIRE(c.player == "mpv");
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "/opt/mpv");
}

TEST_CASE("player path row treats an unknown player as mpv") {
  SettingsState s;
  Config c;
  c.player = "vlc";  // plays as mpv.
  c.player_path = "/opt/vlc";
  CHECK(settings_value(c, SettingsRowId::PlayerPath, kProviders) == "mpv");
  at(s, SettingsRowId::PlayerPath);
  press(s, c, {special(KeyEvent::Special::Enter), ch(U'2')});
  CHECK(press(s, c, {special(KeyEvent::Special::Enter)}) == SettingsOutcome::ConfigChanged);
  CHECK(c.mpv_path == "mpv2");
  CHECK(c.player_path == "/opt/vlc");
  CHECK(commit_empty(s, c) == SettingsOutcome::Consumed);
  CHECK(c.mpv_path == "mpv2");
}

TEST_CASE("catalog wheel cycles auto/anilist/mal and snaps unknown to auto") {
  SettingsState s;
  Config c;
  at(s, SettingsRowId::Catalog);
  CHECK(settings_value(c, SettingsRowId::Catalog, kProviders) == "auto");
  CHECK(press(s, c, {ch(U'l')}) == SettingsOutcome::ConfigChanged);
  CHECK(c.catalog == "anilist");
  press(s, c, {ch(U'l')});
  CHECK(c.catalog == "mal");
  press(s, c, {ch(U'l')});
  CHECK(c.catalog == "auto");  // full wheel.
  press(s, c, {ch(U'h')});
  CHECK(c.catalog == "mal");  // and backwards.
  c.catalog = "jikan";
  press(s, c, {ch(U'l')});
  CHECK(c.catalog == "anilist");  // unknown re-enters at auto, then steps.
}

TEST_CASE("layout draws every row once, in table order, under its section") {
  const SettingsEnv env{"~/.cache/shigoku/covers", "anilist-user", "0.2.0", "mal-user"};
  const std::vector<SettingsLine> lines = settings_layout(env);

  std::vector<int> drawn(kSettingsRows.size(), 0);
  std::size_t next = 0;
  for (const SettingsLine& li : lines) {
    if (li.kind != SettingsLine::Kind::Row) continue;
    REQUIRE(li.row < kSettingsRows.size());
    ++drawn[li.row];
    CHECK(li.row == next);  // j/k moves down the screen one row at a time.
    ++next;
  }
  for (std::size_t i = 0; i < drawn.size(); ++i) {
    CHECK_MESSAGE(drawn[i] == 1, "row ", i, " (", std::string(kSettingsRows[i].label),
                  ") drawn ", drawn[i], " times");
  }

  // Each section as its header, then its rows (labels) and inert lines
  // ("(label: value)"), in draw order.
  using Section = std::pair<std::string, std::vector<std::string>>;
  std::vector<Section> got;
  for (const SettingsLine& li : lines) {
    switch (li.kind) {
      case SettingsLine::Kind::Header:
        got.emplace_back(std::string(li.header), std::vector<std::string>{});
        break;
      case SettingsLine::Kind::Row:
        REQUIRE(!got.empty());
        got.back().second.emplace_back(kSettingsRows[li.row].label);
        break;
      case SettingsLine::Kind::Inert:
        REQUIRE(!got.empty());
        got.back().second.push_back("(" + std::string(li.inert_label) + ": " + li.inert_value +
                                    ")");
        break;
      case SettingsLine::Kind::Rule:
      case SettingsLine::Kind::Blank:
        break;
    }
  }
  const std::vector<Section> want = {
      {"Playback",
       {"player", "player path", "default quality", "translation", "resume offset",
        "skip mode"}},
      {"Sources",
       {"catalog", "preferred provider", "(metadata refresh: automatic)",
        "(cover art cache: ~/.cache/shigoku/covers)"}},
      {"Downloads", {"download dir", "download quality", "ffmpeg path"}},
      {"Interface",
       {"cover art", "kanji chips", "palette", "transparent background", "landing view",
        "title language"}},
      {"AniList", {"(account: anilist-user)", "connect", "sync enabled"}},
      {"MyAnimeList", {"(account: mal-user)", "mal client id", "connect"}},
      {"Updates", {"(version: 0.2.0)", "check for updates"}},
  };
  REQUIRE(got.size() == want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    CHECK(got[i].first == want[i].first);
    CHECK(got[i].second == want[i].second);
  }
}
