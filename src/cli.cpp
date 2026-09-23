// cli.cpp — see cli.hpp. Ported 1:1 from sabigoku src/cli.rs.

#include "cli.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

namespace shigoku::cli {

namespace {

constexpr Glyphs kUtf8Glyphs{"✗", "✓", "▶", "↺", "⚠", "⇣", "·", "…"};
constexpr Glyphs kAsciiGlyphs{"x", "+", ">", "<", "!", "v", "-", "..."};
bool g_ascii_glyphs = false;

// The usual line openers: two spaces, the glyph, one space.
std::string fail_lead() { return "  " + std::string(glyphs().fail) + " "; }
std::string ok_lead() { return "  " + std::string(glyphs().ok) + " "; }
// The "  ·  " between a title and its counts.
std::string sep() { return "  " + std::string(glyphs().dot) + "  "; }
std::string ellipsis() { return std::string(glyphs().ellipsis); }

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

// The flags the play-shaped commands share, scanned off argv; `words` are the
// positionals in order (the subcommand word itself, when `subcommand` names
// one, is not a word). `usage` = a flag without its value, a value that does
// not read, or a `--flag` nobody knows. Single-dash words other than the
// known short flags stay query text (only `--` prefixes are flags here).
struct Scan {
  std::vector<std::string_view> words;
  bool dub = false;
  std::optional<std::string> quality;
  std::optional<std::string> episode;
  std::optional<EpisodeRange> range;
  std::optional<std::uint32_t> show;
  std::optional<std::string> provider;
  bool usage = false;
};

bool is_flag(std::string_view a, std::string_view short_name, std::string_view long_name) {
  return a == short_name || a == long_name || starts_with(a, std::string(long_name) + "=");
}

// A flag's value: the `--flag=value` tail, or the next argv word (consumed).
// nullopt = there is none.
std::optional<std::string> take_value(const std::vector<std::string>& args, std::size_t& i,
                                      std::string_view a, std::string_view long_name) {
  const std::string eq = std::string(long_name) + "=";
  if (starts_with(a, eq)) return std::string(a.substr(eq.size()));
  if (i + 1 >= args.size()) return std::nullopt;
  return args[++i];
}

Scan scan_flags(const std::vector<std::string>& args, std::string_view subcommand) {
  Scan s;
  bool seen_subcommand = subcommand.empty();
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (!seen_subcommand && a == subcommand) {
      seen_subcommand = true;  // the subcommand word itself.
    } else if (a == "--dub") {
      s.dub = true;
    } else if (a == "--sub") {
      s.dub = false;
    } else if (a == "--debug" || a == "--ascii") {
      // Global, consumed: not a query word, not an unknown flag.
    } else if (is_flag(a, "-q", "--quality")) {
      auto v = take_value(args, i, a, "--quality");
      if (!v.has_value()) return Scan{{}, false, {}, {}, {}, {}, {}, true};
      s.quality = std::move(*v);
    } else if (is_flag(a, "-e", "--episode")) {
      auto v = take_value(args, i, a, "--episode");
      if (!v.has_value() || v->empty()) return Scan{{}, false, {}, {}, {}, {}, {}, true};
      s.episode = std::move(*v);
    } else if (is_flag(a, "-r", "--range")) {
      auto v = take_value(args, i, a, "--range");
      auto r = v.has_value() ? parse_range(*v) : std::nullopt;
      if (!r.has_value()) return Scan{{}, false, {}, {}, {}, {}, {}, true};
      s.range = std::move(*r);
    } else if (is_flag(a, "-S", "--show")) {
      auto v = take_value(args, i, a, "--show");
      const std::uint32_t n = v.has_value() ? ordinal_of(*v) : 0;
      if (n == 0) return Scan{{}, false, {}, {}, {}, {}, {}, true};
      s.show = n;
    } else if (is_flag(a, "-p", "--provider")) {
      auto v = take_value(args, i, a, "--provider");
      if (!v.has_value() || v->empty()) return Scan{{}, false, {}, {}, {}, {}, {}, true};
      s.provider = std::move(*v);
    } else if (starts_with(a, "--")) {
      return Scan{{}, false, {}, {}, {}, {}, {}, true};
    } else {
      s.words.push_back(a);
    }
  }
  // One episode or a span, never both.
  if (s.episode.has_value() && s.range.has_value()) s.usage = true;
  return s;
}

std::string join_words(const std::vector<std::string_view>& words) {
  std::string joined;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i > 0) joined += ' ';
    joined += std::string(words[i]);
  }
  return joined;
}

Command parse_query(const std::vector<std::string>& args) {
  Scan s = scan_flags(args, "");
  if (s.usage) return Command::usage();
  if (s.words.empty()) {
    // Flags alone open the interface, as ever — except the play flags, which
    // mean nothing there.
    if (s.quality.has_value() || s.episode.has_value() || s.range.has_value() ||
        s.show.has_value() || s.provider.has_value()) {
      return Command::usage();
    }
    return Command::tui();
  }
  PlayArgs a;
  a.query = join_words(s.words);
  a.dub = s.dub;
  a.quality = std::move(s.quality);
  a.episode = std::move(s.episode);
  a.range = std::move(s.range);
  a.show = s.show;
  a.provider = std::move(s.provider);
  return Command::play(std::move(a));
}

// `download <query…> [<ep>]`: the LAST positional is the
// episode label when 2+ positionals follow the subcommand word (or `-e`
// names it — not both), else the lone word is the query (interactive
// episode pick). No positionals at all, or a span, is usage.
Command parse_download(const std::vector<std::string>& args) {
  Scan s = scan_flags(args, "download");
  if (s.usage || s.words.empty() || s.range.has_value()) return Command::usage();
  std::optional<std::string> episode = std::move(s.episode);
  if (s.words.size() >= 2) {
    if (episode.has_value()) return Command::usage();
    episode = std::string(s.words.back());
    s.words.pop_back();
  }
  DownloadArgs a;
  a.query = join_words(s.words);
  a.episode = std::move(episode);
  a.dub = s.dub;
  a.quality = std::move(s.quality);
  a.show = s.show;
  a.provider = std::move(s.provider);
  return Command::download(std::move(a));
}

// `continue [<query…>]`: the positionals, if any, are the title filter. An
// episode or a span is usage — the library decides the episode.
Command parse_continue(const std::vector<std::string>& args) {
  Scan s = scan_flags(args, "continue");
  if (s.usage || s.episode.has_value() || s.range.has_value()) return Command::usage();
  ContinueArgs a;
  if (!s.words.empty()) a.query = join_words(s.words);
  a.dub = s.dub;
  a.quality = std::move(s.quality);
  a.show = s.show;
  a.provider = std::move(s.provider);
  return Command::continue_show(std::move(a));
}

}  // namespace

bool debug_flag(const std::vector<std::string>& args) {
  return std::any_of(args.begin(), args.end(), [](const std::string& a) { return a == "--debug"; });
}

bool ascii_flag(const std::vector<std::string>& args) {
  return std::any_of(args.begin(), args.end(), [](const std::string& a) { return a == "--ascii"; });
}

void set_ascii_glyphs(bool ascii) { g_ascii_glyphs = ascii; }
bool ascii_glyphs() { return g_ascii_glyphs; }
const Glyphs& glyphs() { return g_ascii_glyphs ? kAsciiGlyphs : kUtf8Glyphs; }

bool utf8_locale_name(std::string_view name) {
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return lower.find("utf-8") != std::string::npos || lower.find("utf8") != std::string::npos;
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
  if (is_subcommand(args, "continue")) return parse_continue(args);
  return parse_query(args);
}

const char* const kUsage =
    "  usage: shigoku <query> [--dub] [-q <quality>] [-S <n>] [-e <ep> | -r <a>-<b>] [-p <source>]\n"
    "         shigoku continue [<query>] [--dub] [-q <quality>] [-S <n>] [-p <source>]\n"
    "         shigoku download <query> [<ep>] [--dub] [-q <quality>] [-S <n>] [-p <source>]\n"
    "         shigoku login [--paste]\n"
    "         shigoku sync\n"
    "         shigoku update\n"
    "         shigoku --version\n"
    "\n"
    "    shigoku frieren\n"
    "    shigoku \"cowboy bebop\" --dub\n"
    "    shigoku frieren -S 1 -e 7      the first result, episode 7, no prompts\n"
    "    shigoku frieren -r 1-4         episodes 1 through 4, one after another\n"
    "    shigoku continue               pick up where you left off\n"
    "    shigoku download frieren 7\n"
    "    shigoku login\n"
    "\n"
    "  -q <quality>   best (the default), 1080, 720, 480, or worst.\n"
    "  -S <n>         take the nth search result instead of asking.\n"
    "  -e <ep>        play that episode: its label, or its position in the list.\n"
    "  -r <a>-<b>     play the span in order; leave <b> off for \"to the end\".\n"
    "  -p <source>    ask this source first (shigoku --paths lists them).\n"
    "  --version (or -V) prints the version and exits.\n"
    "  --paths prints the config/data/cache locations and exits.\n"
    "  --ascii keeps the output to plain ASCII (automatic when the locale isn't UTF-8).\n"
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
      return ok_lead() + "signed in as " + name + ". Saved to " + std::string(auth_path) + ".\n";
    }
    case K::NoToken:
      return fail_lead() + (paste ? "couldn't find an access_token in that; aborted.\n"
                                  : "the redirect carried no access_token.\n");
    case K::Rejected:
      return fail_lead() + (paste ? "AniList rejected the token (invalid or expired); re-copy "
                                    "the whole fragment and retry.\n"
                                  : "AniList rejected the token (invalid or expired); re-run to "
                                    "retry.\n");
    case K::NetworkError:
      return fail_lead() + (paste ? "couldn't reach AniList to verify; check your connection "
                                    "and retry.\n"
                                  : "couldn't reach AniList to verify; re-run shortly.\n");
    case K::SaveFailed:
      return fail_lead() + "verified, but couldn't write " + std::string(auth_path) + ".\n";
    case K::BadState:
      return fail_lead() + "login state mismatch.\n";
    case K::Canceled:
      return "  login canceled.\n";
  }
  return {};
}

// Inline show/id lines before "… and N more".
static constexpr std::size_t kShowListCap = 12;

namespace {

// The MAL mirror's own lines, after the AniList ones. Silent unless a MAL
// account was connected (`ran`).
std::string render_mal_pull(const MalPullCounts& m) {
  if (!m.ran) return {};
  if (m.unauthorized) {
    return "  MyAnimeList rejected the token; reconnect it under Settings.\n";
  }
  if (m.failed) {
    return "  pull failed: couldn't fetch your MyAnimeList list; re-run with --debug for "
           "details.\n";
  }
  std::string out;
  if (m.pulled > 0) {
    out += "  pulled " + std::to_string(m.pulled) + " update(s) from MyAnimeList.\n";
  }
  if (m.imported > 0) {
    out += "  imported " + std::to_string(m.imported) +
           " show(s) from MyAnimeList into your library.\n";
  }
  if (m.pulled == 0 && m.imported == 0) out += "  MyAnimeList list already up to date.\n";
  return out;
}

}  // namespace

std::string render_sync_summary(const sync::SyncSummary& s, std::uint32_t mal_pushed,
                                MalPullCounts mal_pull, std::uint32_t airing_refreshed) {
  using O = sync::SyncOutcome;
  std::string out;
  std::string anilist_terminal;
  switch (s.outcome) {
    // Disabled is unreachable from the CLI (sync ignores the master switch, 06
    // §5.5); worded as not-connected to stay total.
    case O::NoToken:
    case O::Disabled:
      anilist_terminal = "not connected: run `shigoku login` first.\n";
      break;
    case O::Expired:
      anilist_terminal = "your AniList token has expired: run `shigoku login` to reconnect.\n";
      break;
    case O::NoUserId:
      anilist_terminal =
          "sync skipped: can't tell which AniList account this token is for; "
          "run `shigoku login` to reconnect.\n";
      break;
    case O::PullUnauthorized:
      anilist_terminal =
          "pull stopped: AniList rejected the token; run `shigoku login` to reconnect.\n";
      break;
    case O::PullRateLimited:
      anilist_terminal =
          "pull stopped: hit AniList's rate limit; run `shigoku sync` again shortly.\n";
      break;
    case O::Failed:
      anilist_terminal = "sync failed: couldn't reach the local library or AniList.\n";
      break;
    case O::Completed:
    case O::Unauthorized:
    case O::RateLimited:
      break;
  }
  if (!anilist_terminal.empty()) {
    // A MAL-only account still gets its own report; the AniList line is then
    // one fact among two rather than the whole verdict.
    if (!mal_pull.ran) return "  " + anilist_terminal;
    return "  AniList: " + anilist_terminal + render_mal_pull(mal_pull);
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
        out += "      " + std::string(glyphs().dot) + " anilist.co/anime/" +
               std::to_string(s.pulled.unmatched[i]) + "\n";
      }
      if (s.pulled.unmatched.size() > shown) {
        out += "      " + ellipsis() + " and " +
               std::to_string(s.pulled.unmatched.size() - shown) + " more\n";
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
  out += render_mal_pull(mal_pull);
  if (airing_refreshed > 0) {
    out += "  refreshed airing times for " + std::to_string(airing_refreshed) + " show(s).\n";
  }
  return out;
}

// A title with its English/romaji alternate in parentheses when one is
// carried and it is not the same name again (ASCII case-folded), both
// stripped of terminal-hostile bytes.
std::string title_with_alt(std::string_view title, const std::optional<std::string>& alt) {
  std::string row = strip_controls(title);
  if (alt.has_value()) {
    const std::string other = strip_controls(*alt);
    if (!other.empty() && !iequals(other, row)) row += " (" + other + ")";
  }
  return row;
}

std::vector<std::string> search_hit_rows(const std::vector<SearchHit>& hits,
                                         Translation translation) {
  std::vector<std::string> rows;
  rows.reserve(hits.size());
  for (const SearchHit& h : hits) {
    std::string row = title_with_alt(h.title, h.title_english);
    const std::uint32_t per_track = translation == Translation::Dub ? h.eps_dub : h.eps_sub;
    if (per_track > 0) {
      row += sep() + std::to_string(per_track) + " " + std::string(to_string(translation)) +
             " eps";
    } else if (h.total_episodes.has_value()) {
      row += sep() + std::to_string(*h.total_episodes) + " eps";
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<std::string> episode_rows(const std::vector<std::string>& labels) {
  std::vector<std::string> rows;
  rows.reserve(labels.size());
  for (const std::string& label : labels) rows.push_back("ep " + strip_controls(label));
  return rows;
}

std::string numbered_rows(const std::vector<std::string>& rows, int width) {
  std::string out;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    out += "  " + rjust(i + 1, width) + ". " + rows[i] + "\n";
  }
  return out;
}

std::string render_search_hits(const std::vector<SearchHit>& hits, Translation translation) {
  return "\n  " + std::to_string(hits.size()) + " result(s):\n\n" +
         numbered_rows(search_hit_rows(hits, translation), 2);
}

std::string render_episode_list(const std::vector<std::string>& labels) {
  return "\n  " + std::to_string(labels.size()) + " episode(s):\n\n" +
         numbered_rows(episode_rows(labels), 3);
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
      return fail_lead() + "can't reach " + p + ": check your network, then try again.\n";
    case K::Forbidden:
      return fail_lead() + p + " is blocking the request (403/451); a VPN may get you through.\n";
    case K::Server:
      return fail_lead() + p + "'s servers are down (5xx); wait a bit and retry.\n";
    // RateLimited (429) postdates the Rust FetchClass (P20 split); read it as a
    // transient down/back-off, the closest of the frozen copy set.
    case K::RateLimited:
      return fail_lead() + p + " is rate-limiting the request; wait a bit and retry.\n";
    case K::Http:
      return fail_lead() + p + " rejected the request; the site may be down or its recipe drifted.\n";
    case K::Decode:
      switch (stage) {
        case FetchStage::Search:
          return fail_lead() + "couldn't parse " + p + "'s search results; its format may have shifted.\n";
        case FetchStage::Episodes:
          return fail_lead() + "couldn't read " + p + "'s episode list; its format may have shifted.\n";
        case FetchStage::Resolve:
          return fail_lead() + p +
                 " returned an unexpected stream payload; the protocol may have shifted.\n";
      }
      return {};
    case K::Unsupported:
      switch (stage) {
        case FetchStage::Search:
          return fail_lead() + p + " can't search directly; use the TUI.\n";
        case FetchStage::Episodes:
          return fail_lead() + p + " can't list episodes for this show.\n";
        case FetchStage::Resolve:
          return fail_lead() + p + " can't provide a playable stream for this episode.\n";
      }
      return {};
  }
  return {};
}

std::optional<std::string> quality_note(std::optional<std::string_view> quality) {
  if (!quality.has_value()) return std::nullopt;
  for (std::string_view known : {"best", "1080", "720", "480", "worst"}) {
    if (*quality == known) return std::nullopt;
  }
  return "  (note: no quality called \"" + strip_controls(*quality) + "\"; using best.)";
}

std::string unknown_source_note(std::string_view asked,
                                const std::vector<std::string_view>& names) {
  std::string list;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) list += ", ";
    list += std::string(names[i]);
  }
  return "  (note: no source called \"" + strip_controls(asked) +
         "\"; the sources that can search are " + list + ".)";
}

std::optional<EpisodeRange> parse_range(std::string_view text) {
  const std::size_t dash = text.find('-');
  if (dash == std::string_view::npos || dash == 0) return std::nullopt;
  const std::string_view rest = text.substr(dash + 1);
  if (rest.find('-') != std::string_view::npos) return std::nullopt;
  EpisodeRange r;
  r.first = std::string(text.substr(0, dash));
  if (!rest.empty()) r.last = std::string(rest);
  return r;
}

std::uint32_t ordinal_of(std::string_view text) {
  if (text.empty()) return 0;
  std::uint64_t n = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return 0;
    n = n * 10 + static_cast<std::uint64_t>(c - '0');
    if (n > std::numeric_limits<std::uint32_t>::max()) return 0;
  }
  return static_cast<std::uint32_t>(n);
}

std::optional<std::pair<std::size_t, std::size_t>> range_indices(
    const std::vector<std::string>& episodes, const EpisodeRange& range) {
  if (episodes.empty()) return std::nullopt;
  auto first = map_episode_index(episodes, range.first, ordinal_of(range.first));
  if (!first.has_value()) return std::nullopt;
  std::size_t last = episodes.size() - 1;
  if (range.last.has_value()) {
    auto l = map_episode_index(episodes, *range.last, ordinal_of(*range.last));
    if (!l.has_value()) return std::nullopt;
    last = *l;
  }
  if (*first > last) return std::nullopt;
  return std::make_pair(*first, last);
}

PostPlayMenu post_play_menu(std::size_t index, const std::vector<std::string>& episodes) {
  PostPlayMenu m;
  auto add = [&m](std::string row, PostPlay action) {
    m.rows.push_back(std::move(row));
    m.actions.push_back(action);
  };
  if (index + 1 < episodes.size()) {
    add("next" + sep() + "ep " + strip_controls(episodes[index + 1]), PostPlay::Next);
  }
  if (index < episodes.size()) {
    add("replay" + sep() + "ep " + strip_controls(episodes[index]), PostPlay::Replay);
  }
  if (index > 0 && index - 1 < episodes.size()) {
    add("previous" + sep() + "ep " + strip_controls(episodes[index - 1]), PostPlay::Previous);
  }
  add("quit", PostPlay::Quit);
  return m;
}

std::vector<std::string> history_rows(const std::vector<Show>& shows) {
  std::vector<std::string> rows;
  rows.reserve(shows.size());
  for (const Show& s : shows) {
    std::string row = title_with_alt(s.enrichment.title_romaji, s.enrichment.title_english);
    if (s.progress > 0) {
      row += sep() + "ep " + std::to_string(s.progress);
      if (s.enrichment.total_episodes.has_value() && *s.enrichment.total_episodes > 0) {
        row += " of " + std::to_string(*s.enrichment.total_episodes);
      }
    } else {
      row += sep() + "not started";
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<std::size_t> history_matches(const std::vector<Show>& shows, std::string_view query) {
  auto fold = [](std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
  };
  const std::string q = fold(query);
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < shows.size(); ++i) {
    const Enrichment& e = shows[i].enrichment;
    const bool hit = q.empty() || fold(e.title_romaji).find(q) != std::string::npos ||
                     (e.title_english.has_value() && fold(*e.title_english).find(q) != std::string::npos) ||
                     (e.title_native.has_value() && fold(*e.title_native).find(q) != std::string::npos);
    if (hit) out.push_back(i);
  }
  return out;
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

std::string search_walk_note(std::string_view skipped, std::optional<ProviderError::Kind> failure,
                             std::string_view next) {
  std::string why;
  if (!failure.has_value()) {
    why = "no results on " + std::string(skipped);
  } else {
    using K = ProviderError::Kind;
    why = std::string(skipped);
    switch (*failure) {
      case K::Network:     why += " is unreachable"; break;
      case K::Forbidden:   why += " is blocking us"; break;
      case K::Server:      why += " is down"; break;
      case K::Http:        why += " rejected the search"; break;
      case K::Decode:      why += " sent a malformed answer"; break;
      case K::Unsupported: why += " can't search"; break;
      case K::RateLimited: why += " is rate-limiting us"; break;
    }
  }
  return "  (" + why + "; trying " + std::string(next) + ellipsis() + ")\n";
}

std::string player_failure_line(PlayError::Kind kind, std::string_view provider,
                                std::optional<ProviderError::Kind> resolve_class) {
  using K = PlayError::Kind;
  switch (kind) {
    case K::MpvNotFound:
      return fail_lead() + "mpv isn't on your PATH; install mpv and try again.\n";
    case K::Exit:
    case K::Wait:
    case K::Spawn:
      return fail_lead() + "mpv exited badly (it closed early or couldn't play the stream).\n";
    case K::OpenFailed:
      return fail_lead() + "couldn't open the stream (the CDN may have blocked it); try again "
             "in a moment.\n";
    case K::Resolve:
      // The class was captured in the caller's resolve closure (player::play
      // collapses it into a detail string). nullopt = a non-provider resolve
      // stop (guard/proxy) — the safe-stop line.
      if (resolve_class.has_value()) {
        return fetch_error_line(FetchStage::Resolve, *resolve_class, provider);
      }
      return fail_lead() + "playback couldn't start safely; try again or pick a different "
             "episode.\n";
    // UnsafeUrl / UnsafeArg are the shigoku-only hardening stops (no zigoku
    // analog): read as a safe stop, like the Rust Internal class.
    case K::UnsafeUrl:
    case K::UnsafeArg:
      return fail_lead() + "playback couldn't start safely; try again or pick a different "
             "episode.\n";
  }
  return {};
}

}  // namespace shigoku::cli
