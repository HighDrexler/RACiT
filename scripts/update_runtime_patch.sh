#!/usr/bin/env bash
# update_runtime_patch.sh RUNTIME PATCH [--reset]: put this repository's runtime patch into the
# pinned runtime checkout (tools/ps3recomp), and swap it when it changed.
# Used by scripts/setup_windows.sh and mac/setup_macos.sh.
#
# RUNTIME/.racit-patch holds the hash of the patch applied, RUNTIME/.racit-patch.applied a copy
# of it. When the patch changed, the old one (that copy, or else a version from this
# repository's history of the patch file, for a checkout set up before the copy was kept) is
# swapped for the new one, so the builder's own edits in the runtime stay. The swap is tried
# first on a copy of the files the two patches touch; the runtime is only changed when it
# works, so on failure the old patch stays applied and the stamp as it was, and running this
# again after resolving the conflict picks up from there. A runtime with no patch in it yet
# gets the new one over whatever local edits it has, if it applies. When nothing works it
# stops and says why; --reset instead resets the runtime to its pinned commit first, which
# throws away every change in it.
set -euo pipefail
RUNTIME="$(cd "$1" && pwd)"
PATCH="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
RESET="${3:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAMP="$RUNTIME/.racit-patch"
APPLIED="$RUNTIME/.racit-patch.applied"
REL=patches/ps3recomp-0001-racacit-runtime-fixes.patch
hash() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -c1-64; }
fits() { git -C "$RUNTIME" apply --check --ignore-whitespace "$@" 2>/dev/null; }
want="$(hash "$PATCH")"
done_() { echo "$want" > "$STAMP"; cp "$PATCH" "$APPLIED"; echo "Runtime patch applied."; exit 0; }

[ "$(cat "$STAMP" 2>/dev/null)" = "$want" ] && { echo "Runtime patch is already applied."; exit 0; }
if fits --reverse "$PATCH"; then
    echo "$want" > "$STAMP"; cp "$PATCH" "$APPLIED"; echo "Runtime patch is already applied."; exit 0
fi
stop() {
    echo "$RUNTIME: $1" >&2
    echo "Commit or stash your changes there (git -C \"$RUNTIME\" stash -u), or run this setup again" >&2
    echo "with --reset to reset the runtime to its pinned commit, which throws them away." >&2
    exit 1
}
# The files a patch touches.
paths() { git apply --numstat "$1" | cut -f3; }
# Whether swapping patch $1 for $2 works, tried on a copy of the files they touch.
swap_fits() {
    local t; t="$(mktemp -d)"
    { paths "$1"; paths "$2"; } | sort -u | while IFS= read -r p; do
        if [ -e "$RUNTIME/$p" ]; then mkdir -p "$t/$(dirname "$p")"; cp -p "$RUNTIME/$p" "$t/$p"; fi
    done
    local ok=0
    ( cd "$t" && export GIT_CEILING_DIRECTORIES="$(dirname "$t")" &&
      git apply --reverse --ignore-whitespace --whitespace=nowarn "$1" 2>/dev/null &&
      git apply --check --ignore-whitespace "$2" 2>/dev/null ) || ok=1
    rm -rf "$t"
    return $ok
}
if [ "$RESET" = --reset ]; then
    echo "Resetting $RUNTIME to its pinned commit (--reset): its changes are thrown away."
    git -C "$RUNTIME" checkout -q -- .
    git -C "$RUNTIME" clean -fdq -e .racit-patch -e .racit-patch.applied
    fits "$PATCH" || stop "the runtime patch doesn't apply to the pinned commit."
    git -C "$RUNTIME" apply --ignore-whitespace --whitespace=nowarn "$PATCH"
    done_
fi
# Which earlier patch is in the runtime, if any.
old=""
if [ -f "$APPLIED" ] && fits --reverse "$APPLIED"; then
    old="$APPLIED"
elif [ -n "$(git -C "$RUNTIME" status --porcelain -- . ':!.racit-patch' ':!.racit-patch.applied')" ]; then
    tmp="$(mktemp)"
    trap 'rm -f "$tmp"' EXIT
    for c in $(git -C "$ROOT" log --format=%H -n 60 -- "$REL" 2>/dev/null); do
        git -C "$ROOT" show "$c:$REL" > "$tmp" 2>/dev/null || continue
        if fits --reverse "$tmp"; then old="$tmp"; echo "The runtime has the patch of $(git -C "$ROOT" log -1 --format='%h (%cs)' "$c")."; break; fi
    done
fi
if [ -z "$old" ]; then
    # No patch of ours in it: the new one goes over whatever edits are there.
    fits "$PATCH" || stop "the runtime patch doesn't apply over the changes there (and they aren't a runtime patch this repository had)."
    git -C "$RUNTIME" apply --ignore-whitespace --whitespace=nowarn "$PATCH"
    done_
fi
swap_fits "$old" "$PATCH" || stop "the new runtime patch doesn't apply over your changes there; the previous one is still applied."
git -C "$RUNTIME" apply --reverse --ignore-whitespace --whitespace=nowarn "$old"
if ! git -C "$RUNTIME" apply --ignore-whitespace --whitespace=nowarn "$PATCH"; then
    git -C "$RUNTIME" apply --ignore-whitespace --whitespace=nowarn "$old"   # as it was
    stop "the new runtime patch didn't apply; the previous one is back."
fi
echo "The previous runtime patch is swapped for the new one."
done_
