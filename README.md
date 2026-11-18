# RACiT: Into the Nexus

Ratchet & Clank: Into the Nexus, recompiled for Windows. It is played from the RACiT launcher,
next to A Crack in Time. This branch is source only. No game data is included, and none ever
will be: you need your own legally dumped copy of the USA disc (`BCUS99245`).

## Install

1. Download a RACiT launcher release that includes Into the Nexus (1.0.0 or newer) from the
   Releases page, and unzip it anywhere.
2. Open `RaCACiT Launcher.exe` and pick **Into the Nexus** in the game bar on the left.
3. **Set up the game**: import your disc image (`.iso`) or game folder, then add `EBOOT.elf`,
   the game's `EBOOT.BIN` decrypted with RPCS3 (Utilities > Decrypt PS3 Binaries).
4. Press Play.

## Build

Windows, Git Bash, git and Python 3.12. Put your decrypted `input/EBOOT.ELF` and
`input/libsre.prx` in place first. The details are in `BUILDING.md`.

```bash
bash scripts/setup_windows.sh
PY="$PWD/tools/venv/Scripts/python -I" bash scripts/pipeline.sh
JOBS=6 bash scripts/build.sh
```

This gives you `game/build-win/game/RaCNexus.exe`. To play it, put it (with `SDL2.dll` from a
release) in the launcher's
`nexus\builds\v<number>\` folder, next to a `version.txt` holding that number.
