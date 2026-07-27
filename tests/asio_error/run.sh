#!/usr/bin/env bash
# Run the asio_error test under a throwaway wineprefix.
#
# The probe is started with XDG_RUNTIME_DIR / PIPEWIRE_RUNTIME_DIR pointed
# at a non-existent path, which forces audio_open() to fail.  That lets the
# test assert that GetErrorMessage() reports the concrete init failure.
#
# On kron4ek/proton runner layouts bin/wine is the 32-bit loader, so prefer
# wine64 for the 64-bit probe (same rule as pipeasio-register, issue #9).
# Registration is silent: regsvr32 /s avoids the success MessageBox.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
probe="${here}/asio_error.exe.so"
[[ -x "$probe" ]] || { echo "asio_error not built: $probe"; exit 1; }

: "${PROBE_PREFIX:=$HOME/.cache/pipeasio-error}"
: "${PIPEASIO_ROOT:=$HOME/.local}"
: "${WINEDEBUG:=-all,+pipeasio,err+all}"

WINE_BIN="$(command -v wine64 || command -v wine)"
[[ -n "$WINE_BIN" ]] || { echo "[run] SKIP: wine64/wine not found"; exit 77; }

# --- preconditions (77 = CTest SKIP) -----------------------------------------
[[ -f "${PIPEASIO_ROOT}/lib/wine/x86_64-unix/pipeasio.dll.so" ]] \
    || { echo "[run] SKIP: driver not installed under $PIPEASIO_ROOT (cmake --install)"; exit 77; }

if [[ -n "${FRESH:-}" ]]; then
    echo "[run] wiping prefix $PROBE_PREFIX"
    rm -rf -- "$PROBE_PREFIX"
fi

mkdir -p "$PROBE_PREFIX"

export WINEPREFIX="$PROBE_PREFIX"
export WINEDLLPATH="${PIPEASIO_ROOT}/lib/wine"
export WINEDEBUG

# Bootstrap the prefix on first run with the 64-bit loader; a 32-bit
# wineboot would create a prefix regsvr32 can't use for pipeasio64.dll.
if [[ ! -d "$PROBE_PREFIX/drive_c" ]]; then
    echo "[run] creating wineprefix at $PROBE_PREFIX"
    "$WINE_BIN" reg query 'HKLM\Software\Wine' >/dev/null 2>&1 || true
fi

# Register PipeASIO silently if not already.
if ! "$WINE_BIN" reg query 'HKLM\Software\ASIO\PipeASIO' >/dev/null 2>&1; then
    echo "[run] registering PipeASIO in $PROBE_PREFIX"
    cp -v --no-preserve mode \
        "${PIPEASIO_ROOT}/lib/wine/x86_64-windows/pipeasio64.dll" \
        "$PROBE_PREFIX/drive_c/windows/system32/"
    "$WINE_BIN" regsvr32 /s pipeasio64.dll \
        || { echo "[run] regsvr32 failed"; exit 1; }
fi

# Hermetic config: shield the run from the user's ~/.config/pipeasio/config.ini.
export XDG_CONFIG_HOME="$PROBE_PREFIX/xdg"

echo "[run] wine:      $WINE_BIN"
echo "[run] prefix:    $WINEPREFIX"
echo "[run] dllpath:   $WINEDLLPATH"
echo "[run] WINEDEBUG: $WINEDEBUG"
echo "[run] starting error-message probe (PipeWire intentionally broken)..."
echo "---"

# Force audio_open() to fail: point PipeWire at a non-existent runtime dir.
# Set only for the probe so registration above still works.
export XDG_RUNTIME_DIR="/nonexistent"
export PIPEWIRE_RUNTIME_DIR="/nonexistent"

exec "$WINE_BIN" "$probe"
