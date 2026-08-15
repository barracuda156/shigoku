// main.cpp — CLI dispatch, then, for the no-subcommand path, the TUI
// bootstrap (paths -> config -> http client -> provider registry -> store ->
// tui::run). Ported from sabigoku src/main.rs.
//
// Dispatch (cli::parse) runs before any bootstrap; Version and Usage never
// touch disk. Exit law: login/sync/update/usage/version exit 0
// unconditionally; the play path is the one command whose failure exits 1.
// Boot failures (no HOME, tui::run error) are a separate pre-existing 1.
// Headless commands never touch the terminal layer (no tui::run, no term
// init) — the tty-less contract for headless mode.

#include <ctime>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "anibd.hpp"
#include "anidbapp.hpp"
#include "anilibria.hpp"
#include "anilist.hpp"
#include "auth.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "domain.hpp"
#include "http.hpp"
#include "login.hpp"
#include "loopback.hpp"
#include "mal_mirror.hpp"
#include "megaplay.hpp"
#include "paths.hpp"
#include "play_cli.hpp"
#include "provider.hpp"
#include "semver.hpp"
#include "senshi.hpp"
#include "store.hpp"
#include "sync.hpp"
#include "updatecheck.hpp"
#include "tui/app.hpp"
#include "tui/covers.hpp"

namespace {

using namespace shigoku;

// Wall-clock seconds; the CLI paths pass this to every `now`-taking seam
// (matches main.rs unix_now, and app.cpp's now_epoch_secs).
std::int64_t unix_now() { return static_cast<std::int64_t>(::time(nullptr)); }

void flush_stdout() { std::fflush(stdout); }

// $HOME-collapse for display rows; a plain display helper.
std::string tilde_path(const std::string& path) {
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0' && path.rfind(home, 0) == 0) {
    return "~" + path.substr(std::string(home).size());
  }
  return path;
}

// Paste line cap: an implicit-grant JWT is ~1 KB; past 8 KB it is not one.
constexpr std::uint64_t kPasteCap = 8192;
// Numbered-pick read cap: a pick is a short number or `q`.
constexpr std::uint64_t kPickCap = 256;
// Reprompt ceiling: a human never fumbles this many times, but a stdin flood
// would otherwise reprompt without end. Bounds the loop to abort instead.
constexpr int kMaxPickAttempts = 1000;

// Build the live provider registry (same lineup as run_tui below, N=5). The
// CLI play path and the TUI boot build from the same list so the lineup never
// forks. Returns nullopt if any provider client fails to init.
std::optional<ProviderRegistry> build_registry() {
  auto megaplay_provider = megaplay::MegaPlay::create();
  auto senshi = senshi::Senshi::create();
  auto anibd_provider = anibd::AniBd::create();
  auto anidbapp_provider = anidbapp::AniDbApp::create();
  auto anilibria_provider = anilibria::AniLibria::create();
  if (!megaplay_provider || !senshi || !anibd_provider || !anidbapp_provider ||
      !anilibria_provider) {
    return std::nullopt;
  }
  std::vector<std::unique_ptr<StreamProvider>> providers;
  providers.push_back(std::make_unique<megaplay::MegaPlay>(std::move(*megaplay_provider)));
  providers.push_back(std::make_unique<senshi::Senshi>(std::move(*senshi)));
  providers.push_back(std::make_unique<anibd::AniBd>(std::move(*anibd_provider)));
  providers.push_back(std::make_unique<anidbapp::AniDbApp>(std::move(*anidbapp_provider)));
  providers.push_back(std::make_unique<anilibria::AniLibria>(std::move(*anilibria_provider)));
  return ProviderRegistry(std::move(providers));
}

// ── login ─────────────────────────────────────────────────────────────────

// One stdin line, capped. None on EOF, read error, or an overlong paste;
// cli::paste_line_usable owns the accept/abort rule.
std::optional<std::string> read_paste_line() {
  std::string line;
  line.reserve(128);
  std::uint64_t read = 0;
  int c;
  while (read < kPasteCap && (c = std::getchar()) != EOF) {
    ++read;
    line.push_back(static_cast<char>(c));
    if (c == '\n') break;
  }
  const std::size_t n = line.size();
  if (!cli::paste_line_usable(n, line, kPasteCap)) return std::nullopt;
  return line;
}

// Trim trailing/leading ASCII whitespace (paste uses login::normalize_paste on
// the trimmed value, like main.rs `line.trim()`).
std::string trim(std::string_view s) {
  std::size_t b = 0, e = s.size();
  auto ws = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
  while (b < e && ws(s[b])) ++b;
  while (e > b && ws(s[e - 1])) --e;
  return std::string(s.substr(b, e - b));
}

bool paste_login(const login::Verifier& verifier, const std::string& auth_path, std::int64_t now) {
  std::printf("Connect your AniList account (OAuth Implicit Grant).\n\n");
  std::printf("1. Open this URL in a browser and approve:\n\n");
  std::printf("   %s\n\n", login::authorize_url_bare().c_str());
  std::printf(
      "2. You land on http://localhost:%u/…; paste that whole URL below. If nothing\n",
      static_cast<unsigned>(login::kLoopbackPort));
  std::printf("   is listening there the page won't load; that's fine, the token is in\n");
  std::printf("   the address bar. Select the ENTIRE URL (the token has three dot-\n");
  std::printf("   separated parts; a double-click grabs only the first).\n\n");
  std::printf("redirect URL> ");
  flush_stdout();

  auto line = read_paste_line();
  if (!line.has_value()) {
    std::printf("\n  no input (or a paste past 8 KB); aborted.\n");
    return false;
  }

  std::printf("\nverifying…\n");
  flush_stdout();
  const std::string raw = login::normalize_paste(trim(*line));
  const login::ConnectResult result = login::complete_login(raw, verifier, auth_path, now);
  std::printf("%s", cli::render_connect_result(result, auth_path, /*paste=*/true).c_str());
  return result.kind == login::ConnectResult::Kind::Ok;
}

bool loopback_login(loopback::Loopback& lp, const login::Verifier& verifier,
                    const std::string& auth_path, std::int64_t now) {
  const std::string url = lp.authorize_url();
  std::printf("Opening your browser to approve AniList access…\n");
  std::printf("  If it doesn't open, visit this URL yourself:\n");
  std::printf("  %s\n\n", url.c_str());
  std::printf("Waiting for the redirect on http://localhost:%u/ …\n",
              static_cast<unsigned>(lp.port()));
  std::printf("  (Ctrl-C to cancel, or re-run `shigoku login --paste` for manual entry.)\n\n");
  flush_stdout();
  login::open_browser(url);

  bool warned = false;
  const login::ConnectResult result = lp.serve(verifier, auth_path, now, [&warned] {
    if (!warned) {
      std::printf("  ⚠ ignoring callback(s) with a bad state (stray or forged requests).\n");
      flush_stdout();
      warned = true;
    }
  });
  std::printf("%s", cli::render_connect_result(result, auth_path, /*paste=*/false).c_str());
  return result.kind == login::ConnectResult::Kind::Ok;
}

int run_sync_cli();  // fwd: a successful login chains a bootstrap sync.

// `shigoku login`: loopback OAuth by default, `--paste` or a bind failure
// falls back to manual paste. A persisted token (and only that) chains the
// bootstrap sync. Exits 0 on every branch.
int run_login_cli(bool paste) {
  auto paths = resolve_paths();
  if (!paths.has_value()) {
    std::printf("  login: couldn't locate a config directory\n");
    return 0;
  }
  ensure_dirs(*paths);
  const std::string auth_path = auth_file_path(*paths);

  auto client = http::Client::create();
  if (!client.has_value()) {
    std::printf("  login: couldn't set up the AniList client\n");
    return 0;
  }
  const login::Verifier verifier = login::make_anilist_verifier(*client);

  const Auth existing = Auth::load(auth_path);
  if (existing.anilist.bearer().has_value()) {
    std::printf("Already signed in as %s; re-running replaces it.\n\n",
                existing.anilist.user_name.c_str());
  }

  const std::int64_t now = unix_now();
  bool signed_in;
  if (paste) {
    signed_in = paste_login(verifier, auth_path, now);
  } else {
    auto lp = loopback::Loopback::start();
    if (lp.has_value()) {
      signed_in = loopback_login(*lp, verifier, auth_path, now);
    } else {
      std::printf("  (couldn't start the loopback listener; falling back to paste)\n\n");
      signed_in = paste_login(verifier, auth_path, now);
    }
  }

  if (signed_in) {
    std::printf("\n");
    return run_sync_cli();
  }
  return 0;
}

// ── sync ──────────────────────────────────────────────────────────────────

// `shigoku sync`: pull then push, gated on token present + unexpired only.
// Ignores anilist_sync_enabled on purpose. Exits 0 on every path, setup
// failures included.
int run_sync_cli() {
  auto paths = resolve_paths();
  if (!paths.has_value()) {
    std::printf("  sync: no local library to sync\n");
    return 0;
  }
  ensure_dirs(*paths);
  const std::string db_path = paths->data + "/shigoku.db";
  auto store = Store::open(db_path);
  if (!store.has_value()) {
    std::printf("  sync: couldn't open the local library (%s)\n", store.error().detail.c_str());
    return 0;
  }
  const Auth auth = Auth::load(auth_file_path(*paths));
  auto client = http::Client::create();
  if (!client.has_value()) {
    std::printf("  sync: couldn't set up the AniList client\n");
    return 0;
  }

  const std::int64_t now = unix_now();
  if (auth.anilist.bearer().has_value() && !auth.anilist.is_expired(now)) {
    // Announce before the paced push; flush so it lands pre-network.
    std::printf("  syncing with AniList, this can take a moment…\n");
    flush_stdout();
  }
  const sync::HttpAniListSync ani(*client);
  // enabled=true: the CLI ignores the master switch. pull_only=false.
  auto summary = sync::run_sync(ani, auth, *store, now, /*enabled=*/true, /*pull_only=*/false,
                                sync::thread_sleep);

  // The MAL mirror push rides this same CLI invocation, with no drain rights
  // of its own (A1) — same rationale as the TUI worker wiring (app.cpp's
  // spawn_sync). Runs regardless of the AniList summary's outcome: MAL is a
  // wholly separate push-only client relationship (mal_mirror.hpp).
  std::uint32_t mal_pushed = 0;
  if (auth.mal.bearer().has_value()) {
    const mal_mirror::HttpMalMirrorClient mal_client(*client);
    auto mirrored = mal_mirror::push_mirror(mal_client, auth.mal, *store, /*enabled=*/true);
    if (mirrored.has_value()) mal_pushed = mirrored->pushed;
  }

  if (summary.has_value()) {
    std::printf("%s", cli::render_sync_summary(*summary, mal_pushed).c_str());
  } else {
    std::printf(
        "  sync failed: couldn't update the local library; re-run with --debug for details.\n");
  }
  return 0;
}

// ── update ──────────────────────────────────────────────────────────────────

// `shigoku update`: check-only, never self-updating. The legacy target's
// binary is package-owned (MacPorts) and shigoku ships no standalone-install
// path to self-update into — every install method's real answer is "use
// your package manager", so there is nothing left for a run() to *do*
// beyond reporting. Bypasses the 1h cache (updatecheck::latest_fresh):
// acting on an hour-old answer risks reporting "up to date" right after a
// release. Exits 0 on every path per the exit law.
int run_update_cli() {
  auto paths = resolve_paths();
  const std::string cache_dir = paths.has_value() ? paths->cache : std::string();
  if (paths.has_value()) ensure_dirs(*paths);

  auto client = http::Client::create();
  if (!client.has_value()) {
    std::printf("  couldn't set up the network client to check for updates.\n");
  } else {
    auto latest = updatecheck::latest_fresh(*client, cache_dir, unix_now());
    if (!latest.has_value()) {
      std::printf("  couldn't reach GitHub to check the latest release.\n");
    } else if (semver::is_newer(*latest, SHIGOKU_VERSION)) {
      std::printf("update available: v%s -> %s\n", SHIGOKU_VERSION, latest->c_str());
    } else {
      std::printf("shigoku v%s is already the latest release.\n", SHIGOKU_VERSION);
      return 0;
    }
  }
  std::printf(
      "\nshigoku doesn't self-update; it's installed via your package manager.\n"
      "  MacPorts:  sudo port upgrade shigoku\n"
      "  otherwise: rebuild from https://github.com/vantroy/shigoku\n");
  return 0;
}

// ── play ────────────────────────────────────────────────────────────────────

// Numbered stdin pick loop: prints the prompt, reads one capped line, and lets
// cli::classify_pick own the accept/reprompt/abort rule. nullopt on `q`, EOF,
// or an overlong line.
std::optional<std::size_t> prompt_pick(const char* prompt, std::size_t max) {
  for (int attempt = 0; attempt < kMaxPickAttempts; ++attempt) {
    std::printf("%s", prompt);
    flush_stdout();
    std::string line;
    line.reserve(16);
    std::uint64_t read = 0;
    int c;
    bool eof = true;
    while (read < kPickCap && (c = std::getchar()) != EOF) {
      eof = false;
      ++read;
      line.push_back(static_cast<char>(c));
      if (c == '\n') break;
    }
    // EOF with nothing read: same as an explicit `q`.
    if (eof) return std::nullopt;
    // A cap-length read with no newline is a truncated flood, not a pick.
    if (read == kPickCap && (line.empty() || line.back() != '\n')) return std::nullopt;
    const cli::PickResult r = cli::classify_pick(line, max);
    switch (r.kind) {
      case cli::PickInput::Pick:
        return r.index;
      case cli::PickInput::Abort:
        return std::nullopt;
      case cli::PickInput::Reprompt:
        break;
      case cli::PickInput::NotNumber:
        std::printf("  ? enter a number 1-%zu (or q)\n", max);
        break;
      case cli::PickInput::OutOfRange:
        std::printf("  ? out of range: pick 1-%zu\n", max);
        break;
    }
  }
  // Ran out of patience: a flood, not a user. Abort like EOF.
  return std::nullopt;
}

// Effective download root: the config value, or <data>/downloads when blank
// — resolved here once so the TUI and both subcommands agree.
std::string resolve_download_dir(const Config& config, const Paths& paths) {
  if (!config.download_dir.empty()) return config.download_dir;
  return paths.data + "/downloads";
}

// `shigoku <query>`: the one command whose failure exits nonzero. One
// provider for the whole run, chosen for search capability (ROD-491);
// interactive stdin picks; store is best-effort and degrades.
int run_play_cli(const cli::PlayArgs& args) {
  auto paths = resolve_paths();
  if (!paths.has_value()) {
    std::fprintf(stderr, "shigoku: could not resolve HOME/XDG dirs\n");
    return 1;
  }
  ensure_dirs(*paths);
  const Config config = Config::load(config_file_path(*paths));

  // Translation is flag-only (parity): --dub/--sub decide it, config's
  // translation never reaches the query path. Not a bug — don't "fix" it.
  const Translation translation = args.dub ? Translation::Dub : Translation::Sub;

  // --quality is parsed but inert: warn once on a non-default value so the
  // flag never looks silently honored. default_quality drives resolve.
  if (cli::quality_note_needed(args.quality.has_value()
                                   ? std::optional<std::string_view>(*args.quality)
                                   : std::nullopt)) {
    std::printf(
        "  (note: --quality isn't wired up yet; playback uses the highest direct stream "
        "available.)\n");
  }

  // Store is best-effort: a library that won't open degrades to play-only and
  // never blocks playback.
  std::optional<Store> store_holder = std::nullopt;
  auto store_r = Store::open(paths->data + "/shigoku.db");
  if (store_r.has_value()) {
    store_holder.emplace(std::move(*store_r));
  } else {
    std::printf("  (couldn't open your library; continuing without history.)\n");
  }
  Store* store = store_holder.has_value() ? &*store_holder : nullptr;

  auto registry = build_registry();
  if (!registry.has_value()) {
    std::printf("  ✗ couldn't set up a provider client.\n");
    return 1;
  }

  // Search-capable, not merely preferred (ROD-491). Whichever answers owns the
  // whole run.
  const std::string_view pref = config.preferred_provider;
  const StreamProvider* provider = registry->preferred_searchable(pref);
  if (provider == nullptr) {
    std::printf("  ✗ no configured source can search.\n");
    return 1;
  }
  const StreamProvider* asked = registry->by_name(pref);
  auto note = cli::provider_override_note(
      asked != nullptr
          ? std::optional<std::pair<std::string_view, std::string_view>>({asked->name(),
                                                                          asked->display_name()})
          : std::nullopt,
      {provider->name(), provider->display_name()});
  if (note.has_value()) std::printf("%s\n", note->c_str());

  const cli_play::PickFn pick = [](const char* prompt, std::size_t max) {
    return prompt_pick(prompt, max);
  };
  return cli_play::play_flow(*provider, pick, translation, config, paths->cache, paths->runtime,
                             resolve_download_dir(config, *paths), store, args);
}

// `shigoku download <query> [<ep>]`: the play CLI's spine with download_link
// at the end. Same bootstrap as run_play_cli.
int run_download_cli(const cli::DownloadArgs& args) {
  auto paths = resolve_paths();
  if (!paths.has_value()) {
    std::fprintf(stderr, "shigoku: could not resolve HOME/XDG dirs\n");
    return 1;
  }
  ensure_dirs(*paths);
  const Config config = Config::load(config_file_path(*paths));
  const Translation translation = args.dub ? Translation::Dub : Translation::Sub;

  std::optional<Store> store_holder = std::nullopt;
  auto store_r = Store::open(paths->data + "/shigoku.db");
  if (store_r.has_value()) {
    store_holder.emplace(std::move(*store_r));
  } else {
    std::printf("  (couldn't open your library; continuing without history.)\n");
  }
  Store* store = store_holder.has_value() ? &*store_holder : nullptr;

  auto registry = build_registry();
  if (!registry.has_value()) {
    std::printf("  ✗ couldn't set up a provider client.\n");
    return 1;
  }
  const std::string_view pref = config.preferred_provider;
  const StreamProvider* provider = registry->preferred_searchable(pref);
  if (provider == nullptr) {
    std::printf("  ✗ no configured source can search.\n");
    return 1;
  }
  const StreamProvider* asked = registry->by_name(pref);
  auto note = cli::provider_override_note(
      asked != nullptr
          ? std::optional<std::pair<std::string_view, std::string_view>>({asked->name(),
                                                                          asked->display_name()})
          : std::nullopt,
      {provider->name(), provider->display_name()});
  if (note.has_value()) std::printf("%s\n", note->c_str());

  const cli_play::PickFn pick = [](const char* prompt, std::size_t max) {
    return prompt_pick(prompt, max);
  };
  return cli_play::download_flow(*provider, pick, translation, config,
                                 resolve_download_dir(config, *paths), store, args);
}

// ── paths / tui ─────────────────────────────────────────────────────────────

int print_paths() {
  auto paths = resolve_paths();
  if (!paths.has_value()) {
    std::fprintf(stderr, "shigoku: could not resolve HOME/XDG dirs\n");
    return 1;
  }
  ensure_dirs(*paths);
  const Config config = Config::load(config_file_path(*paths));
  std::printf("shigoku paths\n");
  std::printf("  config   %s\n", tilde_path(config_file_path(*paths)).c_str());
  std::printf("  db       %s\n", tilde_path(paths->data + "/shigoku.db").c_str());
  std::printf("  cache    %s\n", tilde_path(paths->cache).c_str());
  std::printf("  runtime  %s\n", tilde_path(paths->runtime).c_str());
  std::printf("  mpv      %s\n", config.mpv_path.c_str());
  std::printf("  palette  %s\n", config.palette.c_str());
  return 0;
}

// The TUI bootstrap. Only the no-subcommand path reaches here.
int run_tui() {
  auto paths = resolve_paths();
  if (!paths.has_value()) {
    std::fprintf(stderr, "shigoku: could not resolve HOME/XDG dirs\n");
    return 1;
  }
  ensure_dirs(*paths);

  const std::string config_path = config_file_path(*paths);
  Config config = Config::load(config_path);
  const std::string auth_path = auth_file_path(*paths);  // loaded in run().

  auto client = http::Client::create();
  if (!client.has_value()) {
    std::fprintf(stderr, "shigoku: http client init failed\n");
    return 1;
  }

  auto registry = build_registry();
  if (!registry.has_value()) {
    std::fprintf(stderr, "shigoku: provider init failed\n");
    return 1;
  }

  tui::AppDeps deps;
  deps.registry = &*registry;
  deps.http = &*client;
  // AniList is the only Browse catalog client. deps.search is an indirection
  // app.cpp calls through so an offline test can inject canned rows;
  // production always wraps the real anilist::search. has_next rides along
  // for the Browse load-more.
  deps.search = [&client](std::string_view query, std::uint32_t page)
      -> Result<tui::SearchPage, ProviderError> {
    auto p = anilist::search(*client, query, page);
    if (!p.has_value()) return err(p.error());
    std::vector<CatalogRow> rows;
    rows.reserve(p->entries.size());
    for (auto& e : p->entries) rows.push_back(CatalogRow{std::move(e)});
    return tui::SearchPage{std::move(rows), p->has_next};
  };
  deps.discover = [&client](DiscoverAxis axis, std::uint32_t page,
                            const DiscoverFilters& filters)
      -> Result<tui::DiscoverPage, ProviderError> {
    auto p = anilist::discover(*client, axis, page, filters);
    if (!p.has_value()) return err(p.error());
    std::vector<CatalogRow> rows;
    rows.reserve(p->entries.size());
    for (auto& e : p->entries) rows.push_back(CatalogRow{std::move(e)});
    return tui::DiscoverPage{std::move(rows), p->has_next};
  };
  deps.enrich = [&client](std::int64_t anilist_id)
      -> Result<std::optional<Enrichment>, ProviderError> {
    return anilist::enrich(*client, anilist_id);
  };
  deps.char_recs = [&client](std::int64_t anilist_id)
      -> Result<std::optional<CharactersAndRecommendations>, ProviderError> {
    return anilist::characters_and_recommendations(*client, anilist_id);
  };
  deps.genre_collection = [&client]() -> Result<std::vector<std::string>, ProviderError> {
    return anilist::genre_collection(*client);
  };
  deps.mpv_path = config.mpv_path;
  deps.socket_dir = paths->runtime;
  deps.covers_dir_display = tilde_path(paths->cache + "/covers");
  deps.aniskip_cache_dir = paths->cache + "/aniskip";
  deps.update_cache_dir = paths->cache;
  deps.download_dir = resolve_download_dir(config, *paths);
  deps.ffmpeg_path = config.ffmpeg_path;

  // Cover pipeline.
  tui::Covers covers(tui::make_http_fetch(*client), paths->cache + "/covers");
  deps.covers = &covers;

  // Store (A7): an open failure is not fatal to browsing — degrade to the
  // no-persistence path with a stderr note.
  const std::string db_path = paths->data + "/shigoku.db";
  auto store = Store::open(db_path);
  if (store.has_value()) {
    deps.store = &*store;
  } else {
    std::fprintf(stderr, "shigoku: store open failed (%s); running without resume\n",
                 store.error().detail.c_str());
  }

  return tui::run(/*demo_mode=*/false, &deps, &config, config_path, auth_path, db_path,
                  SHIGOKU_VERSION);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  // --debug is read but shigoku's diagnostic sink is SHIGOKU_DEBUG_LOG-gated
  // (debug_log.hpp), not a runtime-armed logger, so the flag is consumed by the
  // parser and otherwise inert here. Kept in the parse contract for parity and
  // for a future stderr sink.
  const cli::Command cmd = cli::parse(args);
  using K = cli::Command::Kind;
  switch (cmd.kind) {
    case K::Version:
      std::printf("shigoku v%s\n", SHIGOKU_VERSION);
      return 0;
    case K::Paths:
      return print_paths();
    case K::Login:
      return run_login_cli(cmd.paste);
    case K::Sync:
      return run_sync_cli();
    case K::Update:
      return run_update_cli();
    case K::Usage:
      std::printf("%s", cli::kUsage);
      return 0;
    case K::Play:
      return run_play_cli(cmd.play_args);
    case K::Download:
      return run_download_cli(cmd.download_args);
    case K::Tui:
      return run_tui();
  }
  return 0;
}
