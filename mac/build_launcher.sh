#!/bin/bash
# mac/build_launcher.sh: "RaCACiT Launcher.app" -> launcher/build/ (macOS $MACOS_MIN+, default 14.0, Apple Silicon).
# The launcher's shared source is launcher/src; its macOS parts are in mac/launcher.
# clang/clang++ from the Xcode command line tools, SDL2 from Homebrew (sdl2-compat and the
# SDL3 it loads go inside the .app), libcurl and the frameworks from macOS itself.
# MACOS_LIB_PREFIX: take SDL2/SDL3 from that prefix instead (libraries built for MACOS_MIN,
# mac/build_macos_libs.sh). Ad-hoc signed. mac/package_macos.sh puts it in a release folder
# beside the game build.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LAUNCHER="$ROOT/launcher"
BREW="${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}"
LIBS="${MACOS_LIB_PREFIX:-$BREW}"
MACOS_MIN="${MACOS_MIN:-14.0}"
OUT="$LAUNCHER/build"
APP="$OUT/RaCACiT Launcher.app"
VERSION="$(sed -n 's/^static const char\* kLauncherVersion = "\(.*\)";/\1/p' "$LAUNCHER/src/app.cpp")"
OBJ="$OUT/obj-macos"
mkdir -p "$OBJ"
. "$HERE/macos_dylibs.sh"

FLAGS=(-arch arm64 -mmacosx-version-min=$MACOS_MIN -O2 -Wall -I"$LIBS/include" -DNDEBUG -I"$LAUNCHER/src" -I"$HERE/launcher")
cd "$LAUNCHER/src"
for f in app.cpp iso.cpp store.cpp util.cpp; do
    clang++ -std=c++17 "${FLAGS[@]}" -c "$f" -o "$OBJ/${f%.*}.o" &
done
for f in gfx_mac.mm util_mac.mm; do
    clang++ -std=c++17 -fobjc-arc "${FLAGS[@]}" -c "$HERE/launcher/$f" -o "$OBJ/${f%.*}.o" &
done
wait_all() { local ok=0; for j in $(jobs -p); do wait "$j" || ok=1; done; return $ok; }
wait_all

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
BIN="$APP/Contents/MacOS/RaCACiT Launcher"
clang++ -arch arm64 -mmacosx-version-min=$MACOS_MIN "$OBJ"/*.o -o "$BIN" \
    -L"$LIBS/lib" -lSDL2 -lcurl \
    -framework AppKit -framework CoreText -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
    -Wl,-dead_strip
bundle_dylibs "$BIN" "$APP/Contents/Frameworks" "@executable_path/../Frameworks" "$LIBS/lib/libSDL3.0.dylib=libSDL3.dylib"

# The icon: the RACiT emblem (assets/emblem.png, 1024 px) at every size macOS asks for.
ICONSET="$OBJ/icon.iconset"
rm -rf "$ICONSET"; mkdir -p "$ICONSET"
for z in 16 32 128 256 512; do
    sips -z $z $z "$LAUNCHER/assets/emblem.png" --out "$ICONSET/icon_${z}x${z}.png" >/dev/null
    sips -z $((z * 2)) $((z * 2)) "$LAUNCHER/assets/emblem.png" --out "$ICONSET/icon_${z}x${z}@2x.png" >/dev/null
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
    <key>LSMinimumSystemVersion</key><string>$MACOS_MIN</string>
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
[ -z "$left" ] || { echo "build_launcher.sh: still outside the bundle: $left" >&2; exit 1; }
"$BIN" --self-test
echo "$APP (v$VERSION)"
