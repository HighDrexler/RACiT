#!/usr/bin/env bash
# update_runtime_patch.sh RUNTIME PATCH [--reset]: put this repository's runtime patch into the
# pinned runtime checkout (tools/ps3recomp). Used by scripts/setup_windows.sh and
# mac/setup_macos.sh. The pin is the runtime's checked-out commit (the setups check it).
#
# - The runtime holds exactly this patch and nothing else: up to date.
# - It holds nothing: the patch is applied.
# - It holds exactly a patch this repository had before (the copy kept in RUNTIME/.git, or a
#   version from the patch file's history): reset to the pin, the files that patch created
#   removed, and the new patch applied.
# - Anything else (your own edits, staged changes, a patch half taken out): it stops, and
#   --reset resets the runtime to the pin (git reset --hard, git clean -fdx) and applies the
#   patch, which throws those changes away.
set -euo pipefail
RUNTIME="$(cd "$1" && pwd)"
PATCH="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
RESET="${3:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REL=patches/ps3recomp-0001-racacit-runtime-fixes.patch
KEPT="$RUNTIME/.git/racit-patch.applied"
# Every git here runs with LF line endings whatever the user's settings (Git for Windows
# defaults core.autocrlf to true), so a reset or a show gives the bytes the patch is made
# against, and from the runtime or the repository root, never the caller's directory.
g() { git -c core.autocrlf=false -c core.eol=lf "$@"; }
PIN="$(g -C "$RUNTIME" rev-parse HEAD)"
rt() { g -C "$RUNTIME" "$@"; }
# Earlier setups kept their stamp in the runtime's tree: never count those as changes.
rm -f "$RUNTIME/.racit-patch" "$RUNTIME/.racit-patch.applied"

# The files patch $1 touches, one per line.
paths() { rt apply --numstat "$1" | cut -f3; }
# Whether the runtime is exactly the pin plus patch $1: nothing staged, no change outside the
# patch's files, and each of them as the patch makes it from the pin.
holds() {
    local p t ok=0
    rt diff --cached --quiet || return 1
    while IFS= read -r p; do
        grep -qxF -- "$p" < <(paths "$1") || return 1
    done < <(rt status --porcelain --untracked-files=all | cut -c4-)
    t="$(mktemp -d)"
    while IFS= read -r p; do
        if rt cat-file -e "$PIN:$p" 2>/dev/null; then mkdir -p "$t/$(dirname "$p")"; rt show "$PIN:$p" > "$t/$p"; fi
    done < <(paths "$1")
    if ( cd "$t" && export GIT_CEILING_DIRECTORIES="$(dirname "$t")" &&
         g apply --ignore-whitespace --whitespace=nowarn "$1" 2>/dev/null ); then
        while IFS= read -r p; do
            # Line endings aside: a checkout made before with core.autocrlf=true has CRLF.
            if [ -e "$t/$p" ]; then cmp -s <(tr -d '\r' < "$t/$p") <(tr -d '\r' < "$RUNTIME/$p") || { ok=1; break; }
            elif [ -e "$RUNTIME/$p" ]; then ok=1; break; fi
        done < <(paths "$1")
    else
        ok=1
    fi
    rm -rf "$t"
    return $ok
}
apply_new() {
    rt apply --ignore-whitespace --whitespace=nowarn "$PATCH"
    cp "$PATCH" "$KEPT"
    echo "Runtime patch applied."
}
to_pin() { rt reset -q --hard "$PIN"; }

if [ "$RESET" = --reset ]; then
    echo "Resetting $RUNTIME to its pinned commit (--reset): its changes are thrown away."
    to_pin
    rt clean -fdxq
    apply_new
    exit 0
fi
if holds "$PATCH"; then
    cp "$PATCH" "$KEPT"
    echo "Runtime patch is already applied."
    exit 0
fi
if rt diff --cached --quiet && [ -z "$(rt status --porcelain --untracked-files=all)" ]; then
    apply_new
    exit 0
fi
# A patch this repository had before, and nothing else?
old=""
if [ -f "$KEPT" ] && holds "$KEPT"; then
    old="$KEPT"
else
    tmp="$(mktemp)"
    trap 'rm -f "$tmp"' EXIT
    for c in $(g -C "$ROOT" log --format=%H -n 60 -- "$REL" 2>/dev/null); do
        g -C "$ROOT" show "$c:$REL" > "$tmp" 2>/dev/null || continue
        if holds "$tmp"; then old="$tmp"; echo "The runtime has the patch of $(g -C "$ROOT" log -1 --format='%h (%cs)' "$c")."; break; fi
    done
fi
if [ -z "$old" ]; then
    echo "$RUNTIME has local changes (edits, staged changes, or a runtime patch only partly there)." >&2
    echo "Commit or move them, then run this setup again with --reset (which resets it to the pinned commit)." >&2
    exit 1
fi
# Back to the pin: tracked files reset, and only the files the old patch created removed.
created="$(paths "$old" | while IFS= read -r p; do rt cat-file -e "$PIN:$p" 2>/dev/null || printf '%s\n' "$p"; done)"
to_pin
printf '%s\n' "$created" | while IFS= read -r p; do
    [ -n "$p" ] || continue
    rm -f -- "$RUNTIME/$p"
    rmdir -p -- "$(dirname "$RUNTIME/$p")" 2>/dev/null || true   # folders it made, once empty
done
echo "The previous runtime patch is out."
apply_new
