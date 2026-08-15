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

  auto saved = cfg.save(path);
  REQUIRE(saved.has_value());
  CHECK(Config::load(path) == cfg);
}

TEST_CASE("missing file is defaults") {
  CHECK(Config::load("/nonexistent/config.json") == Config{});
}

TEST_CASE("corrupt file is defaults") {
  const std::string path = tmp("corrupt.json");
  write_file(path, "{ this is not json");
  CHECK(Config::load(path) == Config{});
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
