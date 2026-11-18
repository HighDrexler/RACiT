#!/usr/bin/env bash
# Ratchet & Clank: Into the Nexus (BCUS99245) — static recompilation pipeline.
#
# Usage: scripts/pipeline.sh [path/to/EBOOT.ELF]
# Default input is input/EBOOT.ELF (decrypted from the disc's EBOOT.BIN with
# scripts/unself.py; see docs/dev/HANDOFF.md).
#
# Stages:
#   1. ppu_loader       -> out/  (function table, imports, image)
#   2. ghidra (optional)-> analysis/ghidra/  (names, only if Ghidra output exists)
#   3. ppu_lifter       -> game/src/recomp/
#   4. SPU extract+lift -> game/spu/
#
# Started from RACiT's pipeline (rac-acit/scripts/pipeline.sh). Differences:
#   - RACiT's patch_lift.py is never run here: every patch in it is keyed to A
#     Crack in Time's function addresses (kept for reference as
#     scripts/acit_patch_lift.py, which refuses to run). Nexus's own patches are
#     scripts/patch_lift_nexus.py, run after the PPU lift.
#   - The SPU job lifts use Nexus's own ranges and lists (spu_starts.txt,
#     spu_submodules.txt, spu_submodule_entries.txt; ACiT's are acit_*.txt).
#     NEXUS_SPU_SUB=0 skips the sub-module lift.
#   - A re-lift moves chunk boundaries and forces a full rebuild. After a lifter
#     fix, lift into a scratch folder and use scripts/dev/splice_relift.py to
#     replace only the functions that changed.
#
# Heavy: the PPU lift and the compile take a long time and a lot of RAM. The
# owner asks before every build: run it low-priority, never while RaCACiT.exe or
# a game is running.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PS3R="$ROOT/tools/ps3recomp"
ELF="${1:-$ROOT/input/EBOOT.ELF}"
OUT="$ROOT/out"
RECOMP="$ROOT/game/src/recomp"
SPU="$ROOT/game/spu"
# Python: the toolchain shared with RACiT (rac-acit/tools/venv) unless PY is set.
SHARED_PY="$ROOT/../rac-acit/tools/venv/Scripts/python.exe"
if [ -z "${PY:-}" ]; then
  if [ -x "$ROOT/tools/venv/Scripts/python.exe" ]; then PY="$ROOT/tools/venv/Scripts/python.exe -I"
  elif [ -x "$SHARED_PY" ]; then PY="$SHARED_PY -I"
  else PY="python3 -I"; fi
fi

[ -f "$ELF" ] || { echo "No ELF at $ELF — decrypt EBOOT.BIN into input/EBOOT.ELF (for example with RPCS3; see BUILDING.md)"; exit 1; }
mkdir -p "$OUT" "$RECOMP" "$SPU"

echo "== 1/4 Loader"
$PY "$PS3R/tools/ppu_loader.py" "$ELF" --output "$OUT"
BASE="$(basename "${ELF%.*}")"
FUNCS="$OUT/$BASE.functions.json"
if ! $PY -c "import json,sys;d=json.load(open(sys.argv[1]));f=d.get('functions',d) if isinstance(d,dict) else d;sys.exit(0 if f else 1)" "$FUNCS"; then
  echo "   Loader found no OPD functions — falling back to heuristic find_functions.py"
  FUNCS="$OUT/$BASE.found_functions.json"
  $PY "$PS3R/tools/find_functions.py" "$ELF" --output "$FUNCS"
fi

echo "== 2/4 Ghidra names (optional)"
if [ -f "$ROOT/analysis/ghidra/functions.json" ]; then
  $PY "$PS3R/tools/ghidra_names.py" "$ROOT/analysis/ghidra" -o "$OUT/ghidra_names.json" \
    || echo "   ghidra_names failed; continuing with func_ADDR names"
else
  echo "   No analysis/ghidra/functions.json yet — skipping"
fi

echo "== 3/4 PPU lift"
NAMES=()
[ -f "$OUT/names.json" ] && NAMES=(--names "$OUT/names.json")
PS3RECOMP_CHUNK_LINES=${PS3RECOMP_CHUNK_LINES:-100000} $PY "$PS3R/tools/ppu_lifter.py" "$ELF" \
  --functions "$FUNCS" \
  --hle-stubs "$OUT/$BASE.imports.json" \
  "${NAMES[@]}" \
  --output "$RECOMP"

# Nexus's own patches (scripts/patch_lift_nexus.py). RACiT's patch_lift.py is
# keyed to ACiT addresses and is never run on this lift.
$PY "$ROOT/scripts/patch_lift_nexus.py" "$RECOMP"

echo "== 4/4 SPU images"
$PY "$PS3R/tools/extract_spu_images.py" "$ELF" --output "$OUT/spu_images" \
  || echo "   No embedded SPU images extracted"
# The SPU ELF images and libsre are lifted together by lift_libsre.py (one
# symbol prefix and image id per image, game/spu/spu_workloads.c). libsre is a
# firmware library: decrypt dev_flash/sys/external/libsre.sprx with RPCS3 (the
# same file RACiT uses).
LIBSRE="${LIBSRE:-$ROOT/input/libsre.prx}"
[ -f "$LIBSRE" ] || { echo "No libsre.prx at $LIBSRE"; exit 1; }
$PY "$ROOT/scripts/lift_libsre.py" "$LIBSRE"

# Nexus's two SPURS policy modules (WWS job manager, igPm) and the igPm runtime
# block it copies to LS 0x3F580: their tables hold Nexus addresses (2026-11-09).
$PY "$ROOT/scripts/lift_policy_modules.py" "$ELF"
$PY "$ROOT/scripts/lift_spu_jobs.py" "$ELF" --extra
# Nexus's raw igPm jobs (0xC15000..0xD19B00, 161 jobs, 2026-11-10): gameplay went
# from ~4 fps (every job interpreted) to the game's 30 fps cap.
$PY "$ROOT/scripts/lift_spu_jobs.py" "$ELF"
# Job modules a job loads above itself (scripts/spu_submodules.txt, Nexus's own
# list since 2026-11-10: 112 rows, 28 modules; ACiT's kept as acit_*.txt). The
# main worker job swaps a dozen modules through five slots; unlifted they ran in
# the SPU interpreter and caused the streaming hitches. NEXUS_SPU_SUB=0 skips.
if [ "${NEXUS_SPU_SUB:-1}" = 1 ]; then
  $PY "$ROOT/scripts/lift_spu_jobs.py" "$ELF" --sub
fi

echo "Lifted: $RECOMP and $SPU"
