// cli_tests.cpp — P27 CLI surface tests.
//
// Ports sabigoku cli.rs's #[test] mod 1:1 (parse dispatch, the sync/connect/
// search/pick renderers, classify_pick, paste_line_usable, fetch/player error
// copy) AND main.rs's play_flow exit-table mod (06 §7.4: the play path is the
// one nonzero exit; every early return is a clean 0). Offline: a scripted fake
// provider + a scripted picker, no network, no mpv spawn (the exit cases all
// return before mpv would launch — resolve runs first each attempt).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../src/cli.hpp"
#include "../src/config.hpp"
#include "../src/domain.hpp"
#include "../src/download.hpp"
#include "../src/idmap.hpp"
#include "../src/login.hpp"
#include "../src/play_cli.hpp"
#include "../src/provider.hpp"
#include "../src/store.hpp"
#include "../src/sync.hpp"

using namespace shigoku;
using shigoku::cli::Command;
using shigoku::cli::PlayArgs;

namespace {

Command parse_of(std::vector<std::string> args) { return cli::parse(args); }

PlayArgs play(std::string q, bool dub = false, std::optional<std::string> quality = std::nullopt) {
  PlayArgs a;
  a.query = std::move(q);
  a.dub = dub;
  a.quality = std::move(quality);
  return a;
}

cli::DownloadArgs dl(std::string q, std::optional<std::string> ep = std::nullopt,
                     bool dub = false) {
  cli::DownloadArgs a;
  a.query = std::move(q);
  a.episode = std::move(ep);
  a.dub = dub;
  return a;
}

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

std::size_t count_of(std::string_view hay, std::string_view needle) {
  std::size_t n = 0, pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string_view::npos) {
    ++n;
    pos += needle.size();
  }
  return n;
}

std::size_t line_count(std::string_view s) {
  if (s.empty()) return 0;
  std::size_t n = 0;
  for (char c : s) {
    if (c == '\n') ++n;
  }
  // A trailing-newline-terminated block of k lines has k newlines; the Rust
  // `.lines().count()` counts k too (it ignores a final empty split).
  return n;
}

// Append the UTF-8 encoding of a codepoint. Hostile codepoints (ESC, bidi
// controls) are built this way, never pasted raw — a literal U+202E/ESC in
// this source would itself trip -Werror=bidi-chars (see domain_tests put_cp).
void put_cp(std::string& out, unsigned long c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

// A hostile "ro\x1b[31md" + RTL-override, as strip_controls' input.
std::string hostile_name() {
  std::string s = "ro";
  put_cp(s, 0x1B);  // ESC
  s += "[31md";
  put_cp(s, 0x202E);  // RIGHT-TO-LEFT OVERRIDE
  return s;
}

// True if the raw ESC (0x1B) or the U+202E UTF-8 bytes survived in `s`.
bool has_esc(std::string_view s) { return s.find('\x1b') != std::string_view::npos; }
bool has_bidi(std::string_view s) {
  std::string rlo;
  put_cp(rlo, 0x202E);
  return s.find(rlo) != std::string_view::npos;
}

}  // namespace

// ── parse (cli.rs parse tests) ──────────────────────────────────────────────

TEST_CASE("no_args_and_bare_flags_launch_the_tui") {
  CHECK(parse_of({}) == Command::tui());
  CHECK(parse_of({"--dub"}) == Command::tui());
  CHECK(parse_of({"--debug", "--sub"}) == Command::tui());
}

TEST_CASE("version_flag_wins_anywhere_even_over_a_query_or_bad_flag") {
  CHECK(parse_of({"--version"}) == Command::version());
  CHECK(parse_of({"-V"}) == Command::version());
  CHECK(parse_of({"frieren", "--version"}) == Command::version());
  CHECK(parse_of({"--nope", "-V"}) == Command::version());
}

TEST_CASE("flags_may_precede_a_subcommand") {
  CHECK(parse_of({"--debug", "login"}) == Command::login(false));
  CHECK(parse_of({"--paste", "login"}) == Command::login(true));
  CHECK(parse_of({"login", "--paste"}) == Command::login(true));
  CHECK(parse_of({"--debug", "sync"}) == Command::sync());
  CHECK(parse_of({"update"}) == Command::update());
}

TEST_CASE("after_a_query_word_subcommand_names_are_search_text") {
  CHECK(parse_of({"frieren", "login"}) == Command::play(play("frieren login")));
  CHECK(parse_of({"cowboy", "sync"}) == Command::play(play("cowboy sync")));
  CHECK(parse_of({"frieren", "download"}) ==
        Command::play(play("frieren download")));
}

TEST_CASE("download_parses_query_then_episode (P35 slice 3)") {
  // Last positional = episode when 2+ follow; a lone word = interactive pick.
  CHECK(parse_of({"download", "frieren", "7"}) ==
        Command::download(dl("frieren", std::string("7"))));
  CHECK(parse_of({"download", "cowboy", "bebop", "SP1"}) ==
        Command::download(dl("cowboy bebop", std::string("SP1"))));
  CHECK(parse_of({"download", "frieren"}) == Command::download(dl("frieren")));
  // Flags ride anywhere; no positionals at all is usage; unknown flag too.
  CHECK(parse_of({"download", "frieren", "7", "--dub"}) ==
        Command::download(dl("frieren", std::string("7"), true)));
  CHECK(parse_of({"--dub", "download", "frieren"}) ==
        Command::download(dl("frieren", std::nullopt, true)));
  CHECK(parse_of({"download"}) == Command::usage());
  CHECK(parse_of({"download", "x", "--nope"}) == Command::usage());
}

TEST_CASE("translation_flags_apply_and_the_last_one_wins") {
  CHECK(parse_of({"x", "--dub"}) == Command::play(play("x", true)));
  CHECK(parse_of({"x", "--dub", "--sub"}) == Command::play(play("x", false)));
}

TEST_CASE("quality_takes_both_forms_and_a_missing_value_is_usage") {
  const Command want = Command::play(play("x", false, std::string("1080")));
  CHECK(parse_of({"x", "--quality", "1080"}) == want);
  CHECK(parse_of({"x", "--quality=1080"}) == want);
  CHECK(parse_of({"x", "--quality"}) == Command::usage());
}

TEST_CASE("paths_flag_outranks_a_subcommand") {
  CHECK(parse_of({"login", "--paths"}) == Command::paths());
  CHECK(parse_of({"--paths", "sync"}) == Command::paths());
}

TEST_CASE("unknown_double_dash_flag_is_usage_but_single_dash_is_query_text") {
  CHECK(parse_of({"--nope"}) == Command::usage());
  CHECK(parse_of({"frieren", "--nope"}) == Command::usage());
  // --paste is only login's flag; elsewhere it is an unknown flag.
  CHECK(parse_of({"--paste"}) == Command::usage());
  CHECK(parse_of({"-x"}) == Command::play(play("-x")));
}

TEST_CASE("debug_flag_is_consumed_globally_and_detected") {
  const std::vector<std::string> args{"frieren", "--debug"};
  CHECK(cli::debug_flag(args));
  CHECK(cli::parse(args) == Command::play(play("frieren")));
  CHECK_FALSE(cli::debug_flag({"frieren"}));
}

TEST_CASE("ascii_flag_is_global_and_consumed_like_debug") {
  const std::vector<std::string> args{"frieren", "--ascii"};
  CHECK(cli::ascii_flag(args));
  CHECK(cli::parse(args) == Command::play(play("frieren")));
  CHECK(cli::parse({"--ascii", "login"}) == Command::login(false));
  CHECK(cli::parse({"--ascii"}) == Command::tui());
  CHECK_FALSE(cli::ascii_flag({"frieren"}));
}

namespace {
PlayArgs play_with(std::string q, std::optional<std::string> episode,
                   std::optional<cli::EpisodeRange> range, std::optional<std::uint32_t> show,
                   std::optional<std::string> provider,
                   std::optional<std::string> quality = std::nullopt, bool dub = false) {
  PlayArgs a = play(std::move(q), dub, std::move(quality));
  a.episode = std::move(episode);
  a.range = std::move(range);
  a.show = show;
  a.provider = std::move(provider);
  return a;
}
}  // namespace

TEST_CASE("play_flags_take_short_long_and_equals_forms_anywhere") {
  const Command want = Command::play(
      play_with("x", std::string("7"), std::nullopt, 1, std::string("senshi"), std::string("720")));
  CHECK(parse_of({"x", "-e", "7", "-S", "1", "-p", "senshi", "-q", "720"}) == want);
  CHECK(parse_of({"x", "--episode", "7", "--show", "1", "--provider", "senshi", "--quality",
                  "720"}) == want);
  CHECK(parse_of({"x", "--episode=7", "--show=1", "--provider=senshi", "--quality=720"}) == want);
  CHECK(parse_of({"-e", "7", "-S", "1", "-p", "senshi", "-q", "720", "x"}) == want);
  // A span, closed and open; the words around the flags still join.
  CHECK(parse_of({"cowboy", "-r", "1-5", "bebop"}) ==
        Command::play(play_with("cowboy bebop", std::nullopt,
                                cli::EpisodeRange{"1", std::string("5")}, std::nullopt,
                                std::nullopt)));
  CHECK(parse_of({"x", "--range=4-"}) ==
        Command::play(play_with("x", std::nullopt, cli::EpisodeRange{"4", std::nullopt},
                                std::nullopt, std::nullopt)));
  // Other single-dash words are still query text.
  CHECK(parse_of({"-x", "-e", "2"}) ==
        Command::play(play_with("-x", std::string("2"), std::nullopt, std::nullopt, std::nullopt)));
}

TEST_CASE("play_flags_without_a_value_or_with_a_bad_one_are_usage") {
  CHECK(parse_of({"x", "-e"}) == Command::usage());
  CHECK(parse_of({"x", "--episode="}) == Command::usage());
  CHECK(parse_of({"x", "-S"}) == Command::usage());
  CHECK(parse_of({"x", "-S", "0"}) == Command::usage());
  CHECK(parse_of({"x", "-S", "two"}) == Command::usage());
  CHECK(parse_of({"x", "-p"}) == Command::usage());
  CHECK(parse_of({"x", "-r", "5"}) == Command::usage());      // a bare number is -e's job
  CHECK(parse_of({"x", "-r", "-3"}) == Command::usage());     // no start
  CHECK(parse_of({"x", "-r", "1-2-3"}) == Command::usage());  // a second dash
  CHECK(parse_of({"x", "-r"}) == Command::usage());
  // One episode or a span, never both.
  CHECK(parse_of({"x", "-e", "3", "-r", "1-2"}) == Command::usage());
  // The play flags with no query open nothing: usage, not the interface.
  CHECK(parse_of({"-e", "3"}) == Command::usage());
  CHECK(parse_of({"-S", "1"}) == Command::usage());
  CHECK(parse_of({"-p", "senshi"}) == Command::usage());
  CHECK(parse_of({"--quality", "720"}) == Command::usage());
  CHECK(parse_of({"--dub"}) == Command::tui());
}

TEST_CASE("download_takes_the_new_flags_and_refuses_two_episodes_or_a_span") {
  cli::DownloadArgs want = dl("frieren", std::string("7"));
  want.quality = "480";
  want.show = 2;
  want.provider = "hianime";
  CHECK(parse_of({"download", "frieren", "-e", "7", "-q", "480", "-S", "2", "-p", "hianime"}) ==
        Command::download(want));
  CHECK(parse_of({"download", "frieren", "7", "-q", "480", "-S", "2", "-p", "hianime"}) ==
        Command::download(want));
  CHECK(parse_of({"download", "frieren", "7", "-e", "8"}) == Command::usage());
  CHECK(parse_of({"download", "frieren", "-r", "1-3"}) == Command::usage());
}

TEST_CASE("continue_parses_with_and_without_a_query") {
  cli::ContinueArgs bare;
  CHECK(parse_of({"continue"}) == Command::continue_show(bare));
  CHECK(parse_of({"--debug", "continue"}) == Command::continue_show(bare));
  cli::ContinueArgs narrowed;
  narrowed.query = "cowboy bebop";
  narrowed.dub = true;
  CHECK(parse_of({"continue", "cowboy", "bebop", "--dub"}) == Command::continue_show(narrowed));
  cli::ContinueArgs nth;
  nth.show = 2;
  nth.provider = "senshi";
  nth.quality = "720";
  CHECK(parse_of({"continue", "-S", "2", "-p", "senshi", "-q", "720"}) ==
        Command::continue_show(nth));
  // The library says which episode: -e / -r are usage here.
  CHECK(parse_of({"continue", "-e", "3"}) == Command::usage());
  CHECK(parse_of({"continue", "-r", "1-3"}) == Command::usage());
  // After a query word the name is search text, as for every subcommand.
  CHECK(parse_of({"frieren", "continue"}) == Command::play(play("frieren continue")));
}

TEST_CASE("range_parses_closed_and_open_spans_only") {
  CHECK(cli::parse_range("1-5") == cli::EpisodeRange{"1", std::string("5")});
  CHECK(cli::parse_range("4-") == cli::EpisodeRange{"4", std::nullopt});
  CHECK(cli::parse_range("SP1-SP3") == cli::EpisodeRange{"SP1", std::string("SP3")});
  CHECK_FALSE(cli::parse_range("5").has_value());
  CHECK_FALSE(cli::parse_range("-5").has_value());
  CHECK_FALSE(cli::parse_range("1-2-3").has_value());
  CHECK_FALSE(cli::parse_range("").has_value());
}

TEST_CASE("ordinal_reads_digits_only") {
  CHECK(cli::ordinal_of("7") == 7);
  CHECK(cli::ordinal_of("07") == 7);
  CHECK(cli::ordinal_of("") == 0);
  CHECK(cli::ordinal_of("7a") == 0);
  CHECK(cli::ordinal_of("SP1") == 0);
  CHECK(cli::ordinal_of("99999999999") == 0);  // past uint32: no ordinal reading
}

TEST_CASE("range_indices_map_labels_and_positions_and_refuse_missing_or_crossed_ends") {
  const std::vector<std::string> eps = {"1", "2", "3", "SP1"};
  using P = std::pair<std::size_t, std::size_t>;
  CHECK(cli::range_indices(eps, {"2", std::string("3")}) == P{1, 2});
  CHECK(cli::range_indices(eps, {"2", std::nullopt}) == P{1, 3});
  CHECK(cli::range_indices(eps, {"SP1", std::nullopt}) == P{3, 3});
  CHECK(cli::range_indices(eps, {"2", std::string("SP1")}) == P{1, 3});
  CHECK(cli::range_indices(eps, {"4", std::string("4")}) == P{3, 3});  // position 4 = SP1
  CHECK_FALSE(cli::range_indices(eps, {"3", std::string("2")}).has_value());
  CHECK_FALSE(cli::range_indices(eps, {"9", std::nullopt}).has_value());
  CHECK_FALSE(cli::range_indices(eps, {"1", std::string("9")}).has_value());
  CHECK_FALSE(cli::range_indices({}, {"1", std::nullopt}).has_value());
}

TEST_CASE("post_play_menu_offers_next_and_previous_only_when_they_exist") {
  using A = cli::PostPlay;
  const std::vector<std::string> eps = {"1", "2", "3"};
  const auto first = cli::post_play_menu(0, eps);
  CHECK(first.rows == std::vector<std::string>{"next  \xC2\xB7  ep 2", "replay  \xC2\xB7  ep 1", "quit"});
  CHECK(first.actions == std::vector<A>{A::Next, A::Replay, A::Quit});
  const auto middle = cli::post_play_menu(1, eps);
  CHECK(middle.rows == std::vector<std::string>{"next  \xC2\xB7  ep 3", "replay  \xC2\xB7  ep 2",
                                                "previous  \xC2\xB7  ep 1", "quit"});
  CHECK(middle.actions == std::vector<A>{A::Next, A::Replay, A::Previous, A::Quit});
  const auto last = cli::post_play_menu(2, eps);
  CHECK(last.rows == std::vector<std::string>{"replay  \xC2\xB7  ep 3", "previous  \xC2\xB7  ep 2", "quit"});
  CHECK(last.actions == std::vector<A>{A::Replay, A::Previous, A::Quit});
  const auto lone = cli::post_play_menu(0, {"1"});
  CHECK(lone.actions == std::vector<A>{A::Replay, A::Quit});
}

namespace {
Show library_show(std::int64_t id, std::string romaji, std::optional<std::string> english,
                  std::uint32_t progress, std::optional<std::uint32_t> total) {
  Show s;
  s.enrichment.anilist_id = id;
  s.enrichment.title_romaji = std::move(romaji);
  s.enrichment.title_english = std::move(english);
  s.enrichment.total_episodes = total;
  s.progress = progress;
  return s;
}
}  // namespace

TEST_CASE("history_rows_say_the_title_and_how_far_and_matches_fold_case_over_every_title") {
  const std::vector<Show> shows = {
      library_show(1, "Sousou no Frieren", std::string("Frieren: Beyond Journey's End"), 7, 28),
      library_show(2, "Cowboy Bebop", std::nullopt, 0, 26),
      library_show(3, "Mystery", std::string("mystery"), 3, std::nullopt),
  };
  const auto rows = cli::history_rows(shows);
  REQUIRE(rows.size() == 3);
  CHECK(rows[0] == "Sousou no Frieren (Frieren: Beyond Journey's End)  \xC2\xB7  ep 7 of 28");
  CHECK(rows[1] == "Cowboy Bebop  \xC2\xB7  not started");
  CHECK(rows[2] == "Mystery  \xC2\xB7  ep 3");  // the same name again is not repeated
  using V = std::vector<std::size_t>;
  CHECK(cli::history_matches(shows, "frieren") == V{0});
  CHECK(cli::history_matches(shows, "BEBOP") == V{1});
  CHECK(cli::history_matches(shows, "journey") == V{0});  // the English title counts
  CHECK(cli::history_matches(shows, "") == V{0, 1, 2});
  CHECK(cli::history_matches(shows, "zzz") == V{});
}

namespace {
bool all_ascii(std::string_view s) {
  for (unsigned char c : s) {
    if (c >= 0x80) return false;
  }
  return true;
}
// Flip the glyph table to ASCII for one scope; every other test sees UTF-8.
struct AsciiGuard {
  AsciiGuard() { cli::set_ascii_glyphs(true); }
  ~AsciiGuard() { cli::set_ascii_glyphs(false); }
};
}  // namespace

TEST_CASE("ascii_glyphs_swap_every_mark_in_the_renderers_for_a_plain_one") {
  CHECK_FALSE(cli::ascii_glyphs());
  const std::string utf = cli::fetch_error_line(cli::FetchStage::Search, ProviderError::Kind::Network, "senshi");
  CHECK_FALSE(all_ascii(utf));
  {
    AsciiGuard ascii;
    CHECK(cli::ascii_glyphs());
    const std::string line = cli::fetch_error_line(cli::FetchStage::Search, ProviderError::Kind::Network, "senshi");
    CHECK_MESSAGE(line.rfind("  x can't reach senshi", 0) == 0, line);
    CHECK(cli::search_walk_note("A", std::nullopt, "B") == "  (no results on A; trying B...)\n");
    std::vector<SearchHit> hits;
    SearchHit h;
    h.title = "Frieren";
    h.eps_sub = 28;
    hits.push_back(h);
    CHECK(cli::search_hit_rows(hits, Translation::Sub) == std::vector<std::string>{"Frieren  -  28 sub eps"});
    CHECK(cli::post_play_menu(0, {"1", "2"}).rows[0] == "next  -  ep 2");
    const std::string ok = cli::render_connect_result(login::ConnectResult::ok("rod"), "/tmp/a", false);
    CHECK_MESSAGE(ok.rfind("  + signed in as rod", 0) == 0, ok);
    sync::SyncSummary s;
    s.outcome = sync::SyncOutcome::Completed;
    for (std::int64_t i = 1; i <= 14; ++i) s.pulled.unmatched.push_back(i);
    const std::string sync_text = cli::render_sync_summary(s);
    CHECK_MESSAGE(contains(sync_text, "      - anilist.co/anime/1\n"), sync_text);
    CHECK_MESSAGE(contains(sync_text, "      ... and 2 more"), sync_text);
    // Every line the renderers can produce is plain bytes in this mode.
    for (auto kind : {PlayError::Kind::MpvNotFound, PlayError::Kind::Exit, PlayError::Kind::OpenFailed,
                      PlayError::Kind::Resolve, PlayError::Kind::UnsafeUrl}) {
      CHECK(all_ascii(cli::player_failure_line(kind, "senshi", std::nullopt)));
    }
    for (auto kind : {ProviderError::Kind::Network, ProviderError::Kind::Forbidden, ProviderError::Kind::Server,
                      ProviderError::Kind::RateLimited, ProviderError::Kind::Http, ProviderError::Kind::Decode,
                      ProviderError::Kind::Unsupported}) {
      for (auto stage : {cli::FetchStage::Search, cli::FetchStage::Episodes, cli::FetchStage::Resolve}) {
        CHECK(all_ascii(cli::fetch_error_line(stage, kind, "senshi")));
      }
      CHECK(all_ascii(cli::search_walk_note("A", kind, "B")));
    }
    CHECK(all_ascii(sync_text));
    CHECK(all_ascii(cli::kUsage));
  }
  CHECK_FALSE(cli::ascii_glyphs());
  CHECK(cli::search_walk_note("A", std::nullopt, "B") == "  (no results on A; trying B\xE2\x80\xA6)\n");
}

TEST_CASE("utf8_locale_names_are_read_from_the_codeset_or_the_name") {
  for (const char* yes : {"en_US.UTF-8", "C.utf8", "UTF-8", "ru_RU.utf8", "de_DE.UTF-8@euro"}) {
    CHECK_MESSAGE(cli::utf8_locale_name(yes), yes);
  }
  for (const char* no : {"C", "POSIX", "", "ANSI_X3.4-1968", "US-ASCII", "en_US.ISO8859-1", "ja_JP.eucJP"}) {
    CHECK_MESSAGE(!cli::utf8_locale_name(no), no);
  }
}

// ── render_sync_summary (cli.rs sync tests) ──────────────────────────────────

namespace {
sync::SyncSummary summary(sync::SyncOutcome o) {
  sync::SyncSummary s;
  s.outcome = o;
  return s;
}
}  // namespace

TEST_CASE("terminal_outcomes_render_one_line_each") {
  using O = sync::SyncOutcome;
  const std::pair<O, std::string_view> cases[] = {
      {O::NoToken, "not connected"},
      {O::Expired, "token has expired"},
      {O::NoUserId, "which AniList account"},
      {O::PullUnauthorized, "pull stopped: AniList rejected"},
      {O::PullRateLimited, "pull stopped: hit AniList's rate limit"},
      {O::Failed, "sync failed"},
  };
  for (auto& [outcome, needle] : cases) {
    const std::string text = cli::render_sync_summary(summary(outcome));
    CHECK_MESSAGE(contains(text, needle), text);
    CHECK_MESSAGE(line_count(text) == 1, text);
  }
}

TEST_CASE("clean_run_says_up_to_date_on_both_sides") {
  const std::string text = cli::render_sync_summary(summary(sync::SyncOutcome::Completed));
  CHECK_MESSAGE(contains(text, "nothing to pull in"), text);
  CHECK_MESSAGE(contains(text, "nothing to push"), text);
}

// P31 §9.1 slice 4: mal_pushed defaults to 0 (silent, same as the TUI's
// on_sync_flushed toast logic) and only renders a line when something moved.
TEST_CASE("mal_pushed_is_silent_by_default_and_renders_only_when_nonzero") {
  const std::string quiet = cli::render_sync_summary(summary(sync::SyncOutcome::Completed));
  CHECK_MESSAGE(!contains(quiet, "MyAnimeList"), quiet);

  const std::string loud =
      cli::render_sync_summary(summary(sync::SyncOutcome::Completed), /*mal_pushed=*/3);
  CHECK_MESSAGE(contains(loud, "pushed 3 change(s) to MyAnimeList"), loud);
}

TEST_CASE("rows_feed_the_picker_and_the_renderers_number_them") {
  std::vector<SearchHit> hits;
  SearchHit a;
  a.title = "Frieren";
  a.eps_sub = 28;
  hits.push_back(a);
  SearchHit b;
  b.title = "Bare\x1b[31m";  // a control byte in a provider claim.
  hits.push_back(b);
  const auto rows = cli::search_hit_rows(hits, Translation::Sub);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0] == "Frieren  \xC2\xB7  28 sub eps");
  CHECK(rows[1] == "Bare[31m");
  CHECK(cli::episode_rows({"1", "OVA"}) == std::vector<std::string>{"ep 1", "ep OVA"});
  CHECK(cli::numbered_rows({"x", "y"}, 2) == "   1. x\n   2. y\n");
}

TEST_CASE("search_rows_show_a_differing_english_title_in_parentheses") {
  // The AniLibria shape: a Russian title with the romaji/English alongside.
  SearchHit ru;
  ru.title = "\xD0\xA4\xD1\x80\xD0\xB8\xD1\x80\xD0\xB5\xD0\xBD";  // Фрирен
  ru.title_english = "Sousou no Frieren";
  ru.total_episodes = 28;
  // The same name again (case aside) is not repeated; an empty one is nothing.
  SearchHit same;
  same.title = "Cowboy Bebop";
  same.title_english = "cowboy bebop";
  SearchHit blank;
  blank.title = "Mystery";
  blank.title_english = std::string();
  const auto rows = cli::search_hit_rows({ru, same, blank}, Translation::Sub);
  REQUIRE(rows.size() == 3);
  CHECK(rows[0] == std::string(ru.title) + " (Sousou no Frieren)  \xC2\xB7  28 eps");
  CHECK(rows[1] == "Cowboy Bebop");
  CHECK(rows[2] == "Mystery");
}

TEST_CASE("mal_pull_lines_follow_the_anilist_report_and_outlive_a_missing_anilist_account") {
  cli::MalPullCounts m;
  m.ran = true;
  m.pulled = 2;
  m.imported = 41;
  const std::string both = cli::render_sync_summary(summary(sync::SyncOutcome::Completed), 0, m);
  CHECK_MESSAGE(contains(both, "pulled 2 update(s) from MyAnimeList"), both);
  CHECK_MESSAGE(contains(both, "imported 41 show(s) from MyAnimeList"), both);
  // A MAL-only account: the AniList verdict becomes one line among two.
  const std::string mal_only = cli::render_sync_summary(summary(sync::SyncOutcome::NoToken), 0, m);
  CHECK_MESSAGE(contains(mal_only, "AniList: not connected"), mal_only);
  CHECK_MESSAGE(contains(mal_only, "imported 41"), mal_only);
  cli::MalPullCounts quiet;
  quiet.ran = true;
  CHECK(contains(cli::render_sync_summary(summary(sync::SyncOutcome::Completed), 0, quiet),
                 "MyAnimeList list already up to date"));
  cli::MalPullCounts refused;
  refused.ran = true;
  refused.unauthorized = true;
  CHECK(contains(cli::render_sync_summary(summary(sync::SyncOutcome::Completed), 0, refused),
                 "MyAnimeList rejected the token"));
  cli::MalPullCounts missed;
  missed.ran = true;
  missed.failed = true;
  CHECK(contains(cli::render_sync_summary(summary(sync::SyncOutcome::Completed), 0, missed),
                 "couldn't fetch your MyAnimeList list"));
  // No MAL account: not a word about it, and the one-line verdict stays one line.
  const std::string none = cli::render_sync_summary(summary(sync::SyncOutcome::NoToken));
  CHECK(!contains(none, "MyAnimeList"));
  CHECK(line_count(none) == 1);
}

TEST_CASE("airing_refresh_count_renders_only_when_nonzero") {
  const std::string quiet = cli::render_sync_summary(summary(sync::SyncOutcome::Completed));
  CHECK(!contains(quiet, "airing"));
  const std::string loud =
      cli::render_sync_summary(summary(sync::SyncOutcome::Completed), 0, {}, 12);
  CHECK_MESSAGE(contains(loud, "refreshed airing times for 12 show(s)"), loud);
}

TEST_CASE("counts_render_and_conflicts_suppress_up_to_date") {
  auto s = summary(sync::SyncOutcome::Completed);
  s.pulled.reconciled = 2;
  s.pulled.imported = 1;
  s.pulled.conflicts = 3;
  s.pulled.contended = {77};
  s.dirty = 5;
  s.pushed = 4;
  s.push_failed = 1;
  s.push_skipped = 1;
  const std::string text = cli::render_sync_summary(s);
  CHECK_MESSAGE(contains(text, "pulled 2 update(s)"), text);
  CHECK_MESSAGE(contains(text, "imported 1 show(s)"), text);
  CHECK_MESSAGE(contains(text, "(3 show(s) kept your local status"), text);
  CHECK_MESSAGE(contains(text, "(1 show(s) changed mid-sync"), text);
  CHECK_MESSAGE(contains(text, "pushed 4 of 5 change(s)"), text);
  CHECK_MESSAGE(contains(text, "1 push(es) failed"), text);
  CHECK_MESSAGE(contains(text, "(1 change(s) held back"), text);
  CHECK_MESSAGE(!contains(text, "up to date"), text);

  auto s2 = summary(sync::SyncOutcome::Completed);
  s2.pulled.conflicts = 1;
  CHECK_MESSAGE(!contains(cli::render_sync_summary(s2), "nothing to pull in"), "conflicts suppress");
}

TEST_CASE("an_import_only_pull_suppresses_up_to_date_by_itself") {
  auto s = summary(sync::SyncOutcome::Completed);
  s.pulled.imported = 3;
  const std::string text = cli::render_sync_summary(s);
  CHECK_MESSAGE(contains(text, "imported 3 show(s)"), text);
  CHECK_MESSAGE(!contains(text, "nothing to pull in"), text);
}

TEST_CASE("unmatched_listing_caps_at_twelve_and_counts_the_rest") {
  auto s = summary(sync::SyncOutcome::Completed);
  for (std::int64_t i = 1; i <= 14; ++i) s.pulled.unmatched.push_back(i);
  const std::string text = cli::render_sync_summary(s);
  CHECK_MESSAGE(contains(text, "(14 AniList show(s)"), text);
  CHECK_MESSAGE(count_of(text, "anilist.co/anime/") == 12, text);
  CHECK_MESSAGE(contains(text, "and 2 more"), text);
}

TEST_CASE("pull_transport_miss_reports_and_still_prints_the_push_side") {
  auto s = summary(sync::SyncOutcome::Completed);
  s.pull_failed = true;
  s.dirty = 2;
  s.pushed = 2;
  const std::string text = cli::render_sync_summary(s);
  CHECK_MESSAGE(contains(text, "pull failed"), text);
  CHECK_MESSAGE(contains(text, "pushed 2 of 2"), text);
}

TEST_CASE("push_walls_append_their_stop_line") {
  auto s = summary(sync::SyncOutcome::Unauthorized);
  s.dirty = 2;
  const std::string text = cli::render_sync_summary(s);
  CHECK_MESSAGE(contains(text, "pushed 0 of 2"), text);
  CHECK_MESSAGE(contains(text, "rejected the token mid-run"), text);

  auto s2 = summary(sync::SyncOutcome::RateLimited);
  s2.dirty = 3;
  s2.pushed = 1;
  CHECK_MESSAGE(contains(cli::render_sync_summary(s2), "hit AniList's rate limit"), "rate wall");
}

// ── paste_line_usable ───────────────────────────────────────────────────────

TEST_CASE("paste_line_accepts_a_complete_line_with_or_without_a_trailing_newline") {
  constexpr std::uint64_t CAP = 8192;
  CHECK_FALSE(cli::paste_line_usable(0, "", CAP));                 // empty EOF aborts
  CHECK(cli::paste_line_usable(10, "a-url-here\n", CAP));          // newline: usable
  CHECK(cli::paste_line_usable(9, "a-url-her", CAP));              // short EOF line: usable
  const std::string maxed(CAP, 'x');                              // cap read, no newline: abort
  CHECK_FALSE(cli::paste_line_usable(CAP, maxed, CAP));
  std::string edge(CAP - 1, 'x');                                // cap read ending in \n: real
  edge.push_back('\n');
  CHECK(cli::paste_line_usable(CAP, edge, CAP));
}

// ── render_connect_result ────────────────────────────────────────────────────

TEST_CASE("ok_result_strips_control_bytes_from_the_anilist_name") {
  auto hostile = login::ConnectResult::ok(hostile_name());
  const std::string text = cli::render_connect_result(hostile, "/tmp/auth.json", false);
  CHECK_MESSAGE(contains(text, "signed in as"), text);
  CHECK_MESSAGE(!has_esc(text), "escape leaked");
  CHECK_MESSAGE(!has_bidi(text), "bidi leaked");
}

TEST_CASE("connect_results_render_one_line_each_and_paste_picks_the_coaching") {
  const char* path = "/tmp/auth.json";
  const std::string ok = cli::render_connect_result(login::ConnectResult::ok("rod"), path, false);
  CHECK_MESSAGE(contains(ok, "signed in as rod"), ok);
  CHECK_MESSAGE(contains(ok, "/tmp/auth.json"), ok);

  struct C {
    login::ConnectResult r;
    bool paste;
    std::string_view needle;
  };
  const C cases[] = {
      {login::ConnectResult::no_token(), true, "couldn't find an access_token"},
      {login::ConnectResult::no_token(), false, "redirect carried no access_token"},
      {login::ConnectResult::rejected(), true, "re-copy the whole fragment"},
      {login::ConnectResult::rejected(), false, "re-run to retry"},
      {login::ConnectResult::network_error(), true, "check your connection"},
      {login::ConnectResult::network_error(), false, "re-run shortly"},
      {login::ConnectResult::save_failed(), false, "couldn't write /tmp/auth.json"},
  };
  for (auto& c : cases) {
    const std::string text = cli::render_connect_result(c.r, path, c.paste);
    CHECK_MESSAGE(contains(text, c.needle), text);
    CHECK_MESSAGE(line_count(text) == 1, text);
  }
}

// ── render_search_hits / render_episode_list ─────────────────────────────────

namespace {
SearchHit hit(std::string title, std::uint32_t eps_sub, std::uint32_t eps_dub,
              std::optional<std::uint32_t> total) {
  SearchHit h;
  h.title = std::move(title);
  h.eps_sub = eps_sub;
  h.eps_dub = eps_dub;
  h.total_episodes = total;
  return h;
}
}  // namespace

TEST_CASE("search_hits_pick_per_track_then_total_then_bare") {
  const std::vector<SearchHit> hits = {
      hit("Frieren", 28, 0, 28),
      hit("Cowboy Bebop", 0, 26, 26),
      hit("Mystery", 0, 0, std::nullopt),
  };
  const std::string sub = cli::render_search_hits(hits, Translation::Sub);
  CHECK_MESSAGE(contains(sub, "3 result(s):"), sub);
  CHECK_MESSAGE(contains(sub, " 1. Frieren  ·  28 sub eps"), sub);
  CHECK_MESSAGE(contains(sub, " 2. Cowboy Bebop  ·  26 eps"), sub);
  CHECK_MESSAGE(contains(sub, " 3. Mystery\n"), sub);

  const std::string dub = cli::render_search_hits(hits, Translation::Dub);
  CHECK_MESSAGE(contains(dub, " 2. Cowboy Bebop  ·  26 dub eps"), dub);
}

TEST_CASE("search_hits_strip_control_bytes_from_the_title") {
  const std::vector<SearchHit> hits = {hit(hostile_name(), 1, 0, std::nullopt)};
  const std::string out = cli::render_search_hits(hits, Translation::Sub);
  CHECK_MESSAGE(!has_esc(out), "escape leaked");
  CHECK_MESSAGE(!has_bidi(out), "bidi leaked");
}

TEST_CASE("episode_list_numbers_and_strips_labels") {
  std::string ova = "OVA";
  put_cp(ova, 0x202E);
  const std::vector<std::string> labels = {"1", "2", ova};
  const std::string out = cli::render_episode_list(labels);
  CHECK_MESSAGE(contains(out, "3 episode(s):"), out);
  CHECK_MESSAGE(contains(out, "  1. ep 1"), out);
  CHECK_MESSAGE(contains(out, "  3. ep OVA"), out);
  CHECK_MESSAGE(!has_bidi(out), "bidi leaked");
}

// ── classify_pick ────────────────────────────────────────────────────────────

TEST_CASE("pick_classifies_abort_reprompt_and_range") {
  using cli::PickInput;
  CHECK(cli::classify_pick("2\n", 5) == cli::PickResult{PickInput::Pick, 1});
  CHECK(cli::classify_pick("  3 \n", 5) == cli::PickResult{PickInput::Pick, 2});
  CHECK(cli::classify_pick("q\n", 5) == cli::PickResult{PickInput::Abort, 0});
  CHECK(cli::classify_pick("Q", 5) == cli::PickResult{PickInput::Abort, 0});
  CHECK(cli::classify_pick("\n", 5) == cli::PickResult{PickInput::Reprompt, 0});
  CHECK(cli::classify_pick("   ", 5) == cli::PickResult{PickInput::Reprompt, 0});
  CHECK(cli::classify_pick("x", 5) == cli::PickResult{PickInput::NotNumber, 0});
  CHECK(cli::classify_pick("0", 5) == cli::PickResult{PickInput::OutOfRange, 0});
  CHECK(cli::classify_pick("6", 5) == cli::PickResult{PickInput::OutOfRange, 0});
  CHECK(cli::classify_pick("5", 5) == cli::PickResult{PickInput::Pick, 4});
}

// ── fetch_error_line / quality_note / provider_override / player_failure ─────

TEST_CASE("search_unsupported_no_longer_nudges_at_config") {
  for (auto stage : {cli::FetchStage::Search, cli::FetchStage::Episodes, cli::FetchStage::Resolve}) {
    const std::string line = cli::fetch_error_line(stage, ProviderError::Kind::Unsupported, "megaplay");
    CHECK_MESSAGE(!contains(line, "preferred_provider"), line);
  }
  const std::string s =
      cli::fetch_error_line(cli::FetchStage::Search, ProviderError::Kind::Unsupported, "megaplay");
  CHECK_MESSAGE(contains(s, "can't search directly"), s);
}

TEST_CASE("fetch_error_rows_name_the_provider_and_render_one_block") {
  for (auto kind : {ProviderError::Kind::Network, ProviderError::Kind::Forbidden,
                    ProviderError::Kind::Server, ProviderError::Kind::Http}) {
    const std::string line = cli::fetch_error_line(cli::FetchStage::Search, kind, "senshi");
    CHECK_MESSAGE(contains(line, "senshi"), line);
    const bool ends_nl = !line.empty() && line.back() == '\n';
    CHECK_MESSAGE(ends_nl, line);
  }
  const std::string s = cli::fetch_error_line(cli::FetchStage::Search, ProviderError::Kind::Decode, "senshi");
  const std::string r = cli::fetch_error_line(cli::FetchStage::Resolve, ProviderError::Kind::Decode, "senshi");
  CHECK_MESSAGE(contains(s, "search results"), s);
  CHECK_MESSAGE(contains(r, "stream payload"), r);
}

TEST_CASE("quality_note_fires_only_for_a_spelling_parse_quality_does_not_know") {
  CHECK_FALSE(cli::quality_note(std::nullopt).has_value());
  for (const char* q : {"best", "1080", "720", "480", "worst"}) {
    CHECK_FALSE(cli::quality_note(std::optional<std::string_view>(q)).has_value());
  }
  // parse_quality's spellings are exact: "Best" would play best by fallthrough,
  // so it is said rather than silently honoured.
  auto note = cli::quality_note(std::optional<std::string_view>("Best"));
  REQUIRE(note.has_value());
  CHECK_MESSAGE(contains(*note, "no quality called \"Best\""), *note);
  CHECK_MESSAGE(contains(*note, "using best"), *note);
  CHECK(cli::quality_note(std::optional<std::string_view>("4k")).has_value());
}

TEST_CASE("unknown_source_note_names_the_walk") {
  const std::string note = cli::unknown_source_note("nope", {"senshi", "hianime"});
  CHECK_MESSAGE(contains(note, "no source called \"nope\""), note);
  CHECK_MESSAGE(contains(note, "senshi, hianime"), note);
}

TEST_CASE("override_note_fires_only_when_a_preference_was_walked_past") {
  const std::pair<std::string_view, std::string_view> senshi{"senshi", "Senshi"};
  CHECK(cli::provider_override_note(std::nullopt, senshi) == std::nullopt);
  CHECK(cli::provider_override_note(std::make_pair(std::string_view("senshi"), std::string_view("Senshi")),
                                    senshi) == std::nullopt);
  auto note = cli::provider_override_note(
      std::make_pair(std::string_view("megaplay"), std::string_view("MegaPlay")), senshi);
  REQUIRE(note.has_value());
  CHECK(contains(*note, "MegaPlay"));
  CHECK(contains(*note, "Senshi"));
  CHECK(contains(*note, "can't search"));
}

TEST_CASE("override_note_compares_names_not_display_strings") {
  CHECK(cli::provider_override_note(
            std::make_pair(std::string_view("senshi"), std::string_view("Same Label")),
            {"senshi", "Same Label"}) == std::nullopt);
  const bool differ = cli::provider_override_note(
                          std::make_pair(std::string_view("a"), std::string_view("Same Label")),
                          {"b", "Same Label"})
                          .has_value();
  CHECK(differ);
}

TEST_CASE("player_failures_read_per_class_and_resolve_reuses_the_fetch_copy") {
  CHECK(contains(cli::player_failure_line(PlayError::Kind::MpvNotFound, "senshi", std::nullopt),
                 "mpv isn't on your PATH"));
  CHECK(contains(cli::player_failure_line(PlayError::Kind::OpenFailed, "senshi", std::nullopt),
                 "couldn't open the stream"));
  CHECK(contains(cli::player_failure_line(PlayError::Kind::UnsafeUrl, "senshi", std::nullopt),
                 "couldn't start safely"));
  // A resolve with a captured class routes through the resolve-stage fetch copy.
  const std::string net =
      cli::player_failure_line(PlayError::Kind::Resolve, "senshi", ProviderError::Kind::Network);
  CHECK(net == cli::fetch_error_line(cli::FetchStage::Resolve, ProviderError::Kind::Network, "senshi"));
  CHECK(contains(net, "senshi"));
  // A resolve with no captured class (proxy/guard stop) is the safe-stop line.
  CHECK(contains(cli::player_failure_line(PlayError::Kind::Resolve, "senshi", std::nullopt),
                 "couldn't start safely"));
}

// ── play_flow exit table (main.rs play_flow mod, 06 §7.4) ────────────────────

namespace {

// A provider whose search/episodes/resolve outcomes are scripted, so the play
// path reaches a chosen exit without network or mpv. A hit with no anilist_id
// and no mal_id skips every store hop and plays AniSkip-plain.
class Fake final : public StreamProvider {
 public:
  using SearchR = Result<std::vector<SearchHit>, ProviderError>;
  using EpR = Result<std::vector<std::string>, ProviderError>;
  using ResolveR = Result<StreamLink, ProviderError>;
  SearchR (*search_)(std::string_view);
  EpR (*episodes_)();
  ResolveR (*resolve_)();
  // Identity + search capability, settable so a registry / search-walk test
  // can tell its fakes apart (string literals: the views outlive the fake).
  std::string_view name_ = "fake";
  std::string_view display_ = "Fake";
  bool searchable_ = true;
  bool localized_ = false;

  [[nodiscard]] std::string_view name() const override { return name_; }
  [[nodiscard]] std::string_view display_name() const override { return display_; }
  [[nodiscard]] bool supports_search() const override { return searchable_; }
  [[nodiscard]] bool localized_titles() const override { return localized_; }
  [[nodiscard]] std::optional<std::string> canonical_key(const Enrichment&) const override {
    return std::nullopt;
  }
  [[nodiscard]] SearchR search(std::string_view q, const SearchOptions&) const override {
    return search_(q);
  }
  [[nodiscard]] EpR episodes(std::string_view, Translation,
                             std::optional<std::uint32_t>) const override {
    return episodes_();
  }
  [[nodiscard]] ResolveR resolve(std::string_view, std::string_view, Translation,
                                 Quality) const override {
    return resolve_();
  }
  [[nodiscard]] Result<CoverRequest, ProviderError> cover_request(
      std::string_view) const override {
    return err(ProviderError::unsupported());
  }
};

SearchHit one_hit() {
  SearchHit h;
  h.provider_id = "id";
  h.title = "Frieren";
  h.total_episodes = 1;
  h.eps_sub = 1;
  h.eps_dub = 1;
  return h;
}

// No store, default config, empty cache_dir (skips aniskip so no Jikan net),
// empty download_dir (downloads disabled — the local-preference scan never
// runs): one_hit()'s absent ids mean the store/paths are never read on the
// way to the exit under test.
int run_all(const shigoku::cli_play::Sources& sources, shigoku::cli_play::PickFn pick) {
  const Config config;
  return shigoku::cli_play::play_flow(sources, pick, Translation::Sub, config,
                                      /*cache_dir=*/"", /*runtime_dir=*/"/tmp",
                                      /*download_dir=*/"", /*store=*/nullptr, play("frieren"));
}

int run(const Fake& p, shigoku::cli_play::PickFn pick) { return run_all({&p}, std::move(pick)); }

Fake make(Fake::SearchR (*s)(std::string_view), Fake::EpR (*e)(), Fake::ResolveR (*r)()) {
  Fake f;
  f.search_ = s;
  f.episodes_ = e;
  f.resolve_ = r;
  return f;
}

}  // namespace

TEST_CASE("no_results_exits_zero") {
  auto p = make([](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{}; },
                []() -> Fake::EpR { return std::vector<std::string>{}; },
                []() -> Fake::ResolveR { return err(ProviderError::network()); });
  CHECK(run(p, [](std::string_view, const std::vector<std::string>&) { return std::optional<std::size_t>(0); }) == 0);
}

TEST_CASE("search_failure_exits_one") {
  auto p = make([](std::string_view) -> Fake::SearchR { return err(ProviderError::network()); },
                []() -> Fake::EpR { return std::vector<std::string>{}; },
                []() -> Fake::ResolveR { return err(ProviderError::network()); });
  CHECK(run(p, [](std::string_view, const std::vector<std::string>&) { return std::optional<std::size_t>(0); }) == 1);
}

TEST_CASE("quitting_the_show_pick_exits_zero") {
  auto p = make([](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{one_hit()}; },
                []() -> Fake::EpR { return std::vector<std::string>{"1"}; },
                []() -> Fake::ResolveR { return err(ProviderError::network()); });
  CHECK(run(p, [](std::string_view, const std::vector<std::string>&) { return std::optional<std::size_t>{}; }) == 0);
}

TEST_CASE("resolve_failure_exits_one") {
  // Auto-pick show then episode; resolve errors before mpv is ever built, so
  // play() returns Err on the first attempt with no retry or backoff.
  auto p = make([](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{one_hit()}; },
                []() -> Fake::EpR { return std::vector<std::string>{"1"}; },
                []() -> Fake::ResolveR { return err(ProviderError::network()); });
  CHECK(run(p, [](std::string_view, const std::vector<std::string>&) { return std::optional<std::size_t>(0); }) == 1);
}

// ── search walk: the CLI tries the next searchable source ───────────────────

namespace {

// Search outcomes for the walk fakes; episodes/resolve are never reached when
// the pick quits, so they are scripted to fail loudly if they ever are.
Fake::EpR no_episodes() { return err(ProviderError::network()); }
Fake::ResolveR no_resolve() { return err(ProviderError::network()); }

Fake source_down(std::string_view name) {
  auto f = make([](std::string_view) -> Fake::SearchR { return err(ProviderError::server(503)); },
                no_episodes, no_resolve);
  f.name_ = name;
  f.display_ = name;
  return f;
}

Fake source_empty(std::string_view name) {
  auto f = make([](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{}; },
                no_episodes, no_resolve);
  f.name_ = name;
  f.display_ = name;
  return f;
}

// Two hits, so a pick prompt sized 2 proves THIS source's list was offered.
Fake source_up(std::string_view name) {
  auto f = make(
      [](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{one_hit(), one_hit()}; },
      no_episodes, no_resolve);
  f.name_ = name;
  f.display_ = name;
  return f;
}

// A pick that records the prompt size and quits: the exit is the clean 0 and
// `seen` says whose hits reached the prompt.
struct QuitPick {
  std::size_t seen = 0;
  shigoku::cli_play::PickFn fn() {
    return [this](std::string_view, const std::vector<std::string>& rows) {
      seen = rows.size();
      return std::optional<std::size_t>{};
    };
  }
};

}  // namespace

TEST_CASE("search_walks_past_a_failing_source_to_the_next") {
  const Fake down = source_down("down");
  const Fake up = source_up("up");
  QuitPick pick;
  CHECK(run_all({&down, &up}, pick.fn()) == 0);
  CHECK(pick.seen == 2);
}

TEST_CASE("search_walks_past_an_empty_source_to_the_next") {
  const Fake empty = source_empty("empty");
  const Fake up = source_up("up");
  QuitPick pick;
  CHECK(run_all({&empty, &up}, pick.fn()) == 0);
  CHECK(pick.seen == 2);
}

TEST_CASE("search_failing_on_every_source_exits_one") {
  const Fake a = source_down("a");
  const Fake b = source_down("b");
  QuitPick pick;
  CHECK(run_all({&a, &b}, pick.fn()) == 1);
  CHECK(pick.seen == 0);
}

TEST_CASE("a_healthy_empty_source_makes_the_walk_a_clean_no_results") {
  // Either order: a source that answered "nothing" outranks the failures for
  // the exit — the title is more likely absent than the network broken.
  const Fake down = source_down("down");
  const Fake empty = source_empty("empty");
  QuitPick pick;
  CHECK(run_all({&down, &empty}, pick.fn()) == 0);
  CHECK(run_all({&empty, &down}, pick.fn()) == 0);
  CHECK(pick.seen == 0);
}

TEST_CASE("the_first_answering_source_binds_the_whole_run") {
  // `first` answers the search but its episodes fail; `second` would answer
  // everything. The run stays bound to `first` and exits 1 at the episodes
  // stage — the walk is a search-stage mechanism only.
  auto first = make(
      [](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{one_hit()}; },
      no_episodes, no_resolve);
  first.name_ = "first";
  first.display_ = "first";
  auto second = make(
      [](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{one_hit()}; },
      []() -> Fake::EpR { return std::vector<std::string>{"1"}; }, no_resolve);
  second.name_ = "second";
  second.display_ = "second";
  CHECK(run_all({&first, &second},
                [](std::string_view, const std::vector<std::string>&) { return std::optional<std::size_t>(0); }) == 1);
}

TEST_CASE("no_sources_at_all_exits_one") {
  QuitPick pick;
  CHECK(run_all({}, pick.fn()) == 1);
}

TEST_CASE("search_walk_note_reads_per_class_and_for_an_empty_answer") {
  using K = ProviderError::Kind;
  CHECK(cli::search_walk_note("Senshi", K::Server, "AniLibria") ==
        "  (Senshi is down; trying AniLibria…)\n");
  CHECK(cli::search_walk_note("Senshi", K::Http, "AniLibria") ==
        "  (Senshi rejected the search; trying AniLibria…)\n");
  CHECK(cli::search_walk_note("Senshi", std::nullopt, "AniLibria") ==
        "  (no results on Senshi; trying AniLibria…)\n");
  // Every class renders a name-led sentence; none falls through blank.
  for (K k : {K::Network, K::Forbidden, K::Server, K::Http, K::Decode, K::Unsupported,
              K::RateLimited}) {
    const std::string line = cli::search_walk_note("X", k, "Y");
    CHECK_MESSAGE(contains(line, "(X "), line);
    CHECK_MESSAGE(contains(line, "; trying Y"), line);
  }
}

TEST_CASE("registry_searchable_lists_search_capable_sources_preferred_first") {
  std::vector<std::unique_ptr<StreamProvider>> ps;
  ps.push_back(std::make_unique<Fake>(source_up("a")));
  {
    auto b = std::make_unique<Fake>(source_up("b"));
    b->searchable_ = false;  // the megaplay/anibd shape: listed, cannot search.
    ps.push_back(std::move(b));
  }
  ps.push_back(std::make_unique<Fake>(source_up("c")));
  const ProviderRegistry reg(std::move(ps));
  auto names = [](const std::vector<const StreamProvider*>& v) {
    std::vector<std::string> out;
    for (const StreamProvider* p : v) out.emplace_back(p->name());
    return out;
  };
  CHECK(names(reg.searchable(std::nullopt)) == std::vector<std::string>{"a", "c"});
  CHECK(names(reg.searchable("c")) == std::vector<std::string>{"c", "a"});
  // A preference that cannot search is walked past, not promoted (ROD-491).
  CHECK(names(reg.searchable("b")) == std::vector<std::string>{"a", "c"});
  CHECK(names(reg.searchable("nope")) == std::vector<std::string>{"a", "c"});
  REQUIRE(reg.preferred_searchable("b") != nullptr);
  CHECK(reg.preferred_searchable("b")->name() == "a");
}

TEST_CASE("registry_searchable_walks_localized_title_sources_last") {
  // The anilibria shape: it can search, but its titles are Russian — every
  // English-titled source is asked first, wherever it sits in the registry.
  std::vector<std::unique_ptr<StreamProvider>> ps;
  {
    auto ru = std::make_unique<Fake>(source_up("ru"));
    ru->localized_ = true;
    ps.push_back(std::move(ru));
  }
  ps.push_back(std::make_unique<Fake>(source_up("a")));
  {
    auto b = std::make_unique<Fake>(source_up("b"));
    b->searchable_ = false;
    ps.push_back(std::move(b));
  }
  ps.push_back(std::make_unique<Fake>(source_up("c")));
  const ProviderRegistry reg(std::move(ps));
  auto names = [](const std::vector<const StreamProvider*>& v) {
    std::vector<std::string> out;
    for (const StreamProvider* p : v) out.emplace_back(p->name());
    return out;
  };
  CHECK(names(reg.searchable(std::nullopt)) == std::vector<std::string>{"a", "c", "ru"});
  CHECK(names(reg.searchable("c")) == std::vector<std::string>{"c", "a", "ru"});
  CHECK(names(reg.searchable("b")) == std::vector<std::string>{"a", "c", "ru"});
  // Asked for by name, the localized source leads like any other preference.
  CHECK(names(reg.searchable("ru")) == std::vector<std::string>{"ru", "a", "c"});
  REQUIRE(reg.preferred_searchable("ru") != nullptr);
  CHECK(reg.preferred_searchable("ru")->name() == "ru");
  REQUIRE(reg.preferred_searchable(std::nullopt) != nullptr);
  CHECK(reg.preferred_searchable(std::nullopt)->name() == "a");
}

// ── play-prefers-local (P35 slice 4) ─────────────────────────────────────────

namespace {

// A stub mpv on disk: exits 0 immediately — the clean no-position exit (the
// gate-shut Ok arm), so the flow lands on its 0 without IPC or store writes.
std::string write_stub_mpv() {
  const std::string path = "/tmp/shigoku-cli-test-stub-mpv-" +
                           std::to_string(static_cast<long>(::getpid())) + ".sh";
  FILE* f = std::fopen(path.c_str(), "wb");
  REQUIRE(f != nullptr);
  std::fputs("#!/bin/sh\nexit 0\n", f);
  std::fclose(f);
  REQUIRE(::chmod(path.c_str(), 0755) == 0);
  return path;
}

// mkdir -p + tiny file: a planted completed download.
void plant_download(const std::string& path) {
  REQUIRE(download::ensure_parent_dirs(path).has_value());
  FILE* f = std::fopen(path.c_str(), "wb");
  REQUIRE(f != nullptr);
  std::fputs("x", f);
  std::fclose(f);
}

}  // namespace

TEST_CASE("play_prefers_a_completed_local_download_over_resolving (P35 slice 4)") {
  // The hit carries an anilist_id (the local-preference gate) and resolve is
  // scripted to FAIL: a 0 exit is reachable ONLY through the local file — the
  // resolve pipeline was never consulted.
  auto p = make(
      [](std::string_view) -> Fake::SearchR {
        auto h = one_hit();
        h.anilist_id = 700;
        return std::vector<SearchHit>{h};
      },
      []() -> Fake::EpR { return std::vector<std::string>{"1"}; },
      []() -> Fake::ResolveR { return err(ProviderError::network()); });
  const std::string dl =
      "/tmp/shigoku-cli-test-dl-" + std::to_string(static_cast<long>(::getpid()));
  plant_download(dl + "/700/sub/1.mp4");
  Config config;
  config.mpv_path = write_stub_mpv();
  // The first row for the show and the episode; the post-play menu's first
  // row would be "replay" (one episode, no next), so quit there instead.
  const auto pick = [](std::string_view prompt, const std::vector<std::string>&) {
    return prompt == "what next" ? std::optional<std::size_t>{} : std::optional<std::size_t>(0);
  };
  const cli::PlayArgs args = play("frieren");
  CHECK(shigoku::cli_play::play_flow({&p}, pick, Translation::Sub, config, /*cache_dir=*/"",
                                     /*runtime_dir=*/"/tmp", dl, /*store=*/nullptr,
                                     args) == 0);
  // Track mismatch never matches (the DoD case): the same show played as DUB
  // finds no /700/dub/ file, falls through to the scripted resolve failure,
  // and exits 1 — the sub download was never adopted.
  CHECK(shigoku::cli_play::play_flow({&p}, pick, Translation::Dub, config, /*cache_dir=*/"",
                                     /*runtime_dir=*/"/tmp", dl, /*store=*/nullptr,
                                     args) == 1);
}

// ── -S / -e / -r, the post-play menu, and `continue` ─────────────────────────

namespace {

// A pick that answers from a script, in order (exhausted = nullopt, the
// user's EOF), and remembers every prompt and its rows.
struct ScriptedPick {
  std::vector<std::optional<std::size_t>> answers;
  std::vector<std::string> prompts;
  std::vector<std::vector<std::string>> rows_seen;
  shigoku::cli_play::PickFn fn() {
    return [this](std::string_view prompt, const std::vector<std::string>& rows) {
      prompts.emplace_back(prompt);
      rows_seen.push_back(rows);
      if (answers.empty()) return std::optional<std::size_t>{};
      auto a = answers.front();
      answers.erase(answers.begin());
      return a;
    };
  }
  [[nodiscard]] std::size_t asked(std::string_view prompt) const {
    std::size_t n = 0;
    for (const auto& p : prompts) {
      if (p == prompt) ++n;
    }
    return n;
  }
};

Fake::EpR three_episodes() { return std::vector<std::string>{"1", "2", "3"}; }

// One hit under a real anilist_id (700): the store and local-download gates
// open, so a planted file under <dl>/700/sub/<ep>.mp4 plays through the stub
// mpv (exit 0) and an unplanted episode falls to the scripted resolve failure
// (exit 1) — which episode a run reached is readable from its exit alone.
Fake::SearchR hit_700(std::string_view) {
  auto h = one_hit();
  h.anilist_id = 700;
  h.total_episodes = 3;
  return std::vector<SearchHit>{h};
}

Fake show_700() {
  auto f = make(hit_700, three_episodes,
                []() -> Fake::ResolveR { return err(ProviderError::network()); });
  return f;
}

std::string fresh_download_dir(const char* tag) {
  return "/tmp/shigoku-cli-test-dl-" + std::to_string(static_cast<long>(::getpid())) + "-" + tag;
}

cli::PlayArgs play_args() {
  cli::PlayArgs a;
  a.query = "frieren";
  return a;
}

int run_play(const Fake& p, const Config& config, const std::string& dl, const cli::PlayArgs& args,
             shigoku::cli_play::PickFn pick) {
  return shigoku::cli_play::play_flow({&p}, std::move(pick), Translation::Sub, config,
                                      /*cache_dir=*/"", /*runtime_dir=*/"/tmp", dl,
                                      /*store=*/nullptr, args);
}

}  // namespace

TEST_CASE("show_flag_takes_the_nth_result_without_a_prompt_and_past_the_list_is_a_clean_zero") {
  const Fake up = source_up("up");  // two hits, episodes fail: exit 1 = the show was taken.
  ScriptedPick pick;
  cli::PlayArgs args = play_args();
  args.show = 2;
  CHECK(run_all({&up}, pick.fn()) == 0);  // sanity: the prompt path quits on EOF
  CHECK(pick.asked("pick a show") == 1);
  ScriptedPick nth;
  CHECK(shigoku::cli_play::play_flow({&up}, nth.fn(), Translation::Sub, Config{}, "", "/tmp", "",
                                     nullptr, args) == 1);
  CHECK(nth.prompts.empty());
  args.show = 3;
  ScriptedPick past;
  CHECK(shigoku::cli_play::play_flow({&up}, past.fn(), Translation::Sub, Config{}, "", "/tmp", "",
                                     nullptr, args) == 0);
  CHECK(past.prompts.empty());
}

TEST_CASE("episode_flag_skips_the_episode_prompt_and_an_unknown_one_is_a_clean_zero") {
  const Fake p = show_700();
  ScriptedPick pick;
  pick.answers = {0};  // the show
  cli::PlayArgs args = play_args();
  args.episode = "2";
  CHECK(run_play(p, Config{}, "", args, pick.fn()) == 1);  // resolve reached: the episode was taken
  CHECK(pick.asked("pick an episode") == 0);
  args.episode = "9";
  ScriptedPick miss;
  miss.answers = {0};
  CHECK(run_play(p, Config{}, "", args, miss.fn()) == 0);
  args.episode = "SP1";
  ScriptedPick miss2;
  miss2.answers = {0};
  CHECK(run_play(p, Config{}, "", args, miss2.fn()) == 0);
}

TEST_CASE("range_plays_the_span_in_order_with_no_menu_and_stops_on_a_failed_episode") {
  const Fake p = show_700();
  const std::string dl = fresh_download_dir("range");
  plant_download(dl + "/700/sub/1.mp4");
  plant_download(dl + "/700/sub/2.mp4");
  Config config;
  config.mpv_path = write_stub_mpv();
  cli::PlayArgs args = play_args();
  args.show = 1;
  args.range = cli::EpisodeRange{"1", std::string("2")};
  ScriptedPick none;
  CHECK(run_play(p, config, dl, args, none.fn()) == 0);
  CHECK(none.prompts.empty());  // no show prompt, no episode prompt, no menu
  // Episode 3 has no local copy: the span reaches it, resolve fails, exit 1.
  args.range = cli::EpisodeRange{"1", std::nullopt};
  ScriptedPick none2;
  CHECK(run_play(p, config, dl, args, none2.fn()) == 1);
  CHECK(none2.prompts.empty());
  // A span past the list is a clean 0 before anything plays.
  args.range = cli::EpisodeRange{"1", std::string("9")};
  ScriptedPick none3;
  CHECK(run_play(p, config, dl, args, none3.fn()) == 0);
}

TEST_CASE("post_play_menu_walks_next_and_previous_until_quit") {
  const Fake p = show_700();
  const std::string dl = fresh_download_dir("menu");
  for (const char* ep : {"1", "2", "3"}) plant_download(dl + "/700/sub/" + ep + ".mp4");
  Config config;
  config.mpv_path = write_stub_mpv();
  ScriptedPick pick;
  // show, episode 1, then: next (ep 2), next (ep 3), previous (ep 2), quit.
  pick.answers = {0, 0, 0, 0, 1, 3};
  CHECK(run_play(p, config, dl, play_args(), pick.fn()) == 0);
  CHECK(pick.asked("what next") == 4);
  REQUIRE(pick.rows_seen.size() == 6);
  CHECK(pick.rows_seen[2][0] == "next  \xC2\xB7  ep 2");      // after ep 1
  CHECK(pick.rows_seen[3][0] == "next  \xC2\xB7  ep 3");      // after ep 2
  CHECK(pick.rows_seen[4][0] == "replay  \xC2\xB7  ep 3");    // after ep 3: no next
  CHECK(pick.rows_seen[4][2] == "quit");
  CHECK(pick.rows_seen[5][3] == "quit");                       // after ep 2 again
  // EOF at the menu is the "bye" exit, a clean 0.
  ScriptedPick eof;
  eof.answers = {0, 0};
  CHECK(run_play(p, config, dl, play_args(), eof.fn()) == 0);
  CHECK(eof.asked("what next") == 1);
  // `-e` starts at the episode and then asks like a prompted pick would.
  cli::PlayArgs from_two = play_args();
  from_two.episode = "2";
  ScriptedPick e;
  e.answers = {0, 0};  // show; then "next" = ep 3; then EOF
  CHECK(run_play(p, config, dl, from_two, e.fn()) == 0);
  CHECK(e.asked("pick an episode") == 0);
  CHECK(e.asked("what next") == 2);
  CHECK(e.rows_seen[1][0] == "next  \xC2\xB7  ep 3");
}

namespace {

// A fresh library at a temp path, holding show 700 ("Frieren", 3 episodes)
// bound to `provider` with episode 1 finished (progress 1).
std::string fresh_db(const char* tag) {
  const std::string base = "/tmp/shigoku-cli-test-" + std::to_string(static_cast<long>(::getpid())) +
                           "-" + tag + ".db";
  for (const char* suffix : {"", "-wal", "-shm"}) std::remove((base + suffix).c_str());
  return base;
}

constexpr std::int64_t kNow = 1'700'000'000;

void seed_frieren(Store& store, std::string_view provider, std::uint32_t finished) {
  Enrichment e;
  e.anilist_id = 700;
  e.title_romaji = "Frieren";
  e.total_episodes = 3;
  REQUIRE(store.add_to_library(e, kNow).has_value());
  REQUIRE(store.bind_provider(e, provider, "id", kNow).has_value());
  for (std::uint32_t i = 1; i <= finished; ++i) {
    // Past the watched ratio: the play counts and progress ratchets to i.
    REQUIRE(store.record_finish(700, Translation::Sub, std::to_string(i), i, 1180.0, 1200.0,
                                provider, kNow + i)
                .has_value());
  }
}

int run_continue(const ProviderRegistry& registry, const shigoku::cli_play::Sources& sources,
                 const Config& config, const std::string& dl, Store& store,
                 const cli::ContinueArgs& args, shigoku::cli_play::PickFn pick) {
  return shigoku::cli_play::continue_flow(registry, sources, std::move(pick), Translation::Sub,
                                          config, /*cache_dir=*/"", /*runtime_dir=*/"/tmp", dl,
                                          store, args);
}

}  // namespace

TEST_CASE("continue_plays_the_episode_after_the_last_finished_from_the_bound_source") {
  auto opened = Store::open(fresh_db("continue-next"));
  REQUIRE(opened.has_value());
  Store& store = *opened;
  seed_frieren(store, "fake", /*finished=*/1);
  // A search that must never run: the binding answers.
  auto f = make([](std::string_view) -> Fake::SearchR { return err(ProviderError::server(500)); },
                three_episodes, []() -> Fake::ResolveR { return err(ProviderError::network()); });
  std::vector<std::unique_ptr<StreamProvider>> ps;
  ps.push_back(std::make_unique<Fake>(f));
  const ProviderRegistry registry(std::move(ps));
  const shigoku::cli_play::Sources sources = {registry.at(0)};
  const std::string dl = fresh_download_dir("continue-next");
  plant_download(dl + "/700/sub/2.mp4");  // only ep 2: exit 0 = ep 2 was the one played
  Config config;
  config.mpv_path = write_stub_mpv();
  ScriptedPick pick;
  pick.answers = {std::nullopt};  // the menu: quit
  CHECK(run_continue(registry, sources, config, dl, store, cli::ContinueArgs{}, pick.fn()) == 0);
  CHECK(pick.asked("continue which show") == 0);  // one row needs no prompt
  CHECK(pick.asked("what next") == 1);
}

TEST_CASE("continue_prefers_the_freshest_partial_watch_over_progress") {
  auto opened = Store::open(fresh_db("continue-partial"));
  REQUIRE(opened.has_value());
  Store& store = *opened;
  seed_frieren(store, "fake", /*finished=*/1);
  // A quarter of ep 3, later than the finish: the resume row alone (under
  // half, the play does not count), and it is where the show is picked up.
  REQUIRE(store.record_finish(700, Translation::Sub, "3", 3, 300.0, 1200.0, "fake", kNow + 10)
              .has_value());
  auto f = make([](std::string_view) -> Fake::SearchR { return err(ProviderError::server(500)); },
                three_episodes, []() -> Fake::ResolveR { return err(ProviderError::network()); });
  std::vector<std::unique_ptr<StreamProvider>> ps;
  ps.push_back(std::make_unique<Fake>(f));
  const ProviderRegistry registry(std::move(ps));
  const std::string dl = fresh_download_dir("continue-partial");
  plant_download(dl + "/700/sub/3.mp4");
  Config config;
  config.mpv_path = write_stub_mpv();
  ScriptedPick pick;
  CHECK(run_continue(registry, {registry.at(0)}, config, dl, store, cli::ContinueArgs{},
                     pick.fn()) == 0);
}

TEST_CASE("continue_with_a_dead_binding_searches_again_and_takes_the_hit_with_its_id") {
  auto opened = Store::open(fresh_db("continue-dead"));
  REQUIRE(opened.has_value());
  Store& store = *opened;
  seed_frieren(store, "dead", /*finished=*/1);
  auto dead = make([](std::string_view) -> Fake::SearchR { return err(ProviderError::server(500)); },
                   no_episodes, no_resolve);
  dead.name_ = "dead";
  dead.display_ = "dead";
  // Two hits; the second carries the library row's id and is taken unasked.
  auto up = make(
      [](std::string_view) -> Fake::SearchR {
        auto other = one_hit();
        other.anilist_id = 1;
        other.title = "Frieren Movie";
        auto ours = one_hit();
        ours.anilist_id = 700;
        return std::vector<SearchHit>{other, ours};
      },
      three_episodes, []() -> Fake::ResolveR { return err(ProviderError::network()); });
  up.name_ = "up";
  up.display_ = "up";
  std::vector<std::unique_ptr<StreamProvider>> ps;
  ps.push_back(std::make_unique<Fake>(dead));
  ps.push_back(std::make_unique<Fake>(up));
  const ProviderRegistry registry(std::move(ps));
  const std::string dl = fresh_download_dir("continue-dead");
  plant_download(dl + "/700/sub/2.mp4");
  Config config;
  config.mpv_path = write_stub_mpv();
  ScriptedPick pick;
  CHECK(run_continue(registry, {registry.at(1)}, config, dl, store, cli::ContinueArgs{},
                     pick.fn()) == 0);
  CHECK(pick.asked("pick a show") == 0);
  // The re-found source is now bound too, so the next continue asks it first.
  auto bindings = store.bindings_for(700);
  REQUIRE(bindings.has_value());
  bool bound_up = false;
  for (const Binding& b : *bindings) bound_up = bound_up || b.provider == "up";
  CHECK(bound_up);
}

TEST_CASE("continue_says_caught_up_filters_by_query_and_honours_the_show_flag") {
  auto opened = Store::open(fresh_db("continue-caught"));
  REQUIRE(opened.has_value());
  Store& store = *opened;
  auto f = make([](std::string_view) -> Fake::SearchR { return err(ProviderError::server(500)); },
                three_episodes, []() -> Fake::ResolveR { return err(ProviderError::network()); });
  std::vector<std::unique_ptr<StreamProvider>> ps;
  ps.push_back(std::make_unique<Fake>(f));
  const ProviderRegistry registry(std::move(ps));
  const shigoku::cli_play::Sources sources = {registry.at(0)};
  const Config config;  // no stub mpv: any play attempt fails (resolve) with 1
  ScriptedPick pick;
  // An empty library is a clean 0.
  CHECK(run_continue(registry, sources, config, "", store, cli::ContinueArgs{}, pick.fn()) == 0);
  seed_frieren(store, "fake", /*finished=*/3);
  // All three watched: caught up, nothing played.
  CHECK(run_continue(registry, sources, config, "", store, cli::ContinueArgs{}, pick.fn()) == 0);
  // A query nothing matches, and a -S past the list.
  cli::ContinueArgs miss;
  miss.query = "zzz";
  CHECK(run_continue(registry, sources, config, "", store, miss, pick.fn()) == 0);
  cli::ContinueArgs past;
  past.show = 2;
  CHECK(run_continue(registry, sources, config, "", store, past, pick.fn()) == 0);
  CHECK(pick.prompts.empty());
  // A second show makes it a list; -S 2 takes it unasked and its ep 1 (nothing
  // finished) is attempted: resolve fails, the play-path 1.
  Enrichment e;
  e.anilist_id = 701;
  e.title_romaji = "Cowboy Bebop";
  REQUIRE(store.add_to_library(e, kNow).has_value());
  REQUIRE(store.bind_provider(e, "fake", "id2", kNow).has_value());
  cli::ContinueArgs second;
  second.show = 2;
  CHECK(run_continue(registry, sources, config, "", store, second, pick.fn()) == 1);
  CHECK(pick.prompts.empty());
  // Without -S the list is offered, most recently watched first.
  ScriptedPick ask;
  CHECK(run_continue(registry, sources, config, "", store, cli::ContinueArgs{}, ask.fn()) == 0);
  CHECK(ask.asked("continue which show") == 1);
  REQUIRE(ask.rows_seen.size() == 1);
  CHECK(ask.rows_seen[0][0] == "Frieren  \xC2\xB7  ep 3 of 3");
  CHECK(ask.rows_seen[0][1] == "Cowboy Bebop  \xC2\xB7  not started");
}

TEST_CASE("a_hit_with_only_a_mal_id_is_filed_under_the_bridged_anilist_id") {
  auto opened = Store::open(fresh_db("mal-only"));
  REQUIRE(opened.has_value());
  Store& store = *opened;
  // A hit shaped like senshi's: a MAL id, no AniList id. The table knows
  // Frieren (52991); 2000000000 it does not.
  static const std::int64_t kFrieren = 52991;
  static const std::int64_t kUnknown = 2000000000;
  REQUIRE(idmap::to_anilist(kFrieren).has_value());
  REQUIRE_FALSE(idmap::to_anilist(kUnknown).has_value());
  auto known = make(
      [](std::string_view) -> Fake::SearchR {
        auto h = one_hit();
        h.mal_id = kFrieren;
        return std::vector<SearchHit>{h};
      },
      []() -> Fake::EpR { return std::vector<std::string>{"1"}; },
      []() -> Fake::ResolveR { return err(ProviderError::network()); });
  known.name_ = "known";
  auto unknown = make(
      [](std::string_view) -> Fake::SearchR {
        auto h = one_hit();
        h.mal_id = kUnknown;
        return std::vector<SearchHit>{h};
      },
      []() -> Fake::EpR { return std::vector<std::string>{"1"}; },
      []() -> Fake::ResolveR { return err(ProviderError::network()); });
  unknown.name_ = "unknown";
  ScriptedPick pick;
  pick.answers = {0, 0, 0, 0, 0, 0};  // show + episode, three runs
  cli::PlayArgs args = play_args();
  // Resolve fails after the bind, so each run is the play-path 1 with the
  // library touched on the way.
  CHECK(shigoku::cli_play::play_flow({&known}, pick.fn(), Translation::Sub, Config{}, "", "/tmp",
                                     "", &store, args) == 1);
  CHECK(shigoku::cli_play::play_flow({&unknown}, pick.fn(), Translation::Sub, Config{}, "", "/tmp",
                                     "", &store, args) == 1);
  auto real = store.bindings_for(*idmap::to_anilist(kFrieren));
  REQUIRE(real.has_value());
  REQUIRE(real->size() == 1);
  CHECK((*real)[0].provider == "known");
  auto synthetic = store.bindings_for(-kUnknown);
  REQUIRE(synthetic.has_value());
  REQUIRE(synthetic->size() == 1);
  CHECK((*synthetic)[0].provider == "unknown");
  // A hit with neither id binds nothing: play-only, as ever.
  auto neither = make([](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{one_hit()}; },
                      []() -> Fake::EpR { return std::vector<std::string>{"1"}; },
                      []() -> Fake::ResolveR { return err(ProviderError::network()); });
  neither.name_ = "neither";
  CHECK(shigoku::cli_play::play_flow({&neither}, pick.fn(), Translation::Sub, Config{}, "", "/tmp",
                                     "", &store, args) == 1);
  auto rows = store.list_history();
  REQUIRE(rows.has_value());
  CHECK(rows->empty());  // bindings mint identity rows, not library rows
  auto none = store.show_id_for_binding("neither", "id");
  REQUIRE(none.has_value());
  CHECK_FALSE(none->has_value());
}
