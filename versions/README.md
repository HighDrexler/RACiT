# Executable profiles

`profiles.json` identifies a decrypted executable by SHA-256. `RACACIT_GAME_PROFILE` optionally requires a particular profile. The pipeline checks this identity before creating generated output.

Run the pipeline with your own executable.

```sh
scripts/pipeline.sh /path/to/EBOOT.ELF
```

Loader data, PPU output, SPU output, firmware-derived code and the desktop build use separate directories under `out/<profile>/`. The default NA disc profile is `BCUS98124-1.00`.

The update 1.20 PPU consumer verifies the input and output hashes of its source edits. An existing update lift can use the ordinary patcher entry point.

```sh
python3 -I scripts/patch_lift.py /path/to/update/recomp \
  --elf /path/to/update/EBOOT.ELF --profile BCUS98124-1.20
```

Full update generation remains disabled while version-specific host and SPU integration is pending. PAL support is coordinated through PR #26.

The standalone game CMake project accepts `RECOMP_DIR`, `SPU_DIR` and `PRX_DIR`. The Windows CMake project also accepts these paths, allowing an isolated generated profile to be selected explicitly.

```sh
cmake -S windows -B out/BCUS98124-1.00/windows-build \
  -DRECOMP_DIR="$PWD/out/BCUS98124-1.00/recomp" \
  -DSPU_DIR="$PWD/out/BCUS98124-1.00/spu" \
  -DPRX_DIR="$PWD/out/BCUS98124-1.00/prx"
```
