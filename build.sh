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
# assets/icons/*.png -> build/icons_data.h (kEmbeddedIcons, ARGB pixels): the pickup log's built-in item pictures.
python3 - <<'PY'
import glob, os, struct, zlib
def rgba(path):  # 8-bit RGBA, non-interlaced (as the icons are saved)
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path
    i, idat = 8, b""
    while i < len(d):
        n, t = struct.unpack(">I4s", d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, lace = struct.unpack(">IIBBBBB", c)
            assert depth == 8 and ctype == 6 and lace == 0, path
        elif t == b"IDAT":
            idat += c
        i += 12 + n
    raw, stride, out, prev = zlib.decompress(idat), w * 4, [], bytearray(w * 4)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b, c = prev[x], prev[x - 4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += [line[x + 3] << 24 | line[x] << 16 | line[x + 1] << 8 | line[x + 2] for x in range(0, stride, 4)]
        prev = line
    return w, h, out
src, table = "", ""
for k, path in enumerate(sorted(glob.glob("assets/icons/*.png"))):
    w, h, px = rgba(path)
    src += f"static const uint32_t kIconPx{k}[] = {{" + ",".join(f"0x{p:X}" for p in px) + "};\n"
    table += f'    {{"{os.path.basename(path)[:-4]}", {w}, {h}, kIconPx{k}}},\n'
open("build/icons_data.h", "w").write(src + "static const EmbeddedIcon kEmbeddedIcons[] = {\n" + table + "};\n")
PY
MH=third_party/minhook/src
for f in hook buffer trampoline hde/hde32; do
  $ZIG cc -target x86-windows-gnu -O2 -c $MH/$f.c -o build/mh_$(basename $f).o -Ithird_party/minhook/include
done
$ZIG c++ -target x86-windows-gnu -O2 -std=c++17 -shared -fno-exceptions -fno-rtti -g0 -s \
  "-DRE4MC_VERSION_STR=\"$VERSION\"" \
  -Ithird_party/minhook/include -Isrc -Ibuild src/*.cpp build/mh_*.o src/xinput1_3.def \
  -o build/xinput1_3.dll -luser32 -lkernel32 -lgdi32
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
