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

// The dispatch verdict over argv (without argv0). `parse` is pure; main owns
// the process exit (06 §7.4).
struct PlayArgs {
  std::string query;
  bool dub = false;
  // Raw value, parity-inert: default_quality drives playback (06 §7). Parsed
  // so the flag never silently swallows its argument, but never wired to
  // resolve — a heads-up fires on a non-default value (quality_note_needed).
  std::optional<std::string> quality;
  friend bool operator==(const PlayArgs&, const PlayArgs&) = default;
};

// `shigoku download <query> [<ep>]` (P35 slice 3, shigoku-only §9). The last
// positional is the episode when two or more are given; a lone positional is
// the query and the episode is picked interactively (the play flow's prompt).
struct DownloadArgs {
  std::string query;
  std::optional<std::string> episode;
  bool dub = false;
  friend bool operator==(const DownloadArgs&, const DownloadArgs&) = default;
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
  };
  Kind kind = Kind::Tui;
  bool paste = false;  // Login only.
  PlayArgs play_args;  // Play only.
  DownloadArgs download_args;  // Download only.

  friend bool operator==(const Command&, const Command&) = default;

  static Command tui() { return {Kind::Tui, false, {}, {}}; }
  static Command version() { return {Kind::Version, false, {}, {}}; }
  static Command paths() { return {Kind::Paths, false, {}, {}}; }
  static Command usage() { return {Kind::Usage, false, {}, {}}; }
  static Command login(bool paste) { return {Kind::Login, paste, {}, {}}; }
  static Command sync() { return {Kind::Sync, false, {}, {}}; }
  static Command update() { return {Kind::Update, false, {}, {}}; }
  static Command play(PlayArgs a) { return {Kind::Play, false, std::move(a), {}}; }
  static Command download(DownloadArgs a) {
    return {Kind::Download, false, {}, std::move(a)};
  }
};

// `--debug` is global and consumed by every path (06 §7.2); read it before
// dispatch so the debug sink is armed for every command.
[[nodiscard]] bool debug_flag(const std::vector<std::string>& args);

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
[[nodiscard]] std::string render_sync_summary(const sync::SyncSummary& s,
                                               std::uint32_t mal_pushed = 0);

// Which network call failed. Data and Unsupported read per stage: a search
// miss is not a resolve miss, and search-stage Unsupported is the
// default-provider trap (megaplay cannot search), not a dead episode.
enum class FetchStage {
  Search,
  Episodes,
  Resolve,
};

// Numbered search results, the three-branch count line: per-track when the
// chosen track is stocked, else the catalog total, else bare. Titles are
// provider claims — strip terminal-hostile bytes before stdout.
[[nodiscard]] std::string render_search_hits(const std::vector<SearchHit>& hits,
                                            Translation translation);

// Numbered episode list, width-3 `ep <label>` rows. Labels are provider claims
// — strip terminal-hostile bytes before stdout.
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

// Whether the inert `--quality` heads-up fires (06 §7 parity): only when a
// non-default value was passed. No flag, or an explicit `best`, stays silent.
[[nodiscard]] bool quality_note_needed(std::optional<std::string_view> quality);

// Heads-up when the configured source was walked past because it cannot search
// (ROD-491). nullopt when nothing was overridden: no preference set, or the
// preference is the source the run is using. Identity compares on the stable
// name, the copy shows the display one.
[[nodiscard]] std::optional<std::string> provider_override_note(
    std::optional<std::pair<std::string_view, std::string_view>> asked,
    std::pair<std::string_view, std::string_view> chosen);

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
