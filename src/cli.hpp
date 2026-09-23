// cli.hpp — CLI dispatch + the sync/connect/search/pick renderers (06 §7,
// P27). Ported 1:1 from sabigoku src/cli.rs. Pure over argv and over the
// domain summaries: no I/O, no process exit — main.cpp owns those (06 §7.4
// exit law). Subcommand = first non-flag positional matching a known name;
// flags may precede it; after a real query word, subcommand names are search
// text (06 §7.1). Version outranks everything.
//
// The Rust routed provider/play failures through two intermediary enums
// (tui::event::FetchClass, PlayFailure); shigoku never grew those (the toast
// matrix keys straight off ProviderError::Kind / PlayError::Kind, see
// error.hpp / app.cpp play_error_toast). So the CLI copy keys directly off
// those kinds here — same message set, one fewer indirection.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain.hpp"    // Translation
#include "error.hpp"     // ProviderError::Kind, PlayError::Kind
#include "login.hpp"     // ConnectResult
#include "provider.hpp"  // SearchHit
#include "sync.hpp"      // SyncSummary

namespace shigoku::cli {

// An inclusive span of episodes for `-r`: each end is an episode label, or a
// 1-based position in the list (map_episode_index's two readings); `last`
// nullopt = through the end of the list (`-r 4-`).
struct EpisodeRange {
  std::string first;
  std::optional<std::string> last;
  friend bool operator==(const EpisodeRange&, const EpisodeRange&) = default;
};

// The dispatch verdict over argv (without argv0). `parse` is pure; main owns
// the process exit (06 §7.4).
struct PlayArgs {
  std::string query;
  bool dub = false;
  // `-q`: the stream quality for this run, over the config's default_quality
  // (parse_quality reads either; a spelling it does not know plays best,
  // with quality_note's heads-up).
  std::optional<std::string> quality;
  // `-e`: play this episode with no episode prompt (exact label first, then
  // 1-based position). `-r`: play a span in list order, no prompt and no
  // post-play menu. Never both — the parser makes that usage.
  std::optional<std::string> episode;
  std::optional<EpisodeRange> range;
  // `-S N`: take the Nth search result (1-based) with no show prompt.
  std::optional<std::uint32_t> show;
  // `-p <name>`: the source asked first this run (its stable name: senshi,
  // hianime, …), over the config's preferred_provider.
  std::optional<std::string> provider;
  friend bool operator==(const PlayArgs&, const PlayArgs&) = default;
};

// `shigoku download <query> [<ep>]` (P35 slice 3, shigoku-only §9). The last
// positional is the episode when two or more are given (or `-e` names it);
// a lone positional is the query and the episode is picked interactively
// (the play flow's prompt). `-q` / `-S` / `-p` read as for play.
struct DownloadArgs {
  std::string query;
  std::optional<std::string> episode;
  bool dub = false;
  std::optional<std::string> quality;
  std::optional<std::uint32_t> show;
  std::optional<std::string> provider;
  friend bool operator==(const DownloadArgs&, const DownloadArgs&) = default;
};

// `shigoku continue [<query>]`: pick a library show back up where it was
// left. No query = the whole history to choose from, most recently watched
// first; a query narrows it by title, and a single match needs no prompt.
// `-S` takes the Nth row of that list. The episode is never a flag here —
// the library says which one comes next.
struct ContinueArgs {
  std::optional<std::string> query;
  bool dub = false;
  std::optional<std::string> quality;
  std::optional<std::uint32_t> show;
  std::optional<std::string> provider;
  friend bool operator==(const ContinueArgs&, const ContinueArgs&) = default;
};

struct Command {
  enum class Kind {
    Tui,
    Version,
    Paths,
    Usage,
    Login,
    Sync,
    Update,
    Play,
    Download,
    Continue,
  };
  Kind kind = Kind::Tui;
  bool paste = false;  // Login only.
  PlayArgs play_args;  // Play only.
  DownloadArgs download_args;  // Download only.
  ContinueArgs continue_args;  // Continue only.

  friend bool operator==(const Command&, const Command&) = default;

  static Command tui() { return {Kind::Tui, false, {}, {}, {}}; }
  static Command version() { return {Kind::Version, false, {}, {}, {}}; }
  static Command paths() { return {Kind::Paths, false, {}, {}, {}}; }
  static Command usage() { return {Kind::Usage, false, {}, {}, {}}; }
  static Command login(bool paste) { return {Kind::Login, paste, {}, {}, {}}; }
  static Command sync() { return {Kind::Sync, false, {}, {}, {}}; }
  static Command update() { return {Kind::Update, false, {}, {}, {}}; }
  static Command play(PlayArgs a) { return {Kind::Play, false, std::move(a), {}, {}}; }
  static Command download(DownloadArgs a) {
    return {Kind::Download, false, {}, std::move(a), {}};
  }
  static Command continue_show(ContinueArgs a) {
    return {Kind::Continue, false, {}, {}, std::move(a)};
  }
};

// `--debug` is global and consumed by every path (06 §7.2); read it before
// dispatch so the debug sink is armed for every command.
[[nodiscard]] bool debug_flag(const std::vector<std::string>& args);

// `--ascii` is global like `--debug`: every glyph the command line prints
// becomes its plain-ASCII stand-in, whatever the locale says.
[[nodiscard]] bool ascii_flag(const std::vector<std::string>& args);

// The glyphs the command line prints. UTF-8 by default; set_ascii_glyphs(true)
// swaps each for an ASCII stand-in — for a locale that is not UTF-8, or
// `--ascii`. One process-wide setting, read by every renderer here and by
// the flows' own lines; main sets it once before dispatch.
struct Glyphs {
  std::string_view fail;      // ✗   x
  std::string_view ok;        // ✓   +
  std::string_view play;      // ▶   >
  std::string_view resume;    // ↺   <
  std::string_view warn;      // ⚠   !
  std::string_view fetch;     // ⇣   v
  std::string_view dot;       // ·   -
  std::string_view ellipsis;  // …   ...
};
void set_ascii_glyphs(bool ascii);
[[nodiscard]] bool ascii_glyphs();
[[nodiscard]] const Glyphs& glyphs();

// Whether a locale name or codeset says UTF-8 — "en_US.UTF-8", "C.utf8",
// "UTF-8" do; "C", "POSIX", "" and "ANSI_X3.4-1968" do not. The platform
// lookup (setlocale / nl_langinfo / the LC_* variables) is main's.
[[nodiscard]] bool utf8_locale_name(std::string_view name);

// Dispatch over argv (without argv0), pure (06 §7).
[[nodiscard]] Command parse(const std::vector<std::string>& args);

// All commands listed, a deliberate deviation from zigoku's query-only usage.
extern const char* const kUsage;

// Whether a completed stdin read yields a usable paste line, given the byte
// count and the buffer. A complete line is accepted even without a trailing
// newline (06 §4 ratified looser than zigoku); only an empty EOF (n == 0) or a
// cap-length read with no newline (a truncated overlong paste) aborts.
[[nodiscard]] bool paste_line_usable(std::size_t n, std::string_view line, std::uint64_t cap);

// Login outcome line (login/loopback wording). `paste` picks the retry
// coaching: re-copy the fragment vs re-run the flow. BadState/Canceled never
// reach the CLI (serve waits through bad states, Ctrl-C kills the process);
// their arms exist to stay total.
[[nodiscard]] std::string render_connect_result(const login::ConnectResult& result,
                                                std::string_view auth_path, bool paste);

// `shigoku sync` human summary (06 §5.2 CLI row). Every terminal outcome is a
// single line; a completed run reads the counts. `mal_pushed` (P31 §9.1
// slice 4) is a plain count, not a sync::SyncSummary field — the MAL mirror
// is a separate push with its own outcome (mal_mirror.hpp), silent on
// anything but a completed push (same rationale as the TUI's on_sync_flushed
// toast); only that one count rides into this render.
// The MAL mirror's pull, flattened for the render (mal_mirror.hpp stays out
// of this header's graph, like sync's own counts ride SyncSummary). `ran` =
// a MAL account was connected so the pull was attempted at all.
struct MalPullCounts {
  bool ran = false;
  bool failed = false;        // transport/decode miss: nothing adopted.
  bool unauthorized = false;  // MAL refused the token.
  std::uint32_t pulled = 0;
  std::uint32_t imported = 0;
};

[[nodiscard]] std::string render_sync_summary(const sync::SyncSummary& s,
                                               std::uint32_t mal_pushed = 0,
                                               MalPullCounts mal_pull = {},
                                               std::uint32_t airing_refreshed = 0);

// Which network call failed. Data and Unsupported read per stage: a search
// miss is not a resolve miss, and search-stage Unsupported is the
// default-provider trap (megaplay cannot search), not a dead episode.
enum class FetchStage {
  Search,
  Episodes,
  Resolve,
};

// One unnumbered row per hit ("Frieren  ·  28 sub eps": per-track count when
// the chosen track is stocked, else the catalog total, else bare; a source
// whose hits carry a second, English or romaji title that differs from the
// first — AniLibria's Russian titles — shows it in parentheses) and per
// episode ("ep 7"): the picker seam's input. Titles and labels are provider
// claims — terminal-hostile bytes are stripped here.
[[nodiscard]] std::vector<std::string> search_hit_rows(const std::vector<SearchHit>& hits,
                                                       Translation translation);
[[nodiscard]] std::vector<std::string> episode_rows(const std::vector<std::string>& labels);

// "  NN. row\n" per row, numbers right-aligned to `width` — the numbered
// prompt's list.
[[nodiscard]] std::string numbered_rows(const std::vector<std::string>& rows, int width);

// The count line + the numbered rows (search: width 2, episodes: width 3).
[[nodiscard]] std::string render_search_hits(const std::vector<SearchHit>& hits,
                                            Translation translation);
[[nodiscard]] std::string render_episode_list(const std::vector<std::string>& labels);

// One resolved pick from a numbered prompt. Reprompt is a blank line (retry,
// no coaching); the two error kinds carry their own copy, formatted against
// `max` in the IO loop. EOF / overlong read are the caller's abort, never here.
enum class PickInput {
  Abort,
  Reprompt,
  NotNumber,
  OutOfRange,
  Pick,
};

// classify_pick's verdict plus, for Pick, the 0-based index.
struct PickResult {
  PickInput kind = PickInput::Reprompt;
  std::size_t index = 0;  // meaningful only for Pick.
  friend bool operator==(const PickResult&, const PickResult&) = default;
};

// promptChoice, IO-free: trim, `q` aborts, blank reprompts, a non-number and
// an out-of-[1, max] each reprompt, else the 0-based index.
[[nodiscard]] PickResult classify_pick(std::string_view line, std::size_t max);

// Provider-failure copy in the CLI's sentence register (the TUI's terse toast
// rows are separate). Search-stage Unsupported is the fresh-install trap: the
// default preferred provider is megaplay, which cannot search, so steer to the
// fix rather than parrot "unsupported". `provider` is the display name.
[[nodiscard]] std::string fetch_error_line(FetchStage stage, ProviderError::Kind kind,
                                          std::string_view provider);

// The heads-up for a `-q` spelling parse_quality does not know ("(note: no
// quality called "x"; using best.)"); nullopt for a known one, or no flag.
[[nodiscard]] std::optional<std::string> quality_note(std::optional<std::string_view> quality);

// `-p` named a source the registry has not got: one line naming the ones a
// run can search, in walk order.
[[nodiscard]] std::string unknown_source_note(std::string_view asked,
                                              const std::vector<std::string_view>& names);

// `-r` text -> span: "3-7", or "3-" for "through the end". Anything else (a
// bare number — that is `-e` —, an empty start, a second dash) is nullopt,
// which the parser reads as usage.
[[nodiscard]] std::optional<EpisodeRange> parse_range(std::string_view text);

// `-e` / `-S` text as a 1-based ordinal: all digits -> the number (0 for an
// empty string or an overflow); any other byte -> 0, map_episode_index's
// "no ordinal reading".
[[nodiscard]] std::uint32_t ordinal_of(std::string_view text);

// The span as 0-based [first, last] over a real episode list, each end read
// exact-label-first then as a 1-based position (map_episode_index); nullopt
// when an end is not in the list or the two cross.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> range_indices(
    const std::vector<std::string>& episodes, const EpisodeRange& range);

// What to do once an episode has played, offered through the pick seam like
// any other list: next and previous only when such an episode exists, replay
// and quit always; `actions` is parallel to `rows`.
enum class PostPlay {
  Next,
  Replay,
  Previous,
  Quit,
};
struct PostPlayMenu {
  std::vector<std::string> rows;  // "next  ·  ep 8", …, "quit"
  std::vector<PostPlay> actions;
};
[[nodiscard]] PostPlayMenu post_play_menu(std::size_t index,
                                          const std::vector<std::string>& episodes);

// `shigoku continue`'s list: one row per library show — the title (English
// one in parentheses when it differs), then how far it got ("ep 7 of 28",
// "ep 7", or "not started").
[[nodiscard]] std::vector<std::string> history_rows(const std::vector<Show>& shows);

// The rows of `shows` whose romaji, English or native title contains `query`,
// ASCII case-folded (a non-ASCII query matches on its exact bytes). Empty
// query = every row.
[[nodiscard]] std::vector<std::size_t> history_matches(const std::vector<Show>& shows,
                                                       std::string_view query);

// Heads-up when the configured source was walked past because it cannot search
// (ROD-491). nullopt when nothing was overridden: no preference set, or the
// preference is the source the run is using. Identity compares on the stable
// name, the copy shows the display one.
[[nodiscard]] std::optional<std::string> provider_override_note(
    std::optional<std::pair<std::string_view, std::string_view>> asked,
    std::pair<std::string_view, std::string_view> chosen);

// Search-walk note: `skipped` either failed its search (`failure` carries the
// class) or answered with nothing (nullopt), and the run moves on to `next`.
// One parenthesised line in the override note's register; the walk itself
// lives in play_cli. Display names in, terminal-hostile bytes are not a
// concern (provider display names are compile-time constants).
[[nodiscard]] std::string search_walk_note(std::string_view skipped,
                                           std::optional<ProviderError::Kind> failure,
                                           std::string_view next);

// Play-failure copy in the CLI's sentence register, keyed off PlayError::Kind.
// A Resolve failure delegates to fetch_error_line at the resolve stage — but
// shigoku's PlayError::Resolve carries only a detail string (the ProviderError
// class is collapsed in player::play), so the caller passes the class it
// captured in its resolve closure via `resolve_class`. nullopt there = the
// generic resolve line (no class was observed, e.g. a proxy/guard resolve).
[[nodiscard]] std::string player_failure_line(
    PlayError::Kind kind, std::string_view provider,
    std::optional<ProviderError::Kind> resolve_class);

}  // namespace shigoku::cli
