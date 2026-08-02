#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Verify failed ASIOInit reports PipeWire without crashing.
set -euo pipefail

runner=${1:?usage: run_err.sh <run.sh|run32.sh>}

if [[ ! -x "$runner" ]]; then
    echo "[err] runner unavailable: $runner" >&2
    exit 77
fi
for tool in wine pw-cli; do
    command -v "$tool" >/dev/null || { echo "[err] $tool unavailable"; exit 77; }
done
# The host daemon must exist; only the probe's view of it is poisoned.
pw-cli info 0 >/dev/null 2>&1 || { echo "[err] no PipeWire daemon"; exit 77; }

export PROBE_EXPECT_DEAD_DAEMON=1
export PIPEWIRE_REMOTE="pipeasio-dead-probe-$$"

log=$(mktemp)
trap 'rm -f "$log"' EXIT

status=0
"$runner" 1 >"$log" 2>&1 || status=$?
cat "$log"

if (( status == 77 )); then
    exit 77
fi
if (( status != 1 )); then
    echo "[err] expected the probe's Init failure exit (1), got $status" >&2
    exit 1
fi
if grep -qiE 'Unhandled page fault|Unhandled exception|starting debugger|Backtrace:' "$log"; then
    echo "[err] probe crashed while reporting or releasing the failed Init" >&2
    exit 1
fi
if ! grep -q '\[probe\] Init failed:' "$log"; then
    echo "[err] probe failed without printing the GetErrorMessage line" >&2
    exit 1
fi
if ! grep 'Init failed:' "$log" | grep -qi 'pipewire'; then
    echo "[err] init failure message is not actionable (does not name PipeWire)" >&2
    exit 1
fi

echo "[err] init failure carries an actionable PipeWire message"
