# Building RACiT

For forks and mods. Players only need the launcher from the Releases page.

This repository holds source only. No game code or assets are included, and the build
generates its C code from your own legally dumped copy of the game.

## You need

- Windows, Git Bash, git, and Python 3.12
- About 32 GB of RAM for the default build (`-j6` below; lower it on less)
- From your own console and disc (`BCUS98124`), in `input/`:
  - `input/EBOOT.ELF`: the decrypted game executable
  - `input/libsre.prx`: decrypted from `dev_flash/sys/external/libsre.sprx`, for example with RPCS3

## Build

Run these from the repository root in Git Bash.

```bash
# 1. The toolchain and libraries, at the versions releases use, into tools/
bash scripts/setup_windows.sh

# 2. Generate the game's C code into game/src/recomp and game/spu
PY="$PWD/tools/venv/Scripts/python -I" SKIP_DESKTOP=1 bash scripts/pipeline.sh

# 3. Compile (about 10 minutes from scratch)
export PATH="$PWD/tools/venv/Scripts:$PATH"
cmake -S windows -B game/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/windows/toolchain/zig-windows.cmake" \
  -DSDL2_DIR="$PWD/tools/sdl2-win/lib/cmake/SDL2" \
  -DPython3_EXECUTABLE="$PWD/tools/venv/Scripts/python.exe"
cmake --build game/build-win -j6

# 4. The launcher
bash launcher/build.sh
```

This gives you `game/build-win/game/RaCACiT.exe` and `launcher/build/RaCACiT Launcher.exe`.

## Run your build

Put the exe in a launcher's `builds\v<number>\` folder, next to a `version.txt` holding that
number. Play starts the newest build. Settings, saves and `log.txt` live in the launcher's
`data\` folder.

## macOS Apple Silicon build

Use Xcode Command Line Tools, Python 3.12 or later, CMake, Ninja, SDL2, zlib,
Vulkan headers, the Vulkan loader, MoltenVK, glslang and SPIRV-Cross. The Vulkan
SDK and package-manager installations can supply these libraries. Pass their
installation prefixes through `CMAKE_PREFIX_PATH` when CMake needs help finding
them. CMake discovers the loader library and SDL uses that same library.

The ordinary pipeline lifts the NA disc executable and builds the game for the
host architecture. Supply the same private `input/EBOOT.ELF` and
`input/libsre.prx` described above, plus the complete decrypted disc directory
containing `PS3_GAME` for play. The graphical launcher currently targets Windows.

```bash
# Create a local Python environment and install the generator dependencies.
python3 -m venv tools/venv-macos
tools/venv-macos/bin/python -m pip install \
  capstone==5.0.9 construct==2.10.70 pycryptodome==3.24.1 \
  tabulate==0.10.0 tqdm==4.70.1

# Prepare the pinned ps3recomp runtime with this repository's runtime patch.
bash scripts/setup_macos.sh

# Build through the normal pipeline. Two workers limit peak memory use.
PY="$PWD/tools/venv-macos/bin/python -I" CMAKE_BUILD_PARALLEL_LEVEL=2 \
  bash scripts/pipeline.sh
```

`BUILD_DIR` selects an external build directory. `PS3RECOMP_VULKAN_INCLUDE_DIR`
selects Vulkan headers explicitly. `PS3RECOMP_VULKAN_LIBRARY` overrides the
discovered loader at run time. The loader's normal `VK_DRIVER_FILES` setting can
select an installed MoltenVK ICD when needed.

Run from the repository root with the disc directory and a persistent data
directory selected explicitly.

```bash
mkdir -p user-data
export PS3_VFS_ROOT="/path/to/decrypted-disc-directory"
export PS3_HDD0_ROOT="$PWD/user-data/hdd0"
export RACACIT_SETTINGS_FILE="$PWD/user-data/settings.txt"
export RSX_SHADER_CACHE="$PWD/user-data/shaders.bin"
export RSX_PIPELINE_CACHE="$PWD/user-data/pipelines.bin"
export RSX_PIPELINE_LIST="$PWD/user-data/pipeline-list.bin"
game/build/RaCACiTRecomp "$PWD/input/EBOOT.ELF"
```

The Apple Vulkan entry point starts the window on the process main thread and
runs guest code on its own large-stack thread. It defaults to safe MoltenVK
shader math and synchronous pipelines to preserve complete rendering passes.
`PS3RECOMP_VK_WINDOW=0` selects the existing offscreen mode. Save progress through
the game's normal save/checkpoint system before closing the window.

## Where things are

| Path | What it is |
| --- | --- |
| `game/` | The port: startup, hooks into the game's code, Discord, button prompts |
| `patches/ps3recomp-0001-racacit-runtime-fixes.patch` | All changes to the [ps3recomp](https://github.com/sp00nznet/ps3recomp) runtime: renderer, audio, SPU, input |
| `launcher/` | The launcher, with built-in mods in `launcher/mods/` |
| `scripts/` | The lifting pipeline and its fixes to the generated code |
| `windows/` | The Windows build and the zig toolchain files |

**Changing the runtime:** edit `tools/ps3recomp`, then save your work back into the patch:

```bash
git -C tools/ps3recomp add -N .
(cd tools/ps3recomp && git diff --ignore-cr-at-eol HEAD | tr -d '\r') > patches/ps3recomp-0001-racacit-runtime-fixes.patch
```

**Hot code lists:** `game/hot_*` only group frequently used code together in the exe for speed.
They can go stale after big changes without breaking anything. `scripts/dev/hot_funcs.py` regenerates them.

**Mods:** see `launcher/mods/dualsense_plus/mod.json` for the format.
