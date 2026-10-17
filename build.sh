#!/usr/bin/env bash
# Builds the 32-bit proxy xinput1_3.dll with zig (clang + lld, mingw target): build/xinput1_3.dll
#   bash build.sh                      zig on PATH
#   ZIG="uv tool run --from ziglang python -m ziglang" bash build.sh
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
mkdir -p build
MH=third_party/minhook/src
for f in hook buffer trampoline hde/hde32; do
  $ZIG cc -target x86-windows-gnu -O2 -c $MH/$f.c -o build/mh_$(basename $f).o -Ithird_party/minhook/include
done
$ZIG c++ -target x86-windows-gnu -O2 -std=c++17 -shared -fno-exceptions -fno-rtti -g0 -s \
  -Ithird_party/minhook/include -Isrc src/*.cpp build/mh_*.o src/xinput1_3.def \
  -o build/xinput1_3.dll -luser32 -lkernel32
ls -la build/xinput1_3.dll
