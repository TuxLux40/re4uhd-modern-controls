#!/usr/bin/env bash
# Builds the 32-bit proxy xinput1_3.dll with zig (clang + lld, mingw target): build/xinput1_3.dll
#   bash build.sh                       development build (version "dev")
#   VERSION=0.1.0 bash build.sh package release build + dist/RE4ModernControls-v0.1.0.zip
# zig comes from PATH, or set ZIG, e.g. ZIG="uv tool run --from ziglang==0.17.0 python -m ziglang".
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
VERSION="${VERSION:-dev}"
mkdir -p build
MH=third_party/minhook/src
for f in hook buffer trampoline hde/hde32; do
  $ZIG cc -target x86-windows-gnu -O2 -c $MH/$f.c -o build/mh_$(basename $f).o -Ithird_party/minhook/include
done
$ZIG c++ -target x86-windows-gnu -O2 -std=c++17 -shared -fno-exceptions -fno-rtti -g0 -s \
  "-DRE4MC_VERSION_STR=\"$VERSION\"" \
  -Ithird_party/minhook/include -Isrc src/*.cpp build/mh_*.o src/xinput1_3.def \
  -o build/xinput1_3.dll -luser32 -lkernel32
ls -la build/xinput1_3.dll

if [ "${1:-}" = package ]; then
  mkdir -p dist
  python3 - "$VERSION" <<'PY'
import sys, zipfile
v = sys.argv[1]
out = f"dist/RE4ModernControls-v{v}.zip"
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    # Mirrors the game folder: extract into "Resident Evil 4" and the DLL lands next to bio4.exe.
    z.write("build/xinput1_3.dll", "Bin32/xinput1_3.dll")
    z.write("docs/INSTALL.txt", "RE4ModernControls-INSTALL.txt")
    z.write("LICENSE", "RE4ModernControls-LICENSE.txt")
print(out)
PY
fi
