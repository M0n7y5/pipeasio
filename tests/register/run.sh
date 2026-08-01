#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises pipeasio-register's install-root discovery and warnings. A stub
# `wine` that succeeds at everything stands in for the real binary, so no
# Wine installation or prefix is needed. Covers:
#   1. a clean single install registers without warnings
#   2. aliased candidates (symlink, PIPEASIO_PREFIX naming the same install)
#      do not trigger the stale-install warning
#   3. with distinct installs, the copy in wine's own library dir wins
#      (wine searches it before any WINEDLLPATH entry), and the derived
#      libdir is scanned even for custom Wine roots outside the built-ins
#   4. a WineHQ-style lib(32-bit)/lib64(64-bit) split resolves the libdir
#      to lib64/wine
#   5. a Debian-style multiarch libdir is selected, and bare arch dirs from
#      a PipeASIO-only install cannot masquerade as the libdir (#19)
#   6. PIPEASIO_REGISTER_CANDIDATES replaces the built-in list, and with no
#      install anywhere the script fails and lists what it searched
# Every scan scenario passes a temp-only candidate list so host installs
# cannot leak in.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
register=${PIPEASIO_REGISTER:-"${script_dir}/../../pipeasio-register"}
if [[ ! -x $register ]]; then
    echo "[register-test] pipeasio-register not found at $register"
    exit 77
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail=0

# Point PATH at a fresh fake Wine root: bin/ holds the stubs, the caller
# then shapes lib/ as the scenario needs.
use_root() {
    local root=$1
    mkdir -p "$root/bin"
    for tool in wine wineboot; do
        printf '#!/usr/bin/env bash\nexit 0\n' > "$root/bin/$tool"
        chmod +x "$root/bin/$tool"
    done
    export PATH="$root/bin:/usr/bin:/bin"
}

# A Wine prefix skeleton so the script skips wineboot and cp has a target.
export WINEPREFIX="$work/prefix"
mkdir -p "$WINEPREFIX/drive_c/windows/system32"
export PIPEASIO_REGISTER_WITHOUT_LOADING=1
# Isolate from the host's real installs.
export HOME="$work/home"
mkdir -p "$HOME"

mkpair() { # <wine lib dir>
    mkdir -p "$1/x86_64-unix" "$1/x86_64-windows"
    : > "$1/x86_64-unix/pipeasio64.dll.so"
    : > "$1/x86_64-windows/pipeasio64.dll"
}

# Real Wine payload sentinels: pipeasio-register only accepts a libdir that
# has one, so scenarios exercising the libdir check must plant these.
mkwine() { # <wine lib dir>
    mkdir -p "$1/x86_64-unix" "$1/x86_64-windows"
    : > "$1/x86_64-unix/ntdll.so"
    : > "$1/x86_64-windows/ntdll.dll"
}

check() { # <desc> <haystack> <expected substring>
    if [[ $2 == *"$3"* ]]; then
        echo "ok - $1"
    else
        echo "FAIL - $1 (missing: $3)"
        fail=1
    fi
}

check_absent() { # <desc> <haystack> <forbidden substring>
    if [[ $2 != *"$3"* ]]; then
        echo "ok - $1"
    else
        echo "FAIL - $1 (unexpected: $3)"
        fail=1
    fi
}

run() { # <PIPEASIO_PREFIX or ""> <PIPEASIO_REGISTER_CANDIDATES>
    local st=0
    out=$(PIPEASIO_PREFIX="${1:-}" PIPEASIO_REGISTER_CANDIDATES="$2" \
          bash "$register" 2>&1) || st=$?
    status=$st
}

# Temp-only candidate list: the real defaults are never searched here.
tc="$HOME/.local/lib/wine"

# 1. Clean single install at the wine root's own lib dir: no warnings.
root="$work/root1"
use_root "$root"
mkwine "$root/lib/wine"
mkpair "$root/lib/wine"
run "$root" "$tc"
check "registers cleanly" "$out" "registered in"
check_absent "no stale warning on single install" "$out" "multiple PipeASIO installs"
check_absent "no libdir warning when root matches" "$out" "outside wine's library dir"
((status == 0)) || { echo "FAIL - clean run exit status ($status)"; fail=1; }

# 2. Same install reachable through an alias: still no stale warning.
mkdir -p "$HOME/.local/lib"
ln -s "$root/lib/wine" "$HOME/.local/lib/wine"
run "$root" "$tc"
check_absent "aliased duplicate does not warn" "$out" "multiple PipeASIO installs"
check "alias run registers" "$out" "registered in"

# 3. Distinct installs in the explicit prefix and in wine's own libdir: wine
#    loads the libdir copy first, so the script must use it and name the
#    ignored prefix install.
other="$work/other"
mkpair "$other/lib/wine"
run "$other" "$tc"
check "stale shadow warns" "$out" "multiple PipeASIO installs"
check "wine libdir copy wins" "$out" "using PipeASIO install at $root/lib/wine"
check "warning names the ignored install" "$out" "ignored: $other/lib/wine"
check "shadowed run still registers" "$out" "registered in"
((status == 0)) || { echo "FAIL - shadow run exit status ($status)"; fail=1; }
rm -rf "$HOME/.local/lib/wine"

# 3b. Bottles-style custom Wine root: its libdir is in no built-in candidate,
#     but it is still scanned and still wins over the explicit prefix.
custom="$work/bottles/runners/caffe"
use_root "$custom"
mkwine "$custom/lib/wine"
mkpair "$custom/lib/wine"
run "$other" "$tc"
check "custom-root shadow warns" "$out" "multiple PipeASIO installs"
check "custom-root libdir is scanned and wins" "$out" \
      "using PipeASIO install at $custom/lib/wine"
run "" "$tc"
check "custom-root solo install registers" "$out" "registered in"
check_absent "custom-root solo no libdir warning" "$out" "outside wine's library dir"

# 4. WineHQ-style split: 32-bit in lib/wine, 64-bit in lib64/wine. Only the
#    payload lives in lib64/wine (no PipeASIO pair), so the libdir detection
#    must pick it by payload and the warning must name lib64/wine.
root4="$work/root4"
use_root "$root4"
mkdir -p "$root4/lib/wine/i386-windows" "$root4/lib/wine/i386-unix"
mkwine "$root4/lib64/wine"
run "$other" "$tc"
check "lib64 split warning names lib64" "$out" "outside wine's library dir ($root4/lib64/wine)"
rm -rf "$other"

# 5. Debian-style multiarch libdir is selected.
root5="$work/root5"
use_root "$root5"
mkwine "$root5/lib/x86_64-linux-gnu/wine"
probe="$work/probe"
mkpair "$probe/lib/wine"
run "$probe" "$tc"
check "multiarch libdir selected" "$out" "outside wine's library dir ($root5/lib/x86_64-linux-gnu/wine)"

# 5b. #19 regression: the bad --prefix /usr install created bare arch dirs in
#     lib/wine; the real Wine payload lives in the multiarch dir. The bare
#     dirs must not masquerade as the libdir and silence the warning.
root5b="$work/root5b"
use_root "$root5b"
mkpair "$root5b/lib/wine"               # bare: only PipeASIO, no Wine payload
mkwine "$root5b/lib/x86_64-linux-gnu/wine"
run "$root5b" "$tc"
check "bare arch dirs do not fake the libdir" "$out" \
      "outside wine's library dir ($root5b/lib/x86_64-linux-gnu/wine)"

# 6a. The override replaces the built-in list: a pair in the second of two
#     override entries is found even though it matches no default location.
root6="$work/root6"
use_root "$root6"
rm -rf "$probe"
odd="$work/odd-place"
mkpair "$odd"
run "" "$work/nowhere:$odd"
check "override second entry searched" "$out" "registered in"
((status == 0)) || { echo "FAIL - override run exit status ($status)"; fail=1; }

# 6b. Nothing installed in the override roots: fail and list them.
rm -rf "$odd"
run "" "$work/nowhere1:$work/nowhere2"
check "missing install fails with guidance" "$out" "PipeASIO install not found"
check "failure lists the override roots" "$out" "$work/nowhere2"
((status != 0)) || { echo "FAIL - not-found run should exit nonzero"; fail=1; }

# 6d. Default locations are not consulted while the override is set.
mkpair "$HOME/.local/lib/wine"
run "" "$work/nowhere1"
check "override hides default locations" "$out" "PipeASIO install not found"
((status != 0)) || { echo "FAIL - override should replace, not extend"; fail=1; }
rm -rf "$HOME/.local/lib/wine"

# 6e. The built-in defaults themselves (static; an override-less run would
#     depend on what the host has installed).
for d in /usr/local/lib/wine /usr/lib/x86_64-linux-gnu/wine \
         /opt/wine-staging/lib/wine /opt/wine-staging/lib64/wine; do
    if grep -q -- "$d" "$register"; then
        echo "ok - default candidate present: $d"
    else
        echo "FAIL - default candidate missing: $d"
        fail=1
    fi
done

if ((fail)); then
    echo "[register-test] FAIL"
    exit 1
fi
echo "[register-test] all scenarios passed"
