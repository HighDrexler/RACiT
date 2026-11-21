# RACiT for Android

Ratchet & Clank Future: A Crack in Time, recompiled for 64-bit ARM Android (arm64-v8a, Vulkan).
This branch is source only. No game data is included, and none ever will be: you need your own
legally dumped copy of the USA disc (`BCUS98124`).

## Install

1. Download `RACiT-<version>-arm64.apk` from an Android release on the Releases page (tagged
   `android-v<version>`), and
   install it. Android asks you to allow installs from the app you opened it with.
2. Put your own game folder (the one holding `PS3_GAME`) or its `.iso` on the device's storage
   (about 25 GB).
3. Open RACiT, then **Import game**, and **Add EBOOT.elf**: the game's `EBOOT.BIN` decrypted with
   RPCS3 (Utilities > Decrypt PS3 Binaries).
4. Pick a graphics preset, run **Shaders > Prepare shaders** once, and play.

Newer releases install over older ones, and the app checks GitHub for them when it opens.

## Graphics startup

RACiT requires a working Vulkan driver and the Vulkan features reported in the game log. If graphics fail to start, open **Diagnostics > Last game log** and share the first `[RSX vulkan]` failure with your device model and Android version. If the game reports an allocation failure, try the **Low** graphics preset once and compare the log. A device with no usable Vulkan driver needs a compatible system driver or another device to run this build.

## Build

On Windows, with Git Bash, Python 3.12, and one folder holding `android-ndk-r29`,
`android-sdk`, `gradle-8.13` and `jdk-17.0.20.1+1`. Put your decrypted `input/EBOOT.ELF` and
`input/libsre.prx` in place first (see `BUILDING.md`).

```bash
bash scripts/setup_android.sh
PY="$PWD/tools/venv/Scripts/python -I" SKIP_DESKTOP=1 bash scripts/pipeline.sh
tools/venv/Scripts/python -I scripts/android_spu_fast.py
tools/venv/Scripts/python -I scripts/upgrade-vmx-permute.py
powershell -File scripts/build-android.ps1 -ToolchainsRoot <toolchains folder> -PythonExecutable "$PWD/tools/venv/Scripts/python.exe"
```

The APK lands in `artifacts/`. The first build makes its own signing key
(`android/racit-independent.keystore`, never committed); an APK installs over another only when
both carry the same key.
