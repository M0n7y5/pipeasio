#!/usr/bin/env bash
# Steam Runtime 4 leg: assert the PipeWire floor this leg exists to prove,
# then emit the same dpkg-buildflags hardening set as the Debian/Ubuntu leg.
# The SDK image is digest-pinned in run.sh and ships libpipewire preinstalled,
# so a version other than the floor means the pin moved, not that apt drifted.
# Requires: pkg-config, dpkg-dev
set -euo pipefail

want=1.4.2
have="$(pkg-config --modversion libpipewire-0.3 2>/dev/null || true)"

[[ -n "$have" ]] || {
    echo "steamrt4 flags: libpipewire-0.3 not found via pkg-config" >&2
    exit 1
}

if [[ "$have" != "$want" ]]; then
    {
        echo "steamrt4 flags: libpipewire-0.3 is ${have}, not the ${want} floor"
        echo "  this leg exists to prove. Re-pin the image digest in run.sh, or"
        echo "  move the floor in CMakeLists.txt, README.md and docs/index.html."
    } >&2
    exit 1
fi

echo "steamrt4 flags: libpipewire-0.3 ${have} (floor ${want})" >&2

# stdout is eval'd by run.sh, so only the export lines may go there.
exec bash "$(dirname "${BASH_SOURCE[0]}")/ubuntu.sh"
