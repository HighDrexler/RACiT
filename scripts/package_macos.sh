#!/bin/bash
# package_macos.sh: a macOS (Apple Silicon) release folder and its zip, laid out as the
# Windows release is:
#   RACiT-<launcher version>-macos/
#     RaCACiT Launcher.app          the launcher (launcher/build_macos.sh)
#     README.txt                    launcher/README.txt
#     builds/v<N>/RaCACiT           the game build (game/build/RaCACiTRecomp), version.txt
#     builds/v<N>/lib/              its libraries: the Vulkan loader, MoltenVK (+ MoltenVK_icd.json),
#                                   SDL2 (sdl2-compat) with the SDL3 it loads, glslang, SPIRV-Tools
#     mods/                         the built-in mods (launcher/mods)
# Nothing in it loads from Homebrew: every library is copied in and linked by @rpath. All of
# it is signed ad hoc (codesign -s -), so Gatekeeper asks once ("Open Anyway").
#
# usage: scripts/package_macos.sh --version N [--game PATH] [--out DIR] [--no-zip]
#   --version N   the game build's number (builds/vN), as the Windows releases count them
#   --game PATH   the game binary (default game/build/RaCACiTRecomp)
#   --out DIR     where the folder and zip go (default out/macos)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BREW="${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}"
GAME="$ROOT/game/build/RaCACiTRecomp"
OUT="$ROOT/out/macos"
VER=""
ZIP=1
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VER="$2"; shift 2;;
        --game) GAME="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        --no-zip) ZIP=0; shift;;
        *) echo "usage: $0 --version N [--game PATH] [--out DIR] [--no-zip]" >&2; exit 2;;
    esac
done
case "$VER" in ''|*[!0-9]*) echo "package_macos.sh: --version N (the build number, builds/vN) is needed" >&2; exit 2;; esac
[ -f "$GAME" ] || { echo "package_macos.sh: no game binary at $GAME (see BUILDING.md, macOS Apple Silicon build)" >&2; exit 1; }
. "$ROOT/scripts/macos_dylibs.sh"

bash "$ROOT/launcher/build_macos.sh"
LVER="$(sed -n 's/^static const char\* kLauncherVersion = "\(.*\)";/\1/p' "$ROOT/launcher/src/app.cpp")"
NAME="RACiT-$LVER-macos"
REL="$OUT/$NAME"
rm -rf "$REL"; mkdir -p "$REL/builds/v$VER/lib"

ditto --norsrc --noextattr "$ROOT/launcher/build/RaCACiT Launcher.app" "$REL/RaCACiT Launcher.app"
cp "$ROOT/launcher/README.txt" "$REL/README.txt"
cp -R "$ROOT/launcher/mods" "$REL/mods"

B="$REL/builds/v$VER"
cp "$GAME" "$B/RaCACiT"; chmod 755 "$B/RaCACiT"
echo "$VER" > "$B/version.txt"
# The game build's own caches, when they sit beside the binary (the launcher seeds data/cache from them).
for c in shadercache.bin pipelinelist.bin; do [ -f "$(dirname "$GAME")/$c" ] && cp "$(dirname "$GAME")/$c" "$B/"; done

# Libraries: what the game links, plus what it dlopen()s -- the Vulkan loader
# (PS3RECOMP_VULKAN_LIBRARY), MoltenVK (through its ICD file) and SDL3 (through sdl2-compat).
bundle_dylibs "$B/RaCACiT" "$B/lib" "@executable_path/lib" \
    "$BREW/lib/libvulkan.1.dylib" "$BREW/lib/libMoltenVK.dylib" "$BREW/lib/libSDL3.0.dylib=libSDL3.dylib"
cat > "$B/lib/MoltenVK_icd.json" <<JSON
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./libMoltenVK.dylib",
        "api_version": "$(sed -n 's/.*"api_version" *: *"\([^"]*\)".*/\1/p' "$BREW/etc/vulkan/icd.d/MoltenVK_icd.json")",
        "is_portability_driver": true
    }
}
JSON

codesign --force -s - "$B/lib/"*.dylib
codesign --force -s - "$B/RaCACiT"
codesign --force -s - "$REL/RaCACiT Launcher.app/Contents/Frameworks/"*.dylib
codesign --force -s - "$REL/RaCACiT Launcher.app"

# Check: nothing outside the system and the bundle, every signature good.
bad=0
for f in "$B/RaCACiT" "$B/lib/"*.dylib "$REL/RaCACiT Launcher.app/Contents/MacOS/RaCACiT Launcher" "$REL/RaCACiT Launcher.app/Contents/Frameworks/"*.dylib; do
    left="$(foreign_deps "$f")"
    [ -z "$left" ] || { echo "still outside the bundle in $f: $left" >&2; bad=1; }
    for d in $(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep '^@rpath/'); do
        n="${d#@rpath/}"
        [ -f "$(dirname "$f")/$n" ] || [ -f "$B/lib/$n" ] || [ -f "$REL/RaCACiT Launcher.app/Contents/Frameworks/$n" ] || { echo "$f needs $n, not bundled" >&2; bad=1; }
    done
done
codesign --verify --strict "$REL/RaCACiT Launcher.app" || bad=1
codesign --verify --strict "$B/RaCACiT" || bad=1
[ $bad = 0 ] || exit 1
echo "lowest macOS the bundled libraries need:"
for f in "$B/RaCACiT" "$B/lib/"*.dylib "$REL/RaCACiT Launcher.app/Contents/Frameworks/"*.dylib; do
    printf '  %-28s %s\n' "$(basename "$f")" "$(vtool -show-build "$f" 2>/dev/null | awk '/minos/{print $2; exit}')"
done

if [ $ZIP = 1 ]; then
    # "launcher" and "macos" in the name: the Mac launcher's updater takes this asset.
    # No AppleDouble (._) files or quarantine flags inside.
    (cd "$OUT" && rm -f "RACiT-$LVER-macos.zip" && ditto -c -k --norsrc --noextattr --noqtn --keepParent "$NAME" "RACiT-$LVER-macos.zip")
    echo "$OUT/RACiT-$LVER-macos.zip"
fi
echo "$REL"
