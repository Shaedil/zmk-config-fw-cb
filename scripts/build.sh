#!/usr/bin/env bash
#
# Build and sign locally, as CI does.
#
#   scripts/build.sh --setup     create .zmk/: venv, ZMK, Zephyr (~4 GB)
#   scripts/build.sh             build and sign
#   scripts/build.sh --pristine  same, from a clean build directory
#   scripts/build.sh --update    west update after editing config/west.yml
#   scripts/build.sh --patch     only apply patches/ (CI)
#
# Env: SHIELD, BOARD, SNIPPET (e.g. framework-cb-uart-log), ZMK_WORKSPACE
# (default .zmk/), PYTHON (default python3).
# Uses arm-none-eabi-gcc from PATH if present, otherwise the Zephyr SDK.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="${ZMK_WORKSPACE:-$REPO/.zmk}"
BOARD="${BOARD:-framework_wireless_tp_kb/nrf54lm20a/cpuapp/zmk}"
SHIELD="${SHIELD:-framework_cb_matrix}"
SNIPPET="${SNIPPET:-}"
BUILD_DIR="$REPO/build/$SHIELD"
PYTHON="${PYTHON:-python3}"

die() { echo "error: $*" >&2; exit 1; }

sync_manifest() {
    mkdir -p "$WS/config"
    cp "$REPO/config/west.yml" "$WS/config/west.yml"
}

PATCHED=(zmk zephyr)

reset_checkout() {
    git -C "$1" checkout -q -- . && git -C "$1" clean -fdq
}

patch_project() {
    local dir="$WS/$1"
    local patches=("$REPO/patches/$1"/*.patch)
    [ -e "${patches[0]}" ] || return 0
    cat "${patches[@]}" | git -C "$dir" apply --reverse --check 2>/dev/null && return 0
    reset_checkout "$dir"
    cat "${patches[@]}" | git -C "$dir" apply ||
        die "patches/$1/ does not apply to the $1 revision in config/west.yml"
}

patch_all() {
    local p
    for p in "${PATCHED[@]}"; do patch_project "$p"; done
}

unpatch_all() {
    local p
    for p in "${PATCHED[@]}"; do
        [ -d "$WS/$p" ] || continue
        reset_checkout "$WS/$p"
    done
}

activate_venv() {
    if [ -x "$WS/.venv/bin/python" ]; then
        export PATH="$WS/.venv/bin:$PATH"
    fi
}

setup() {
    sync_manifest
    "$PYTHON" -m venv "$WS/.venv"
    "$WS/.venv/bin/python" -m pip install -q --upgrade pip
    "$WS/.venv/bin/python" -m pip install -q -r "$REPO/scripts/requirements.txt"
    activate_venv

    [ -d "$WS/.west" ] || (cd "$WS" && west init -l config)
    unpatch_all
    (cd "$WS" && west update --narrow -o=--depth=1)
    patch_all

    "$WS/.venv/bin/python" -m pip install -q \
        -r "$WS/zephyr/scripts/requirements-base.txt" \
        -r "$REPO/scripts/requirements.txt"
    (cd "$WS" && west zephyr-export)
    echo "Workspace ready at $WS"
}

update() {
    sync_manifest
    activate_venv
    unpatch_all
    (cd "$WS" && west update --narrow -o=--depth=1)
    patch_all
}

choose_toolchain() {
    [ -n "${ZEPHYR_TOOLCHAIN_VARIANT:-}${ZEPHYR_SDK_INSTALL_DIR:-}" ] && return
    local gcc
    gcc="$(command -v arm-none-eabi-gcc || true)"
    [ -n "$gcc" ] || return 0
    export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
    GNUARMEMB_TOOLCHAIN_PATH="$(dirname "$(dirname "$gcc")")"
    export GNUARMEMB_TOOLCHAIN_PATH
}

build() {
    [ -d "$WS/zephyr" ] || die "no workspace at $WS; run $0 --setup first"
    cmp -s "$REPO/config/west.yml" "$WS/config/west.yml" ||
        die "config/west.yml changed since the last update; run $0 --update"

    patch_all
    activate_venv
    choose_toolchain
    command -v imgtool >/dev/null || die "imgtool not found; run $0 --setup or pip install -r scripts/requirements.txt"

    (cd "$WS" && west build -s zmk/app -d "$BUILD_DIR" -b "$BOARD" ${SNIPPET:+-S "$SNIPPET"} -- \
        -DSHIELD="$SHIELD" \
        -DZMK_CONFIG="$REPO/config" \
        -DZMK_EXTRA_MODULES="$REPO")

    local image="$BUILD_DIR/zephyr/zmk.signed.bin"
    [ -f "$image" ] || die "build finished but $image is missing"
    echo
    echo "Signed image: $image"
}

case "${1:-}" in
    --setup)    setup ;;
    --update)   update ;;
    --patch)    patch_all ;;
    --pristine) rm -rf "$BUILD_DIR"; build ;;
    "")         build ;;
    -h|--help)  sed -n '3,/^$/s/^# \{0,1\}//p' "$0" ;;
    *)          die "unknown option: $1" ;;
esac
