#!/usr/bin/env bash
# setup_windows.sh: everything the Windows build needs into tools/, at the exact
# versions the releases are built with. Run from Git Bash; needs git and Python 3.12.
# PYTHON=<command> picks the interpreter (default: py -3.12). Safe to run again.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p tools input

# pin <dir> <repository> <commit or tag>
pin() {
    [ -d "tools/$1/.git" ] && return 0
    git init -q "tools/$1"
    git -C "tools/$1" remote add origin "https://github.com/$2"
    git -C "tools/$1" -c core.autocrlf=false fetch -q --depth 1 origin "$3"
    git -C "tools/$1" -c core.autocrlf=false checkout -q FETCH_HEAD
    echo "tools/$1: $2 at $3"
}

echo "== Python tools"
[ -x tools/venv/Scripts/python.exe ] || ${PYTHON:-py -3.12} -m venv tools/venv
tools/venv/Scripts/python.exe -m pip install -q --disable-pip-version-check \
    ziglang==0.16.0 cmake==4.4.4 ninja==1.13.2 capstone==5.0.9 construct==2.10.70 \
    pycryptodome==3.24.1 tabulate==0.10.0 tqdm==4.70.1

echo "== Sources"
fresh_runtime=0
[ -d tools/ps3recomp/.git ] || fresh_runtime=1
pin ps3recomp      sp00nznet/ps3recomp         5244db92669b54831c6d3ebb7a96d58c4dbfa6df
pin Vulkan-Headers KhronosGroup/Vulkan-Headers c46850864f4661461b0f6cb9922c058ffea4915e
pin glslang-src    KhronosGroup/glslang        2fbed5e1fb43a1c480fe020e3bb686a31010989a
pin zlib           madler/zlib                 767c4c947852e143f582c85f14cf573411df1b35
pin SDL2-src       libsdl-org/SDL              release-2.32.10

# --ignore-whitespace: some of ps3recomp's files mix line endings.
if [ "$fresh_runtime" = 1 ]; then
    git -C tools/ps3recomp apply --ignore-whitespace ../../patches/ps3recomp-0001-racacit-runtime-fixes.patch
    echo "tools/ps3recomp: patched"
fi

echo "== SDL2 (static)"
if [ ! -f tools/sdl2-win/lib/libSDL2.a ]; then
    export PATH="$ROOT/tools/venv/Scripts:$PATH"
    cmake -S tools/SDL2-src -B tools/sdl2-win-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/windows/toolchain/zig-windows.cmake" \
        -DCMAKE_INSTALL_PREFIX="$ROOT/tools/sdl2-win" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF > tools/sdl2-win-config.log
    cmake --build tools/sdl2-win-build --target install > tools/sdl2-win-build.log
fi
echo "Done. Next: scripts/pipeline.sh, see BUILDING.md."
