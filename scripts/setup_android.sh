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

# pin <dir> <repository> <commit>
pin() {
    local dir="tools/$1" repository="$2" revision="$3"
    if [ ! -e "$dir/.git" ]; then
        if [ -d "$dir" ] && [ -n "$(ls -A "$dir")" ]; then
            echo "$dir is not a Git checkout. Use an empty directory or the pinned checkout." >&2
            return 1
        fi
        git init -q "$dir"
        git -C "$dir" remote add origin "https://github.com/$repository"
    fi
    if ! git -C "$dir" rev-parse --verify HEAD >/dev/null 2>&1; then
        git -C "$dir" -c core.autocrlf=false fetch -q --depth 1 "https://github.com/$repository" "$revision"
        git -C "$dir" -c core.autocrlf=false checkout -q --detach FETCH_HEAD
    fi
    local actual
    actual=$(git -C "$dir" rev-parse HEAD)
    if [ "$actual" != "$revision" ]; then
        echo "$dir is at $actual. Use the pinned checkout at $revision." >&2
        return 1
    fi
    echo "$dir: $repository at $revision"
}

apply_dependency_patch() {
    local dir="tools/$1" patch="$ROOT/patches/$2"
    if git -C "$dir" apply --reverse --check --ignore-whitespace "$patch" 2>/dev/null; then
        echo "$dir: $2 already applied"
        return
    fi
    if ! git -C "$dir" apply --ignore-whitespace "$patch"; then
        echo "$dir requires a clean pinned checkout for $2." >&2
        return 1
    fi
    echo "$dir: $2 applied"
}

echo "== Python tools (the lift: scripts/pipeline.sh)"
[ -x tools/venv/Scripts/python.exe ] || ${PYTHON:-py -3.12} -m venv tools/venv
tools/venv/Scripts/python.exe -m pip install -q --disable-pip-version-check \
    ziglang==0.16.0 cmake==4.4.4 ninja==1.13.2 capstone==5.0.9 construct==2.10.70 \
    pycryptodome==3.24.1 tabulate==0.10.0 tqdm==4.70.1

echo "== Sources"
pin ps3recomp      sp00nznet/ps3recomp         5244db92669b54831c6d3ebb7a96d58c4dbfa6df
pin Vulkan-Headers KhronosGroup/Vulkan-Headers c46850864f4661461b0f6cb9922c058ffea4915e
pin glslang-src    KhronosGroup/glslang        2fbed5e1fb43a1c480fe020e3bb686a31010989a
pin zlib           madler/zlib                767c4c947852e143f582c85f14cf573411df1b35
pin SDL2-src       libsdl-org/SDL              5d249570393f7a37e037abf22cd6012a4cc56a71

# Some of the runtime source files mix line endings.
apply_dependency_patch ps3recomp ps3recomp-android.patch
apply_dependency_patch SDL2-src sdl2-android.patch
echo "Done."
