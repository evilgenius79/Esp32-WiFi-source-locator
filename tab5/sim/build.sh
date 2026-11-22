#!/bin/sh
# Builds the PC preview against the M5GFX copy PlatformIO downloaded for the
# Tab5 build (run `pio run` in tab5/ once first).  Usage: sim/build.sh [outdir]
set -e
cd "$(dirname "$0")/.."
GFX=.pio/libdeps/tab5/M5GFX/src
OUT=${1:-sim/out}
mkdir -p "$OUT" .pio/sim
if [ ! -f .pio/sim/libgfx.a ]; then
  for f in $(find $GFX/lgfx -name '*.cpp' -o -name '*.c' | grep -v '/platforms/esp32'); do
    o=.pio/sim/o$(echo "$f" | tr '/' '_').o
    case "$f" in
      *.c) gcc -c -O2 -DLGFX_LINUX_FB -I$GFX "$f" -o "$o" ;;
      *) g++ -c -O2 -std=c++17 -DLGFX_LINUX_FB -I$GFX "$f" -o "$o" ;;
    esac
  done
  ar rcs .pio/sim/libgfx.a .pio/sim/*.o
fi
g++ -O2 -std=c++17 -Wall -DLGFX_LINUX_FB -I$GFX -I../common sim/sim.cpp src/app.cpp .pio/sim/libgfx.a -o .pio/sim/sim
.pio/sim/sim "$OUT"
