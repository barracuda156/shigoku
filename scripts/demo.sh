#!/usr/bin/env bash
# scripts/demo.sh — P10 narrated acceptance walk (PORT_CPP.md §0, 04 §11).
#
# The offline demo/acceptance gate for shigoku: build, run the full ctest
# contract, drive the automated pty walk (search -> grid -> detail -> play
# against a stub mpv, then a clean quit), and audit the quit path — exit 0,
# terminal usable after, and no stray mpv socket / tmp litter in the runtime
# dir. Fully offline (no SHIGOKU_LIVE); safe to run from a cold cache.
#
#   source scripts/env.sh && ./scripts/demo.sh          # one clean walk
#   source scripts/env.sh && ./scripts/demo.sh --twice  # P10 DoD: twice in a row
#
# Honours SHIGOKU_JOBS for the parallelism (default: nproc, or 1 if unknown —
# the constrained dev box wants -j1; pass SHIGOKU_JOBS=1 there).

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# --- narration helpers -------------------------------------------------------
bold=$'\e[1m'; dim=$'\e[2m'; grn=$'\e[32m'; red=$'\e[31m'; rst=$'\e[0m'
step() { printf '%s\n==> %s%s\n' "${bold}" "$*" "${rst}"; }
note() { printf '%s    %s%s\n' "${dim}" "$*" "${rst}"; }
ok()   { printf '%s    ✓ %s%s\n' "${grn}" "$*" "${rst}"; }
die()  { printf '%s    ✗ %s%s\n' "${red}" "$*" "${rst}" >&2; exit 1; }

JOBS="${SHIGOKU_JOBS:-$(nproc 2>/dev/null || echo 1)}"
PTY_RUNTIME_DIR="/tmp/shigoku-pty-test"   # fixed by pty_walk_tests.cpp.

# One full acceptance walk. Returns non-zero on any failure (set -e + die).
run_once() {
  # 1) Build the offline world: the app, the demo binary, and every test.
  step "Build (offline, -j${JOBS})"
  cmake -S . -B build >/dev/null
  cmake --build build -j"${JOBS}"
  ok "built shigoku + drive_tui + tests"

  # 2) The port contract: the whole offline ctest suite must be green. Live
  #    smokes are gated behind SHIGOKU_LIVE and excluded here by not setting it.
  step "ctest — the offline port contract"
  ctest --test-dir build --output-on-failure
  ok "all ctest cases passed"

  # 3) Quit-path audit prep: start the pty walk from a COLD runtime dir so any
  #    leftover socket/tmp afterwards is litter this run produced, not history.
  step "Acceptance walk — pty: search → grid → play → quit"
  rm -rf "${PTY_RUNTIME_DIR}"
  note "cold runtime dir: ${PTY_RUNTIME_DIR}"

  # The pty_walk test IS the narrated walk: it forkpty()s the real app wired to
  # fixtures + a stub mpv, types the keystrokes, and asserts the frames. Its
  # final REQUIRE is child exit 0 (the clean quit path). Run just that test.
  local before_stty after_stty
  before_stty="$(stty -g 2>/dev/null || echo 'no-tty')"

  ctest --test-dir build --output-on-failure -R '^pty_walk_tests$'
  ok "walk reached play and quit; child exited 0"

  # 4) echo $? == 0 for the app on the clean path — the pty test asserts the
  #    child's WEXITSTATUS==0 internally; here we assert the drive_tui demo the
  #    §0 script names also builds and is launchable (it needs a real tty, so we
  #    only check the binary exists — the pty walk covers the run behavior).
  test -x build/drive_tui || die "drive_tui demo binary missing"
  ok "drive_tui demo binary present (run interactively: ./build/drive_tui)"

  # 5) Terminal-usable-after: the pty walk runs the app on its OWN pty, so this
  #    controlling tty was never touched — but assert it regardless, since the
  #    whole point of the P10 audit is "shell looks untouched afterward".
  after_stty="$(stty -g 2>/dev/null || echo 'no-tty')"
  if [ "${before_stty}" = "${after_stty}" ]; then
    ok "controlling terminal settings unchanged (usable after)"
  else
    die "terminal settings changed across the walk (restore leaked to our tty)"
  fi

  # 6) No stray mpv socket / tmp litter in the runtime dir. Every play unlinks
  #    its own shigoku-mpv-*.sock (player.cpp), so a clean walk leaves none.
  step "Litter audit — runtime dir must be clean"
  local socks
  socks="$(find "${PTY_RUNTIME_DIR}" -maxdepth 1 -name 'shigoku-mpv-*.sock' 2>/dev/null || true)"
  if [ -n "${socks}" ]; then
    printf '%s\n' "${socks}" >&2
    die "stray mpv socket(s) left behind"
  fi
  ok "no shigoku-mpv-*.sock left"

  # Nothing else should have been written into the runtime dir by a clean run.
  local leftovers
  leftovers="$(find "${PTY_RUNTIME_DIR}" -mindepth 1 2>/dev/null || true)"
  if [ -n "${leftovers}" ]; then
    printf '%s\n' "${leftovers}" >&2
    die "unexpected files in runtime dir after a clean walk"
  fi
  ok "runtime dir empty — no socket/tmp litter"

  # Also confirm no orphaned stub-mpv/mpv processes from this walk survive.
  if pgrep -f 'shigoku-mpv-.*\.sock' >/dev/null 2>&1; then
    die "orphaned mpv-ish process still holding a shigoku socket"
  fi
  ok "no orphaned player process"

  rm -rf "${PTY_RUNTIME_DIR}"
}

main() {
  local passes=1
  if [ "${1:-}" = "--twice" ]; then passes=2; fi

  for i in $(seq 1 "${passes}"); do
    if [ "${passes}" -gt 1 ]; then
      printf '\n%s########## acceptance pass %d/%d ##########%s\n' \
        "${bold}" "${i}" "${passes}" "${rst}"
    fi
    run_once
  done

  printf '\n%s%s DEMO PASSED — terminal usable, exit 0, no litter %s\n' \
    "${grn}${bold}" "✓" "${rst}"
}

main "$@"
