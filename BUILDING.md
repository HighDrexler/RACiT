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

# 3b. Optional: the older-CPU build (no AVX2: Sandy/Ivy Bridge, AMD FX), RaCACiT_compat.exe
cmake -S windows -B game/build-win-compat -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/windows/toolchain/zig-windows.cmake" \
  -DSDL2_DIR="$PWD/tools/sdl2-win/lib/cmake/SDL2" \
  -DPython3_EXECUTABLE="$PWD/tools/venv/Scripts/python.exe" -DRACACIT_CPU_COMPAT=ON
cmake --build game/build-win-compat -j6

# 4. The launcher
bash launcher/build.sh
```

This gives you `game/build-win/game/RaCACiT.exe` and `launcher/build/RaCACiT Launcher.exe`.

## The European disc (`BCES00511`)

The sources stay written for `BCUS98124`. For the European disc the build makes a converted
copy of them in `build/BCES00511/`, using the address map in `versions/BCES00511/addr_map.tsv`.
Steps 1 to 4 above are not affected, and the two builds do not share generated code.

You need, from your own console and disc, in `input/BCES00511/`:

- `EBOOT.ELF`: the decrypted game executable of `BCES00511`, version 1.00
- `libsre.prx`: the same file as for the USA build

```bash
bash scripts/setup_windows.sh                 # if you have not built yet
bash scripts/build_version.sh BCES00511
```

This gives you `build/BCES00511/RaCACiT_BCES00511.exe` and the older-processor
`RaCACiT_BCES00511_compat.exe` (`NO_COMPAT=1` skips it). Put them in the same `builds\v<number>\`
folder as `RaCACiT.exe`; the launcher starts the program of the disc it finds.

**Changing the game code:** keep writing `BCUS98124` addresses. `scripts/retarget.py` converts
them when it copies the sources. With the USA `input/EBOOT.ELF` present it also checks every
converted code address against both executables and stops on a mismatch; without it, it warns
about any address the map does not have. In the copy, `patch_lift.py` stops the build if a hook
does not land: add a new hook's name to `REQUIRED_HOOKS` in `retarget.py`. Code mods hook US
addresses, so the European build does not load them.

## Run your build

Put the exe in a launcher's `builds\v<number>\` folder, next to a `version.txt` holding that
number (and `RaCACiT_compat.exe` beside it, if you built one: the launcher starts it on
processors without AVX2, or when Settings > Performance > Game build says so). Play starts the newest build. Settings, saves and `log.txt` live in the launcher's
`data\` folder.

## macOS Apple Silicon build

Use Xcode Command Line Tools, Python 3.12 or later, CMake with Make or Ninja, SDL2, zlib,
Vulkan headers, the Vulkan loader, MoltenVK, glslang and SPIRV-Cross. The Vulkan
SDK and package-manager installations can supply these libraries. Pass their
installation prefixes through `CMAKE_PREFIX_PATH` when CMake needs help finding
them. CMake discovers the loader library and SDL uses that same library.

The ordinary pipeline lifts the NA disc executable and builds the game for the
host architecture. Supply the same private `input/EBOOT.ELF` and
`input/libsre.prx` described above, plus the complete decrypted disc directory
containing `PS3_GAME` for play. The launcher builds for macOS too (below).

```bash
# Prepare the pinned ps3recomp runtime with this repository's runtime patch.
bash mac/setup_macos.sh

# Build through the normal pipeline. Two workers limit peak memory use.
PY="python3 -I" CMAKE_BUILD_PARALLEL_LEVEL=2 CMAKE_GENERATOR="Unix Makefiles" \
  bash scripts/pipeline.sh
```

`BUILD_DIR` selects an external build directory. `CMAKE_GENERATOR` selects Make
or Ninja, whose default is retained when the variable is unset.
`PS3RECOMP_VULKAN_INCLUDE_DIR`
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

### macOS launcher and release

The launcher is the same program as on Windows (`launcher/src`; the Mac parts are in
`mac/launcher` and behind `#ifdef __APPLE__`): an SDL window drawn with CoreGraphics
and CoreText. It needs the Xcode command line tools and Homebrew's `sdl2` (sdl2-compat
with SDL3), `vulkan-loader` and `molten-vk`.

```bash
# RaCACiT Launcher.app -> launcher/build/ (SDL inside it, ad-hoc signed)
bash mac/build_launcher.sh
# A release folder and zip -> out/macos/: the .app, builds/v<N>/RaCACiT (the game build
# above) with its libraries in builds/v<N>/lib (Vulkan loader, MoltenVK and its ICD
# file, SDL2/SDL3, glslang, SPIRV-Tools), README.txt and mods/. Nothing loads from Homebrew.
bash mac/package_macos.sh --version <N>
```

Players open the `.app` once with System Settings > Privacy & Security > Open Anyway
(the Mac counterpart of SmartScreen's "Run anyway"); the launcher then sets up the game
exactly as on Windows. Play passes the build the same environment the Windows launcher
does, plus `PS3_VFS_ROOT`, `PS3RECOMP_VULKAN_LIBRARY` and `VK_DRIVER_FILES` for the
bundled loader and MoltenVK, and writes its output to `data/log.txt`. Homebrew's
libraries carry the macOS version they were built on as their minimum (the script lists
them); build SDL, MoltenVK and the loader with `MACOSX_DEPLOYMENT_TARGET` set for a
release that runs on older macOS. The launcher's binary
(`RaCACiT Launcher.app/Contents/MacOS/RaCACiT Launcher`) with `--snapshot <dir>`, and
optionally `--root <folder>`, draws every screen to PNGs with no window.

## Where things are

| Path | What it is |
| --- | --- |
| `game/` | The port: startup, hooks into the game's code, Discord, button prompts |
| `patches/ps3recomp-0001-racacit-runtime-fixes.patch` | All changes to the [ps3recomp](https://github.com/sp00nznet/ps3recomp) runtime: renderer, audio, SPU, input |
| `launcher/` | The launcher, with built-in mods in `launcher/mods/` |
| `scripts/` | The lifting pipeline and its fixes to the generated code |
| `windows/` | The Windows build and the zig toolchain files |
| `mac/` | Everything macOS-only: setup, the launcher's Mac parts and build, the release packaging, the link order file |
| `versions/` | Per-version data: profiles, and update 1.20's hooks, addresses and SPU payloads |

**Changing the runtime:** edit `tools/ps3recomp`, then save your work back into the patch:

```bash
git -C tools/ps3recomp add -N .
(cd tools/ps3recomp && git diff --ignore-cr-at-eol HEAD | tr -d '\r') > patches/ps3recomp-0001-racacit-runtime-fixes.patch
```

**Hot code lists:** `game/hot_*` only group frequently used code together in the exe for speed.
They can go stale after big changes without breaking anything. `scripts/dev/hot_funcs.py` regenerates them.

**Mods:** see `launcher/mods/dualsense_plus/mod.json` for the format.

## Game versions and updates

Generation fingerprints the decrypted `EBOOT.ELF` before creating output or applying address-specific hooks. `versions/profiles.json` identifies a region and executable version separately. A title ID alone does not identify the update's address layout.

```bash
# Identify and validate the input without generating code.
python3 -I scripts/patch_lift.py --check-profile input/EBOOT.ELF
# Optionally require an exact version during the normal pipeline.
RACACIT_GAME_PROFILE=BCUS98124-1.00 PY="$PWD/tools/venv/Scripts/python -I" SKIP_DESKTOP=1 bash scripts/pipeline.sh
```

The repository root builds the **BCUS98124 disc 1.00** layout. **BCES00511 disc 1.00** is built from a converted copy (see "The European disc" above): given to the root pipeline, it stops and says so. **BCUS98124 update 1.20** builds into its own tree (below). Unknown executables and explicit profile mismatches stop before generation. There is no force option that applies another version's hooks.

The European disc fingerprint was checked against a decrypted original executable, as were both NA ones. Enabling another version needs its own complete hook/data/SPU mappings, separate generated/build outputs, launcher selection, and runtime/save checks. Do not infer an address offset from a neighbouring function: updates can change function bodies and data layouts.

### Update 1.20 (`BCUS98124` 1.20, public PRs #37, #59, #67)

Give the pipeline the decrypted update executable (`USRDIR/EBOOT.BIN` of the installed update):

```bash
LIBSRE="$PWD/input/libsre.prx" PY="$PWD/tools/venv/Scripts/python -I" bash scripts/pipeline.sh /path/to/update120/EBOOT.ELF
```

The update gets a tree of its own, `out/BCUS98124-1.20/` (`recomp/`, `spu/`, `prx/`, `host/`,
`build/`), so it never overwrites the disc's `game/src/recomp` and `game/spu`. Its hooks,
function and data addresses and SPU payloads are in `versions/` (see `versions/README.md`);
each hook checks the function's fingerprint before it edits it. The release launcher builds
only the disc; the update build has not been run on Windows yet.

## Keyboard menu artwork

Prepare a separate archive from your own game dump with `python scripts/prepare_menu_keycaps.py ORIGINAL_GLOBAL_CACHED.PSARC PREPARED_GLOBAL_CACHED.PSARC`. The output directory must exist. The tool checks the bundled menu source and compiled actions, preserves the original archive, and records a receipt beside the prepared file. Repeating the command verifies and reuses the matching output. A changed input or output requires a fresh output path.

Or let the game do it, which it does by default on Windows, macOS and Linux (`RACACIT_MENU_KEYCAPS=0` in the environment or the settings file turns it off; Android needs `RACACIT_MENU_KEYCAPS=1`): with no `RACACIT_MENU_ARCHIVE`, the game prepares the same archive the first time it opens `global_cached.psarc` and keeps it in a `menu-keycaps` folder next to the settings file, reusing it while the original's size and time are unchanged. It takes about half a second and needs as much free space as the original (about 220 MB).

Set `RACACIT_MENU_ARCHIVE` to the prepared file when running the player through its usual entry point. The player validates the archive header before booting the guest and redirects read-only opens of `packed/game/global_cached.psarc`. Original stat, installation and write paths retain the owner's game files. Leave the variable unset to use the original archive.

The menu hooks support BCUS98124 NA disc 1.00 and update 1.20. The bundled action payload contains only the adjacent owned AS2 source compiled into AVM1 actions. Game artwork and movies are supplied by the owner's archive.
