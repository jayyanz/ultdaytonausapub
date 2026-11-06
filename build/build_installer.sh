#!/bin/sh
# Builds dist/<VERSION>/ultdaytonausa-setup.exe (32-bit, MinGW-w64), the installer carrying this build's ddraw.dll
# and default ddraw_proxy.ini. Run by build_mingw.sh after the proxy. Reproducible (no timestamps).
set -e
cd "$(dirname "$0")/.."
VER=$(tr -d ' \r\n' < VERSION)
OUT="dist/$VER"
[ -f "$OUT/ddraw.dll" ] || { echo "build the proxy first ($OUT/ddraw.dll)" >&2; exit 1; }
sh build/build_flac.sh
i686-w64-mingw32-windres -I out -I src/installer \
    -DDDRAW_DLL="\\\"$PWD/$OUT/ddraw.dll\\\"" -DPROXY_INI="\\\"$PWD/$OUT/ddraw_proxy.ini\\\"" \
    -o out/setup_res.o src/installer/setup.rc
i686-w64-mingw32-g++-win32 -std=c++17 -O2 -Wall -s -municode -mwindows -static -static-libgcc -static-libstdc++ \
    -Wl,--no-insert-timestamp -include out/version.gen.h -DFLAC__NO_DLL -I out/flac/include -I src \
    -ffile-prefix-map="$PWD"=. \
    -o "$OUT/ultdaytonausa-setup.exe" src/installer/setup.cpp src/installer/disc.cpp src/installer/rip.cpp out/setup_res.o \
    -Lout/flac/lib -lFLAC -lbcrypt -lole32 -lshell32 -lcomctl32 -lcomdlg32 -luuid -ladvapi32 -luser32 -lgdi32 -lkernel32
cp src/installer/ultdaytonausa-cleanup.cmd "$OUT/"   # removes what 0.0.14 to 0.0.21 left outside the game folder
sha1sum "$OUT/ultdaytonausa-setup.exe" "$OUT/ultdaytonausa-cleanup.cmd" | tee -a "$OUT/SHA1.txt"
