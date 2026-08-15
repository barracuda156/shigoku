#!/usr/bin/env bash
# decloak_spike_standalone.sh — P11 de-cloak decision-gate spike, SELF-CONTAINED.
#
# No shigoku, no Rust, no repo needed. Just: bash + curl + mpv.
# It resolves a raw megaplay master .m3u8 for you (curl), then runs bare mpv
# across a ladder of demuxer flag-sets to see if ANY of them beats megaplay's
# byte-prefix segment decoy WITHOUT a stripping proxy.
#
# THE QUESTION (PORT_PARITY.md P11): megaplay cloaks segments two ways —
#   1. extension cloak (.ts served as .jpg)  -> fixed by allowed_extensions=ALL
#   2. byte-prefix decoy (image header PREPENDED to each segment, so the TS
#      sync byte 0x47 is no longer at offset 0) -> the Rust reference fixes this
#      ONLY with a loopback byte-strip proxy.
# P11 asks: does some deeper mpv/lavf knob survive the byte-prefix decoy anyway?
# Static prediction: NO (selection flags don't relocate the parse origin) =>
# verdict (b), port the proxy. This confirms or overturns that on a live stream.
#
# USAGE
#   ./decloak_spike_standalone.sh <MAL_ID> [EPISODE] [sub|dub]
#   e.g.  ./decloak_spike_standalone.sh 52991 1 sub      # Frieren S1, ep 1, sub
#
# If auto-resolve fails (site changed shape), fall back to a URL you grab from
# browser DevTools -> Network -> the master .m3u8:
#   DECLOAK_URL='https://.../master.m3u8' ./decloak_spike_standalone.sh
#
# REPORT BACK: the final "SPIKE RESULT" line, plus which rung (if any) PLAYED.

set -uo pipefail

HOST="https://megaplay.buzz"
REFERER="${DECLOAK_REFERER:-https://megaplay.buzz/}"
UA="${DECLOAK_UA:-Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36}"
MPV="${MPV:-mpv}"
PLAY_SECS="${DECLOAK_PLAY_SECS:-8}"
LOGDIR="${DECLOAK_LOGDIR:-./decloak-spike-logs}"

MAL="${1:-}"
EP="${2:-1}"
LANG="${3:-sub}"
URL="${DECLOAK_URL:-}"

bold=$'\e[1m'; grn=$'\e[32m'; red=$'\e[31m'; yel=$'\e[33m'; dim=$'\e[2m'; rst=$'\e[0m'

command -v curl >/dev/null 2>&1 || { printf '%s✗ curl not found%s\n' "$red" "$rst" >&2; exit 2; }
if ! command -v "$MPV" >/dev/null 2>&1; then
  printf '%s✗ mpv not found (set MPV=/path/to/mpv)%s\n' "$red" "$rst" >&2; exit 2
fi

# run_capped <secs> <cmd...> — run a command with a wall-clock cap. macOS has no
# GNU `timeout`; prefer it, then `gtimeout` (coreutils), else a pure-bash
# background+kill fallback so this works everywhere with zero extra installs.
if command -v timeout >/dev/null 2>&1; then
  run_capped() { local s="$1"; shift; timeout "$s" "$@"; }
elif command -v gtimeout >/dev/null 2>&1; then
  run_capped() { local s="$1"; shift; gtimeout "$s" "$@"; }
else
  run_capped() {
    local s="$1"; shift
    "$@" & local pid=$!
    ( sleep "$s"; kill -TERM "$pid" 2>/dev/null; sleep 2; kill -KILL "$pid" 2>/dev/null ) & local watcher=$!
    wait "$pid" 2>/dev/null; local rc=$?
    kill -TERM "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
    return "$rc"
  }
fi

# ── Resolve a raw master .m3u8 via curl (megaplay.rs chain) ──────────────────
# 1. GET embed page /stream/mal/{mal}/{ep}/{lang} -> scrape data-id
# 2. GET /stream/getSources?id={data-id} (XHR headers) -> JSON .sources.file
if [ -z "$URL" ]; then
  if [ -z "$MAL" ]; then
    printf '%s✗ give a MAL id:  %s <MAL_ID> [EP] [sub|dub]%s\n' "$red" "$0" "$rst" >&2
    printf '%s  or set DECLOAK_URL to a raw master .m3u8 from DevTools.%s\n' "$dim" "$rst" >&2
    exit 2
  fi
  case "$LANG" in sub|dub) ;; *) printf '%s✗ lang must be sub or dub%s\n' "$red" "$rst" >&2; exit 2;; esac

  EMBED="$HOST/stream/mal/$MAL/$EP/$LANG"
  printf '%s→ embed: %s%s\n' "$dim" "$EMBED" "$rst"
  html="$(curl -fsSL --max-time 20 \
      -A "$UA" -H "Referer: $REFERER" \
      "$EMBED" 2>/dev/null)" || {
    printf '%s✗ embed GET failed (network? or not stocked). Try DevTools + DECLOAK_URL.%s\n' "$red" "$rst" >&2
    exit 3
  }
  # First numeric data-id (quoted or bare), must beat data-realid/data-mediaid.
  data_id="$(printf '%s' "$html" | grep -oE 'data-id=["'"'"']?[0-9]+' | head -1 | grep -oE '[0-9]+')"
  if [ -z "$data_id" ]; then
    printf '%s✗ no data-id in embed HTML — likely NOT STOCKED for mal=%s ep=%s %s.%s\n' "$red" "$MAL" "$EP" "$LANG" "$rst" >&2
    printf '%s  Try another episode/lang, or grab the .m3u8 from DevTools + DECLOAK_URL.%s\n' "$dim" "$rst" >&2
    exit 3
  fi
  printf '%s→ data-id: %s%s\n' "$dim" "$data_id" "$rst"

  SRC="$HOST/stream/getSources?id=$data_id"
  printf '%s→ getSources: %s%s\n' "$dim" "$SRC" "$rst"
  json="$(curl -fsSL --max-time 20 \
      -A "$UA" -H "Referer: $REFERER" \
      -H "X-Requested-With: XMLHttpRequest" -H "Accept: application/json" \
      "$SRC" 2>/dev/null)" || {
    printf '%s✗ getSources GET failed.%s\n' "$red" "$rst" >&2; exit 3
  }
  # .sources.file — pull the first "file":"..." under sources without needing jq.
  URL="$(printf '%s' "$json" | grep -oE '"file"[[:space:]]*:[[:space:]]*"[^"]+\.m3u8[^"]*"' | head -1 | sed -E 's/.*"file"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/')"
  # JSON escapes slashes: "\/" -> "/", "\\" -> "\". Unescape or mpv gets garbage.
  URL="${URL//\\\//\/}"
  URL="${URL//\\\\/\\}"
  if [ -z "$URL" ]; then
    printf '%s✗ no .m3u8 in getSources JSON. Raw response head:%s\n' "$red" "$rst" >&2
    printf '%s%s%s\n' "$dim" "$(printf '%s' "$json" | head -c 400)" "$rst" >&2
    exit 3
  fi
  printf '%s✓ resolved master: %s%s\n\n' "$grn" "$URL" "$rst"
fi

# ── The flag ladder (bare mpv, NO proxy) ─────────────────────────────────────
mkdir -p "$LOGDIR"
printf '%smpv: %s%s\n' "$dim" "$("$MPV" --version 2>/dev/null | head -1)" "$rst"
printf '%sstream: %s%s\n' "$dim" "$URL" "$rst"
printf '%sper-rung play window: %ss%s\n\n' "$dim" "$PLAY_SECS" "$rst"

declare -a RUNG_NAME=(
  "ref:allowed_extensions=ALL"
  "+extension_picky=0"
  "+wewbo-deep(scan_all_pmts=-1,fflags=+discardcorrupt,err_detect=ignore_err,skip_estimate_duration_from_pts=1)"
  "+resync/probe(analyzeduration=100M,probesize=100M,ts_packetsize=188)"
)
declare -a RUNG_DEMUX=(
  "allowed_extensions=ALL"
  "allowed_extensions=ALL,extension_picky=0"
  "allowed_extensions=ALL,extension_picky=0,scan_all_pmts=-1,fflags=+discardcorrupt,err_detect=ignore_err,skip_estimate_duration_from_pts=1"
  "allowed_extensions=ALL,extension_picky=0,analyzeduration=100M,probesize=100M,ts_packetsize=188"
)

overall="ALL-FAILED"
for i in "${!RUNG_NAME[@]}"; do
  name="${RUNG_NAME[$i]}"; demux="${RUNG_DEMUX[$i]}"; log="$LOGDIR/rung-$i.log"
  printf '%s==> rung %d: %s%s\n' "$bold" "$i" "$name" "$rst"

  run_capped "$((PLAY_SECS + 6))" "$MPV" \
    --no-config --no-audio-display --vo=null --ao=null \
    --msg-level=all=v \
    --user-agent="$UA" \
    --http-header-fields-append="Referer: $REFERER" \
    --stream-lavf-o=multiple_requests=1,icy=0 \
    --demuxer-lavf-o="$demux" \
    --length="$PLAY_SECS" \
    "$URL" >"$log" 2>&1
  rc=$?

  # Did mpv even RUN? rc 127/126 = not-found/not-executable; a near-empty log
  # means it never opened the URL. That is a HARNESS error, NOT a demux verdict.
  logsize=$(wc -c <"$log" 2>/dev/null || echo 0)
  if [ "$rc" = 127 ] || [ "$rc" = 126 ] || [ "${logsize:-0}" -lt 200 ]; then
    printf '%s    ⚠ mpv DID NOT RUN (rc=%s, log %sB) — spike harness error, not a stream verdict.%s\n' "$yel" "$rc" "${logsize:-0}" "$rst"
    printf '%s      log head: %s%s\n' "$dim" "$(head -c 200 "$log" 2>/dev/null | tr '\n' ' ')" "$rst"
    overall="HARNESS-ERROR"; continue
  fi

  played_video=0; advanced=0; failed_demux=0
  grep -Eiq 'VO:|Video --vid|opening video decoder|--- video ---|Selected video codec' "$log" && played_video=1
  if grep -Eoq '(AV?|A|V): *0*[1-9][0-9]*[.:]' "$log" || grep -Eq 'V: +0*[1-9]' "$log"; then advanced=1; fi
  grep -Eiq 'Failed to (recognize|open) file format|could not open codec|Invalid data found|no (video|audio) or video|Failed to open|demuxer: no' "$log" && failed_demux=1

  if [ "$played_video" = 1 ] && [ "$advanced" = 1 ] && [ "$failed_demux" = 0 ]; then
    printf '%s    ✓ PLAYED — this rung beat the decoy (proxy-less)%s\n' "$grn" "$rst"
    overall="PLAYED@rung-$i"; break
  elif [ "$failed_demux" = 1 ]; then
    printf '%s    ✗ FAILED — demux/open error; decoy survived (rc=%s)%s\n' "$red" "$rc" "$rst"
    printf '%s      %s%s\n' "$dim" "$(grep -Eim1 'Failed to|Invalid data|could not open' "$log" || echo '(see log)')" "$rst"
    [ "$overall" = "ALL-FAILED" ] || overall="ALL-FAILED"
  else
    printf '%s    ? INCONCLUSIVE — no clear video/advance, no explicit demux error (rc=%s); read %s%s\n' "$yel" "$rc" "$log" "$rst"
    [ "$overall" = "ALL-FAILED" ] || overall="ALL-FAILED"
  fi
  printf '%s      log: %s%s\n' "$dim" "$log" "$rst"
done

printf '\n%s============================================================%s\n' "$bold" "$rst"
case "$overall" in
  PLAYED@*)
    printf '%s%sSPIKE RESULT: %s → VERDICT (a): a demuxer flag-set survives the decoy.%s\n' "$grn" "$bold" "$overall" "$rst"
    printf '%s  P12 (proxy) may be SKIPPED. Tell Claude WHICH rung played.%s\n' "$grn" "$rst"
    ;;
  HARNESS-ERROR)
    printf '%s%sSPIKE RESULT: HARNESS ERROR — mpv never actually ran. NO VERDICT.%s\n' "$yel" "$bold" "$rst"
    printf '%s  This is NOT verdict (b). Fix the harness (see the ⚠ line above) and re-run.%s\n' "$yel" "$rst"
    ;;
  *)
    printf '%s%sSPIKE RESULT: ALL RUNGS FAILED → VERDICT (b): knobs do NOT beat the decoy.%s\n' "$red" "$bold" "$rst"
    printf '%s  Confirms the prediction: port the byte-strip proxy (P12).%s\n' "$red" "$rst"
    ;;
esac
printf '%slogs: %s/%s\n' "$dim" "$LOGDIR" "$rst"
