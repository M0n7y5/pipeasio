#!/usr/bin/env bash
# Run the probe, let it crash, then dump the per-thread backtrace from
# the systemd-coredump capture.
#
# Why this path: live-attach via winedbg --gdb only shows PE DLLs, not
# the Linux libc / ntdll-unix / pipewire / pipeasio.dll.so halves where
# the smash actually lives.  systemd-coredump captures the abort, and
# `coredumpctl info` resolves symbols (including OUR static functions
# in pipeasio.dll.so, via DWARF in the unstripped Debug build) for every
# thread.  Zero gdb gymnastics.
#
# Usage:
#   tests/asio_probe/gdb.sh                  # run + summarize core
#   tests/asio_probe/gdb.sh gdb              # drop into gdb on the core
#   tests/asio_probe/gdb.sh last             # don't re-run, just inspect
#                                             the most recent core
#
# Env knobs (in addition to those run.sh honors):
#   PROBE_SECONDS  : seconds arg passed to the probe (default 3)

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
probe="${here}/asio_probe.exe.so"
[[ -x "$probe" ]] || { echo "asio_probe not built: $probe"; exit 1; }

mode="${1:-summary}"
seconds="${PROBE_SECONDS:-3}"
# Wine 11+ refuses to create config dirs in /tmp ("not owned by you,
# refusing to create a configuration directory there"), so we land the
# throwaway prefix under $HOME by default.
: "${PROBE_PREFIX:=$HOME/.cache/pipeasio-probe}"
: "${PIPEASIO_ROOT:=$HOME/.local}"
: "${PIPEWIRE_DEBUG:=2}"
: "${WINEDEBUG:=-all,+pipeasio,err+all}"

# Run the probe unless told to inspect the previous core.
if [[ "$mode" != "last" ]]; then
    export WINEPREFIX="$PROBE_PREFIX"
    export WINEDLLPATH="${PIPEASIO_ROOT}/lib/wine"
    export WINEDEBUG
    export PIPEWIRE_DEBUG
    ulimit -c unlimited

    if [[ ! -d "$PROBE_PREFIX/drive_c" ]]; then
        echo "[gdb] creating wineprefix at $PROBE_PREFIX"
        wineboot --init >/dev/null 2>&1
    fi
    if ! wine reg query 'HKLM\Software\ASIO\PipeASIO' >/dev/null 2>&1; then
        "${PIPEASIO_ROOT}/bin/pipeasio-register" \
            || { echo "[gdb] pipeasio-register failed"; exit 1; }
    fi
    # Hermetic config: shield the run from the user's config.ini and keep
    # ports isolated (the driver reads INI + env, never the registry).
    export XDG_CONFIG_HOME="$PROBE_PREFIX/xdg"
    export PIPEASIO_CONNECT_TO_HARDWARE=off

    echo "[gdb] running probe (${seconds}s, expect it to crash)..."
    wine "$probe" "$seconds" 2>/tmp/probe.err >/dev/null || true
    sleep 1  # let systemd-coredump finish writing the core
fi

# Find the latest SIGABRT core under wine-preloader.  coredumpctl list
# doesn't print the COMM column, so we can't directly grep for
# "asio_probe" - but every asio_probe crash aborts under wine-preloader,
# and SIGABRT cores from other wine-preloader programs in the past hour
# would be very unusual (and we cross-check the command line below).
crash_pid="$(coredumpctl list 2>/dev/null \
    | awk '/SIGABRT.*wine-preloader/ {pid=$5} END {print pid}')"
if [[ -z "$crash_pid" ]]; then
    echo "[gdb] no SIGABRT core under wine-preloader.  Did the probe crash?"
    echo "[gdb] (last 5 lines of /tmp/probe.err:)"
    tail -5 /tmp/probe.err 2>/dev/null
    exit 1
fi

# Cross-check: confirm the core belongs to asio_probe.
if ! coredumpctl info "$crash_pid" 2>/dev/null \
        | grep -q "Command Line:.*asio_probe"; then
    echo "[gdb] WARNING: latest SIGABRT wine-preloader pid $crash_pid"
    echo "[gdb]          isn't asio_probe - running:"
    coredumpctl info "$crash_pid" 2>/dev/null | grep "Command Line:"
fi

echo "[gdb] inspecting core for pid $crash_pid"
echo "---"

if [[ "$mode" == "gdb" ]]; then
    exec coredumpctl gdb "$crash_pid"
fi

# Default summary mode: show coredumpctl info's resolved per-thread
# backtraces.  All symbols (libc, ntdll-unix, libpipewire, libspa, and
# OUR pipeasio64.dll.so static functions via DWARF) resolve here.
coredumpctl info "$crash_pid" \
    | sed -n '/Stack trace of thread/,$p'
