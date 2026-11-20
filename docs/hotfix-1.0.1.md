# 1.0.1 hotfix PRs

These are the PR descriptions for the 1.0.1 hotfix. Each one is its own branch on main, and `hotfix/1.0.1` just merges them all so they can be tested together. Windows isn't built here (I only have a Mac), so everything below was built and run on macOS, and the Windows side is only checked by the data-free tests and by reading the code.

The data-free tests run anywhere with Python 3, no game files needed:

    python -m unittest discover -s tests

## Menus still show controller buttons with a keyboard (#22)

Branches: `fix/menu-keycaps-actions` (#70) and `feat/menu-keycaps-auto` (#76), with `fix/keycaps-everywhere` on top.

In 1.0.0 the menus kept the PS3 buttons with a keyboard for a few reasons. The compiled menu actions file got dropped by the `*.BIN` rule in .gitignore (#70), and even with it the keyboard menus needed a Python step players never run, so the game now makes the menu archive itself (#76). This branch turns that on by default on Windows, macOS and Linux (`RACACIT_MENU_KEYCAPS=0` turns it off, Android stays opt-in) and adds the rest of the keycaps: the HUD's face button pictures, the Kinetic Tether picture, the quick select L1/R1 badges, the d-pad glyphs, and the font's smaller mip levels, which is what the small hint text in vendors and quick select uses.

The quick select badges show the keys that actually page rings while the ring is open, R and T by default (QS_PREV_RING / QS_NEXT_RING, or whatever settings.txt binds them to). Before they showed the gameplay L1/R1 keys, and with the ring open left click is the previous ring and the R1 key does nothing.

#76 also got two fixes from review. Two games starting at once used to write the same .tmp file, and a later start trusted any file at the cache path, so now the temporary is per process and a cached archive is only reused when its PSARC header and size check out (otherwise it's made again). And a failed fclose no longer closes the stream a second time.

Review round 2 added a few more. Each HUD picture write now checks again, right before copying, that the table entry still points at the same address and the bytes are still what the scan saw (the only window left is between that compare and the copy, since the guest runs on its own threads). The Kinetic Tether picture gets resolved on every scan, so it follows the texture when it moves. Before, it was found during the level load before its pixels were in, got refused, and never looked at again, so it kept showing R2 + square. `RACACIT_PROMPT_CHECK` now counts a tether still showing the controller's art as wrong. Keys bound to punctuation (`[ ] ; ' , . / \ - = \``) get their own letters, and anything else shows its SDL name shortened (KP5, RALT, PGDN) instead of a "?". #76 also only reuses a cached archive with a `.ok` marker it wrote itself after checking it, and makes a new one by renaming over the old one in one step.

It also fixes a crash in those HUD keycaps. A level's HUD textures sit in main memory for a bit and then move, and the game reuses the old memory. The keycap code kept drawing into it, and on the Mac the game crashed after saving or loading from the pause menu. Now it checks the picture is still where the table says and still looks like itself before writing anything.

For tests there's `RACACIT_PROMPT_CHECK=1`, which logs every 5 s whether any button picture still shows the other input's art, and `tests/test_menu_keycaps.py`, which builds a fake global_cached.psarc and checks the script and the game's own C++ make the same archive byte for byte, that it's on by default and off with 0, and that the actions file is actually in the repo.

Tested: RESULTS_KEYCAPS

Not tested: Windows, Android, a real controller plugged in (the headless runs force the switch back to controller glyphs instead), the EU disc (no EU EBOOT here).

Still showing controller buttons: the L1/R1 tab badges at the top of the inventory (and probably other tabbed menus). Those are `btn_L1`/`btn_R1` clips in pausemenu.swf with "L1" text, not ButtonPrompts or font glyphs, so they need a change to menu_keycaps.as and recompiled actions. That's a follow-up.

## Update 1.20 doesn't build (`fix/u120-build`)

The 1.0.0 notes say 1.20 support is untested, and it turns out it can't build at all. scripts/prepare_game_host.py stops with "Missing host function mapping 00467A58" because the cheat menu code calls disc functions that versions/BCUS98124-1.20-host-functions.tsv doesn't map. This maps the six functions (byte-identical in 1.20 apart from branches and TOC offsets) and leaves the disc-only ports of 1.20's own fixes out of the 1.20 build, since 1.20 has them already.

`tests/test_update120_host.py` runs the host source step for 1.20 without an executable and fails on 1.0.0 with that same missing mapping.

Tested: RESULTS_U120

Not tested: Windows, 1.20 on anything but the Zolar Forest route.

## Builds that silently miss hooks (`fix/hook-check`)

The lift is generated once and patch_lift.py patches it, so a lift patched before a hook was added builds fine without it. That already shipped a Mac build with no mouse camera and no menu keycaps once. Now CMake re-runs patch_lift.py at configure time when it, its inputs, a hook list or the lift changed (a change to the patcher or a list configures again), and the build step only checks, so it never rewrites a source Ninja already decided was up to date. If a hook is still missing the build stops with the list.

versions/*-hook-symbols.txt list every hook for 1.00, 1.20 and the EU disc. Only real calls count, so comments, strings, declarations and the helper each chunk defines don't, and a commented-out hook shows up as missing. The counts are the real call counts of freshly patched 1.00 and 1.20 lifts, and the 1.00 list now has the cheat menu's three hooks too. The EU list is the USA one with its @function lines converted through versions/BCES00511/addr_map.tsv and without the hooks patch_lift.py only puts into the USA disc, and scripts/build_version.sh now passes the EU profile and EBOOT so the EU build checks against it instead of the USA list. I don't have an EU EBOOT, so the EU build and its counts aren't checked here. Also fixes 1.20 chunks patched by an older recipe getting two preambles. `RACACIT_CHECK_HOOKS=OFF` skips it.

Round 2 made the check run on every build (not only when re-patching), so a USA lift doesn't pass as the EU one, and calls only count inside lifted function bodies and outside `#if 0`.

`tests/test_lift_hooks.py` checks the lists cover everything patch_lift.py and the 1.20 recipe insert, that the EU list matches the USA one converted, and runs the checker on small fake lifts: complete ones pass, one missing or commented-out hook fails, the count doesn't change with 1, 3 or 8 chunk files, `--patch` with no stamp puts a missing hook into a stale lift, and the check alone never touches a file.

Tested: RESULTS_HOOKS

Not tested: Windows (it runs Python from CMake there too, with the venv's interpreter).

## Mac build needs macOS 27 (`fix/macos-14`)

The 1.0.0 Mac zip (RACiT-0.1.0-macos.zip) only starts on macOS 27.2 or newer: the game and the bundled SDL, Vulkan loader, glslang and SPIRV-Tools all have minos 27, because nothing set a deployment target and Homebrew's libraries are built for the build machine. The game now targets macOS 14 (with a build error for any unguarded newer API), mac/build_macos_libs.sh builds the bundled libraries for 14 from their hash-checked source tarballs, and packaging fails if anything in the zip needs a newer macOS.

The draft #74 has the same target change, but it sits on the ps3recomp v0.19 stack, so it can't go in a hotfix on main. This one is the same two changes on main.

Tested: RESULTS_MAC14

Not tested: actually running on a macOS 14 machine (only checked with vtool), Windows (no change there).

## macOS launcher doesn't compile (#77)

Branch: `fix/macos-launcher-build`. On 1.0.0 the Mac launcher didn't build, launcher/src/store.cpp used Windows APIs outside `_WIN32` and launch_mac() was missing its closing brace. From review it also got two more fixes. The updater cleared `<launcher>.old` with remove_all, so a folder with that name got deleted with everything in it, now it only removes a file (or on macOS the old .app it set aside itself). And the AVX2 check skipped MSVC x86 builds and always said yes, now it uses `__cpuid`/`_xgetbv` from intrin.h there.

Tested: RESULTS_LAUNCHER

Not tested: Windows or MSVC (no Windows machine here, the MSVC path is only read, not compiled), an actual launcher update.

## macOS freezes when Qwark kicks down a wall (`fix/rsx-retire-no-writewatch`)

On the Mac the picture froze in Zolar Forest when Qwark kicks down one of the walls (audio kept going). Without the write watch, which is always off outside Windows, eng_surface_retire wrote retired render targets back into main memory over whatever the game keeps there now, SPU job code included. This skips that write-back to main memory when the write watch is off. Local memory works like before, and Windows with the write watch doesn't change. It's the same hunk as #71, which is on the v0.19 runtime, ported to main's runtime patch.

Tested: RESULTS_RETIRE

Not tested: Windows and Linux.

## Mouse feels like a thumbstick outside the follow camera (#68)

Branch: `fix/mouse-direct-all-cameras`, see its own PR. Merged here so it gets tested with the rest. From review it got two fixes too. Re-patching a lift that 1.0.0 already patched put a second mouse_angles into the follow camera, so the lift didn't compile, now the old patch comes out first (`tests/test_mouse_repatch.py` re-patches a small lift with either 1.0.0 patch). And X and Y of a mouse move now change together under one lock, so a camera never gets half a diagonal move (`tests/desktop_mouse_test.cpp` has a two-thread check).

Round 2 also fixed two upgrade problems. The setups only patched a fresh tools/ps3recomp, so a 1.0.0 checkout kept the old cellPad.c, which calls racit_mouse_step and racit_camera_follow_active and doesn't link anymore. Now scripts/setup_windows.sh and mac/setup_macos.sh keep the hash of the applied patch in tools/ps3recomp/.racit-patch and reset and re-apply when it changes, and desktop_mouse.cpp keeps tiny versions of those two old calls anyway. And motion fed right before the cameras pause (a menu, a cutscene) is dropped after 250 ms instead of turning the camera when play resumes.

Whichever of this and `fix/hook-check` goes in second needs the small change the hotfix merge has: the hook lists expect racit_camera_turn (17), racit_camera_peek (1) and racit_mouse_now (18) instead of racit_camera_take, and the `--patch` test lift needs the ten camera functions.

## hotfix/1.0.1 all together

RESULTS_COMBINED
