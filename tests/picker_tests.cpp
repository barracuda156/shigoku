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

TEST_CASE("choose: mode, binary and terminal decide the picker, and the prompt names its reason") {
  {
    // `prompt` never looks for a binary.
    const Choice c = choose(Mode::Prompt, kStub, true);
    CHECK(!c.binary.has_value());
    CHECK(!c.binary_missing);
    CHECK(c.why == "cli_picker = prompt");
  }
  CHECK(choose(Mode::Auto, kStub, true).binary == kStub);
  {
    // `auto` keeps the prompt for a piped run; a forced `fzf` spawns anyway.
    const Choice c = choose(Mode::Auto, kStub, false);
    CHECK(!c.binary.has_value());
    CHECK(!c.binary_missing);
    CHECK(c.why == "stdin/stdout not a terminal");
    CHECK(choose(Mode::Fzf, kStub, false).binary == kStub);
  }
  {
    const Choice c = choose(Mode::Fzf, "/nonexistent/fzf", true);
    CHECK(!c.binary.has_value());
    CHECK(c.binary_missing);
    CHECK(c.why == "/nonexistent/fzf isn't there or isn't executable");
  }
  {
    EnvVar path("PATH", "/nonexistent");
    Choice c = choose(Mode::Auto, "", true);
    CHECK(c.binary_missing);
    CHECK(c.why == "no fzf on PATH");
    c = choose(Mode::Auto, "fzfpp", true);
    CHECK(c.binary_missing);
    CHECK(c.why == "no fzfpp on PATH");
  }
}

TEST_CASE("fzf_pick: the stub's selection comes back as an index") {
  {
    EnvVar line("STUB_FZF_LINE", "2");
    const Pick p = fzf_pick(kStub, "pick a show", kRows);
    CHECK(p.kind == Pick::Kind::Picked);
    CHECK(p.index == 1);
  }
  {
    EnvVar line("STUB_FZF_LINE", "3");
    const Pick p = fzf_pick(kStub, "pick a show", kRows);
    CHECK(p.kind == Pick::Kind::Picked);
    CHECK(p.index == 2);
  }
  // Nothing to pick from: a decline, not a failure.
  CHECK(fzf_pick(kStub, "pick a show", {}).kind == Pick::Kind::Declined);
}

TEST_CASE("fzf_pick: abort and no match decline; an error exit or a missing binary is a failure") {
  for (const char* code : {"130", "1"}) {
    EnvVar e("STUB_FZF_EXIT", code);
    CAPTURE(code);
    const Pick p = fzf_pick(kStub, "pick a show", kRows);
    CHECK(p.kind == Pick::Kind::Declined);
    CHECK(p.detail.empty());
  }
  {
    EnvVar e("STUB_FZF_EXIT", "2");
    const Pick p = fzf_pick(kStub, "pick a show", kRows);
    CHECK(p.kind == Pick::Kind::Failed);
    CHECK(p.detail == kStub + " failed (exit 2)");
  }
  {
    const Pick p = fzf_pick("/nonexistent/fzf", "pick a show", kRows);
    CHECK(p.kind == Pick::Kind::Failed);
    CHECK(p.detail.rfind("couldn't start /nonexistent/fzf: ", 0) == 0);
  }
}

TEST_CASE("fzf_pick: the prompt and the inline-height flags ride the argv") {
  EnvVar e("STUB_FZF_ECHO_ARGS", "1");
  // The stub prints its argv one per line; "--prompt=…" is no numbered row,
  // so the verdict is an unreadable answer — a failure, never a "bye".
  const Pick p = fzf_pick(kStub, "pick an episode", kRows);
  CHECK(p.kind == Pick::Kind::Failed);
  CHECK(p.detail == "couldn't read " + kStub + "'s answer");
}
