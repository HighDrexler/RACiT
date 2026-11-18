#!/usr/bin/env bash
# Configure (first time) and build RaCNexus.exe with the toolchain shared with
# RACiT (../rac-acit/tools). Heavy: ask the owner first, and run it through
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/lowprio.ps1 "bash scripts/build.sh" out/build.log
# JOBS (default 6: ~32 GB RAM, as RACiT's BUILDING.md says).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# tools/ from scripts/setup_windows.sh if it is there, else the toolchain shared with RACiT.
if [ -z "${NEXUS_SHARED_TOOLS:-}" ] && [ -x "$ROOT/tools/venv/Scripts/python.exe" ]; then NEXUS_SHARED_TOOLS="$ROOT/tools"; fi
SHARED="${NEXUS_SHARED_TOOLS:-$ROOT/../rac-acit/tools}"
SHARED="$(cd "$SHARED" && pwd)"
W() { cygpath -m "$1"; }
export PATH="$SHARED/venv/Scripts:$PATH"
BUILD="$ROOT/game/build-win"
cd "$ROOT"
if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -S windows -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$(W "$ROOT/windows/toolchain/zig-windows.cmake")" \
    -DSDL2_DIR="$(W "$SHARED/sdl2-win/lib/cmake/SDL2")" \
    -DPython3_EXECUTABLE="$(W "$SHARED/venv/Scripts/python.exe")"
fi
cmake --build "$BUILD" -j"${JOBS:-6}"
ls -la "$BUILD/game/RaCNexus.exe"
