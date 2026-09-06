#!/usr/bin/env bash
# Compile include/pipeasio_unix_abi.h at both pointer widths.
set -euo pipefail
: "${CC:=cc}"
: "${SRC:?SRC must be set}"
: "${INC:?INC must be set}"

echo "[abi] 64-bit layout check"
"$CC" -m64 -DEXPECTED_POINTER_SIZE=8 -I "$INC" -fsyntax-only "$SRC"
echo "[abi] 64-bit OK"

# 32-bit needs gcc multilib; the probe includes a libc header so a missing
# gnu/stubs-32.h is detected (a bare -m32 compile of trivial code is not enough).
if echo '#include <stdint.h>' | "$CC" -m32 -xc - -fsyntax-only >/dev/null 2>&1; then
    echo "[abi] 32-bit layout check"
    "$CC" -m32 -DEXPECTED_POINTER_SIZE=4 -I "$INC" -fsyntax-only "$SRC"
    echo "[abi] 32-bit OK"
else
    echo "[abi] no 32-bit multilib - 32-bit layout check skipped"
fi
echo "[abi] PASS"
