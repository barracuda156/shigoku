#!/usr/bin/env bash
# scripts/decloak_spike.sh — P11 de-cloak decision-gate spike (PORT_PARITY.md
# P11). RUN ON HARDWARE with mpv + network to megaplay.buzz; this box has
# neither, so the live leg is the user's. Output drops straight into the
# NOTES.md P11 verdict.
#
# THE QUESTION P11 decides:
#   megaplay segments are cloaked TWO ways (megaplay.rs / proxy.rs):
#     1. EXTENSION cloak (ROD-301): .ts served as .jpg. Fixed by an mpv/lavf
#        flag — `--demuxer-lavf-o=allowed_extensions=ALL`. Cheap, no proxy.
#     2. BYTE-PREFIX decoy (ROD-443): a PNG/image header is PREPENDED to each
#        segment, so the TS sync byte 0x47 no longer sits at offset 0. The Rust
#        reference fixes this ONLY with the loopback proxy's byte-strip
#        (`decloak` at proxy.rs:509 — skip to the first 0x47 at 188-byte stride).
#   P11 asks empirically: does SOME deeper demuxer knob-set survive the
#   byte-prefix decoy anyway (so we can SKIP porting the proxy)?
#
# STATIC PREDICTION (see NOTES.md P11): NO. allowed_extensions / extension_picky
# are demuxer-SELECTION options (which demuxer lavf tries for a filename), not
# byte-OFFSET options. Once mpegts is selected it still needs 0x47 sync at the
# stream start; ffmpeg's TS resync scans only a bounded window and gives up — a
# decoy prefix past that window is a black screen. No `-demuxer-lavf-o` option
# relocates the parse origin. => verdict (b): port the proxy as planned. This
# script CONFIRMS that prediction on a live stream (or refutes it, flipping to
# verdict (a) skip-P12).
#
# HOW TO RUN
#   1. Get a RAW megaplay master m3u8 URL (NOT proxied). Two ways:
#        a. Rust harness — resolve+play once with mpv verbose logging and read
#           the upstream URLs mpv opens:
#             cd ../sabigoku
#             MPV_VERBOSE=1 cargo run --example play_e2e -- \
#               --provider megaplay --episode 1 --mal <MAL_ID> "<title>"
#           …then read the .jpg/.ts segment URL mpv logs (proxy fetches it
#           upstream; it appears in mpv -v and in the proxy warn logs).
#        b. Browser devtools on megaplay.buzz → Network → the master .m3u8.
#      Export it:  export DECLOAK_URL='https://.../master.m3u8'
#      And the origin the CDN gates on (constant for megaplay):
#        export DECLOAK_REFERER='https://megaplay.buzz/'
#        export DECLOAK_UA='Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36'
#   2. ./scripts/decloak_spike.sh
#      Each flag rung launches bare mpv (NO proxy) for ~8s and greps the verbose
#      log for evidence of real decode vs. a demux failure. Logs land in
#      ./decloak-spike-logs/ for the NOTES.md evidence capture.
#
# WHAT COUNTS AS SUCCESS PER RUNG
#   PLAYED  — the verbose log shows a video track opened AND a growing time-pos
#             (mpv decoded frames): this rung's flags beat the decoy.
#   FAILED  — demux/open error, or no video track, or time-pos never advances:
#             the decoy survived these flags.
# If the ONLY rung that PLAYS is one that also needs the proxy (it won't here —
# every rung is proxy-less), that's verdict (a). If ALL rungs FAIL, verdict (b).

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

MPV="${MPV:-mpv}"
URL="${DECLOAK_URL:-}"
REFERER="${DECLOAK_REFERER:-https://megaplay.buzz/}"
UA="${DECLOAK_UA:-Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36}"
PLAY_SECS="${DECLOAK_PLAY_SECS:-8}"
LOGDIR="./decloak-spike-logs"

bold=$'\e[1m'; grn=$'\e[32m'; red=$'\e[31m'; yel=$'\e[33m'; dim=$'\e[2m'; rst=$'\e[0m'

if ! command -v "${MPV}" >/dev/null 2>&1; then
  printf '%s✗ mpv not found (set MPV=/path/to/mpv). This spike needs a real mpv.%s\n' "${red}" "${rst}" >&2
  exit 2
fi
if [ -z "${URL}" ]; then
  printf '%s✗ set DECLOAK_URL to a RAW megaplay master m3u8 (see the header of this script).%s\n' "${red}" "${rst}" >&2
  exit 2
fi

mkdir -p "${LOGDIR}"
printf '%smpv: %s%s\n' "${dim}" "$("${MPV}" --version 2>/dev/null | head -1)" "${rst}"
printf '%sstream: %s%s\n' "${dim}" "${URL}" "${rst}"
printf '%sper-rung play window: %ss%s\n\n' "${dim}" "${PLAY_SECS}" "${rst}"

# The flag ladder. Rung 0 is the Rust reference's actual cloaked_segments flag
# (extension cloak only); the higher rungs add the speculative wewbo-style deep
# demuxer knobs P11 names, to see if any of them incidentally beat the byte
# prefix. --stream-lavf-o mirrors what player.rs always sets for http urls.
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
  name="${RUNG_NAME[$i]}"
  demux="${RUNG_DEMUX[$i]}"
  log="${LOGDIR}/rung-${i}.log"
  printf '%s==> rung %d: %s%s\n' "${bold}" "${i}" "${name}" "${rst}"

  # Bare mpv, NO proxy, no window needed. --no-config so the user's mpv.conf
  # can't taint the result; --msg-level for the demux/ffmpeg chatter we grep.
  timeout "$((PLAY_SECS + 6))" "${MPV}" \
    --no-config --no-audio-display --vo=null --ao=null \
    --msg-level=all=v \
    --user-agent="${UA}" \
    --http-header-fields-append="Referer: ${REFERER}" \
    --stream-lavf-o=multiple_requests=1,icy=0 \
    --demuxer-lavf-o="${demux}" \
    --length="${PLAY_SECS}" \
    "${URL}" >"${log}" 2>&1
  rc=$?

  # Verdict for this rung: a real video track + advancing playback = PLAYED.
  played_video=0; advanced=0
  grep -Eiq 'VO:|Video --vid|opening video decoder|--- video ---|Selected video codec' "${log}" && played_video=1
  # A time-pos line > 0.5s anywhere (mpv -v prints AV/A: lines with the clock).
  if grep -Eoq '(AV?|A|V): *0*[1-9][0-9]*[.:]' "${log}" \
     || grep -Eq 'V: +0*[1-9]' "${log}"; then advanced=1; fi
  # Hard demux failures the decoy produces.
  failed_demux=0
  grep -Eiq 'Failed to (recognize|open) file format|could not open codec|Invalid data found|no (video|audio) or video|Failed to open|demuxer: no' "${log}" && failed_demux=1

  if [ "${played_video}" = 1 ] && [ "${advanced}" = 1 ] && [ "${failed_demux}" = 0 ]; then
    printf '%s    ✓ PLAYED — this rung beat the decoy (proxy-less)%s\n' "${grn}" "${rst}"
    printf '%s      → verdict (a) candidate: skip P12, set decloak handling to this flag-set%s\n' "${grn}" "${rst}"
    overall="PLAYED@rung-${i}"
    # Keep going: record all rungs, but the first PLAYED is the headline.
    [ "${overall}" = "PLAYED@rung-${i}" ] && break
  elif [ "${failed_demux}" = 1 ]; then
    printf '%s    ✗ FAILED — demux/open error; decoy survived (rc=%s)%s\n' "${red}" "${rc}" "${rst}"
    printf '%s      %s%s\n' "${dim}" "$(grep -Eim1 'Failed to|Invalid data|could not open' "${log}" || echo '(see log)')" "${rst}"
  else
    printf '%s    ? INCONCLUSIVE — no clear video/advance and no explicit demux error (rc=%s); read %s%s\n' "${yel}" "${rc}" "${log}" "${rst}"
  fi
  printf '%s      log: %s%s\n' "${dim}" "${log}" "${rst}"
done

printf '\n%s%s============================================================%s\n' "${bold}" "" "${rst}"
case "${overall}" in
  PLAYED@*)
    printf '%s%sSPIKE RESULT: %s → VERDICT (a): a demuxer flag-set survives the decoy.%s\n' "${grn}" "${bold}" "${overall}" "${rst}"
    printf '%s  P12 (proxy) may be SKIPPED; P13 sets decloak_segments handling to that flag-set.%s\n' "${grn}" "${rst}"
    printf '%s  Paste the winning rung + its log excerpt into NOTES.md P11.%s\n' "${grn}" "${rst}"
    ;;
  *)
    printf '%s%sSPIKE RESULT: ALL RUNGS FAILED → VERDICT (b): knobs do NOT beat the byte-prefix decoy.%s\n' "${red}" "${bold}" "${rst}"
    printf '%s  Port the byte-strip proxy as planned (P12), exactly as the Rust reference does.%s\n' "${red}" "${rst}"
    printf '%s  This confirms the static prediction. Paste %s/*.log excerpts into NOTES.md P11.%s\n' "${red}" "${LOGDIR}" "${rst}"
    ;;
esac
printf '%slogs: %s/%s\n' "${dim}" "${LOGDIR}" "${rst}"
