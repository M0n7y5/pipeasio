#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scheduling-topology and xrun legs for the ASIO probe.
#
# Each policy leg runs PROBE_RT_WORKER=1: bufferSwitch and an ordinary
# CreateThread worker stamp Linux comms. This script reads
# /proc/<probe>/task/<tid>/{comm,sched} while the handoff is live.
#
# Policies are external because Wine services file opens in the wineserver, so
# /proc/thread-self inside a PE thread describes the wineserver.
#
# Usage: run_rt.sh <run.sh|run32.sh> [seconds] [label] [leg...]
#
#   default    no config.ini, no RT env    -> cb SCHED_OTHER, worker SCHED_OTHER
#   config-on  config.ini realtime = 1     -> cb SCHED_FIFO,  worker SCHED_OTHER
#   env-off    same file + RT env off      -> cb SCHED_OTHER, worker SCHED_OTHER
#
# Xruns: pw-top ERR is per pw-top instance, not a global counter.  One long-lived
# `pw-top -b` stream is kept across baseline → stall/window → after, and the
# node is tracked by PipeWire ID within that stream.  The probe is a follower of
# a forced-quantum null sink (manual pw-link, follow-device, no autoconnect).  A
# positive-control phase first proves ERR can move (one-shot callback stall armed
# after profiling). The clean policy legs stay stall-free and assert the window
# delta stays within PROBE_XRUN_TOLERANCE.
#
# Ambient PIPEASIO_RT_PRIORITY wins over the per-leg value (negative control).
set -euo pipefail

SCHED_OTHER=0
SCHED_FIFO=1
cb_comm=pa-probe-cb
worker_comm=pa-probe-wrk
probe_node=pipeasio-rt-probe
sink_node=pipeasio-rt-sink
xrun_tolerance=${PROBE_XRUN_TOLERANCE:-4}
xrun_window=${PROBE_XRUN_WINDOW:-2}

runner=${1:?usage: run_rt.sh <run.sh|run32.sh> [seconds] [label] [leg...]}
seconds=${2:-2}
label=${3:-native}
if (( $# > 3 )); then
    shift 3
    legs=("$@")
else
    legs=(default config-on env-off)
fi

if [[ ! -x "$runner" ]]; then
    echo "[rt] runner unavailable: $runner" >&2
    exit 77
fi
if [[ ! -r /proc/self/sched ]]; then
    echo "[rt] procfs scheduler information unavailable" >&2
    exit 77
fi
for tool in pw-cli pw-top pw-link pw-dump; do
    command -v "$tool" >/dev/null || { echo "[rt] $tool unavailable" >&2; exit 77; }
done
pw-cli info 0 >/dev/null 2>&1 || { echo "[rt] no PipeWire daemon" >&2; exit 77; }
rt_limit=$(ulimit -r)
if [[ "$rt_limit" != unlimited ]] && (( rt_limit == 0 )); then
    echo "[rt] RLIMIT_RTPRIO is zero; cannot prove SCHED_FIFO" >&2
    exit 77
fi
self_policy=$(sed -n 's/^policy[[:space:]]*:[[:space:]]*//p' /proc/self/sched)
if [[ "$self_policy" != "$SCHED_OTHER" ]]; then
    echo "[rt] runner policy is $self_policy, not SCHED_OTHER; re-exec under chrt" >&2
    command -v chrt >/dev/null || { echo "[rt] chrt unavailable" >&2; exit 77; }
    exec chrt --other 0 "$0" "$runner" "$seconds" "$label" "${legs[@]}"
fi

base=${PROBE_PREFIX:-"$HOME/.cache/pipeasio-probe"}
export PROBE_PREFIX="${base}-rt-${label}"
export PIPEASIO_CLIENT_NAME="$probe_node"
export PIPEASIO_FOLLOW_DEVICE_CLOCK=on
# These (plus the per-leg RT_PRIORITY) are deliberate. The runners' ambient-env
# scrub keeps anything not listed here.
export PROBE_ENV_KEEP="PIPEASIO_CLIENT_NAME PIPEASIO_FOLLOW_DEVICE_CLOCK PIPEASIO_RT_PRIORITY PROBE_RT_WORKER PROBE_RT_SCAN_FILE PROBE_XRUN_ARM_FILE PROBE_XRUN_STALL_MS"
# Autoconnect stays off (WoW64 CreateBuffers fails when it is on).
# Manual pw-link + follow-device puts the probe in a profiled follower graph.
config_dir="$PROBE_PREFIX/xdg/pipeasio"
config="$config_dir/config.ini"
rt_env_override=${PIPEASIO_RT_PRIORITY-}

workdir=$(mktemp -d)
probe_pid=
sink_pid=
pwt_pid=
cleanup() {
    if [[ -n "${pwt_pid:-}" ]] && kill -0 "$pwt_pid" 2>/dev/null; then
        kill "$pwt_pid" 2>/dev/null || true
        wait "$pwt_pid" 2>/dev/null || true
    fi
    if [[ -n "${probe_pid:-}" ]] && kill -0 "$probe_pid" 2>/dev/null; then
        kill "$probe_pid" 2>/dev/null || true
        wait "$probe_pid" 2>/dev/null || true
    fi
    if [[ -n "${sink_pid:-}" ]] && kill -0 "$sink_pid" 2>/dev/null; then
        kill "$sink_pid" 2>/dev/null || true
        wait "$sink_pid" 2>/dev/null || true
    fi
    rm -rf -- "$workdir"
}
trap cleanup EXIT

pw-cli -m create-node adapter \
    "{ factory.name=support.null-audio-sink node.name=$sink_node media.class=Audio/Sink \
       object.linger=false audio.position=[FL FR] monitor.channel-volumes=false \
       audio.rate=48000 node.force-quantum=256 \
       node.description=\"PipeASIO rt-leg sink\" }" >/dev/null 2>&1 &
sink_pid=$!
sink_ready=0
for _ in $(seq 1 100); do
    if pw-cli ls Node 2>/dev/null | grep -q "\"$sink_node\""; then
        sink_ready=1
        break
    fi
    sleep 0.05
done
if (( ! sink_ready )); then
    echo "[rt] null sink $sink_node never appeared" >&2
    exit 77
fi

link_probe_ports() {
    pw-link "$probe_node:out_1" "$sink_node:playback_FL" >/dev/null 2>&1 || true
    pw-link "$probe_node:out_2" "$sink_node:playback_FR" >/dev/null 2>&1 || true
}

node_id_by_name() {
    local want=$1
    pw-dump 2>/dev/null | python3 -c '
import json, sys
want = sys.argv[1]
best = None
for o in json.load(sys.stdin):
    info = o.get("info") or {}
    props = info.get("props") or {}
    if props.get("node.name") != want:
        continue
    state = info.get("state") or ""
    nid = o.get("id")
    if state == "running":
        print(nid)
        raise SystemExit(0)
    if best is None:
        best = nid
if best is None:
    raise SystemExit(1)
print(best)
' "$want"
}

# Latest ERR and QUANT for node id from a growing single-instance pw-top -b log.
latest_err_quant() {
    local log=$1 id=$2
    awk -v id="$id" '
        $1 == "S" && $2 == "ID" { next }
        NF >= 10 && ($2 + 0) == (id + 0) {
            err = $9 + 0
            quant = $3 + 0
            seen = 1
        }
        END {
            if (!seen) exit 1
            printf "%d %d\n", err, quant
        }' "$log"
}

start_pwtop() {
    local log=$1
    rm -f -- "$log"
    pw-top -b >"$log" 2>/dev/null &
    pwt_pid=$!
}

stop_pwtop() {
    if [[ -n "${pwt_pid:-}" ]] && kill -0 "$pwt_pid" 2>/dev/null; then
        kill "$pwt_pid" 2>/dev/null || true
        wait "$pwt_pid" 2>/dev/null || true
    fi
    pwt_pid=
}

wait_profiled() {
    local pwt_log=$1 nid_out=$2 sample_out=$3
    local i found_id sample quant
    for ((i = 0; i < 100; i++)); do
        found_id=$(node_id_by_name "$probe_node") || found_id=
        if [[ -n "$found_id" ]] && sample=$(latest_err_quant "$pwt_log" "$found_id"); then
            read -r _ quant <<<"$sample"
            if (( quant > 0 )); then
                printf -v "$nid_out" '%s' "$found_id"
                printf -v "$sample_out" '%s' "$sample"
                return 0
            fi
        fi
        sleep 0.1
    done
    return 1
}


find_probe_pid() {
    local process entries entry task comm
    for process in /proc/[0-9]*; do
        mapfile -d '' -t entries 2>/dev/null <"$process/environ" || continue
        local matching_prefix=0
        for entry in "${entries[@]}"; do
            if [[ "$entry" == "WINEPREFIX=$PROBE_PREFIX" ]]; then
                matching_prefix=1
                break
            fi
        done
        (( matching_prefix )) || continue
        IFS= read -r comm 2>/dev/null <"$process/comm" || continue
        if [[ "$comm" == asio_probe* ]]; then
            printf '%s' "${process##*/}"
            return 0
        fi
        for task in "$process"/task/*; do
            [[ -r "$task/comm" ]] || continue
            IFS= read -r comm 2>/dev/null <"$task/comm" || continue
            if [[ "$comm" == "$cb_comm" || "$comm" == "$worker_comm" || "$comm" == asio_probe* ]]; then
                printf '%s' "${process##*/}"
                return 0
            fi
        done
    done
    return 1
}

find_named_task() {
    local process=$1 want=$2 task comm policy
    for task in "/proc/$process"/task/*; do
        [[ -r "$task/comm" && -r "$task/sched" ]] || continue
        IFS= read -r comm 2>/dev/null <"$task/comm" || continue
        [[ "$comm" == "$want" ]] || continue
        policy=$(sed -n 's/^policy[[:space:]]*:[[:space:]]*//p' "$task/sched")
        [[ -n "$policy" ]] || continue
        printf '%s:%s' "${task##*/}" "$policy"
        return 0
    done
    return 1
}

positive_control_xrun() {
    local log="$workdir/positive.log"
    local pwt_log="$workdir/positive.pwt"
    local arm="$workdir/positive.arm"
    local nid='' before='' sample='' err0=0 q0=0 err=0 max=0 delta=0
    local i

    echo "[rt] positive-control: proving pw-top ERR moves under callback stall"
    rm -f -- "$config"
    unset PIPEASIO_RT_PRIORITY || true
    unset PROBE_RT_WORKER || true
    unset PROBE_RT_SCAN_FILE || true
    # One-shot bufferSwitch stall, armed by creating the arm file only after
    # the pw-top stream is profiling the node.
    rm -f -- "$arm"
    export PROBE_XRUN_ARM_FILE="Z:$arm"
    export PROBE_XRUN_STALL_MS=500

    "$runner" 10 >"$log" 2>&1 &
    probe_pid=$!

    for ((i = 0; i < 400; i++)); do
        if grep -q 'Start OK' "$log" 2>/dev/null; then
            break
        fi
        kill -0 "$probe_pid" 2>/dev/null || break
        sleep 0.05
    done
    if ! grep -q 'Start OK' "$log" 2>/dev/null; then
        local rc=
        if kill -0 "$probe_pid" 2>/dev/null; then
            kill "$probe_pid" 2>/dev/null || true
            wait "$probe_pid" 2>/dev/null || true
        else
            # Propagate the runner's own SKIP (77: no wine, no installed
            # driver) instead of failing - a daemon can be reachable while
            # the Wine side is absent (e.g. a distrobox sharing the host
            # PipeWire socket).
            wait "$probe_pid" 2>/dev/null && rc=0 || rc=$?
        fi
        probe_pid=
        cat "$log"
        if [[ "${rc:-}" == 77 ]]; then
            echo "[rt] positive-control: runner preconditions unmet -> SKIP" >&2
            exit 77
        fi
        echo "[rt] positive-control: probe never reached Start" >&2
        return 1
    fi

    start_pwtop "$pwt_log"
    for ((i = 0; i < 40; i++)); do
        link_probe_ports
        sleep 0.05
    done

    if ! wait_profiled "$pwt_log" nid before; then
        cat "$log"
        stop_pwtop
        echo "[rt] positive-control: node $probe_node not profiled (quant>0)" >&2
        kill "$probe_pid" 2>/dev/null || true
        wait "$probe_pid" 2>/dev/null || true
        probe_pid=
        exit 77
    fi
    read -r err0 q0 <<<"$before"
    max=$err0

    echo "[rt] positive-control: id=$nid baseline err=$err0 quant=$q0; arming stall"
    : >"$arm"

    local stalled=0
    for ((i = 0; i < 200; i++)); do
        if grep -q 'xrun-stall: done' "$log" 2>/dev/null; then
            stalled=1
            break
        fi
        kill -0 "$probe_pid" 2>/dev/null || break
        sleep 0.1
    done

    # Track the maximum ERR across several subsequent tables of the SAME
    # pw-top stream: the counter is per-instance, so one table pair could miss
    # a recovery-cycle reset.
    for ((i = 0; i < 24; i++)); do
        sleep 0.25
        sample=$(latest_err_quant "$pwt_log" "$nid") || continue
        read -r err _ <<<"$sample"
        (( err > max )) && max=$err
    done
    stop_pwtop

    kill "$probe_pid" 2>/dev/null || true
    wait "$probe_pid" 2>/dev/null || true
    probe_pid=
    cat "$log"

    if (( ! stalled )); then
        echo "[rt] positive-control: stall never fired" >&2
        return 1
    fi
    delta=$(( max - err0 ))
    if (( delta <= 0 )); then
        echo "[rt] positive-control: ERR did not rise (baseline=$err0 max=$max id=$nid)" >&2
        return 1
    fi
    echo "[rt] positive-control ok: id=$nid ERR +$delta (baseline $err0 -> max $max, quant $q0)"
}

run_leg() {
    local leg=$1 want_cb=$2 want_worker=$3
    local log="$workdir/$leg.log"
    local ack="$workdir/$leg.scanned"
    local pwt_log="$workdir/$leg.pwt"
    local status=0 pid='' cb='' worker='' i
    local nid='' before='' after='' err0=0 q0=0 err1=0 delta=0
    local tables0=0 tables1=0

    mkdir -p "$config_dir"
    if [[ "$leg" == default ]]; then
        rm -f -- "$config"
    else
        printf '[pipeasio]\nrealtime = 1\n' >"$config"
    fi

    if [[ -n "$rt_env_override" ]]; then
        export PIPEASIO_RT_PRIORITY="$rt_env_override"
    elif [[ "$leg" == env-off ]]; then
        export PIPEASIO_RT_PRIORITY=off
    else
        unset PIPEASIO_RT_PRIORITY
    fi

    export PROBE_RT_WORKER=1
    unset PROBE_XRUN_ARM_FILE || true
    unset PROBE_XRUN_STALL_MS || true
    echo "[rt] leg $leg ($label): running probe for ${seconds}s"
    rm -f -- "$ack"
    export PROBE_RT_SCAN_FILE="Z:${ack}"
    "$runner" "$seconds" >"$log" 2>&1 &
    probe_pid=$!

    local ready=0
    for ((i = 0; i < 1200; i++)); do
        if grep -q 'rt-worker: .*handoff=' "$log" 2>/dev/null; then
            ready=1
            break
        fi
        kill -0 "$probe_pid" 2>/dev/null || break
        sleep 0.05
    done

    if (( ready )); then
        start_pwtop "$pwt_log"
        for ((i = 0; i < 20; i++)); do
            link_probe_ports
            sleep 0.05
        done
        pid=$(find_probe_pid) || pid=
        if [[ -n "$pid" ]]; then
            cb=$(find_named_task "$pid" "$cb_comm") || cb=
            worker=$(find_named_task "$pid" "$worker_comm") || worker=
        fi
        if wait_profiled "$pwt_log" nid before; then
            read -r err0 q0 <<<"$before"
            tables0=$(grep -c 'ID .*QUANT' "$pwt_log" || true)
            sleep "$xrun_window"
            for ((i = 0; i < 40; i++)); do
                tables1=$(grep -c 'ID .*QUANT' "$pwt_log" || true)
                if (( tables1 > tables0 )) && after=$(latest_err_quant "$pwt_log" "$nid"); then
                    read -r err1 _ <<<"$after"
                    break
                fi
                sleep 0.1
            done
            if [[ -n "$after" ]]; then
                delta=$(( err1 - err0 ))
                if (( delta < 0 )); then
                    delta=0
                fi
            fi
        fi
        stop_pwtop
    fi
    : >"$ack"

    status=0
    wait "$probe_pid" || status=$?
    probe_pid=
    cat "$log"

    if (( status == 77 )); then
        echo "[rt] leg $leg: probe reported a skip" >&2
        exit 77
    fi
    if (( status != 0 )); then
        echo "[rt] leg $leg: probe exited $status" >&2
        return 1
    fi
    if (( ! ready )); then
        echo "[rt] leg $leg: probe never reported the callback/worker handoff" >&2
        return 1
    fi
    if [[ -z "$pid" ]]; then
        echo "[rt] leg $leg: no Wine probe process found for prefix $PROBE_PREFIX" >&2
        return 1
    fi
    if [[ -z "$cb" && -z "$worker" ]]; then
        echo "[rt] leg $leg: thread naming is not visible in /proc/$pid/task" >&2
        exit 77
    fi
    if [[ -z "$cb" || -z "$worker" ]]; then
        echo "[rt] leg $leg: found cb='${cb:-none}' worker='${worker:-none}'" >&2
        return 1
    fi
    if [[ "${cb%%:*}" == "${worker%%:*}" ]]; then
        echo "[rt] leg $leg: callback and worker report the same task ${cb%%:*}" >&2
        return 1
    fi
    if [[ "${cb#*:}" != "$want_cb" || "${worker#*:}" != "$want_worker" ]]; then
        echo "[rt] leg $leg: expected cb policy $want_cb / worker policy $want_worker" >&2
        echo "[rt] leg $leg: got cb $cb / worker $worker (tid:policy)" >&2
        return 1
    fi

    if [[ -z "$before" || -z "$after" || q0 -le 0 ]]; then
        echo "[rt] leg $leg: node $probe_node not profiled for ERR window" >&2
        return 1
    fi
    if (( delta > xrun_tolerance )); then
        echo "[rt] leg $leg: pw-top ERR +$delta on id=$nid in ${xrun_window}s," \
             "tolerance $xrun_tolerance ($err0 -> $err1)" >&2
        return 1
    fi

    echo "[rt] leg $leg ($label) ok: pid=$pid cb=$cb worker=$worker (tid:policy)"
    echo "[rt] leg $leg ($label) ok: id=$nid ERR +$delta in ${xrun_window}s" \
         "(tolerance $xrun_tolerance, quant $q0)"
}

positive_control_xrun

for leg in "${legs[@]}"; do
    case "$leg" in
        default)   run_leg default "$SCHED_OTHER" "$SCHED_OTHER" ;;
        config-on) run_leg config-on "$SCHED_FIFO" "$SCHED_OTHER" ;;
        env-off)   run_leg env-off "$SCHED_OTHER" "$SCHED_OTHER" ;;
        *)
            echo "[rt] unknown leg: $leg" >&2
            exit 1
            ;;
    esac
done

echo "[rt] $label: ${#legs[@]} leg(s) passed"
