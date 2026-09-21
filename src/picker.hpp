// picker.hpp — the command line's selector seam. Two ways to answer "which
// of these rows": the numbered stdin prompt (main.cpp's prompt_pick, the
// default and the fallback) and an fzf-compatible picker — fzf itself, or
// fzf++, the C++ reimplementation that builds on the legacy target — spawned
// with the numbered rows on stdin and read back on stdout. The interface's
// own views never use this; it is the ani-cli shape for the headless mode.
//
// The picker draws on /dev/tty, so stdout can be the answer pipe; stderr is
// inherited so its own errors reach the terminal. The output contract is
// fzf's: the selected line on stdout and exit 0; exit 1 = no match, 130 =
// abort (Esc / Ctrl-C) — both the user backing out, the same "bye" the
// prompt's `q` produces. Anything else (no spawn, exit 2, a signal, an
// unreadable answer) is the picker failing, which the caller distinguishes
// so the pick can fall to the numbered prompt with a note instead of ending
// the run as a silent "bye".
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shigoku::picker {

// Config `cli_picker`: `auto` = the fzf-compatible binary when one is found
// AND stdin/stdout are a terminal, else the prompt; `prompt` / `fzf` force
// one (fzf without a binary falls back to the prompt with a note).
enum class Mode {
  Auto,
  Prompt,
  Fzf,
};
[[nodiscard]] Mode parse_mode(std::string_view s);

// The rows as handed to the picker: "1. row", "2. row", … — the number is
// what parse_selection reads back, so titles never have to be unique.
[[nodiscard]] std::vector<std::string> numbered(const std::vector<std::string>& rows);

// The picker's selected line -> 0-based index, via the leading "N." number;
// nullopt for anything else (no number, out of [1, max], junk).
[[nodiscard]] std::optional<std::size_t> parse_selection(std::string_view line, std::size_t max);

// The binary to spawn: `override_path` when set (a bare name is searched on
// PATH, a path is checked as given), else `fzf` on PATH. nullopt = none.
[[nodiscard]] std::optional<std::string> find_binary(std::string_view override_path);

// What a run picks with, under the config's mode and `picker_path` and
// whether stdin and stdout are a terminal: `binary` set = spawn it; nullopt
// = the numbered prompt, with `why` the one clause that ruled the picker out
// ("no fzf on PATH", "cli_picker = prompt", …) and `binary_missing` true
// when that clause is a binary that could not be found — the case worth a
// note when the config asked for one by mode or by path. `shigoku --paths`
// prints the same Choice the pick seam acts on.
struct Choice {
  std::optional<std::string> binary;
  bool binary_missing = false;
  std::string why;
};
[[nodiscard]] Choice choose(Mode mode, std::string_view picker_path, bool at_terminal);

// The picker's verdict on one pick: a selection; a decline (fzf's exit 1 =
// no match, 130 = Esc / Ctrl-C; also an empty row list); or the picker
// itself failing (no spawn, an error exit, a signal, an answer the numbering
// cannot read) with `detail` naming it in one clause.
struct Pick {
  enum class Kind { Picked, Declined, Failed };
  Kind kind = Kind::Declined;
  std::size_t index = 0;  // valid when kind == Picked
  std::string detail;     // set when kind == Failed
};

// Spawn `binary` with the numbered rows on stdin and `prompt` as its prompt,
// wait, and read the selection back. When the picker fails after taking the
// terminal (a crash mid-screen), stdin's line discipline is restored so the
// prompt that follows is usable.
[[nodiscard]] Pick fzf_pick(const std::string& binary, std::string_view prompt,
                            const std::vector<std::string>& rows);

}  // namespace shigoku::picker
