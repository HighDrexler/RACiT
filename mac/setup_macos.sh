#!/usr/bin/env bash
# Fetch the pinned runtime and apply this port's patch. The user's Vulkan SDK
# or package manager supplies the dependencies listed in BUILDING.md.
# --reset: reset the runtime to its pinned commit before patching (throws away edits there).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME="$ROOT/tools/ps3recomp"
COMMIT=de4f090328ea08bef6b2f15caec0b85e343a7ece
PATCH="$ROOT/patches/ps3recomp-0001-racacit-runtime-fixes.patch"

[ "$(uname -s)" = Darwin ] || { echo "Run this setup on macOS."; exit 1; }
mkdir -p "$ROOT/tools" "$ROOT/input"
if [ ! -e "$RUNTIME/.git" ]; then
  mkdir -p "$RUNTIME"
  if [ -n "$(ls -A "$RUNTIME")" ]; then
    echo "Runtime directory is not a Git checkout. Use an empty directory or the pinned runtime checkout." >&2
    exit 1
  fi
  git init -q "$RUNTIME"
  git -C "$RUNTIME" remote add origin https://github.com/sp00nznet/ps3recomp.git
fi
if ! git -C "$RUNTIME" rev-parse --verify HEAD >/dev/null 2>&1; then
  git -C "$RUNTIME" -c core.autocrlf=false -c core.eol=lf fetch --depth 1 origin "$COMMIT"
  git -C "$RUNTIME" -c core.autocrlf=false -c core.eol=lf checkout -q --detach FETCH_HEAD
fi
# A checkout from an earlier pin (1.0.x used 5244db92) moves with --reset, which throws away its changes.
if [ "$(git -C "$RUNTIME" rev-parse HEAD)" != "$COMMIT" ] && [ "${1:-}" = --reset ]; then
  echo "Moving $RUNTIME to $COMMIT (--reset): its changes are thrown away."
  git -C "$RUNTIME" -c core.autocrlf=false -c core.eol=lf fetch -q --depth 1 origin "$COMMIT"
  git -C "$RUNTIME" -c core.autocrlf=false -c core.eol=lf checkout -q -f --detach FETCH_HEAD
fi
[ "$(git -C "$RUNTIME" rev-parse HEAD)" = "$COMMIT" ] || {
  echo "$RUNTIME is at $(git -C "$RUNTIME" rev-parse --short HEAD), this tree needs $COMMIT."
  echo "Run this setup again with --reset to move it (throws away changes there), or use a separate checkout."
  exit 1
}
# The patch, swapped whenever it changed (scripts/update_runtime_patch.sh). Your own edits in
# the runtime stop it; --reset throws them away instead.
bash "$ROOT/scripts/update_runtime_patch.sh" "$RUNTIME" "$PATCH" "${1:-}"
echo "Ready for scripts/pipeline.sh. Dependency and input requirements are in BUILDING.md."
