#!/usr/bin/env bash
# Run the asio_probe under a throwaway wineprefix in /tmp.
#
# Usage:  tests/asio_probe/run.sh            # 5s default
#         tests/asio_probe/run.sh 10         # 10s
#         FRESH=1 tests/asio_probe/run.sh    # destroy + recreate prefix
#
# Env knobs:
#   PIPEWIRE_DEBUG : forwarded to the daemon's client side (default 2)
#   WINEDEBUG      : forwarded to Wine (default -all,+pipeasio,err+all)
#   PROBE_PREFIX   : wineprefix to use (default /tmp/pipeasio-probe)
#   PIPEASIO_ROOT  : install root for the .so (default $HOME/.local)

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
probe="${here}/asio_probe.exe.so"
[[ -x "$probe" ]] || { echo "asio_probe not built: $probe"; exit 1; }

seconds="${1:-5}"
# Wine 11+ refuses to create config dirs under /tmp because of /tmp's
# permissive group/world bits - "not owned by you, refusing to create
# a configuration directory there".  Park the throwaway prefix under
# $HOME instead.
: "${PROBE_PREFIX:=$HOME/.cache/pipeasio-probe}"
if [[ -n "${PIPEASIO_ROOT:-}" && -n "${PIPEASIO_PREFIX:-}"
      && "$PIPEASIO_ROOT" != "$PIPEASIO_PREFIX" ]]; then
    echo "[run] PIPEASIO_ROOT and PIPEASIO_PREFIX must name the same install" >&2
    exit 1
fi
: "${PIPEASIO_ROOT:=${PIPEASIO_PREFIX:-$HOME/.local}}"
PIPEASIO_PREFIX="$PIPEASIO_ROOT"
export PIPEASIO_ROOT PIPEASIO_PREFIX
: "${PIPEWIRE_DEBUG:=2}"
: "${WINEDEBUG:=-all,+pipeasio,err+all}"

# --- preconditions (77 = CTest SKIP) -----------------------------------------
for tool in wine pw-cli; do
    command -v "$tool" >/dev/null || { echo "[run] SKIP: $tool not found"; exit 77; }
done
# run_err.sh deliberately uses an unreachable PipeWire remote.
if [[ "${PROBE_EXPECT_DEAD_DAEMON:-0}" != 1 ]]; then
    pw-cli info 0 >/dev/null 2>&1 || { echo "[run] SKIP: no PipeWire daemon"; exit 77; }
fi
# Unix half: pipeasio64.so (PE + unixlib) or pipeasio64.dll.so (hybrid).
_installed_so="${PIPEASIO_ROOT}/lib/wine/x86_64-unix/pipeasio64.so"
[[ -f "$_installed_so" ]] || _installed_so="${PIPEASIO_ROOT}/lib/wine/x86_64-unix/pipeasio64.dll.so"
[[ -f "$_installed_so" ]] \
    || { echo "[run] SKIP: driver not installed under $PIPEASIO_ROOT (cmake --install)"; exit 77; }

if [[ -n "${FRESH:-}" ]]; then
    echo "[run] wiping prefix $PROBE_PREFIX"
    rm -rf -- "$PROBE_PREFIX"
fi

mkdir -p "$PROBE_PREFIX"

export WINEPREFIX="$PROBE_PREFIX"
# No .NET or Gecko; the Wine Mono prompt would block an unattended run.
export WINEDLLOVERRIDES="mscoree,mshtml=${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export WINEDLLPATH="${PIPEASIO_ROOT}/lib/wine"
export WINEDEBUG
export PIPEWIRE_DEBUG

# Preload sanitizer runtimes before Wine loads an instrumented Unix half.
_sanitized=0
_imports="$(nm -D --undefined-only "$_installed_so" 2>/dev/null || true)"
if grep -q '__asan_init' <<<"$_imports"; then
    _sanitized=1
    _asan="$(gcc -print-file-name=libasan.so)"
    _ubsan="$(gcc -print-file-name=libubsan.so)"
    if [[ ! -f "$_asan" || ! -f "$_ubsan" ]]; then
        echo "[run] instrumented driver requires libasan and libubsan" >&2
        exit 1
    fi
    echo "[run] ASan build detected; enabling fail-fast sanitizer options"
    _sanitize_asan_options="abort_on_error=1:halt_on_error=1:print_stacktrace=1:detect_leaks=0:symbolize=1:verify_asan_link_order=0"
    _sanitize_ubsan_options="halt_on_error=1:print_stacktrace=1"
fi
# CMake substitutes this operand.
# shellcheck disable=SC2050
if [[ "@PIPEASIO_ASAN@" == "ON" && "$_sanitized" != 1 ]]; then
    echo "[run] sanitizer build expected an instrumented installed driver" >&2
    exit 1
fi

# Bootstrap the prefix on first run.
if [[ ! -d "$PROBE_PREFIX/drive_c" ]]; then
    echo "[run] creating wineprefix at $PROBE_PREFIX"
    wineboot --init >/dev/null 2>&1
fi

# Register PipeASIO if not already.
# Re-register when the PE staged in the prefix is not the installed one: a
# prefix registered by an older install keeps a stale copy in system32.
_installed_pe="${PIPEASIO_ROOT}/lib/wine/x86_64-windows/pipeasio64.dll"
if ! wine reg query 'HKLM\Software\ASIO\PipeASIO' >/dev/null 2>&1 \
   || { [[ -e "$_installed_pe" ]] \
        && ! cmp -s "$_installed_pe" "$PROBE_PREFIX/drive_c/windows/system32/pipeasio64.dll"; }; then
    echo "[run] registering PipeASIO in $PROBE_PREFIX"
    if [[ "$_sanitized" == 1 ]]; then
        PIPEASIO_REGISTER_WITHOUT_LOADING=1 \
            "${PIPEASIO_ROOT}/bin/pipeasio-register" \
            || { echo "[run] pipeasio-register failed"; exit 1; }
    else
        "${PIPEASIO_ROOT}/bin/pipeasio-register" \
            || { echo "[run] pipeasio-register failed"; exit 1; }
    fi
fi

# Hermetic config: shield the run from the user's ~/.config/pipeasio/config.ini
# and keep ports isolated (no audible feedback).  PROBE_AUTOCONNECT=1 re-enables.
# The driver also reads PIPEASIO_* env directly, so scrub any ambient values:
# only names listed in PROBE_ENV_KEEP (used by run_rt.sh) survive.
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
    echo "[run] autoconnect disabled (set PROBE_AUTOCONNECT=1 to re-enable)"
fi

echo "[run] prefix:    $WINEPREFIX"
echo "[run] dllpath:   $WINEDLLPATH"
echo "[run] PW debug:  $PIPEWIRE_DEBUG"
echo "[run] WINEDEBUG: $WINEDEBUG"
echo "[run] starting probe (${seconds}s)..."
echo "---"

if [[ "$_sanitized" == 1 ]]; then
    unset LD_PRELOAD
    export ASAN_OPTIONS="$_sanitize_asan_options"
    export UBSAN_OPTIONS="$_sanitize_ubsan_options"
fi
exec wine "$probe" "$seconds"
