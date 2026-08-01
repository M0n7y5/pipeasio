#!/usr/bin/env bash
# Build + ctest PipeASIO inside real distro containers via distrobox.
#
# Catches toolchain drift that host-only CI misses - e.g. Fedora RPM
# injecting -flto=auto into CFLAGS, which broke winebuild's .spec export
# scan (issue #6) until cmake forced -fno-lto on the object libs.
#
# Usage:  tests/distro/run.sh                 # all known distros
#         DISTROS="fedora ubuntu" ./run.sh    # subset
#         FRESH=1 ./run.sh                    # recreate containers first
#         ./run.sh --clean                    # remove harness containers
#
# Env:
#   DISTROS   space-separated names (default: all in the table below)
#   FRESH=1   stop+rm each container before creating

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "${here}/../.." && pwd)"
box_prefix="pipeasio-distro"

# Ensure a user-local distrobox install is visible when present.
export PATH="${HOME}/.local/bin:${PATH}"

# ---------------------------------------------------------------------------
# Distro table (one row per distro). Fields, pipe-separated:
#   name | image | install_cmd | flags_script | assert_lto
#
# install_cmd   package install as root inside the box
# flags_script  path relative to repo root, prints `export CFLAGS=...` lines
# assert_lto    "yes" => fail if CFLAGS lacks -flto (Fedora / issue #6)
# ---------------------------------------------------------------------------
DISTRO_TABLE="
fedora|registry.fedoraproject.org/fedora:latest|dnf install -y --setopt=install_weak_deps=False cmake ninja-build gcc g++ pkgconf wine-devel pipewire-devel redhat-rpm-config git|tests/distro/flags/fedora.sh|yes
ubuntu|docker.io/library/ubuntu:latest|export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y --no-install-recommends cmake ninja-build gcc g++ pkg-config wine64-tools libwine-dev libpipewire-0.3-dev pipewire dpkg-dev git ca-certificates|tests/distro/flags/ubuntu.sh|no
arch|docker.io/library/archlinux:latest|pacman -Syu --noconfirm gcc cmake ninja pkgconf wine libpipewire pipewire git|tests/distro/flags/arch.sh|no
steamrt4|registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk@sha256:2c4c6520a268ef53255d511ae5988e35855b39a4b6c1e9865d56e5011c76ec3e|export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y --no-install-recommends cmake ninja-build gcc g++ pkg-config wine wine64-tools libwine-dev dpkg-dev git ca-certificates|tests/distro/flags/steamrt4.sh|no
"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--clean] [--help]

  (default)   create/reuse containers, install deps, build, ctest
  --clean     remove harness containers (${box_prefix}-*) and exit
  --help      this text

Env:
  DISTROS="fedora ubuntu"   subset of: fedora ubuntu arch steamrt4
  FRESH=1                   recreate containers before running
EOF
}

log() { printf '[distro] %s\n' "$*"; }
die() { printf '[distro] ERROR: %s\n' "$*" >&2; exit 1; }

# --- preconditions (77 = SKIP, matching tests/asio_probe/run.sh) ------------
need_tool() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[distro] SKIP: '$1' not found on PATH"
        exit 77
    }
}

need_tool distrobox

backend=""
if command -v podman >/dev/null 2>&1; then
    backend=podman
elif command -v docker >/dev/null 2>&1; then
    backend=docker
else
    echo "[distro] SKIP: neither podman nor docker found (distrobox backend)"
    exit 77
fi

box_name() { printf '%s-%s' "$box_prefix" "$1"; }

list_known() {
    printf '%s\n' "$DISTRO_TABLE" | while IFS='|' read -r name _rest; do
        [[ -z "${name// }" ]] && continue
        printf '%s\n' "$name"
    done
}

lookup() {
    local want="$1"
    printf '%s\n' "$DISTRO_TABLE" | while IFS= read -r row; do
        [[ -z "${row// }" ]] && continue
        if [[ "${row%%|*}" == "$want" ]]; then
            printf '%s\n' "$row"
            return 0
        fi
    done
}

box_exists() {
    distrobox list 2>/dev/null | awk -F'|' -v n="$1" '
        NR > 1 {
            gsub(/^ +| +$/, "", $2)
            if ($2 == n) found = 1
        }
        END { exit !found }
    '
}

remove_box() {
    local name="$1"
    if box_exists "$name"; then
        log "removing container $name"
        distrobox stop "$name" >/dev/null 2>&1 || true
        distrobox rm -f "$name" >/dev/null 2>&1 || true
    fi
}

ensure_box() {
    local name="$1" image="$2"
    if [[ -n "${FRESH:-}" ]]; then
        remove_box "$name"
    fi
    if box_exists "$name"; then
        log "reusing container $name"
        return 0
    fi
    log "creating $name from $image (backend=$backend)"
    distrobox create --yes --name "$name" --image "$image"
}

# Run a command as root inside the box.  Prefer passwordless sudo (distrobox
# default). Fall back to distrobox enter --root when sudo is unavailable.
enter_root() {
    local name="$1"
    shift
    if distrobox enter "$name" -- sh -lc 'command -v sudo >/dev/null && sudo -n true' \
            >/dev/null 2>&1; then
        distrobox enter "$name" -- sudo -n "$@"
        return
    fi
    if distrobox enter --help 2>&1 | grep -q -- '--root'; then
        distrobox enter --root "$name" -- "$@"
        return
    fi
    die "cannot gain root inside $name (no passwordless sudo, no --root)"
}

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
case "${1:-}" in
    --help|-h)
        usage
        exit 0
        ;;
    --clean)
        log "cleaning harness containers (prefix ${box_prefix}-)"
        while IFS= read -r d; do
            [[ -z "$d" ]] && continue
            remove_box "$(box_name "$d")"
        done < <(list_known)
        log "done"
        exit 0
        ;;
    "")
        ;;
    *)
        die "unknown argument: $1 (try --help)"
        ;;
esac

# ---------------------------------------------------------------------------
# Select distros
# ---------------------------------------------------------------------------
if [[ -n "${DISTROS:-}" ]]; then
    # shellcheck disable=SC2206
    selected=($DISTROS)
else
    mapfile -t selected < <(list_known)
fi

[[ ${#selected[@]} -gt 0 ]] || die "no distros selected"

for d in "${selected[@]}"; do
    row="$(lookup "$d" || true)"
    [[ -n "$row" ]] || die "unknown distro '$d' (known: $(list_known | xargs))"
done

log "repo:    $repo"
log "backend: $backend"
log "distros: ${selected[*]}"
echo

# ---------------------------------------------------------------------------
# Per-distro run
# ---------------------------------------------------------------------------
declare -a results_name=()
declare -a results_status=()
declare -a results_note=()
failures=0

run_one() {
    # Errexit comes from the caller's subshell. Do not set -e here (bash set
    # is global and would re-arm the parent before rc is captured).
    local distro="$1"
    local name image install_cmd flags_script assert_lto box bdir
    local row flag_env

    row="$(lookup "$distro")"
    IFS='|' read -r name image install_cmd flags_script assert_lto <<<"$row"
    box="$(box_name "$name")"
    bdir="${repo}/build-distro-${name}"

    log "======== ${name} ========"
    log "image:   $image"
    log "box:     $box"
    log "build:   $bdir"

    ensure_box "$box" "$image"

    log "installing build deps..."
    # sh -lc: login PATH on minimal images; install_cmd may be a pipeline.
    enter_root "$box" sh -lc "$install_cmd"

    log "resolving distro CFLAGS/LDFLAGS..."
    flag_env="$(
        distrobox enter "$box" -- bash -lc \
            "cd $(printf %q "$repo") && bash $(printf %q "$flags_script")"
    )" || {
        log "flags extraction failed"
        return 1
    }
    if [[ -n "$flag_env" ]]; then
        log "flags:"
        printf '%s\n' "$flag_env" | sed 's/^/[distro]   /'
    else
        log "flags: (toolchain defaults)"
    fi

    if [[ "$assert_lto" == "yes" ]]; then
        if printf '%s\n' "$flag_env" | grep -q -- '-flto'; then
            log "LTO guard: CFLAGS contain -flto (issue #6 regression case)"
        else
            log "LTO guard FAILED: expected -flto in Fedora optflags"
            log "got:"
            printf '%s\n' "$flag_env" | sed 's/^/[distro]   /'
            log "redhat-rpm-config macros may have dropped LTO; update the guard."
            return 1
        fi
    fi

    # Fresh build dir per run keeps configure clean across flag changes.
    rm -rf "$bdir"
    mkdir -p "$bdir"

    log "configure + build + ctest..."
    distrobox enter "$box" -- bash -lc "
        set -euo pipefail
        cd $(printf %q "$repo")
        ${flag_env}
        echo \"[distro:${name}] CFLAGS=\${CFLAGS-}\"
        echo \"[distro:${name}] LDFLAGS=\${LDFLAGS-}\"
        cmake -B $(printf %q "$bdir") -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_SETTINGS_PANEL=OFF
        cmake --build $(printf %q "$bdir")
        # Guard against a silent partial build (e.g. wine headers missing so
        # only unit tests link while the DLL target fails elsewhere).
        test -f $(printf %q "$bdir")/pipeasio64.dll \
            || { echo 'missing pipeasio64.dll after build' >&2; exit 1; }
        test -f $(printf %q "$bdir")/pipeasio64.dll.so \
            || { echo 'missing pipeasio64.dll.so after build' >&2; exit 1; }
        ctest --test-dir $(printf %q "$bdir") --output-on-failure
    " || {
        log "${name}: build/test FAILED"
        return 1
    }

    log "${name}: PASS"
    return 0
}

for d in "${selected[@]}"; do
    # Each leg runs in a subshell with strict mode so failures stay confined
    # (bash `set` is global; set -e inside run_one would abort the parent loop).
    set +e
    ( set -euo pipefail; run_one "$d" )
    rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        status="PASS"
        note="ok"
    else
        status="FAIL"
        note="see log above"
        failures=$((failures + 1))
    fi
    echo
    results_name+=("$d")
    results_status+=("$status")
    results_note+=("$note")
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
log "======== summary ========"
printf '%-12s %-6s %s\n' "DISTRO" "RESULT" "NOTE"
printf '%-12s %-6s %s\n' "------" "------" "----"
i=0
while [[ $i -lt ${#results_name[@]} ]]; do
    printf '%-12s %-6s %s\n' \
        "${results_name[$i]}" "${results_status[$i]}" "${results_note[$i]}"
    i=$((i + 1))
done
echo

if [[ "$failures" -ne 0 ]]; then
    log "$failures distro(s) FAILED"
    exit 1
fi
log "all ${#results_name[@]} distro(s) PASSED"
exit 0
