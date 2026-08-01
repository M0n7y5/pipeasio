#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <ELF artifact>..." >&2
    exit 2
fi

for artifact in "$@"; do
    [[ -f "$artifact" ]] || {
        echo "missing sanitizer artifact: $artifact" >&2
        exit 1
    }

    imports="$(nm -D --undefined-only "$artifact")"
    if ! grep -q '__asan_init' <<<"$imports"; then
        echo "$artifact does not import the AddressSanitizer runtime" >&2
        exit 1
    fi
    if ! grep -q '__ubsan_handle_' <<<"$imports"; then
        echo "$artifact does not import the UndefinedBehaviorSanitizer runtime" >&2
        exit 1
    fi
    printf '%s: ASan and UBSan imports present\n' "$artifact"
done
