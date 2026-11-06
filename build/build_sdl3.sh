#!/bin/sh
# Builds SDL3 as a static library for the proxy (32-bit, MinGW-w64), into out/sdl3/.
# Pinned release; only the parts the proxy uses (joystick, gamepad, HIDAPI, XInput, DirectInput, WGI, Raw Input).
set -e
cd "$(dirname "$0")/.."
TAG=release-3.2.24
OUT="$PWD/out/sdl3"
[ -f "$OUT/lib/libSDL3.a" ] && exit 0
mkdir -p out
[ -d out/sdl3-src ] || git clone -q --depth 1 --branch $TAG https://github.com/libsdl-org/SDL out/sdl3-src
cat > out/mingw32.cmake <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER i686-w64-mingw32-gcc-win32)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++-win32)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
cmake -S out/sdl3-src -B out/sdl3-build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/out/mingw32.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_AUDIO=OFF -DSDL_VIDEO=ON -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_GPU=OFF -DSDL_RENDER=OFF \
  -DSDL_CAMERA=OFF -DSDL_POWER=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_JOYSTICK=ON -DSDL_HAPTIC=ON -DSDL_HIDAPI=ON \
  -DSDL_DIRECTX=ON -DSDL_XINPUT=ON -DCMAKE_INSTALL_PREFIX="$OUT" \
  -DCMAKE_C_FLAGS="-ffile-prefix-map=$PWD/out/sdl3-src=sdl3" >/dev/null
cmake --build out/sdl3-build >/dev/null
cmake --install out/sdl3-build >/dev/null
echo "SDL3 $TAG built into out/sdl3"
