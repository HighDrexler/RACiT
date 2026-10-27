#!/bin/bash
# build.sh: "RaCACiT Launcher.exe" -> launcher/build/ (zig's clang, static SDL2, no runtime DLLs).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
Z="$ROOT/tools/venv/Lib/site-packages/ziglang/zig.exe"
PY="$ROOT/tools/venv/Scripts/python"
OUT="$HERE/build"
mkdir -p "$OUT"
[ -f "$OUT/icon.ico" ] || "$PY" -I "$HERE/make_icon.py" "$OUT/icon.ico"
cd "$HERE/src"
"$Z" c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode \
    -DUNICODE -D_UNICODE \
    app.cpp gfx.cpp iso.cpp store.cpp util.cpp launcher.rc \
    -I"$ROOT/tools/sdl2-win/include" -L"$ROOT/tools/sdl2-win/lib" -lSDL2 \
    -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lwinhttp -lbcrypt \
    -lsetupapi -limm32 -lversion -lwinmm -lgdi32 -luser32 -ladvapi32 \
    -Wl,--subsystem,windows -o "$OUT/RaCACiT Launcher.exe"
ls -la "$OUT/RaCACiT Launcher.exe"
