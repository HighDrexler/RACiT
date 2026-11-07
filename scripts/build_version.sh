#!/usr/bin/env bash
# build_version.sh <TITLE_ID> [jobs]: build the game for a disc other than BCUS98124.
# Needs scripts/setup_windows.sh done, and input/<TITLE_ID>/EBOOT.ELF and libsre.prx.
# Everything happens in build/<TITLE_ID>/; the result is RaCACiT_<TITLE_ID>.exe there.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TITLE="${1:?usage: build_version.sh <TITLE_ID> [jobs]}"
JOBS="${2:-6}"
WORK="$ROOT/build/$TITLE"
PYTHON="$ROOT/tools/venv/Scripts/python"
export PATH="$ROOT/tools/venv/Scripts:$PATH"

"$PYTHON" -I "$ROOT/scripts/retarget.py" "$TITLE"

cd "$WORK"
PY="$PYTHON -I" SKIP_DESKTOP=1 bash scripts/pipeline.sh
cmake -S windows -B game/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$WORK/windows/toolchain/zig-windows.cmake" \
    -DSDL2_DIR="$ROOT/tools/sdl2-win/lib/cmake/SDL2" \
    -DPython3_EXECUTABLE="$PYTHON.exe"
cmake --build game/build-win -j"$JOBS"
cp game/build-win/game/RaCACiT.exe "$WORK/RaCACiT_$TITLE.exe"
echo "Done: build/$TITLE/RaCACiT_$TITLE.exe"
