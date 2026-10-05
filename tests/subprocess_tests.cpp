// subprocess_tests.cpp — the helper runner: stdin fed, stdout kept, exit code
// reported, a missing binary named, the wall-clock bound enforced, the output
// cap enforced, and no wedge when the child exits without reading its input.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "../src/subprocess.hpp"

using namespace shigoku;
using namespace shigoku::subprocess;

TEST_CASE("run_feeds_stdin_keeps_stdout_and_reports_the_exit_code") {
  auto r = run({"/bin/sh", "-c", "cat; printf ' done'; exit 3"}, "hello", 10000);
  REQUIRE(r.has_value());
  CHECK(r->out == "hello done");
  CHECK(r->exit_code == 3);
}

TEST_CASE("run_names_a_missing_binary") {
  auto r = run({"shigoku-no-such-binary-7f3a"}, "", 10000);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == Failure::Kind::NotFound);
  CHECK(r.error().detail.find("shigoku-no-such-binary-7f3a") != std::string::npos);
  auto empty = run({}, "", 1000);
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().kind == Failure::Kind::Spawn);
}

TEST_CASE("run_kills_a_child_that_outlives_the_bound") {
  const auto t0 = std::chrono::steady_clock::now();
  auto r = run({"/bin/sh", "-c", "exec sleep 20"}, "", 300);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == Failure::Kind::Timeout);
  CHECK(ms < 5000);
}

TEST_CASE("run_caps_the_output") {
  auto r = run({"/bin/sh", "-c", "head -c 100000 /dev/zero"}, "", 10000, 1000);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == Failure::Kind::Io);
}

TEST_CASE("run_does_not_wedge_on_a_child_that_never_reads_its_input") {
  const std::string big(600000, 'x');
  auto r = run({"/bin/sh", "-c", "echo ok"}, big, 10000);
  REQUIRE(r.has_value());
  CHECK(r->out == "ok\n");
  CHECK(r->exit_code == 0);
}

TEST_CASE("run_drains_a_large_input_the_child_does_consume") {
  const std::string big(300000, 'y');
  auto r = run({"/bin/sh", "-c", "wc -c | tr -d ' \\n'"}, big, 10000);
  REQUIRE(r.has_value());
  CHECK(r->out == "300000");
  CHECK(r->exit_code == 0);
}

TEST_CASE("run_reports_a_signalled_child_as_minus_one") {
  auto r = run({"/bin/sh", "-c", "kill -9 $$"}, "", 10000);
  REQUIRE(r.has_value());
  CHECK(r->exit_code == -1);
}
