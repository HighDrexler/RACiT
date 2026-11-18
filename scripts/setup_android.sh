#!/usr/bin/env bash
# setup_android.sh: the sources the Android build needs into tools/, at the exact versions
# the Android releases are built with, with the Android runtime, SDL2 changes applied.
# Run from Git Bash; needs git. Safe to run again.
#
# Then: the lift (scripts/pipeline.sh, as for Windows; it writes game/src/recomp, game/spu and
# game/prx from your own EBOOT), python scripts/android_spu_fast.py and
# python scripts/upgrade-vmx-permute.py over the lifted code, and
#   powershell -File scripts/build-android.ps1 -ToolchainsRoot <folder> -PythonExecutable <python>
# where <folder> holds android-ndk-r29, android-sdk, gradle-8.13 and jdk-17.0.20.1+1, and
# HostToolsRoot (default <StateRoot>/toolchains) holds cmake/ and ninja.exe.
# The APK is signed with a key made on the first build (android/racit-independent.keystore,
# never committed): an APK installs over another only when both carry the same key.
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

echo "== Python tools (the lift: scripts/pipeline.sh)"
[ -x tools/venv/Scripts/python.exe ] || ${PYTHON:-py -3.12} -m venv tools/venv
tools/venv/Scripts/python.exe -m pip install -q --disable-pip-version-check \
    ziglang==0.16.0 cmake==4.4.4 ninja==1.13.2 capstone==5.0.9 construct==2.10.70 \
    pycryptodome==3.24.1 tabulate==0.10.0 tqdm==4.70.1

echo "== Sources"
fresh_runtime=0; fresh_sdl=0
[ -d tools/ps3recomp/.git ] || fresh_runtime=1
[ -d tools/SDL2-src/.git ] || fresh_sdl=1
pin ps3recomp      sp00nznet/ps3recomp         5244db92669b54831c6d3ebb7a96d58c4dbfa6df
pin Vulkan-Headers KhronosGroup/Vulkan-Headers c46850864f4661461b0f6cb9922c058ffea4915e
pin glslang-src    KhronosGroup/glslang        2fbed5e1fb43a1c480fe020e3bb686a31010989a
pin zlib           madler/zlib                 767c4c947852e143f582c85f14cf573411df1b35
pin SDL2-src       libsdl-org/SDL              release-2.32.10

# The Android runtime (patches/ps3recomp-android.patch, in place of the Windows patch) and the
# Android changes to SDL2 (patches/sdl2-android.patch). --ignore-whitespace: some files mix line endings.
if [ "$fresh_runtime" = 1 ]; then
    git -C tools/ps3recomp apply --ignore-whitespace ../../patches/ps3recomp-android.patch
    echo "tools/ps3recomp: Android runtime applied"
fi
if [ "$fresh_sdl" = 1 ]; then
    git -C tools/SDL2-src apply --ignore-whitespace ../../patches/sdl2-android.patch
    echo "tools/SDL2-src: Android changes applied"
fi
echo "Done."
