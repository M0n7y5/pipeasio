#!/usr/bin/env bash
# 32-bit analogue of run.sh.  Requires an installed BUILD_WOW64_32 build.
#
# Usage: run32.sh [seconds]
# Env: FRESH=1, PIPEASIO_PREFIX or PIPEASIO_ROOT, PROBE_PREFIX,
#      PROBE_AUTOCONNECT=1

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
probe="${here}/asio_probe32.exe"
[[ -f "$probe" ]] || { echo "asio_probe32 not built: $probe"; exit 1; }

seconds="${1:-5}"
if [[ -n "${PIPEASIO_ROOT:-}" && -n "${PIPEASIO_PREFIX:-}"
      && "$PIPEASIO_ROOT" != "$PIPEASIO_PREFIX" ]]; then
    echo "[run32] PIPEASIO_ROOT and PIPEASIO_PREFIX must name the same install" >&2
    exit 1
fi
: "${PIPEASIO_PREFIX:=${PIPEASIO_ROOT:-$HOME/.local}}"
PIPEASIO_ROOT="$PIPEASIO_PREFIX"
: "${PROBE_PREFIX:=$HOME/.cache/pipeasio-probe32}"
: "${WINEDEBUG:=-all,+pipeasio,err+all}"

if [[ ! -e "${PIPEASIO_PREFIX}/lib/wine/i386-windows/pipeasio32.dll" \
      || ! -e "${PIPEASIO_PREFIX}/lib/wine/x86_64-unix/pipeasio32.so" ]]; then
    echo "[run32] pipeasio32 not installed under ${PIPEASIO_PREFIX}"
    echo "[run32] build with -DBUILD_WOW64_32=ON and 'cmake --install' first - skipping"
    exit 77
fi

export WINEPREFIX="$PROBE_PREFIX"
# No .NET or Gecko; the Wine Mono prompt would block an unattended run.
export WINEDLLOVERRIDES="mscoree,mshtml=${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export PIPEASIO_PREFIX
export PIPEASIO_ROOT
export WINEDEBUG
export WINEDLLPATH="${PIPEASIO_PREFIX}/lib/wine"

if [[ -n "${FRESH:-}" ]]; then
    echo "[run32] wiping prefix $PROBE_PREFIX"
    rm -rf -- "$PROBE_PREFIX"
fi
mkdir -p "$PROBE_PREFIX"
_installed_so="${PIPEASIO_ROOT}/lib/wine/x86_64-unix/pipeasio32.so"
_sanitized=0
_imports="$(nm -D --undefined-only "$_installed_so" 2>/dev/null || true)"
if grep -q '__asan_init' <<<"$_imports"; then
    _sanitized=1
    _asan="$(gcc -print-file-name=libasan.so)"
    _ubsan="$(gcc -print-file-name=libubsan.so)"
    if [[ ! -f "$_asan" || ! -f "$_ubsan" ]]; then
        echo "[run32] instrumented driver requires libasan and libubsan" >&2
        exit 1
    fi
    echo "[run32] ASan build detected; enabling fail-fast sanitizer options"
    _sanitize_asan_options="abort_on_error=1:halt_on_error=1:print_stacktrace=1:detect_leaks=0:symbolize=1:verify_asan_link_order=0"
    _sanitize_ubsan_options="halt_on_error=1:print_stacktrace=1"
fi
# CMake substitutes this operand.
# shellcheck disable=SC2050
if [[ "@PIPEASIO_ASAN@" == "ON" && "$_sanitized" != 1 ]]; then
    echo "[run32] sanitizer build expected an instrumented installed driver" >&2
    exit 1
fi

# Pre-warm the prefix before regsvr32.
if [[ ! -d "$PROBE_PREFIX/drive_c" ]]; then
    echo "[run32] bootstrapping wineprefix at $PROBE_PREFIX ..."
    WINEDEBUG=-all wineboot --init >/dev/null 2>&1 || true
    wineserver -w || true
fi

# Register the 32-bit CLSID view on first run.
if ! wine reg query \
        'HKCR\CLSID\{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}\InprocServer32' \
        /reg:32 >/dev/null 2>&1; then
    echo "[run32] registering PipeASIO (64- + 32-bit views) in $PROBE_PREFIX"
    if [[ "$_sanitized" == 1 ]]; then
        PIPEASIO_REGISTER_WITHOUT_LOADING=1 \
            "${PIPEASIO_PREFIX}/bin/pipeasio-register" \
            || { echo "[run32] pipeasio-register failed"; exit 1; }
    else
        "${PIPEASIO_PREFIX}/bin/pipeasio-register" \
            || { echo "[run32] pipeasio-register failed"; exit 1; }
    fi
fi

# Hermetic config: shield the run from the user's ~/.config/pipeasio/config.ini
# and keep ports isolated (the driver reads INI + env, never the registry).
# Scrub ambient PIPEASIO_* config env too. PROBE_ENV_KEEP lists intentional
# pass-throughs (used by run_rt.sh).
for _cfg_var in PIPEASIO_NUMBER_INPUTS PIPEASIO_NUMBER_OUTPUTS \
                PIPEASIO_FIXED_BUFFERSIZE PIPEASIO_FOLLOW_DEVICE_CLOCK \
                PIPEASIO_PREFERRED_BUFFERSIZE PIPEASIO_SAMPLE_RATE \
                PIPEASIO_OUTPUT_DEVICE PIPEASIO_INPUT_DEVICE \
                PIPEASIO_CLIENT_NAME PIPEASIO_RT_PRIORITY \
                PIPEASIO_CONNECT_TO_HARDWARE; do
    case " ${PROBE_ENV_KEEP:-} " in
        *" $_cfg_var "*) ;;
        *) unset "$_cfg_var" ;;
    esac
done
for _probe_var in PROBE_XRUN_ARM_FILE PROBE_XRUN_STALL_MS PROBE_RT_WORKER \
                  PROBE_RT_SCAN_FILE PROBE_STOP_INTERLEAVE PROBE_GATE_BARRIER; do
    case " ${PROBE_ENV_KEEP:-} " in
        *" $_probe_var "*) ;;
        *) unset "$_probe_var" ;;
    esac
done
export XDG_CONFIG_HOME="$PROBE_PREFIX/xdg"
if [[ "${PROBE_AUTOCONNECT:-0}" != "1" ]]; then
    export PIPEASIO_CONNECT_TO_HARDWARE=off
    echo "[run32] autoconnect disabled (set PROBE_AUTOCONNECT=1 to re-enable)"
fi

echo "[run32] prefix:    $WINEPREFIX"
echo "[run32] dllpath:   $WINEDLLPATH"
echo "[run32] starting 32-bit probe (${seconds}s)..."
echo "---"
if [[ "$_sanitized" == 1 ]]; then
    unset LD_PRELOAD
    export ASAN_OPTIONS="$_sanitize_asan_options"
    export UBSAN_OPTIONS="$_sanitize_ubsan_options"
fi
exec wine "$probe" "$seconds"
