#!/bin/bash
# Settings file migration test: builds tests/settings_test.cpp with the release variant's default ini and runs the
# cases below under Wine (WINEPREFIX as for the game). Run after build/build_mingw.sh (needs out/version.gen.h).
# Usage: tests/settings_test.sh [old ddraw_proxy.ini from a player, e.g. a 0.0.15 file]
set -e
cd "$(dirname "$0")/.."
VER=$(tr -d ' \r\n' < VERSION)
T=out/settings_test; rm -rf "$T"; mkdir -p "$T"
awk '/^;@@debug-begin/{skip=1;next} /^;@@debug-end/{skip=0;next} !skip' src/ddraw_proxy.ini | sed "s/@VER@/$VER/" > "$T/default.ini"
{ echo 'static const char kDefaultIni[] ='
  awk '{ sub(/\r$/, ""); gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); print "    \"" $0 "\\n\"" }' "$T/default.ini"
  echo '    ;'; } > "$T/ini_default.gen.h"
i686-w64-mingw32-g++-win32 -std=c++17 -O2 -Wall -static -include out/version.gen.h -I src -I "$T" \
    -o "$T/settings_test.exe" tests/settings_test.cpp src/hardening.cpp -lpsapi -ladvapi32 -luser32
run() { WINEDEBUG=-all wine "$T/settings_test.exe" "$1" "$VER" | tr -d '\r'; }
fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  FAIL: $1"; fail=1; fi; }

echo "1. no file: the default is written"
run "$T/none.ini" >/dev/null
check "default written, CRLF" "cmp -s '$T/none.ini' '$T/default.ini' || [ \"\$(tr -d '\r' < '$T/none.ini')\" = \"\$(tr -d '\r' < '$T/default.ini')\" ]"
check "CRLF line ends" "grep -q \$'\r' '$T/none.ini'"
echo "2. same version: nothing changes"
cp "$T/none.ini" "$T/same.ini"; echo 'Extra=1' >> "$T/same.ini"
check "no change reported" "[ \"\$(run '$T/same.ini')\" = '(no change)' ]"
check "file untouched" "tail -1 '$T/same.ini' | grep -q Extra=1"
echo "3. old file (synthetic): values, renamed values, retired and unknown settings, controller sections"
printf '; old\r\n[Proxy]\r\nLog=0\r\nMusicVolume=55\r\n[Display]\r\nPicture=Auto\r\nScaling=Nearest\r\n[Controller]\r\nLayout=PS1\r\nApplied=PS1 2\r\n\r\n; my wheel\r\n[Joystick My Wheel]\r\nSteering=1\r\n[Test]\r\nRaceLogicRate=60\r\nRateLog=1\r\nTraceCalls=1\r\n' > "$T/old.ini"
run "$T/old.ini"
O=$(tr -d '\r' < "$T/old.ini")
g() { tr -d '\r' < "$T/old.ini" | awk -v s="[$1]" -v k="$2" 'BEGIN{FS="="} /^\[/{in_=($0==s)} in_ && $1==k {print substr($0, length(k)+2); exit}'; }
check "version set" "[ \"\$(g Proxy SettingsVersion)\" = '$VER' ]"
check "Log=0 kept" "[ \"\$(g Proxy Log)\" = 0 ]"
check "MusicVolume=55 kept" "[ \"\$(g Proxy MusicVolume)\" = 55 ]"
check "Picture Auto -> 16:10" "[ \"\$(g Display Picture)\" = 16:10 ]"
check "Scaling kept" "[ \"\$(g Display Scaling)\" = Nearest ]"
check "new setting with default (VSync=1)" "[ \"\$(g Display VSync)\" = 1 ]"
check "Layout kept" "[ \"\$(g Controller Layout)\" = PS1 ]"
check "Applied dropped (retired 0.0.18)" "[ -z \"\$(g Controller Applied)\" ]"
check "controller section kept with its comment" "printf '%s' \"\$O\" | grep -B1 -A1 '^\[Joystick My Wheel\]' | grep -q '; my wheel' && [ \"\$(g 'Joystick My Wheel' Steering)\" = 1 ]"
check "[Test] section dropped (1.0.0: no test options in the release build)" "! grep -q 'Test\\]\\|TraceCalls\\|RaceLogicRate\\|RateLog=' '$T/old.ini'"
check "backup kept" "[ -f '$T/old.ini.before-$VER' ] && grep -q RaceLogicRate=60 '$T/old.ini.before-$VER'"
check "second run: no change" "[ \"\$(run '$T/old.ini')\" = '(no change)' ]"
check "comments of the default kept" "[ \"\$(grep -c '^;' '$T/old.ini')\" -ge \"\$(grep -c '^;' '$T/default.ini')\" ]"
check "CRLF line ends only" "[ \"\$(grep -c \$'\r\$' '$T/old.ini')\" = \"\$(wc -l < '$T/old.ini')\" ]"
echo "4. read-only file"
cp "$T/old.ini.before-$VER" "$T/ro.ini"; chmod a-w "$T/ro.ini" 2>/dev/null || true
run "$T/ro.ini" >/dev/null
check "read-only file updated" "grep -q SettingsVersion '$T/ro.ini'"
if [ -n "$1" ]; then
    echo "5. player's file: $1"
    cp "$1" "$T/player.ini"
    run "$T/player.ini"
    tr -d '\r' < "$1" | grep -v '^;' | grep '=' | sort > "$T/a"; tr -d '\r' < "$T/player.ini" | grep -v '^;' | grep '=' | sort > "$T/b"
    echo "  settings changed (- old, + new):"; diff "$T/a" "$T/b" | grep '^[<>]' | sed 's/^</  -/; s/^>/  +/' || true
fi
[ $fail = 0 ] && echo "all settings tests passed" || { echo "settings tests FAILED"; exit 1; }
