#!/usr/bin/env bash
# update_runtime_patch.sh RUNTIME PATCH [--reset]: put this repository's runtime patch into the
# pinned runtime checkout (tools/ps3recomp), and swap it when it changed.
# Used by scripts/setup_windows.sh and mac/setup_macos.sh.
#
# RUNTIME/.racit-patch holds the hash of the patch applied, RUNTIME/.racit-patch.applied a copy
# of it. When the patch changed, the old one is reverse-applied (from that copy, or else from
# this repository's history of the patch file, for a checkout set up before the copy was kept)
# and the new one applied, so the builder's own edits in the runtime stay. When neither works it
# stops and says why; --reset instead resets the runtime to its pinned commit first, which
# throws away every change in it.
set -euo pipefail
RUNTIME="$1"
PATCH="$2"
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
if [ "$RESET" = --reset ]; then
    echo "Resetting $RUNTIME to its pinned commit (--reset): its changes are thrown away."
    git -C "$RUNTIME" checkout -q -- .
    git -C "$RUNTIME" clean -fdq -e .racit-patch -e .racit-patch.applied
elif [ -n "$(git -C "$RUNTIME" status --porcelain -- . ':!.racit-patch' ':!.racit-patch.applied')" ]; then
    # Something is applied: take out the patch that is there.
    old=""
    if [ -f "$APPLIED" ] && fits --reverse "$APPLIED"; then
        old="$APPLIED"
    else
        tmp="$(mktemp)"
        trap 'rm -f "$tmp"' EXIT
        for c in $(git -C "$ROOT" log --format=%H -n 60 -- "$REL" 2>/dev/null); do
            git -C "$ROOT" show "$c:$REL" > "$tmp" 2>/dev/null || continue
            if fits --reverse "$tmp"; then old="$tmp"; echo "The runtime has the patch of $(git -C "$ROOT" log -1 --format='%h (%cs)' "$c")."; break; fi
        done
    fi
    [ -n "$old" ] || stop "it has changes that aren't a runtime patch this repository had, so the new patch can't be swapped in."
    git -C "$RUNTIME" apply --reverse --ignore-whitespace --whitespace=nowarn "$old"
    echo "The previous runtime patch is out."
fi
fits "$PATCH" || stop "the new runtime patch doesn't apply over your changes there."
git -C "$RUNTIME" apply --ignore-whitespace --whitespace=nowarn "$PATCH"
done_
