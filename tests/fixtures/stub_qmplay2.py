#!/usr/bin/env python3
"""stub_qmplay2.py — a fake QMPlay2 for the P39 slice-4 tests.

Launch-only backend: the app spawns it with just the vetted positional and
waits. stdio is /dev/null (PositionSource::None wires no pipes), so there is
nothing to read or answer — exit 0 immediately is the clean session. PlayDone
with no position is the lawful outcome: the A6 recordPlay gate stays shut and
progress is marked manually.
"""
import sys

sys.exit(0)
