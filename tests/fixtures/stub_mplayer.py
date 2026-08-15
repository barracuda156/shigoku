#!/usr/bin/env python3
"""stub_mplayer.py — a fake mplayer for the P39 slice-3 tests.

Speaks just enough of the -slave protocol for player::play_local() under the
mplayer backend to see a clean play-through: read `get_time_pos` commands on
stdin, answer `ANS_time_pos=<secs>` on stdout with an advancing position
(and `get_time_length` with `ANS_LENGTH=24.0`, so the 0.80 natural-end ratio
computes: 20/24 = 0.833), then exit 0 after the position answers run out (a
clean exit with a meaningful last position is the PlayDone happy path).
Ignores every other slave command, like real mplayer ignores what it does
not know.

Not a general mplayer stand-in — only the one behavior the tests drive.
"""
import sys

positions = [3.0, 9.0, 20.0]
answered = 0

for line in sys.stdin:
    cmd = line.strip()
    if cmd == "get_time_pos":
        print("ANS_time_pos=%.1f" % positions[min(answered, len(positions) - 1)], flush=True)
        answered += 1
        if answered >= len(positions):
            break
    elif cmd == "get_time_length":
        print("ANS_LENGTH=24.0", flush=True)
    # any other slave command: ignored.

sys.exit(0)
