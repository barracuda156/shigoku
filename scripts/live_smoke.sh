#!/usr/bin/env bash
# scripts/live_smoke.sh — build (if needed) and run the live-network smoke
# binaries, gated by SHIGOKU_LIVE=1 (PORT_CPP.md P3/P4 DoD). Not part of ctest.
#
#   source scripts/env.sh && ./scripts/live_smoke.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

cmake -S . -B build >/dev/null
cmake --build build -j --target anilist_live_smoke senshi_live_smoke

SHIGOKU_LIVE=1 ./build/anilist_live_smoke
SHIGOKU_LIVE=1 ./build/senshi_live_smoke
