#!/usr/bin/env bash
# Ratchet & Clank: A Crack in Time — static recompilation pipeline.
#
# Usage: scripts/pipeline.sh [path/to/EBOOT.ELF]
# Default input is input/EBOOT.ELF (your own decrypted dump).
#
# Stages:
#   1. ppu_loader       -> out/  (function table, imports, image)
#   2. ghidra (optional)-> analysis/ghidra/  (names, only if Ghidra output exists)
#   3. ppu_lifter       -> game/src/recomp/
#   4. SPU extract+lift -> game/spu/<image>/spu_recomp.c
#   5. desktop build    -> game/build/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PS3R="$ROOT/tools/ps3recomp"
ELF="${1:-$ROOT/input/EBOOT.ELF}"
OUT="$ROOT/out"
RECOMP="$ROOT/game/src/recomp"
SPU="$ROOT/game/spu"
PY="${PY:-python3 -I}"

[ -f "$ELF" ] || { echo "No ELF at $ELF — put your decrypted EBOOT.ELF in input/"; exit 1; }
mkdir -p "$OUT" "$RECOMP" "$SPU"

echo "== 1/5 Loader"
$PY "$PS3R/tools/ppu_loader.py" "$ELF" --output "$OUT"
BASE="$(basename "${ELF%.*}")"
FUNCS="$OUT/$BASE.functions.json"
if ! $PY -c "import json,sys;d=json.load(open(sys.argv[1]));f=d.get('functions',d) if isinstance(d,dict) else d;sys.exit(0 if f else 1)" "$FUNCS"; then
  echo "   Loader found no OPD functions — falling back to heuristic find_functions.py"
  FUNCS="$OUT/$BASE.found_functions.json"
  $PY "$PS3R/tools/find_functions.py" "$ELF" --output "$FUNCS"
fi

echo "== 2/5 Ghidra names (optional)"
if [ -f "$ROOT/analysis/ghidra/functions.json" ]; then
  $PY "$PS3R/tools/ghidra_names.py" "$ROOT/analysis/ghidra" -o "$OUT/ghidra_names.json" \
    || echo "   ghidra_names failed; continuing with func_ADDR names"
  $PY "$ROOT/scripts/name_pass.py" \
    || echo "   name_pass failed; continuing with string-derived names only"
else
  echo "   No analysis/ghidra/functions.json yet — skipping (run Ghidra on your PC, copy results in)"
fi

echo "== 3/5 PPU lift"
# names.json (name_pass.py) only adds comments above lifted functions.
NAMES=()
[ -f "$OUT/names.json" ] && NAMES=(--names "$OUT/names.json")
PS3RECOMP_CHUNK_LINES=${PS3RECOMP_CHUNK_LINES:-100000} $PY "$PS3R/tools/ppu_lifter.py" "$ELF" \
  --functions "$FUNCS" \
  --hle-stubs "$OUT/$BASE.imports.json" \
  "${NAMES[@]}" \
  --output "$RECOMP"

$PY "$ROOT/scripts/patch_lift.py" "$RECOMP"

echo "== 4/5 SPU images"
$PY "$PS3R/tools/extract_spu_images.py" "$ELF" --output "$OUT/spu_images" \
  || echo "   No embedded SPU images extracted"
# The SPU ELF images are lifted by lift_libsre.py (through build_spu_workloads:
# one symbol prefix and image id per image, game + libsre in one registry,
# game/spu/spu_workloads.c). Lifting them bare here gives every image the same
# spu_func_* names and the link fails on duplicates. Needs a decrypted
# libsre.prx from your own firmware in input/.
LIBSRE="${LIBSRE:-$ROOT/input/libsre.prx}"
[ -f "$LIBSRE" ] || { echo "No libsre.prx at $LIBSRE — decrypt dev_flash/sys/external/libsre.sprx with RPCS3"; exit 1; }
$PY "$ROOT/scripts/lift_libsre.py" "$LIBSRE"

# Raw SPURS policy modules and igPm job binaries (content-matched overlays),
# and the post-lift fixups (setjmp/longjmp, hoisted GCM read, Lua hooks).
$PY "$ROOT/scripts/lift_policy_modules.py" "$ELF"
$PY "$ROOT/scripts/lift_spu_jobs.py" "$ELF"
$PY "$ROOT/scripts/lift_spu_jobs.py" "$ELF" --extra
$PY "$ROOT/scripts/patch_lift.py" || true

# SKIP_DESKTOP=1: lift only (Windows: the build is windows/, see README).
[ "${SKIP_DESKTOP:-0}" = 1 ] && { echo "Lifted: $RECOMP and $SPU"; exit 0; }

echo "== 5/5 Desktop build"
cmake -S "$ROOT/game" -B "$ROOT/game/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS3RECOMP_DIR="$PS3R" -DRECOMP_DIR="$RECOMP" \
  -DPS3RECOMP_VULKAN_INCLUDE_DIR="$ROOT/tools/Vulkan-Headers/include" -DPS3RECOMP_RSX_VULKAN=ON \
  -DCMAKE_PREFIX_PATH="$ROOT/tools/sdl2"
cmake --build "$ROOT/game/build" -j"$(nproc)"
echo "Done: $ROOT/game/build/RaCACiTRecomp"
