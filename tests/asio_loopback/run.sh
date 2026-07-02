#!/usr/bin/env bash
# Run the asio_loopback analyzer under a throwaway wineprefix with a
# PipeWire null-sink loopback:
#
#     PipeASIO out_1/out_2 -> null sink playback_FL/FR
#     null sink monitor_FL/FR -> PipeASIO in_1/in_2
#
# Usage:  tests/asio_loopback/run.sh            # one phase, preferred size
#         tests/asio_loopback/run.sh 10         # 10 s measurement window
#         SIZES="128 256 512 1024" tests/asio_loopback/run.sh
#         SWEEP=1 tests/asio_loopback/run.sh    # sizes x rates matrix
#         FRESH=1 tests/asio_loopback/run.sh    # destroy + recreate prefix
#
# Env knobs:
#   PROBE_PREFIX   : wineprefix (default $HOME/.cache/pipeasio-probe)
#   PIPEASIO_ROOT  : install root for the .so (default $HOME/.local)
#   SIZES          : space-separated ASIO buffer sizes (default "0" = preferred)
#   RATES          : space-separated forced rates, 0 = follow graph (default "0")
#   SWEEP=1        : SIZES="128 256 512 1024", RATES="0 44100 96000"
#
# Exit: 0 pass, 2 fail, 77 skip (no PipeWire daemon / missing tools).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
probe="${here}/asio_loopback.exe.so"
[[ -x "$probe" ]] || { echo "asio_loopback not built: $probe"; exit 1; }

seconds="${1:-6}"
: "${PROBE_PREFIX:=$HOME/.cache/pipeasio-probe}"
: "${PIPEASIO_ROOT:=$HOME/.local}"
: "${WINEDEBUG:=-all,err+all}"
: "${SIZES:=0}"
: "${RATES:=0}"
if [[ -n "${SWEEP:-}" ]]; then
    SIZES="128 256 512 1024"
    RATES="0 44100 96000"
fi

node="pipeasio-loopback"
sink="pipeasio-loop-sink"

# --- preconditions ----------------------------------------------------------
for tool in pw-cli pw-link wine; do
    command -v "$tool" >/dev/null || { echo "[loop] SKIP: $tool not found"; exit 77; }
done
pw-cli info 0 >/dev/null 2>&1 || { echo "[loop] SKIP: no PipeWire daemon"; exit 77; }
[[ -f "${PIPEASIO_ROOT}/lib/wine/x86_64-unix/pipeasio.dll.so" ]] \
    || { echo "[loop] SKIP: driver not installed under $PIPEASIO_ROOT (cmake --install)"; exit 77; }

if [[ -n "${FRESH:-}" ]]; then
    echo "[loop] wiping prefix $PROBE_PREFIX"
    rm -rf -- "$PROBE_PREFIX"
fi
mkdir -p "$PROBE_PREFIX"

export WINEPREFIX="$PROBE_PREFIX"
export WINEDLLPATH="${PIPEASIO_ROOT}/lib/wine"
export WINEDEBUG

if [[ ! -d "$PROBE_PREFIX/drive_c" ]]; then
    echo "[loop] creating wineprefix at $PROBE_PREFIX"
    wineboot --init >/dev/null 2>&1
fi
if ! wine reg query 'HKLM\Software\ASIO\PipeASIO' >/dev/null 2>&1; then
    echo "[loop] registering PipeASIO in $PROBE_PREFIX"
    "${PIPEASIO_ROOT}/bin/pipeasio-register" \
        || { echo "[loop] pipeasio-register failed"; exit 1; }
fi

# Hermetic config: env overrides shield the run from the user's
# ~/.config/pipeasio/config.ini (e.g. follow_device_clock pins the
# buffer size in GetBufferSize).  Stable node name for pw-link.
export PIPEASIO_CLIENT_NAME="$node"
export PIPEASIO_CONNECT_TO_HARDWARE=off
export PIPEASIO_FIXED_BUFFERSIZE=off
export PIPEASIO_FOLLOW_DEVICE_CLOCK=off
export PIPEASIO_NUMBER_INPUTS=2
export PIPEASIO_NUMBER_OUTPUTS=2

# --- loopback plumbing -------------------------------------------------------
cleanup() {
    [[ -n "${linker_pid:-}" ]] && kill "$linker_pid" 2>/dev/null || true
    [[ -n "${sink_pid:-}"   ]] && kill "$sink_pid"   2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

# Null sink: pw-cli -m keeps running so the node lives until cleanup.
pw-cli -m create-node adapter \
    "{ factory.name=support.null-audio-sink node.name=$sink media.class=Audio/Sink \
       object.linger=false audio.position=[FL FR] monitor.channel-volumes=false \
       node.description=\"PipeASIO loopback sink\" }" >/dev/null 2>&1 &
sink_pid=$!

# Linker watch-loop: the driver's ports (re)appear per phase as buffers are
# re-created, so just keep trying; pw-link is idempotent ("File exists").
(
    while :; do
        pw-link "$node:out_1" "$sink:playback_FL" 2>/dev/null || true
        pw-link "$node:out_2" "$sink:playback_FR" 2>/dev/null || true
        pw-link "$sink:monitor_FL" "$node:in_1"   2>/dev/null || true
        pw-link "$sink:monitor_FR" "$node:in_2"   2>/dev/null || true
        sleep 0.25
    done
) &
linker_pid=$!

# --- run ----------------------------------------------------------------------
overall=0
for rate in $RATES; do
    export PIPEASIO_SAMPLE_RATE="$rate"
    echo "[loop] === rate=$rate (0 = follow graph), sizes: $SIZES ==="
    # shellcheck disable=SC2086
    if ! wine "$probe" "$seconds" $SIZES; then
        overall=2
    fi
done

exit "$overall"
