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
// abort (Esc / Ctrl-C), anything else = error — every non-zero exit is a
// nullopt here, the same "bye" the prompt's `q` produces.
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

// Spawn `binary` with the numbered rows on stdin and `prompt` as its prompt,
// wait, and read the selection back. nullopt on an empty row list, a spawn
// failure, or any non-zero exit (no match / abort / error).
[[nodiscard]] std::optional<std::size_t> fzf_pick(const std::string& binary,
                                                  std::string_view prompt,
                                                  const std::vector<std::string>& rows);

}  // namespace shigoku::picker
