#!/bin/sh
# Builds libFLAC (BSD licence) as a static library for the installer (32-bit, MinGW-w64), into out/flac/.
# Pinned release; encoder and decoder only, no Ogg, no programs. -mstackrealign: its SSE code runs on the
# installer's worker threads, whose stacks Windows aligns to 4 bytes (SSE needs 16).
set -e
cd "$(dirname "$0")/.."
TAG=1.4.3
OUT="$PWD/out/flac"
[ -f "$OUT/lib/libFLAC.a" ] && exit 0
mkdir -p out
[ -f out/mingw32.cmake ] || sh build/build_sdl3.sh >/dev/null
[ -d out/flac-src ] || git -c advice.detachedHead=false clone -q --depth 1 --branch $TAG https://github.com/xiph/flac out/flac-src
cmake -S out/flac-src -B out/flac-build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/out/mingw32.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DWITH_OGG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF \
  -DINSTALL_MANPAGES=OFF -DBUILD_CXXLIBS=OFF -DWITH_FORTIFY_SOURCE=OFF -DWITH_STACK_PROTECTOR=OFF -DCMAKE_INSTALL_PREFIX="$OUT" \
  -DCMAKE_C_FLAGS="-mstackrealign -ffile-prefix-map=$PWD/out/flac-src=flac" >/dev/null
cmake --build out/flac-build >/dev/null
cmake --install out/flac-build >/dev/null
echo "libFLAC $TAG built into out/flac"
