#!/bin/bash
# mac/build_macos_libs.sh: the libraries a macOS release bundles, built from source for an older
# macOS than Homebrew's bottles (which target the build machine's macOS) -- for
#   MACOS_LIB_PREFIX=PREFIX mac/package_macos.sh ...
# Same versions as Homebrew's formulae; each tarball is checked against the formula's sha256.
# MoltenVK is copied from Homebrew: its dylib already targets macOS 12 and checks Metal features
# at run time. The Vulkan and SPIR-V headers come from Homebrew (vulkan-headers, spirv-headers).
#
# usage: mac/build_macos_libs.sh PREFIX     (MACOS_MIN, default 14.0)
set -e
P="${1:?usage: $0 PREFIX}"; mkdir -p "$P"; P="$(cd "$P" && pwd)"
BREW="${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}"
export MACOSX_DEPLOYMENT_TARGET="${MACOS_MIN:-14.0}"
W="$P/.work"; mkdir -p "$W/dl" "$W/src" "$W/build"
SRC=(
  "SPIRV-Tools-vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688"
  "glslang-16.6.0 https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz 9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2"
  "Vulkan-Loader-vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz 941eff558fc74f248745fb7df367640cf89a8457f216dae498a74ae14b6ba6f7"
  "SDL3-3.4.20 https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f"
  "sdl2-compat-2.32.74 https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz ec68abde77e2e459c8abc4f9587b8976b5d2a657b2bfda2fb3a079b0f8924588"
)
for s in "${SRC[@]}"; do
    set -- $s
    [ -f "$W/dl/$1.tar.gz" ] || curl -fsSL -o "$W/dl/$1.tar.gz" "$2"
    echo "$3  $W/dl/$1.tar.gz" | shasum -a 256 -c -
    rm -rf "$W/src/$1"; tar xzf "$W/dl/$1.tar.gz" -C "$W/src"
done
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
        -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_NAME_DIR="$P/lib" -DCMAKE_FIND_FRAMEWORK=LAST
        -DCMAKE_PREFIX_PATH="$P;$BREW/opt/vulkan-headers;$BREW/opt/spirv-headers")
b() { local s="$1"; shift; rm -rf "$W/build/$s"; cmake -S "$W/src/$s" -B "$W/build/$s" "${COMMON[@]}" "$@"; cmake --build "$W/build/$s"; cmake --install "$W/build/$s"; }
b SPIRV-Tools-vulkan-sdk-1.4.363.0 -DBUILD_SHARED_LIBS=ON -DSPIRV_TOOLS_BUILD_STATIC=OFF -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
    -DSPIRV_WERROR=OFF -DSPIRV-Headers_SOURCE_DIR="$BREW/opt/spirv-headers"
b glslang-16.6.0 -DBUILD_SHARED_LIBS=ON -DGLSLANG_TESTS=OFF -DENABLE_OPT=ON -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DBUILD_EXTERNAL=OFF -DENABLE_GLSLANG_BINARIES=OFF
# The loader's config search paths are the usual system ones, not this prefix.
b Vulkan-Loader-vulkan-sdk-1.4.363.0 -DVulkanHeaders_DIR="$BREW/opt/vulkan-headers/share/cmake/VulkanHeaders" -DBUILD_TESTS=OFF \
    -DSYSCONFDIR=/usr/local/etc -DFALLBACK_CONFIG_DIRS=/etc/xdg -DFALLBACK_DATA_DIRS=/usr/local/share:/usr/share
b SDL3-3.4.20 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_X11=OFF
b sdl2-compat-2.32.74 -DSDL2COMPAT_TESTS=OFF -DSDL2COMPAT_STATIC=OFF
cp -f "$BREW/opt/molten-vk/lib/libMoltenVK.dylib" "$P/lib/"
mkdir -p "$P/etc/vulkan/icd.d"; cp -f "$BREW/etc/vulkan/icd.d/MoltenVK_icd.json" "$P/etc/vulkan/icd.d/"
rm -rf "$W/build" "$W/src"
for f in "$P/lib/"*.dylib; do [ -L "$f" ] || printf '  %-48s minos %s\n' "$(basename "$f")" "$(vtool -show-build "$f" | awk '/minos/{print $2; exit}')"; done
