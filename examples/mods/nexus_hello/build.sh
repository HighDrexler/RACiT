#!/bin/sh
# Builds nexus_hello.dll with RACiT's zig (any Windows x86-64 C compiler works).
set -e
cd "$(dirname "$0")"
ZIG="../../../../rac-acit/tools/venv/Lib/site-packages/ziglang/zig.exe"
"$ZIG" cc -target x86_64-windows-gnu -O2 -shared -I../../../game -o nexus_hello.dll nexus_hello.c
rm -f nexus_hello.lib nexus_hello.pdb
echo "built $(pwd)/nexus_hello.dll"
