# Agent instructions for RACiT

Read `BUILDING.md` first. It has the build steps, the layout and how to change the runtime.

## Scripts are Rust

- Write every new script, build step, check and tool in Rust, in a crate under `rust/` (today `rust/racit-lift`, one binary per tool in `src/bin/`). Don't add Python, shell, JS or other languages for this.
- When you change an existing Python or shell script, port it to Rust in the same change, switch its callers and delete the old script. Keep its behavior, its error messages' meaning and its exit codes.
- Check a port against the script it replaces: same generated files byte for byte on NA 1.00 and update 1.20 (and the EU disc when you have it) before the old one goes.
- Exceptions: ps3recomp's own Python tools (`tools/ps3recomp/tools/*.py`, upstream's), CMake files, and short shell entry points such as `scripts/pipeline.sh` that only call tools in order. New logic in those goes into a Rust tool they call.
- Use the latest stable Rust and edition. Run `cargo fmt`, `cargo clippy` and `cargo test` in the crate you touch.

## Game code hooks

- A hook in the generated code goes into both lifts: the disc's (patch-lift) and update 1.20's recipe (`versions/BCUS98124-1.20-hooks.json`, fingerprinted edits per function). Find the 1.20 function by its normalized code, not by its address.
- The build stops when a required hook is missing or applied twice. Keep it that way.

## Runtime changes

- Every change to ps3recomp lives in `patches/ps3recomp-0001-racacit-runtime-fixes.patch`. Regenerate it as `BUILDING.md` says, with `core.autocrlf=false`, so line endings don't churn.

## Pull requests

- One concern per PR, on current `main`.
- The description says what was wrong, who it hits and how it showed, what the change does, and only the checks that actually ran, with their scope (which platform, which game version).
- A PR that changes the build reaches gameplay on 1.00 and 1.20 before it's ready. Keep owner saves untouched: test on copies.
