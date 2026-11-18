// sync_cycle_tests.cpp — the whole tracker run over scripted seams: the
// order its halves go in, the renewed MAL pair's path through it, and what a
// signed-out mirror leaves untouched. Offline: both clients are fakes that
// write their calls to one log.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "../src/auth.hpp"
#include "../src/domain.hpp"
#include "../src/store.hpp"
#include "../src/sync_cycle.hpp"

using namespace shigoku;

namespace {

using Log = std::vector<std::string>;

class LogAni final : public sync::AniListSync {
 public:
  explicit LogAni(Log& log) : log_(log) {}

  [[nodiscard]] Result<std::vector<RemoteEntry>, ProviderError> fetch_list(
      std::string_view, std::int64_t, ScoreFormat) const override {
    log_.push_back("anilist list");
    return std::vector<RemoteEntry>{};
  }
  [[nodiscard]] Result<std::optional<ListEntry>, ProviderError> fetch_entry(
      std::string_view, std::int64_t, std::int64_t, ScoreFormat) const override {
    log_.push_back("anilist entry");
    return std::optional<ListEntry>{};
  }
  [[nodiscard]] Result<std::int64_t, ProviderError> save_entry(
      std::string_view, std::int64_t, ListStatus, std::uint32_t, std::uint32_t,
      ScoreFormat) const override {
    log_.push_back("anilist save");
    return std::int64_t{1};
  }

 private:
  Log& log_;
};

// Every MAL call is logged with the token it was made under.
class LogMal final : public mal_mirror::MalMirrorClient {
 public:
  explicit LogMal(Log& log) : log_(log) {}

  [[nodiscard]] Result<std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>,
                      ProviderError>
  fetch_entry(std::string_view token, std::int64_t) const override {
    log_.push_back("mal entry " + std::string(token));
    return std::optional<std::tuple<ListStatus, std::uint32_t, std::uint32_t>>{};
  }
  [[nodiscard]] Result<Unit, ProviderError> save_entry(
      std::string_view token, std::int64_t, ListStatus, std::uint32_t,
      std::optional<std::uint32_t>) const override {
    log_.push_back("mal save " + std::string(token));
    return Unit{};
  }
  [[nodiscard]] Result<std::vector<mal_catalog::UserListEntry>, ProviderError> fetch_list(
      std::string_view token) const override {
    log_.push_back("mal list " + std::string(token));
    return std::vector<mal_catalog::UserListEntry>{};
  }

 private:
  Log& log_;
};

constexpr std::int64_t kNow = 1000;

Auth signed_in_both() {
  Auth a;
  a.anilist.access_token = "anilist-token";
  a.anilist.user_id = 7;
  a.mal.access_token = "mal-token";
  return a;
}

MalAuth renewed_pair() {
  MalAuth a;
  a.access_token = "mal-token-renewed";
  a.refresh_token = "mal-refresh-renewed";
  a.expires_at = kNow + 2'678'400;
  return a;
}

// A library row both trackers have something to say about, with no airing
// stamp yet.
void add_show(Store& store) {
  Enrichment e;
  e.anilist_id = 21;
  e.mal_id = 5;
  e.title_romaji = "Show";
  e.total_episodes = 12;
  REQUIRE(store.add_to_library(e, 10).has_value());
}

sync::AiringFetch logging_airing(Log& log) {
  return [&log](const std::vector<std::int64_t>&, const std::vector<std::int64_t>&)
             -> Result<std::vector<anilist::AiringRow>, ProviderError> {
    log.push_back("airing");
    return std::vector<anilist::AiringRow>{};
  };
}

void no_sleep(std::chrono::milliseconds) {}

sync::CycleOptions options(std::string auth_path = {}) {
  sync::CycleOptions opt;
  opt.now = kNow;
  opt.auth_path = std::move(auth_path);
  return opt;
}

std::ptrdiff_t first_with(const Log& log, std::string_view prefix) {
  const auto it = std::find_if(log.begin(), log.end(), [&](const std::string& s) {
    return s.rfind(prefix, 0) == 0;
  });
  return it == log.end() ? -1 : it - log.begin();
}

std::ptrdiff_t last_with(const Log& log, std::string_view prefix) {
  std::ptrdiff_t at = -1;
  for (std::size_t i = 0; i < log.size(); ++i) {
    if (log[i].rfind(prefix, 0) == 0) at = static_cast<std::ptrdiff_t>(i);
  }
  return at;
}

std::string tmp(const char* name) {
  std::string path = "/tmp/shigoku-sync-cycle-test-";
  path += std::to_string(static_cast<long>(::getpid()));
  path += "-";
  path += name;
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

}  // namespace

TEST_CASE("the run goes mirror pull, AniList, mirror push, airing") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  add_show(*store);
  Log log;
  const LogAni ani(log);
  const LogMal mal(log);
  Auth auth = signed_in_both();

  const sync::CycleResult ran =
      sync::run_cycle(ani, mal, {}, logging_airing(log), *store, auth, options(), no_sleep);

  REQUIRE(ran.anilist.has_value());
  CHECK(ran.anilist->outcome == sync::SyncOutcome::Completed);
  REQUIRE(ran.mal_pull.has_value());
  CHECK(ran.mal_pull->outcome == mal_mirror::MirrorOutcome::Completed);
  CHECK(ran.mal_pushed == 1);
  CHECK_FALSE(ran.mal_token_unsaved);

  // The mirror's list fetch leads; AniList's whole run sits between it and
  // the mirror's push; the airing stamps come last.
  REQUIRE(!log.empty());
  CHECK(log.front() == "mal list mal-token");
  const std::ptrdiff_t ani_first = first_with(log, "anilist");
  const std::ptrdiff_t ani_last = last_with(log, "anilist");
  const std::ptrdiff_t mal_push = first_with(log, "mal save");
  REQUIRE(ani_first > 0);
  REQUIRE(mal_push > 0);
  CHECK(ani_last < mal_push);
  CHECK(first_with(log, "mal entry") > ani_last);
  CHECK(log.back() == "airing");
}

TEST_CASE("a renewed MAL pair is saved beside the AniList block and carries the push") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  add_show(*store);
  Log log;
  const LogAni ani(log);
  const LogMal mal(log);

  Auth auth = signed_in_both();
  auth.mal.refresh_token = "mal-refresh-value";
  auth.mal.expires_at = kNow - 100;  // past its date: the pull renews it first.
  const std::string path = tmp("rolled.json");
  REQUIRE(auth.save(path).has_value());
  const mal_mirror::Refresher refresh = [](const MalAuth&, std::int64_t) {
    return mal_mirror::Refresh::rolled(renewed_pair());
  };

  const sync::CycleResult ran =
      sync::run_cycle(ani, mal, refresh, logging_airing(log), *store, auth, options(path), no_sleep);

  REQUIRE(ran.mal_pull.has_value());
  REQUIRE(ran.mal_pull->rolled.has_value());
  CHECK_FALSE(ran.mal_token_unsaved);
  CHECK(auth.mal == renewed_pair());
  const Auth disk = Auth::load(path);
  CHECK(disk.mal == renewed_pair());
  CHECK(disk.anilist == auth.anilist);
  // No MAL call went out under the retired token.
  CHECK(first_with(log, "mal list mal-token-renewed") == 0);
  CHECK(first_with(log, "mal save mal-token-renewed") > 0);
  for (const std::string& call : log) {
    if (call.rfind("mal ", 0) == 0) CHECK(call.ends_with("mal-token-renewed"));
  }
  std::remove(path.c_str());

  SUBCASE("with nowhere to save it, the renewal is reported unsaved") {
    Auth again = signed_in_both();
    again.mal.refresh_token = "mal-refresh-value";
    again.mal.expires_at = kNow - 100;
    const sync::CycleResult unsaved =
        sync::run_cycle(ani, mal, refresh, logging_airing(log), *store, again, options(), no_sleep);
    CHECK(unsaved.mal_token_unsaved);
    CHECK(again.mal == renewed_pair());
  }
}

TEST_CASE("a signed-out mirror makes no call and the AniList run still goes") {
  auto store = Store::open_memory();
  REQUIRE(store.has_value());
  add_show(*store);
  Log log;
  const LogAni ani(log);
  const LogMal mal(log);
  Auth auth = signed_in_both();
  auth.mal = MalAuth{};

  const sync::CycleResult ran =
      sync::run_cycle(ani, mal, {}, logging_airing(log), *store, auth, options(), no_sleep);

  REQUIRE(ran.mal_pull.has_value());
  CHECK(ran.mal_pull->outcome == mal_mirror::MirrorOutcome::Disabled);
  CHECK(ran.mal_pushed == 0);
  CHECK(first_with(log, "mal ") == -1);
  REQUIRE(ran.anilist.has_value());
  CHECK(ran.anilist->outcome == sync::SyncOutcome::Completed);
  CHECK(log.front() == "anilist list");
  CHECK(log.back() == "airing");
}
