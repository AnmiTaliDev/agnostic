#!/bin/sh
set -eu

SRC_DIR="$1"
BUILD_DIR="$2"
shift 2

BOOTSTRAP_DIR="$BUILD_DIR-bootstrap"
case "$BOOTSTRAP_DIR" in
    /*) BOOTSTRAP_ABS="$BOOTSTRAP_DIR" ;;
    *) BOOTSTRAP_ABS="$(pwd)/$BOOTSTRAP_DIR" ;;
esac

cmake -S "$SRC_DIR" -B "$BOOTSTRAP_DIR" -DAGNOSTIC_BOOTSTRAP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$BOOTSTRAP_DIR" -j"$(nproc)"

cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DAGNOSTIC_BOOTSTRAP_COMPILER="$BOOTSTRAP_ABS/src/cli/agnostic" \
    -DCMAKE_BUILD_TYPE=Release \
    "$@"
cmake --build "$BUILD_DIR" -j"$(nproc)"
