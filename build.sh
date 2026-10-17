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
# src/default.ini -> build/default_ini.h (kDefaultIni, CRLF line ends): the mod writes it on first start.
python3 - <<'PY'
lines = open("src/default.ini", encoding="utf-8").read().splitlines()
body = "".join('    "' + l.replace("\\", "\\\\").replace('"', '\\"') + '\\r\\n"\n' for l in lines)
open("build/default_ini.h", "w").write("static const char kDefaultIni[] =\n" + body + "    ;\n")
PY
MH=third_party/minhook/src
for f in hook buffer trampoline hde/hde32; do
  $ZIG cc -target x86-windows-gnu -O2 -c $MH/$f.c -o build/mh_$(basename $f).o -Ithird_party/minhook/include
done
$ZIG c++ -target x86-windows-gnu -O2 -std=c++17 -shared -fno-exceptions -fno-rtti -g0 -s \
  "-DRE4MC_VERSION_STR=\"$VERSION\"" \
  -Ithird_party/minhook/include -Isrc -Ibuild src/*.cpp build/mh_*.o src/xinput1_3.def \
  -o build/xinput1_3.dll -luser32 -lkernel32
ls -la build/xinput1_3.dll

if [ "${1:-}" = package ]; then
  mkdir -p dist
  python3 - "$VERSION" <<'PY'
import sys, zipfile
v = sys.argv[1]
root = f"RE4ModernControls-v{v}/"
out = f"dist/RE4ModernControls-v{v}.zip"

def add(z, src, dst, executable=False):
    info = zipfile.ZipInfo.from_file(src, root + dst)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o100755 if executable else 0o100644) << 16
    with open(src, "rb") as f:
        z.writestr(info, f.read())

with zipfile.ZipFile(out, "w") as z:
    add(z, "installer/install.bat", "install.bat")
    add(z, "installer/install.ps1", "install.ps1")
    add(z, "installer/install.sh", "install.sh", executable=True)
    add(z, "docs/INSTALL.txt", "README.txt")
    add(z, "LICENSE", "LICENSE.txt")
    add(z, "build/xinput1_3.dll", "files/xinput1_3.dll")
    add(z, "src/default.ini", "files/default.ini")
print(out)
PY
fi
