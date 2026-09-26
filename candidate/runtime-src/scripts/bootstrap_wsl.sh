#!/usr/bin/env bash
# Isolated aarch64 build dependencies for this U60 Pro UI candidate.
set -euo pipefail

BUILD_BASE="${U60_BUILD_BASE:-$HOME/u60-codex-build}"
mkdir -p "$BUILD_BASE"

TOOLCHAIN="$BUILD_BASE/aarch64--musl--stable-2025.08-1"
if [ ! -x "$TOOLCHAIN/bin/aarch64-linux-gcc" ]; then
    curl -fL --retry 5 --connect-timeout 20 -C - \
      -o "$BUILD_BASE/bootlin-aarch64-musl.tar.xz" \
      'https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/aarch64--musl--stable-2025.08-1.tar.xz'
    tar -C "$BUILD_BASE" -xf "$BUILD_BASE/bootlin-aarch64-musl.tar.xz"
fi

if [ ! -f "$BUILD_BASE/freetype/src/base/ftbase.c" ]; then
    git clone --depth 1 https://github.com/freetype/freetype.git "$BUILD_BASE/freetype"
fi
if [ ! -f "$BUILD_BASE/litehtml/include/litehtml.h" ]; then
    git clone --depth 1 --branch v0.10 https://github.com/litehtml/litehtml.git "$BUILD_BASE/litehtml"
fi

if [ ! -f "$BUILD_BASE/freetype-musl/lib/libfreetype.a" ]; then
    HOME="$BUILD_BASE" bash "$BUILD_BASE/project/scripts/_build_freetype.sh"
fi
if [ ! -f "$BUILD_BASE/litehtml-musl/lib/liblitehtml.a" ]; then
    HOME="$BUILD_BASE" bash "$BUILD_BASE/project/scripts/_build_litehtml.sh"
fi

HOME="$BUILD_BASE" bash "$BUILD_BASE/project/scripts/build.sh"
