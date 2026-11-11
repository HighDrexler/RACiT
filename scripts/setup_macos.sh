#!/usr/bin/env bash
# Fetch the pinned runtime and apply this port's patch. The user's Vulkan SDK
# or package manager supplies the dependencies listed in BUILDING.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME="$ROOT/tools/ps3recomp"
COMMIT=5244db92669b54831c6d3ebb7a96d58c4dbfa6df
PATCH="$ROOT/patches/ps3recomp-0001-racacit-runtime-fixes.patch"

[ "$(uname -s)" = Darwin ] || { echo "Run this setup on macOS."; exit 1; }
mkdir -p "$ROOT/tools" "$ROOT/input"
if [ ! -e "$RUNTIME" ]; then
  git init -q "$RUNTIME"
  git -C "$RUNTIME" remote add origin https://github.com/sp00nznet/ps3recomp.git
fi
if ! git -C "$RUNTIME" rev-parse --verify HEAD >/dev/null 2>&1; then
  git -C "$RUNTIME" -c core.autocrlf=false fetch --depth 1 origin "$COMMIT"
  git -C "$RUNTIME" -c core.autocrlf=false checkout -q --detach FETCH_HEAD
fi
[ "$(git -C "$RUNTIME" rev-parse HEAD)" = "$COMMIT" ] || {
  echo "Runtime revision differs. Use the pinned runtime in a separate checkout."
  exit 1
}
if git -C "$RUNTIME" apply --reverse --check --ignore-whitespace "$PATCH" 2>/dev/null; then
  echo "Runtime patch is already applied."
else
  git -C "$RUNTIME" apply --check --ignore-whitespace "$PATCH"
  git -C "$RUNTIME" apply --ignore-whitespace "$PATCH"
  echo "Runtime patch applied."
fi
echo "Ready for scripts/pipeline.sh. Dependency and input requirements are in BUILDING.md."
