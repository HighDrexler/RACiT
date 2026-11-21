#!/usr/bin/env bash
# build_version.sh <TITLE_ID> [jobs]: build the game for a disc other than BCUS98124.
# Needs scripts/setup_windows.sh done, and input/<TITLE_ID>/EBOOT.ELF and libsre.prx.
# With the USA input/EBOOT.ELF there too, retarget.py checks every converted address.
# Everything happens in build/<TITLE_ID>/; the results there are RaCACiT_<TITLE_ID>.exe
# and, unless NO_COMPAT=1, the older-processor RaCACiT_<TITLE_ID>_compat.exe.
# (Contributed by Norby99 as public PR #26.)
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

# The lift's hooks are checked against versions/<TITLE>-1.00-hook-symbols.txt (game/CMakeLists.txt).
configure() {   # configure <build dir> [extra cmake args]
    local dir="$1"; shift
    cmake -S windows -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$WORK/windows/toolchain/zig-windows.cmake" \
        -DSDL2_DIR="$ROOT/tools/sdl2-win/lib/cmake/SDL2" \
        -DPython3_EXECUTABLE="$PYTHON.exe" \
        -DRACACIT_GAME_PROFILE="$TITLE-1.00" -DRACACIT_ELF="$ROOT/input/$TITLE/EBOOT.ELF" "$@"
}
configure game/build-win
cmake --build game/build-win -j"$JOBS"
cp game/build-win/game/RaCACiT.exe "$WORK/RaCACiT_$TITLE.exe"
echo "Done: build/$TITLE/RaCACiT_$TITLE.exe"

if [ -z "${NO_COMPAT:-}" ]; then
    configure game/build-win-compat -DRACACIT_CPU_COMPAT=ON
    cmake --build game/build-win-compat -j"$JOBS"
    cp game/build-win-compat/game/RaCACiT_compat.exe "$WORK/RaCACiT_${TITLE}_compat.exe"
    echo "Done: build/$TITLE/RaCACiT_${TITLE}_compat.exe"
fi
