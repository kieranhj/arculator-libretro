#!/usr/bin/env bash
# Cross-build arculator_libretro.so for aarch64 Linux handhelds using Zig as the
# C compiler. Needs `zig` and GNU `make` on PATH (on Windows, use the native
# winget builds of both; MSYS2 make strips the environment and breaks Zig).
#
#   scripts/build-aarch64.sh [clean] [glibc-version]
#
# The glibc version pins the symbol versions Zig links against. muOS on the
# RG40XXV ships glibc 2.38, so the default 2.31 is comfortably safe.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ "${1:-}" = "clean" ]; then CLEAN=1; shift; else CLEAN=0; fi
GLIBC="${1:-2.31}"
shift || true
TARGET="aarch64-linux-gnu.${GLIBC}"
OPT="${OPT:--O3 -flto -fno-sanitize=undefined -fno-stack-protector -fomit-frame-pointer -mcpu=cortex_a53}"

export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$ROOT/.zig-cache/global}"
export ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$ROOT/.zig-cache/local}"

cd "$ROOT"
[ -f arculator/src/arc.h ] || { echo "arculator submodule missing: run git submodule update --init"; exit 1; }
[ "$CLEAN" = 1 ] && make clean >/dev/null
rm -f arculator_libretro.so
# LDFLAGS goes through the environment so the Makefile can append its own.
LDFLAGS="-flto" make -j"$(nproc)" CC="zig cc -target $TARGET" CFLAGS="$OPT" "$@"

mkdir -p out
cp arculator_libretro.so arculator_libretro.info out/
echo "Built: $ROOT/out/arculator_libretro.so (glibc $GLIBC)"
