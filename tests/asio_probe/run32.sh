#!/usr/bin/env bash
# 32-bit analogue of run.sh.  Requires an installed BUILD_WOW64_32 build.
#
# Usage: run32.sh [seconds]
# Env: FRESH=1, PIPEASIO_PREFIX, PROBE_PREFIX, PROBE_AUTOCONNECT=1

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
probe="${here}/asio_probe32.exe"
[[ -f "$probe" ]] || { echo "asio_probe32 not built: $probe"; exit 1; }

seconds="${1:-5}"
: "${PIPEASIO_PREFIX:=$HOME/.local}"
: "${PROBE_PREFIX:=$HOME/.cache/pipeasio-probe32}"
: "${WINEDEBUG:=-all,+pipeasio,err+all}"

if [[ ! -e "${PIPEASIO_PREFIX}/lib/wine/i386-windows/pipeasio32.dll" \
      || ! -e "${PIPEASIO_PREFIX}/lib/wine/x86_64-unix/pipeasio32.so" ]]; then
    echo "[run32] pipeasio32 not installed under ${PIPEASIO_PREFIX}"
    echo "[run32] build with -DBUILD_WOW64_32=ON and 'cmake --install' first - skipping"
    exit 77
fi

export WINEPREFIX="$PROBE_PREFIX"
export PIPEASIO_PREFIX
export WINEDEBUG
export WINEDLLPATH="${PIPEASIO_PREFIX}/lib/wine"

if [[ -n "${FRESH:-}" ]]; then
    echo "[run32] wiping prefix $PROBE_PREFIX"
    rm -rf -- "$PROBE_PREFIX"
fi
mkdir -p "$PROBE_PREFIX"

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
    "${PIPEASIO_PREFIX}/bin/pipeasio-register" \
        || { echo "[run32] pipeasio-register failed"; exit 1; }
fi

# Hermetic config: shield the run from the user's ~/.config/pipeasio/config.ini
# and keep ports isolated (the driver reads INI + env, never the registry).
export XDG_CONFIG_HOME="$PROBE_PREFIX/xdg"
if [[ "${PROBE_AUTOCONNECT:-0}" != "1" ]]; then
    export PIPEASIO_CONNECT_TO_HARDWARE=off
    echo "[run32] autoconnect disabled (set PROBE_AUTOCONNECT=1 to re-enable)"
fi

echo "[run32] prefix:    $WINEPREFIX"
echo "[run32] dllpath:   $WINEDLLPATH"
echo "[run32] starting 32-bit probe (${seconds}s)..."
echo "---"
exec wine "$probe" "$seconds"
