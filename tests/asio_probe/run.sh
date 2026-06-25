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
: "${PIPEASIO_ROOT:=$HOME/.local}"
: "${PIPEWIRE_DEBUG:=2}"
: "${WINEDEBUG:=-all,+pipeasio,err+all}"

if [[ -n "${FRESH:-}" ]]; then
    echo "[run] wiping prefix $PROBE_PREFIX"
    rm -rf -- "$PROBE_PREFIX"
fi

mkdir -p "$PROBE_PREFIX"

export WINEPREFIX="$PROBE_PREFIX"
export WINEDLLPATH="${PIPEASIO_ROOT}/lib/wine"
export WINEDEBUG
export PIPEWIRE_DEBUG

# Detect AddressSanitizer-instrumented build (Debug config wires
# -fsanitize=address into the .o files; symbols like __asan_init then
# appear in the .so).  If present, LD_PRELOAD libasan so its runtime
# initializes before Wine loads the DLL.
_installed_so="${PIPEASIO_ROOT}/lib/wine/x86_64-unix/pipeasio.dll.so"
if [[ -f "$_installed_so" ]] \
   && nm -D "$_installed_so" 2>/dev/null | grep -q '__asan_init'; then
    _asan="$(gcc -print-file-name=libasan.so)"
    _ubsan="$(gcc -print-file-name=libubsan.so)"
    if [[ -f "$_asan" ]]; then
        echo "[run] ASan build detected, preloading $_asan"
        if [[ -f "$_ubsan" ]]; then
            export LD_PRELOAD="${_asan}:${_ubsan}${LD_PRELOAD:+:$LD_PRELOAD}"
        else
            export LD_PRELOAD="${_asan}${LD_PRELOAD:+:$LD_PRELOAD}"
        fi
        # halt_on_error=0 collects all findings; detect_leaks=0 because
        # Wine's exit path produces too much leak noise to be useful.
        export ASAN_OPTIONS="abort_on_error=0:halt_on_error=0:print_stacktrace=1:detect_leaks=0:symbolize=1:verify_asan_link_order=0"
        export UBSAN_OPTIONS="print_stacktrace=1"
    fi
fi

# Bootstrap the prefix on first run.
if [[ ! -d "$PROBE_PREFIX/drive_c" ]]; then
    echo "[run] creating wineprefix at $PROBE_PREFIX"
    wineboot --init >/dev/null 2>&1
fi

# Register PipeASIO if not already.
if ! wine reg query 'HKCU\Software\ASIO\PipeASIO' >/dev/null 2>&1; then
    echo "[run] registering PipeASIO in $PROBE_PREFIX"
    "${PIPEASIO_ROOT}/bin/pipeasio-register" \
        || { echo "[run] pipeasio-register failed"; exit 1; }
fi

# Suppress audible feedback during testing: setting "Connect to hardware" to 0
# tells PipeASIO not to autoconnect inputs/outputs to default source/sink, so
# the probe loads with isolated ports.  qpwgraph can still be used to wire
# things up manually if needed.  Override with PROBE_AUTOCONNECT=1.
if [[ "${PROBE_AUTOCONNECT:-0}" != "1" ]]; then
    if wine reg add 'HKCU\Software\Wine\PipeASIO' /v 'Connect to hardware' \
           /t REG_DWORD /d 0 /f 2>&1 | grep -q "completed successfully"; then
        echo "[run] autoconnect disabled (set PROBE_AUTOCONNECT=1 to re-enable)"
    else
        echo "[run] WARNING: failed to disable autoconnect"
    fi
fi

echo "[run] prefix:    $WINEPREFIX"
echo "[run] dllpath:   $WINEDLLPATH"
echo "[run] PW debug:  $PIPEWIRE_DEBUG"
echo "[run] WINEDEBUG: $WINEDEBUG"
echo "[run] starting probe (${seconds}s)..."
echo "---"

exec wine "$probe" "$seconds"
