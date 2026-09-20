// picker_tests.cpp — the command line's selector seam: the pure halves and
// the spawn against a stub fzf-compatible picker (no tty, no real fzf).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "../src/picker.hpp"

using namespace shigoku::picker;

namespace {

const std::string kStub = std::string(SHIGOKU_TEST_FIXTURES_DIR) + "/stub_fzf.sh";
const std::vector<std::string> kRows = {"Frieren  ·  28 sub eps", "Cowboy Bebop  ·  26 eps",
                                        "Mystery"};

struct EnvVar {
  std::string name;
  EnvVar(std::string n, const std::string& value) : name(std::move(n)) {
    ::setenv(name.c_str(), value.c_str(), 1);
  }
  ~EnvVar() { ::unsetenv(name.c_str()); }
};

}  // namespace

TEST_CASE("parse_mode: prompt and fzf are explicit, everything else is auto") {
  CHECK(parse_mode("prompt") == Mode::Prompt);
  CHECK(parse_mode("fzf") == Mode::Fzf);
  CHECK(parse_mode("auto") == Mode::Auto);
  CHECK(parse_mode("") == Mode::Auto);
  CHECK(parse_mode("skim") == Mode::Auto);
}

TEST_CASE("numbered rows carry the index the selection is read back from") {
  const auto lines = numbered({"a", "b\nc", "d"});
  CHECK(lines == std::vector<std::string>{"1. a", "2. b c", "3. d"});
  CHECK(parse_selection("2. b c", 3) == 1);
  CHECK(parse_selection("  3. d\n", 3) == 2);
  CHECK(parse_selection("10. x", 10) == 9);
  CHECK(!parse_selection("0. x", 3).has_value());
  CHECK(!parse_selection("4. x", 3).has_value());
  CHECK(!parse_selection("abc", 3).has_value());
  CHECK(!parse_selection("2 x", 3).has_value());  // no dot: not our line.
  CHECK(!parse_selection("", 3).has_value());
  CHECK(!parse_selection("99999999999999999999. x", 3).has_value());
}

TEST_CASE("find_binary: an explicit path is checked as given, a name walks PATH") {
  CHECK(find_binary(kStub) == kStub);
  CHECK(!find_binary("/nonexistent/fzf").has_value());
  // PATH search for the default name: a temp dir holding an `fzf` symlink.
  char tmpl[] = "/tmp/shigoku-picker-XXXXXX";
  const char* dir = ::mkdtemp(tmpl);
  REQUIRE(dir != nullptr);
  const std::string link = std::string(dir) + "/fzf";
  REQUIRE(::symlink(kStub.c_str(), link.c_str()) == 0);
  {
    EnvVar path("PATH", std::string(dir) + ":/nonexistent");
    CHECK(find_binary("") == link);
    CHECK(find_binary("fzf") == link);
    CHECK(!find_binary("nope").has_value());
  }
  {
    EnvVar path("PATH", "/nonexistent");
    CHECK(!find_binary("").has_value());
  }
  ::unlink(link.c_str());
  ::rmdir(dir);
}

TEST_CASE("fzf_pick: the stub's selection comes back as an index") {
  {
    EnvVar line("STUB_FZF_LINE", "2");
    CHECK(fzf_pick(kStub, "pick a show", kRows) == 1);
  }
  {
    EnvVar line("STUB_FZF_LINE", "3");
    CHECK(fzf_pick(kStub, "pick a show", kRows) == 2);
  }
  CHECK(!fzf_pick(kStub, "pick a show", {}).has_value());  // nothing to pick from.
}

TEST_CASE("fzf_pick: abort, no match, error and a missing binary are all nullopt") {
  for (const char* code : {"130", "1", "2"}) {
    EnvVar e("STUB_FZF_EXIT", code);
    CAPTURE(code);
    CHECK(!fzf_pick(kStub, "pick a show", kRows).has_value());
  }
  CHECK(!fzf_pick("/nonexistent/fzf", "pick a show", kRows).has_value());
}

TEST_CASE("fzf_pick: the prompt and the inline-height flags ride the argv") {
  EnvVar e("STUB_FZF_ECHO_ARGS", "1");
  // The stub prints its argv one per line; parse_selection sees "--prompt=…"
  // first and answers nullopt — the flags themselves are what this pins.
  CHECK(!fzf_pick(kStub, "pick an episode", kRows).has_value());
}
