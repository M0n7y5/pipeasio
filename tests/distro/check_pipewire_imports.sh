#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

if (( $# < 2 )); then
    echo "usage: $0 FLOOR_LIB ARTIFACT..." >&2
    exit 2
fi

floor_lib=$1
shift
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

nm -D --defined-only "$floor_lib" \
    | awk '{print $3}' \
    | sed 's/@.*//' \
    | sort -u > "$work/floor"

status=0
for artifact in "$@"; do
    if [[ ! -f $artifact ]]; then
        echo "missing artifact: $artifact" >&2
        status=1
        continue
    fi
    nm -D --undefined-only "$artifact" \
        | awk '{print $NF}' \
        | sed 's/@.*//' \
        | awk '/^(pw|spa)_/' \
        | sort -u > "$work/used"
    comm -23 "$work/used" "$work/floor" > "$work/missing"
    if [[ -s $work/missing ]]; then
        echo "${artifact}: imports symbols absent from ${floor_lib}:" >&2
        cat "$work/missing" >&2
        status=1
    else
        echo "${artifact}: $(wc -l < "$work/used") PipeWire imports available in ${floor_lib}"
    fi
done
exit "$status"
