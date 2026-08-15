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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../src/cli.hpp"
#include "../src/config.hpp"
#include "../src/domain.hpp"
#include "../src/download.hpp"
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
  return PlayArgs{std::move(q), dub, std::move(quality)};
}

cli::DownloadArgs dl(std::string q, std::optional<std::string> ep = std::nullopt,
                     bool dub = false) {
  return cli::DownloadArgs{std::move(q), std::move(ep), dub};
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

TEST_CASE("quality_note_only_for_a_non_default_value") {
  CHECK_FALSE(cli::quality_note_needed(std::nullopt));
  CHECK_FALSE(cli::quality_note_needed(std::optional<std::string_view>("best")));
  CHECK_FALSE(cli::quality_note_needed(std::optional<std::string_view>("Best")));
  CHECK(cli::quality_note_needed(std::optional<std::string_view>("1080")));
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

  [[nodiscard]] std::string_view name() const override { return "fake"; }
  [[nodiscard]] std::string_view display_name() const override { return "Fake"; }
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
int run(const Fake& p, shigoku::cli_play::PickFn pick) {
  const Config config;
  return shigoku::cli_play::play_flow(p, pick, Translation::Sub, config, /*cache_dir=*/"",
                                      /*runtime_dir=*/"/tmp", /*download_dir=*/"",
                                      /*store=*/nullptr,
                                      cli::PlayArgs{"frieren", false, std::nullopt});
}

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
  CHECK(run(p, [](const char*, std::size_t) { return std::optional<std::size_t>(0); }) == 0);
}

TEST_CASE("search_failure_exits_one") {
  auto p = make([](std::string_view) -> Fake::SearchR { return err(ProviderError::network()); },
                []() -> Fake::EpR { return std::vector<std::string>{}; },
                []() -> Fake::ResolveR { return err(ProviderError::network()); });
  CHECK(run(p, [](const char*, std::size_t) { return std::optional<std::size_t>(0); }) == 1);
}

TEST_CASE("quitting_the_show_pick_exits_zero") {
  auto p = make([](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{one_hit()}; },
                []() -> Fake::EpR { return std::vector<std::string>{"1"}; },
                []() -> Fake::ResolveR { return err(ProviderError::network()); });
  CHECK(run(p, [](const char*, std::size_t) { return std::optional<std::size_t>{}; }) == 0);
}

TEST_CASE("resolve_failure_exits_one") {
  // Auto-pick show then episode; resolve errors before mpv is ever built, so
  // play() returns Err on the first attempt with no retry or backoff.
  auto p = make([](std::string_view) -> Fake::SearchR { return std::vector<SearchHit>{one_hit()}; },
                []() -> Fake::EpR { return std::vector<std::string>{"1"}; },
                []() -> Fake::ResolveR { return err(ProviderError::network()); });
  CHECK(run(p, [](const char*, std::size_t) { return std::optional<std::size_t>(0); }) == 1);
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
  const auto pick = [](const char*, std::size_t) { return std::optional<std::size_t>(0); };
  const cli::PlayArgs args{"frieren", false, std::nullopt};
  CHECK(shigoku::cli_play::play_flow(p, pick, Translation::Sub, config, /*cache_dir=*/"",
                                     /*runtime_dir=*/"/tmp", dl, /*store=*/nullptr,
                                     args) == 0);
  // Track mismatch never matches (the DoD case): the same show played as DUB
  // finds no /700/dub/ file, falls through to the scripted resolve failure,
  // and exits 1 — the sub download was never adopted.
  CHECK(shigoku::cli_play::play_flow(p, pick, Translation::Dub, config, /*cache_dir=*/"",
                                     /*runtime_dir=*/"/tmp", dl, /*store=*/nullptr,
                                     args) == 1);
}
