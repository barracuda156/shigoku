// cli.cpp — see cli.hpp. Ported 1:1 from sabigoku src/cli.rs.

#include "cli.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace shigoku::cli {

namespace {

// ASCII case-insensitive equality (the flag/quality compares are ASCII).
bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}

bool starts_with(std::string_view s, std::string_view p) { return s.rfind(p, 0) == 0; }

// A decimal, right-justified to at least `width` with leading spaces (the
// zigoku `{:>2}` / `{:>3}` list numbering). No fixed buffer — the count can be
// any size.
std::string rjust(std::size_t n, std::size_t width) {
  std::string s = std::to_string(n);
  if (s.size() < width) s.insert(s.begin(), width - s.size(), ' ');
  return s;
}

// First non-flag positional equals `name`; any other positional ends the scan.
bool is_subcommand(const std::vector<std::string>& args, std::string_view name) {
  for (const auto& a : args) {
    if (a == name) return true;
    if (!starts_with(a, "-")) return false;
  }
  return false;
}

Command parse_query(const std::vector<std::string>& args) {
  std::vector<std::string_view> words;
  bool dub = false;
  std::optional<std::string> quality;

  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (a == "--dub") {
      dub = true;
    } else if (a == "--sub") {
      dub = false;
    } else if (a == "--quality") {
      if (i + 1 >= args.size()) return Command::usage();
      quality = args[++i];
    } else if (starts_with(a, "--quality=")) {
      quality = std::string(a.substr(std::string_view("--quality=").size()));
    } else if (a == "--debug") {
      // Global, consumed: not a query word, not an unknown flag.
    } else if (starts_with(a, "--")) {
      return Command::usage();
    } else {
      // Single-dash words fall through to the query (only `--` prefixes are
      // flags here).
      words.push_back(a);
    }
  }

  if (words.empty()) return Command::tui();
  std::string joined;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i > 0) joined += ' ';
    joined += std::string(words[i]);
  }
  return Command::play(PlayArgs{std::move(joined), dub, std::move(quality)});
}

// `download <query…> [<ep>]` (P35 slice 3): flags as the query path; the LAST
// positional is the episode label when 2+ positionals follow the subcommand
// word, else the lone word is the query (interactive episode pick). No
// positionals at all is usage.
Command parse_download(const std::vector<std::string>& args) {
  std::vector<std::string_view> words;
  bool dub = false;
  bool seen_subcommand = false;

  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (!seen_subcommand && a == "download") {
      seen_subcommand = true;  // the subcommand word itself.
    } else if (a == "--dub") {
      dub = true;
    } else if (a == "--sub") {
      dub = false;
    } else if (a == "--debug") {
      // Global, consumed.
    } else if (starts_with(a, "--")) {
      return Command::usage();
    } else {
      // is_subcommand guaranteed only flags precede the subcommand word, so
      // every positional here follows it.
      words.push_back(a);
    }
  }

  if (words.empty()) return Command::usage();
  std::optional<std::string> episode;
  if (words.size() >= 2) {
    episode = std::string(words.back());
    words.pop_back();
  }
  std::string joined;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i > 0) joined += ' ';
    joined += std::string(words[i]);
  }
  return Command::download(DownloadArgs{std::move(joined), std::move(episode), dub});
}

}  // namespace

bool debug_flag(const std::vector<std::string>& args) {
  return std::any_of(args.begin(), args.end(), [](const std::string& a) { return a == "--debug"; });
}

Command parse(const std::vector<std::string>& args) {
  // Version outranks everything, even a bad flag or a query (never routed
  // through the usage fallthrough).
  for (const auto& a : args) {
    if (a == "--version" || a == "-V") return Command::version();
  }
  for (const auto& a : args) {
    if (a == "--paths") return Command::paths();
  }
  if (is_subcommand(args, "login")) {
    bool paste = std::any_of(args.begin(), args.end(),
                             [](const std::string& a) { return a == "--paste"; });
    return Command::login(paste);
  }
  if (is_subcommand(args, "sync")) return Command::sync();
  if (is_subcommand(args, "update")) return Command::update();
  if (is_subcommand(args, "download")) return parse_download(args);
  return parse_query(args);
}

const char* const kUsage =
    "  usage: shigoku <query> [--dub] [--quality <q>] [--debug]\n"
    "         shigoku download <query> [<ep>] [--dub]\n"
    "         shigoku login [--paste]\n"
    "         shigoku sync\n"
    "         shigoku update\n"
    "         shigoku --version\n"
    "\n"
    "    shigoku frieren\n"
    "    shigoku \"cowboy bebop\" --dub\n"
    "    shigoku download frieren 7\n"
    "    shigoku login\n"
    "\n"
    "  --version (or -V) prints the version and exits.\n"
    "  --paths prints the config/data/cache locations and exits.\n"
    "  --debug (or SHIGOKU_DEBUG=1) writes diagnostics: stderr in CLI mode,\n"
    "  ~/.local/share/shigoku/shigoku.log in the TUI.\n";

bool paste_line_usable(std::size_t n, std::string_view line, std::uint64_t cap) {
  const bool ends_nl = !line.empty() && line.back() == '\n';
  return n != 0 && (ends_nl || (static_cast<std::uint64_t>(line.size()) < cap));
}

std::string render_connect_result(const login::ConnectResult& result, std::string_view auth_path,
                                  bool paste) {
  using K = login::ConnectResult::Kind;
  switch (result.kind) {
    case K::Ok: {
      // AniList-supplied name reaches a raw stdout write with no ratatui
      // backstop; strip terminal-hostile bytes before it prints.
      std::string name = strip_controls(result.user_name);
      return "  ✓ signed in as " + name + ". Saved to " + std::string(auth_path) + ".\n";
    }
    case K::NoToken:
      return paste ? "  ✗ couldn't find an access_token in that; aborted.\n"
                   : "  ✗ the redirect carried no access_token.\n";
    case K::Rejected:
      return paste ? "  ✗ AniList rejected the token (invalid or expired); re-copy the whole "
                     "fragment and retry.\n"
                   : "  ✗ AniList rejected the token (invalid or expired); re-run to retry.\n";
    case K::NetworkError:
      return paste ? "  ✗ couldn't reach AniList to verify; check your connection and retry.\n"
                   : "  ✗ couldn't reach AniList to verify; re-run shortly.\n";
    case K::SaveFailed:
      return "  ✗ verified, but couldn't write " + std::string(auth_path) + ".\n";
    case K::BadState:
      return "  ✗ login state mismatch.\n";
    case K::Canceled:
      return "  login canceled.\n";
  }
  return {};
}

// Inline show/id lines before "… and N more".
static constexpr std::size_t kShowListCap = 12;

std::string render_sync_summary(const sync::SyncSummary& s, std::uint32_t mal_pushed) {
  using O = sync::SyncOutcome;
  std::string out;
  switch (s.outcome) {
    // Disabled is unreachable from the CLI (sync ignores the master switch, 06
    // §5.5); worded as not-connected to stay total.
    case O::NoToken:
    case O::Disabled:
      return "  not connected: run `shigoku login` first.\n";
    case O::Expired:
      return "  your AniList token has expired: run `shigoku login` to reconnect.\n";
    case O::NoUserId:
      return "  sync skipped: can't tell which AniList account this token is for; "
             "run `shigoku login` to reconnect.\n";
    case O::PullUnauthorized:
      return "  pull stopped: AniList rejected the token; run `shigoku login` to reconnect.\n";
    case O::PullRateLimited:
      return "  pull stopped: hit AniList's rate limit; run `shigoku sync` again shortly.\n";
    case O::Failed:
      return "  sync failed: couldn't reach the local library or AniList.\n";
    case O::Completed:
    case O::Unauthorized:
    case O::RateLimited:
      break;
  }

  if (s.pull_failed) {
    out += "  pull failed: couldn't fetch your AniList list; re-run with --debug for details.\n";
  } else {
    if (s.pulled.reconciled > 0) {
      out += "  pulled " + std::to_string(s.pulled.reconciled) + " update(s) from AniList.\n";
    } else if (s.pulled.conflicts == 0 && s.pulled.imported == 0) {
      out += "  already up to date: nothing to pull in.\n";
    }
    if (s.pulled.imported > 0) {
      out += "  imported " + std::to_string(s.pulled.imported) +
             " show(s) from AniList into your library.\n";
    }
    if (s.pulled.conflicts > 0) {
      out += "  (" + std::to_string(s.pulled.conflicts) +
             " show(s) kept your local status over AniList's; they'll push back up next sync.)\n";
    }
    if (!s.pulled.contended.empty()) {
      out += "  (" + std::to_string(s.pulled.contended.size()) +
             " show(s) changed mid-sync; left as-is, will reconcile next run.)\n";
    }
    if (!s.pulled.unmatched.empty()) {
      out += "  (" + std::to_string(s.pulled.unmatched.size()) +
             " AniList show(s) aren't in your local library yet; not imported.)\n";
      const std::size_t shown = std::min(s.pulled.unmatched.size(), kShowListCap);
      for (std::size_t i = 0; i < shown; ++i) {
        out += "      · anilist.co/anime/" + std::to_string(s.pulled.unmatched[i]) + "\n";
      }
      if (s.pulled.unmatched.size() > shown) {
        out += "      … and " + std::to_string(s.pulled.unmatched.size() - shown) + " more\n";
      }
    }
  }

  if (s.dirty == 0) {
    out += "  already up to date: nothing to push.\n";
  } else {
    out += "  pushed " + std::to_string(s.pushed) + " of " + std::to_string(s.dirty) +
           " change(s) to AniList.\n";
  }
  if (s.push_failed > 0) {
    out += "  " + std::to_string(s.push_failed) +
           " push(es) failed; re-run with --debug for details.\n";
  }
  if (s.push_skipped > 0) {
    out += "  (" + std::to_string(s.push_skipped) +
           " change(s) held back so they don't overwrite AniList; they'll merge next sync.)\n";
  }
  if (s.outcome == O::Unauthorized) {
    out += "  stopped: AniList rejected the token mid-run; run `shigoku login` to reconnect.\n";
  } else if (s.outcome == O::RateLimited) {
    out += "  stopped: hit AniList's rate limit; run `shigoku sync` again shortly to finish.\n";
  }
  // P31 §9.1 slice 4: the MAL mirror is silent unless it actually pushed
  // something — its own failures/skips just leave rows dirty for next run,
  // same as the TUI's on_sync_flushed toast logic.
  if (mal_pushed > 0) {
    out += "  pushed " + std::to_string(mal_pushed) + " change(s) to MyAnimeList.\n";
  }
  return out;
}

std::string render_search_hits(const std::vector<SearchHit>& hits, Translation translation) {
  std::string out = "\n  " + std::to_string(hits.size()) + " result(s):\n\n";
  for (std::size_t i = 0; i < hits.size(); ++i) {
    const SearchHit& h = hits[i];
    std::string title = strip_controls(h.title);
    const std::uint32_t per_track = translation == Translation::Dub ? h.eps_dub : h.eps_sub;
    const std::string num = rjust(i + 1, 2);
    if (per_track > 0) {
      out += "  " + num + ". " + title + "  ·  " + std::to_string(per_track) + " " +
             std::string(to_string(translation)) + " eps\n";
    } else if (h.total_episodes.has_value()) {
      out += "  " + num + ". " + title + "  ·  " + std::to_string(*h.total_episodes) + " eps\n";
    } else {
      out += "  " + num + ". " + title + "\n";
    }
  }
  return out;
}

std::string render_episode_list(const std::vector<std::string>& labels) {
  std::string out = "\n  " + std::to_string(labels.size()) + " episode(s):\n\n";
  for (std::size_t i = 0; i < labels.size(); ++i) {
    std::string label = strip_controls(labels[i]);
    out += "  " + rjust(i + 1, 3) + ". ep " + label + "\n";
  }
  return out;
}

PickResult classify_pick(std::string_view line, std::size_t max) {
  // Trim ASCII whitespace both ends.
  std::size_t b = 0, e = line.size();
  auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
  while (b < e && is_ws(line[b])) ++b;
  while (e > b && is_ws(line[e - 1])) --e;
  std::string_view t = line.substr(b, e - b);

  if (t.empty()) return {PickInput::Reprompt, 0};
  if (iequals(t, "q")) return {PickInput::Abort, 0};

  // Parse a non-negative decimal; any non-digit byte is NotNumber.
  unsigned long long n = 0;
  bool any = false;
  for (char c : t) {
    if (c < '0' || c > '9') return {PickInput::NotNumber, 0};
    any = true;
    n = n * 10 + static_cast<unsigned long long>(c - '0');
    if (n > 1'000'000'000ULL) return {PickInput::OutOfRange, 0};  // never a valid pick
  }
  if (!any) return {PickInput::NotNumber, 0};
  if (n >= 1 && n <= max) return {PickInput::Pick, static_cast<std::size_t>(n - 1)};
  return {PickInput::OutOfRange, 0};
}

std::string fetch_error_line(FetchStage stage, ProviderError::Kind kind, std::string_view provider) {
  const std::string p(provider);
  using K = ProviderError::Kind;
  switch (kind) {
    case K::Network:
      return "  ✗ can't reach " + p + ": check your network, then try again.\n";
    case K::Forbidden:
      return "  ✗ " + p + " is blocking the request (403/451); a VPN may get you through.\n";
    case K::Server:
      return "  ✗ " + p + "'s servers are down (5xx); wait a bit and retry.\n";
    // RateLimited (429) postdates the Rust FetchClass (P20 split); read it as a
    // transient down/back-off, the closest of the frozen copy set.
    case K::RateLimited:
      return "  ✗ " + p + " is rate-limiting the request; wait a bit and retry.\n";
    case K::Http:
      return "  ✗ " + p + " rejected the request; the site may be down or its recipe drifted.\n";
    case K::Decode:
      switch (stage) {
        case FetchStage::Search:
          return "  ✗ couldn't parse " + p + "'s search results; its format may have shifted.\n";
        case FetchStage::Episodes:
          return "  ✗ couldn't read " + p + "'s episode list; its format may have shifted.\n";
        case FetchStage::Resolve:
          return "  ✗ " + p +
                 " returned an unexpected stream payload; the protocol may have shifted.\n";
      }
      return {};
    case K::Unsupported:
      switch (stage) {
        case FetchStage::Search:
          return "  ✗ " + p + " can't search directly; use the TUI.\n";
        case FetchStage::Episodes:
          return "  ✗ " + p + " can't list episodes for this show.\n";
        case FetchStage::Resolve:
          return "  ✗ " + p + " can't provide a playable stream for this episode.\n";
      }
      return {};
  }
  return {};
}

bool quality_note_needed(std::optional<std::string_view> quality) {
  return quality.has_value() && !iequals(*quality, "best");
}

std::optional<std::string> provider_override_note(
    std::optional<std::pair<std::string_view, std::string_view>> asked,
    std::pair<std::string_view, std::string_view> chosen) {
  if (!asked.has_value()) return std::nullopt;
  const auto [asked_name, asked_display] = *asked;
  if (asked_name == chosen.first) return std::nullopt;
  return "  (note: " + std::string(asked_display) + " can't search, so this run uses " +
         std::string(chosen.second) + ".)";
}

std::string player_failure_line(PlayError::Kind kind, std::string_view provider,
                                std::optional<ProviderError::Kind> resolve_class) {
  using K = PlayError::Kind;
  switch (kind) {
    case K::MpvNotFound:
      return "  ✗ mpv isn't on your PATH; install mpv and try again.\n";
    case K::Exit:
    case K::Wait:
    case K::Spawn:
      return "  ✗ mpv exited badly (it closed early or couldn't play the stream).\n";
    case K::OpenFailed:
      return "  ✗ couldn't open the stream (the CDN may have blocked it); try again in a "
             "moment.\n";
    case K::Resolve:
      // The class was captured in the caller's resolve closure (player::play
      // collapses it into a detail string). nullopt = a non-provider resolve
      // stop (guard/proxy) — the safe-stop line.
      if (resolve_class.has_value()) {
        return fetch_error_line(FetchStage::Resolve, *resolve_class, provider);
      }
      return "  ✗ playback couldn't start safely; try again or pick a different episode.\n";
    // UnsafeUrl / UnsafeArg are the shigoku-only hardening stops (no zigoku
    // analog): read as a safe stop, like the Rust Internal class.
    case K::UnsafeUrl:
    case K::UnsafeArg:
      return "  ✗ playback couldn't start safely; try again or pick a different episode.\n";
  }
  return {};
}

}  // namespace shigoku::cli
