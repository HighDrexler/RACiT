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
