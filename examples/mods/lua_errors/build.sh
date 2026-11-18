#!/bin/sh
# Builds lua_errors.dll with the repo's zig (any Windows x86-64 C compiler works:
#   cl /LD /O2 /I..\..\..\game lua_errors.c   or   x86_64-w64-mingw32-gcc -shared ...).
set -e
cd "$(dirname "$0")"
ZIG="../../../tools/venv/Lib/site-packages/ziglang/zig.exe"
"$ZIG" cc -target x86_64-windows-gnu -O2 -shared -I../../../game -o lua_errors.dll lua_errors.c
rm -f lua_errors.lib lua_errors.pdb
echo "built $(pwd)/lua_errors.dll"
