#!/bin/sh
# stub_fzf.sh — a stand-in fzf-compatible picker for picker_tests. Never
# touches a tty: reads every stdin line, prints the STUB_FZF_LINE-th (default
# 2) and exits 0 — fzf's output contract. STUB_FZF_EXIT overrides the exit code
# (130 = abort, 1 = no match) with nothing printed; STUB_FZF_ECHO_ARGS=1 prints
# the argv instead, so a test can pin the flags the spawn hands over.
n=${STUB_FZF_LINE:-2}
if [ -n "$STUB_FZF_ECHO_ARGS" ]; then
  cat >/dev/null
  printf '%s\n' "$@"
  exit 0
fi
sel=$(sed -n "${n}p")
if [ -n "$STUB_FZF_EXIT" ]; then
  exit "$STUB_FZF_EXIT"
fi
printf '%s\n' "$sel"
exit 0
