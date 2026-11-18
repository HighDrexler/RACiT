# Building Ratchet & Clank: Into the Nexus (RACiT)

For forks and mods. Players only need the RACiT launcher from the Releases page: it carries
Into the Nexus beside A Crack in Time.

This branch holds source only. No game code or assets are included, and the build generates
its C code from your own legally dumped copy of the game.

## You need

- Windows, Git Bash, git, and Python 3.12
- About 32 GB of RAM for the default build (`-j6` below; lower it on less)
- From your own console and disc (`BCUS99245`, version 1.00), in `input/`:
  - `input/EBOOT.ELF`: the decrypted game executable (`PS3_GAME/USRDIR/EBOOT.BIN`, decrypted
    for example with RPCS3's Utilities > Decrypt PS3 Binaries)
  - `input/libsre.prx`: decrypted from `dev_flash/sys/external/libsre.sprx`, for example with RPCS3

## Build

Run these from the repository root in Git Bash.

```bash
# 1. The toolchain and libraries, at the versions releases use, into tools/
#    (also applies both runtime patches to tools/ps3recomp)
bash scripts/setup_windows.sh

# 2. Generate the game's C code into game/src/recomp and game/spu
PY="$PWD/tools/venv/Scripts/python -I" bash scripts/pipeline.sh

# 3. Compile (JOBS: parallel jobs, default 6)
JOBS=6 bash scripts/build.sh
```

This gives you `game/build-win/game/RaCNexus.exe`.

## Run your build

Put `RaCNexus.exe` (with `SDL2.dll` from a release) in the launcher's `nexus\builds\v<number>\`
folder, next to a `version.txt` holding that number. Play starts the newest build. Settings,
saves and `log.txt` live in the launcher's `nexus\data\` folder.

## Layout

| Path | What it is |
| --- | --- |
| `patches/` | The runtime on top of upstream ps3recomp: `0001` = RACiT's fixes, `0002` = Into the Nexus |
| `scripts/pipeline.sh` | Loader, PPU lift, the game's lift patches (`patch_lift_nexus.py`), SPU lifts |
| `scripts/spu_*.txt` | The SPU job modules lifted ahead of time and their entry points |
| `game/` | The game layer: hooks, DualSense Plus, button prompts, Discord status, code mods |
| `windows/` | The CMake project and the zig toolchain files |
| `examples/mods/` | An example code mod; see `docs/MODDING.md` |
