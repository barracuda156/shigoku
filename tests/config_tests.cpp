// config_tests.cpp — P18 config load/save tests.
//
// Ports the sabigoku config.rs #[test] cases 1:1 (§8: golden tests are the
// port contract). Offline, tmp files in the OS tmpdir; no network, no store.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <string>

#include "../src/config.hpp"

using namespace shigoku;

namespace {

// Fresh path in the OS tmpdir (config.rs tmp()); each call removes any prior
// leftover so a test starts blank.
std::string tmp(const char* name) {
  std::string path = "/tmp/shigoku-config-test-";
  path += std::to_string(static_cast<long>(::getpid()));
  path += "-";
  path += name;
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

void write_file(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

}  // namespace

TEST_CASE("round trip preserves everything") {
  const std::string path = tmp("round_trip.json");
  Config cfg;
  cfg.mpv_path = "/opt/mpv/bin/mpv";
  cfg.translation = "dub";
  cfg.resume_offset_sec = 12;
  cfg.cover_art = false;
  cfg.transparent_background = true;
  cfg.preferred_provider = "allanime";
  cfg.player = "mplayer";           // P39 slice 2.
  cfg.player_path = "/opt/mplayer";
  cfg.senshi_node = "/opt/local/bin/node";
  cfg.catalog = "mal";
  cfg.cli_picker = "fzf";
  cfg.picker_path = "/opt/local/bin/fzf";
  cfg.disabled_providers = {"anibd", "anidbapp"};
  cfg.default_quality = "1080";
  cfg.download_quality = "480";
  cfg.download_parallel = 2;
  cfg.history_sort = "title";

  auto saved = cfg.save(path);
  REQUIRE(saved.has_value());
  const Config loaded = Config::load(path);
  CHECK(loaded == cfg);
  CHECK(loaded.download_quality == "480");
  CHECK(loaded.effective_download_quality() == "480");
}

TEST_CASE("download quality follows the default quality until set") {
  Config cfg;
  CHECK(cfg.download_quality.empty());
  CHECK(cfg.effective_download_quality() == "best");
  cfg.default_quality = "720";
  CHECK(cfg.effective_download_quality() == "720");
  cfg.download_quality = "worst";
  CHECK(cfg.effective_download_quality() == "worst");
  CHECK(cfg.default_quality == "720");

  // A blank value round-trips as blank and keeps following the default.
  const std::string path = tmp("download_quality.json");
  cfg.download_quality.clear();
  REQUIRE(cfg.save(path).has_value());
  const Config blank = Config::load(path);
  CHECK(blank.download_quality.empty());
  CHECK(blank.effective_download_quality() == "720");

  write_file(path, R"({"default_quality": "1080", "download_quality": "480"})");
  const Config set = Config::load(path);
  CHECK(set.download_quality == "480");
  CHECK(set.effective_download_quality() == "480");
  CHECK(set.default_quality == "1080");
}

TEST_CASE("missing file is defaults") {
  CHECK(Config::load("/nonexistent/config.json") == Config{});
}

TEST_CASE("corrupt file is defaults") {
  const std::string path = tmp("corrupt.json");
  write_file(path, "{ this is not json");
  CHECK(Config::load(path) == Config{});
}

TEST_CASE("a value of the wrong type keeps that key's default and the rest of the file") {
  const std::string path = tmp("wrong_types.json");
  write_file(path, R"({
    "mpv_path": 7,
    "cover_art": "yes",
    "resume_offset_sec": "12",
    "discover_cover_concurrency": -3,
    "download_parallel": 2.5,
    "translation": "dub",
    "kanji_chips": false
  })");
  const Config loaded = Config::load(path);
  Config want;
  want.translation = "dub";
  want.kanji_chips = false;
  CHECK(loaded == want);

  // A number too large for the field is a bad value too.
  write_file(path, R"({"resume_offset_sec": 99999999999, "landing": "browse"})");
  Config big;
  big.landing = "browse";
  CHECK(Config::load(path) == big);
}

TEST_CASE("oversized file is defaults") {
  const std::string path = tmp("oversized.json");
  std::string text = "{\"mpv_path\": \"mpv\", \"pad\": \"";
  text.append(80 * 1024, 'x');
  text += "\"}";
  REQUIRE(static_cast<std::uint64_t>(text.size()) > kMaxConfigBytes);
  write_file(path, text);
  CHECK(Config::load(path) == Config{});
}

TEST_CASE("unknown fields ignored missing fields defaulted") {
  const std::string path = tmp("partial.json");
  write_file(path, R"({"translation": "dub", "from_the_future": true})");
  Config cfg = Config::load(path);
  CHECK(cfg.translation == "dub");
  CHECK(cfg.mpv_path == "mpv");
  CHECK(cfg.resume_offset_sec == 5);
}

TEST_CASE("history_sort defaults to recent and keeps an unknown value verbatim") {
  CHECK(Config{}.history_sort == "recent");
  const std::string path = tmp("history_sort.json");
  write_file(path, R"({"history_sort": "by-moon-phase"})");
  const Config odd = Config::load(path);
  CHECK(odd.history_sort == "by-moon-phase");
  REQUIRE(odd.save(path).has_value());
  CHECK(Config::load(path).history_sort == "by-moon-phase");
  write_file(path, R"({"history_sort": 1, "translation": "dub"})");
  const Config bad = Config::load(path);
  CHECK(bad.history_sort == "recent");
  CHECK(bad.translation == "dub");
}

TEST_CASE("disabled_providers keeps string names only") {
  const std::string path = tmp("disabled.json");
  write_file(path, R"({"disabled_providers": ["anibd", 7, "", null, "senshi"]})");
  CHECK(Config::load(path).disabled_providers == std::vector<std::string>{"anibd", "senshi"});
  write_file(path, R"({"disabled_providers": "anibd", "translation": "dub"})");
  const Config cfg = Config::load(path);
  CHECK(cfg.disabled_providers.empty());
  CHECK(cfg.translation == "dub");
}

TEST_CASE("save failure surfaces an error") {
  // The temp-then-rename write fails at the temp file when the dir is gone.
  const std::string path = "/nonexistent-dir-shigoku/config.json";
  auto r = Config{}.save(path);
  CHECK(!r.has_value());
}

TEST_CASE("save is temp then rename and leaves no temp behind") {
  const std::string path = tmp("atomic.json");
  Config cfg;
  cfg.image_protocol = "kitty";
  REQUIRE(cfg.save(path).has_value());

  struct stat st{};
  CHECK(::stat((path + ".tmp").c_str(), &st) != 0);  // no leftover tmp file.

  // A non-default value for a key no Settings row surfaces survives the
  // round-trip (the file is fully rewritten, never merged).
  CHECK(Config::load(path).image_protocol == "kitty");
}

TEST_CASE("fifo path is defaults not a hang") {
  const std::string path = tmp("fifo.json");
  REQUIRE(::mkfifo(path.c_str(), 0600) == 0);

  auto fut = std::async(std::launch::async, [&path]() { return Config::load(path); });
  REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(fut.get() == Config{});

  std::remove(path.c_str());
}

TEST_CASE("cover concurrency clamps at read") {
  Config cfg;
  cfg.discover_cover_concurrency = 0;
  CHECK(cfg.effective_cover_concurrency() == 1);
  cfg.discover_cover_concurrency = 99;
  CHECK(cfg.effective_cover_concurrency() == 16);
  cfg.discover_cover_concurrency = 4;
  CHECK(cfg.effective_cover_concurrency() == 4);
}

TEST_CASE("download parallelism defaults to one and clamps at read") {
  Config cfg;
  CHECK(cfg.download_parallel == 1);
  CHECK(cfg.effective_download_parallel() == 1);
  cfg.download_parallel = 0;
  CHECK(cfg.effective_download_parallel() == 1);
  cfg.download_parallel = 2;
  CHECK(cfg.effective_download_parallel() == 2);
  cfg.download_parallel = 9;
  CHECK(cfg.effective_download_parallel() == 3);

  // Stored as written, clamped only when read.
  const std::string path = tmp("download_parallel.json");
  REQUIRE(cfg.save(path).has_value());
  const Config loaded = Config::load(path);
  CHECK(loaded.download_parallel == 9);
  CHECK(loaded.effective_download_parallel() == 3);

  write_file(path, R"({"download_parallel": 2})");
  CHECK(Config::load(path).download_parallel == 2);
  write_file(path, R"({"translation": "dub"})");
  CHECK(Config::load(path).download_parallel == 1);
}
