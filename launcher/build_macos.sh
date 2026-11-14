#!/bin/bash
# build_macos.sh: "RaCACiT Launcher.app" -> launcher/build/ (macOS 11+, Apple Silicon).
# clang/clang++ from the Xcode command line tools, SDL2 from Homebrew (sdl2-compat and the
# SDL3 it loads go inside the .app), libcurl and the frameworks from macOS itself.
# Ad-hoc signed. scripts/package_macos.sh puts it in a release folder beside the game build.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BREW="${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}"
OUT="$HERE/build"
APP="$OUT/RaCACiT Launcher.app"
VERSION="$(sed -n 's/^static const char\* kLauncherVersion = "\(.*\)";/\1/p' "$HERE/src/app.cpp")"
OBJ="$OUT/obj-macos"
mkdir -p "$OBJ"
. "$ROOT/scripts/macos_dylibs.sh"

FLAGS=(-arch arm64 -mmacosx-version-min=11.0 -O2 -Wall -I"$BREW/include" -DNDEBUG)
cd "$HERE/src"
for f in app.cpp iso.cpp store.cpp util.cpp; do
    clang++ -std=c++17 "${FLAGS[@]}" -c "$f" -o "$OBJ/${f%.*}.o" &
done
for f in gfx_mac.mm util_mac.mm; do
    clang++ -std=c++17 -fobjc-arc "${FLAGS[@]}" -c "$f" -o "$OBJ/${f%.*}.o" &
done
wait_all() { local ok=0; for j in $(jobs -p); do wait "$j" || ok=1; done; return $ok; }
wait_all

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
BIN="$APP/Contents/MacOS/RaCACiT Launcher"
clang++ -arch arm64 -mmacosx-version-min=11.0 "$OBJ"/*.o -o "$BIN" \
    -L"$BREW/lib" -lSDL2 -lcurl \
    -framework AppKit -framework CoreText -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
    -Wl,-dead_strip
bundle_dylibs "$BIN" "$APP/Contents/Frameworks" "@executable_path/../Frameworks" "$BREW/lib/libSDL3.0.dylib=libSDL3.dylib"

# The icon: the RACiT emblem (assets/emblem.png, 1024 px) at every size macOS asks for.
ICONSET="$OBJ/icon.iconset"
rm -rf "$ICONSET"; mkdir -p "$ICONSET"
for z in 16 32 128 256 512; do
    sips -z $z $z "$HERE/assets/emblem.png" --out "$ICONSET/icon_${z}x${z}.png" >/dev/null
    sips -z $((z * 2)) $((z * 2)) "$HERE/assets/emblem.png" --out "$ICONSET/icon_${z}x${z}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/icon.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>RACiT Launcher</string>
    <key>CFBundleDisplayName</key><string>RACiT Launcher</string>
    <key>CFBundleExecutable</key><string>RaCACiT Launcher</string>
    <key>CFBundleIdentifier</key><string>io.github.highdrexler.racit.launcher</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>CFBundleIconFile</key><string>icon</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
    <key>NSHumanReadableCopyright</key><string>Ratchet &amp; Clank: A Crack in Time (PC recompilation) launcher</string>
</dict>
</plist>
PLIST
printf 'APPL????' > "$APP/Contents/PkgInfo"

codesign --force -s - "$APP/Contents/Frameworks/"*.dylib
codesign --force -s - "$APP"
left="$(foreign_deps "$BIN"; for l in "$APP/Contents/Frameworks/"*.dylib; do foreign_deps "$l"; done)"
[ -z "$left" ] || { echo "build_macos.sh: still outside the bundle: $left" >&2; exit 1; }
"$BIN" --self-test
echo "$APP (v$VERSION)"
