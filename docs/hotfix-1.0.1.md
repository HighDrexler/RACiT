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

Round 3 tightened that more. Each HUD picture is now copied once up front, and finding it, telling which button it is and making the key all work from that copy, so the compare right before the write is against what was actually identified. Before it read the live memory again in between, so a new occupant arriving in that window could still get a key. The tether check also compares against the font's R1/R2 art saved from before the keycaps went in. When you start on a keyboard the keycaps are already in the font page before the tether is first checked, so it compared the picture with keys and never matched. On rename failure #76 now only falls back to an archive it checked itself, and otherwise plays that session from its checked temporary and clears leftover temporaries on the next start.

Round 4 gave the controller side's R1 swap in the tether picture the same one-copy-then-check as the keys. The tether also keeps its keys when the game moves it (it stages HUD textures in main memory and copies them to video memory, keys and all), and a tether left alone gets looked at again every second instead of at the next scan, so after loading a save it gets its keys within a second. And `NOMINMAX` goes before anything that brings in windows.h, since its min/max macros break std::min/std::max with MSVC (checked with clang and a stand-in header defining those macros, not with MSVC itself). The test for the rename fallback now covers the actual case, a plausible archive with no marker and a failing rename (a locked file on macOS, so it's skipped elsewhere).

Round 7 made `RACACIT_PROMPT_CHECK` check the tether and quick select badges with the controller too. Before it only counted them there, so one still showing keys after switching back passed. Now each has to show the game's own picture or the controller art hud_sync draws for the other layout.

Round 8 made that tether check compare the whole prompt: with the shoulder button redrawn for the other layout, the "+" and the square still have to be the game's own blocks, so a key left on the square side counts as wrong.

It also fixes a crash in those HUD keycaps. A level's HUD textures sit in main memory for a bit and then move, and the game reuses the old memory. The keycap code kept drawing into it, and on the Mac the game crashed after saving or loading from the pause menu. Now it checks the picture is still where the table says and still looks like itself before writing anything.

For tests there's `RACACIT_PROMPT_CHECK=1`, which logs every 5 s whether any button picture still shows the other input's art, and `tests/test_menu_keycaps.py`, which builds a fake global_cached.psarc and checks the script and the game's own C++ make the same archive byte for byte, that it's on by default and off with 0, and that the actions file is actually in the repo.

Tested: RESULTS_KEYCAPS

Not tested: Windows, Android, a real controller plugged in (the headless runs force the switch back to controller glyphs instead), the EU disc (no EU EBOOT here).

Still showing controller buttons: the L1/R1 tab badges at the top of the inventory (and probably other tabbed menus). Those are `btn_L1`/`btn_R1` clips in pausemenu.swf with "L1" text, not ButtonPrompts or font glyphs, so they need a change to menu_keycaps.as and recompiled actions. That's a follow-up.

## The comma key can't be bound (`fix/comma-binding`)

A binding is a list of SDL key names split at commas, and SDL calls the comma key ",", so binding it saved something that read back as nothing. The launcher now saves it as "Comma", the game and the keycap prompts read that, and settings saved before (`,` or `A,,`) still read as the comma key. Round 4 added the menu labels (menu_keycaps.h) to that. `tests/test_comma_binding.py` builds the launcher's and the game's list readers and checks both read the same keys.

Tested: RESULTS_COMMA

## Version 1.0.1 (`release/1.0.1-version`)

Just the version bump, merged last so it's easy to drop if you number it differently: kLauncherVersion (which the Mac zip name and the release lookup use), the Windows resource, the game version the launcher shows, and the launcher README. mac/package_macos.sh's `file_format_version` is the manifest format, so it stays 1.0.0.

## Update 1.20 doesn't build (`fix/u120-build`)

The 1.0.0 notes say 1.20 support is untested, and it turns out it can't build at all. scripts/prepare_game_host.py stops with "Missing host function mapping 00467A58" because the cheat menu code calls disc functions that versions/BCUS98124-1.20-host-functions.tsv doesn't map. This maps the six functions (byte-identical in 1.20 apart from branches and TOC offsets) and leaves the disc-only ports of 1.20's own fixes out of the 1.20 build, since 1.20 has them already.

Round 5 fixed the same build on Windows. With Python's default cp1252 there, chunks got written in the locale's encoding and read back as UTF-8. The pipeline now runs Python in UTF-8 mode (`-X utf8`, which the lifter follows too), and every text open in the lift scripts names its encoding, with surrogateescape where lifted sources are read and written so bytes that aren't UTF-8 come back unchanged. `tests/test_lift_encoding.py` checks those opens and runs the 1.20 source hooks with cp1252 forced as the default. Round 6 added fix_vmx_alias.py, which patch_lift.py starts on its own, to that.

`tests/test_update120_host.py` runs the host source step for 1.20 without an executable and fails on 1.0.0 with that same missing mapping.

Tested: RESULTS_U120

Not tested: Windows, 1.20 on anything but the Zolar Forest route.

Round 9: on Windows the 1.20 compat build (RaCACiT_compat.exe) still asked for AVX2, since the compat define went on game/main.cpp and the 1.20 build compiles a generated copy. It goes on the copy actually compiled now (checked in the 1.20 compat build's compile commands).

## Builds that silently miss hooks (`fix/hook-check`)

The lift is generated once and patch_lift.py patches it, so a lift patched before a hook was added builds fine without it. That already shipped a Mac build with no mouse camera and no menu keycaps once. It also shipped on Windows: the v1.0.0 USA exes (RaCACiT.exe and RaCACiT_compat.exe) were built from a lift patched before #66 and #67 and never patched again. Their follow camera has the old #34 mouse hook, and the #67 skip-PRESS-START hook (racacit_front_end_menu, called from func_004C8898) has no callers, so under Wine the compat exe went title, attract movie, title 44 times in a row. The EU exe has both. That's likely #68 too (the mouse felt like a stick on the USA exe). Now CMake re-runs patch_lift.py at configure time when it, its inputs, a hook list or the lift changed (a change to the patcher or a list configures again), and the build step only checks, so it never rewrites a source Ninja already decided was up to date. If a hook is still missing the build stops with the list.

versions/*-hook-symbols.txt list every hook for 1.00, 1.20 and the EU disc. Only real calls count, so comments, strings, declarations and the helper each chunk defines don't, and a commented-out hook shows up as missing. The counts are the real call counts of freshly patched 1.00 and 1.20 lifts, and the 1.00 list now has the cheat menu's three hooks too. The EU list is the USA one with its @function lines converted through versions/BCES00511/addr_map.tsv and without the hooks patch_lift.py only puts into the USA disc, and scripts/build_version.sh now passes the EU profile and EBOOT so the EU build checks against it instead of the USA list. I don't have an EU EBOOT, so the EU build and its counts aren't checked here. Also fixes 1.20 chunks patched by an older recipe getting two preambles. `RACACIT_CHECK_HOOKS=OFF` skips it.

Round 3 made `#if`/`#elif`/`#else` work as a proper stack, so only the live branch of nested blocks counts.

Round 2 made the check run on every build (not only when re-patching), so a USA lift doesn't pass as the EU one, and calls only count inside lifted function bodies and outside `#if 0`.

`tests/test_lift_hooks.py` checks the lists cover everything patch_lift.py and the 1.20 recipe insert, that the EU list matches the USA one converted, and runs the checker on small fake lifts: complete ones pass, one missing or commented-out hook fails, the count doesn't change with 1, 3 or 8 chunk files, `--patch` with no stamp puts a missing hook into a stale lift, and the check alone never touches a file.

For whoever builds the release: the v1.0.1 Windows exes have to come from a lift that passes this check (scripts/check_lift_hooks.py, at configure time and again at build time), on every exe including compat, so a stale lift can't ship again.

Tested: RESULTS_HOOKS

Not tested: Windows (it runs Python from CMake there too, with the venv's interpreter).

Round 8: on a cp1252 Windows console the check stopped a valid build with UnicodeEncodeError when the checkout path had other characters (C:\Users\李). It writes UTF-8 now and CMake runs it with `-X utf8` (`tests/test_lift_hooks.py` runs it on such a path with a cp1252 console).

## Mac build needs macOS 27 (`fix/macos-14`)

The 1.0.0 Mac zip (RACiT-0.1.0-macos.zip) only starts on macOS 27.2 or newer: the game and the bundled SDL, Vulkan loader, glslang and SPIRV-Tools all have minos 27, because nothing set a deployment target and Homebrew's libraries are built for the build machine. The game now targets macOS 14 (with a build error for any unguarded newer API), mac/build_macos_libs.sh builds the bundled libraries for 14 from their hash-checked source tarballs, and packaging fails if anything in the zip needs a newer macOS.

The draft #74 has the same target change, but it sits on the ps3recomp v0.19 stack, so it can't go in a hotfix on main. This one is the same two changes on main.

Round 5: a MACOS_LIB_PREFIX with spaces in its path broke bundling, since otool's output got split on spaces. It's read one path per line now (`tests/test_macos_dylibs.py` bundles from such a prefix and runs the result).

Tested: RESULTS_MAC14

Not tested: actually running on a macOS 14 machine (only checked with vtool), Windows (no change there).

## macOS launcher doesn't compile (#77)

Branch: `fix/macos-launcher-build`. On 1.0.0 the Mac launcher didn't build, launcher/src/store.cpp used Windows APIs outside `_WIN32` and launch_mac() was missing its closing brace. From review it also got two more fixes. The updater cleared `<launcher>.old` with remove_all, so a folder with that name got deleted with everything in it, now it only removes a file (or on macOS the old .app it set aside itself). And the AVX2 check skipped MSVC x86 builds and always said yes, now it uses `__cpuid`/`_xgetbv` from intrin.h there.

Round 7 fixed updates on macOS. The update checked a release's game build and launcher for Windows' "MZ", so on a Mac every update got turned down as having no game program. Now it checks for a Mach-O header there (thin or universal, launcher/src/program_magic.h, with `tests/test_program_magic.py`). The .app also never got unpacked from a release, since only a top-level launcher .exe counted, so now everything under `RaCACiT Launcher.app/` does.

Round 8 fixed which release file a Mac updates from. It only took files with "launcher" in the name, so it never found `RACiT-<version>-macos.zip` (what mac/package_macos.sh makes) and took the Windows zip from a release that has both. Now a Mac takes only the -macos.zip and Windows only the launcher zip, and a release with only the other platform's counts as none (`tests/test_release_asset.py`).

It also builds with MSVC again. windows.h's min/max macros broke std::min/std::max in the launcher, so `NOMINMAX` now goes before every windows.h include in launcher/src (checked with clang-cl against the MSVC headers, the zig/MinGW build wasn't affected).

Tested: RESULTS_LAUNCHER

Not tested: Windows or MSVC (no Windows machine here, the MSVC path is only read, not compiled), an actual launcher update.

## macOS freezes when Qwark kicks down a wall (`fix/rsx-retire-no-writewatch`)

On the Mac the picture froze in Zolar Forest when Qwark kicks down one of the walls (audio kept going). Without the write watch, which is always off outside Windows, eng_surface_retire wrote retired render targets back into main memory over whatever the game keeps there now, SPU job code included. This skips that write-back to main memory when the write watch is off, and from round 3 such a target isn't evicted at all then (the GPU has the only copy of its pixels), so eviction picks another one, and from round 4 the search covers the whole list, not just the 16 oldest. If every target is like that, the list grows into a spare quarter, and past that a new target isn't made (its draws get skipped) rather than dropping another target's only copy. Both get logged. Round 6 made "drawn over" stricter: it only counts the rows a copy target actually holds, and without the write watch a target that can't be written back only counts as drawn over when newer targets cover all of it. Round 5 also counts a target whose format can't be written back (GPU blit targets have no RSX surface format) as unsaved, so neither the victim search nor the budget pass drops it. Local memory works like before, and Windows with the write watch doesn't change. It's the same hunk as #71, which is on the v0.19 runtime, ported to main's runtime patch.

Tested: RESULTS_RETIRE

Not tested: Windows and Linux.

## Mouse feels like a thumbstick outside the follow camera (#68)

Branch: `fix/mouse-direct-all-cameras`, see its own PR. Merged here so it gets tested with the rest. From review it got two fixes too. Re-patching a lift that 1.0.0 already patched put a second mouse_angles into the follow camera, so the lift didn't compile, now the old patch comes out first (`tests/test_mouse_repatch.py` re-patches a small lift with either 1.0.0 patch). And X and Y of a mouse move now change together under one lock, so a camera never gets half a diagonal move (`tests/desktop_mouse_test.cpp` has a two-thread check).

Part of #68 on Windows was likely a stale lift rather than this: the v1.0.0 USA exes still had the old #34 follow-camera hook (see the hook check above).

Round 2 also fixed two upgrade problems. The setups only patched a fresh tools/ps3recomp, so a 1.0.0 checkout kept the old cellPad.c, which calls racit_mouse_step and racit_camera_follow_active and doesn't link anymore. Now scripts/setup_windows.sh and mac/setup_macos.sh keep the hash of the applied patch in tools/ps3recomp/.racit-patch and reset and re-apply when it changes, and desktop_mouse.cpp keeps tiny versions of those two old calls anyway. Round 3 made the setups stop wiping local changes, and round 4 tries the swap on a copy of the touched files first, so a conflict leaves the old patch and stamp in place and running setup again after fixing it works. A runtime with no patch of ours yet gets it over unrelated local edits. `tests/test_update_runtime_patch.py` covers those cases on a small fake runtime. That swap needed fixes three rounds in a row, so round 6 replaced it with a simple rule. A runtime that's exactly the pin plus this patch is up to date, one with no changes gets the patch, and one that's exactly the pin plus an earlier patch of ours gets reset to the pin, has that patch's own files removed and gets the new one. Anything else stops and asks you to commit or move your changes and run setup with `--reset` (reset to the pin, `git clean -fdx`, patch). The test runs all of that on a small fake runtime, and with `RACIT_RUNTIME_CLONE` also on the real ps3recomp and patch, including a patch half there after `git restore .` and staged edits. scripts/update_runtime_patch.sh reverse-applies the patch that was applied before and then applies the new one, and if that doesn't work it stops and says to commit or stash, or to pass `--reset`. The stale-motion check and its timestamp now both happen under the lock, feeding new motion resets it, and from round 4 the poll also decides whether a camera is active under that lock.

And motion fed right before the cameras pause (a menu, a cutscene) is dropped after 250 ms instead of turning the camera when play resumes.

Whichever of this and `fix/hook-check` goes in second needs the small change the hotfix merge has: the hook lists expect racit_camera_turn (17), racit_camera_peek (1) and racit_mouse_now (18) instead of racit_camera_take, and the `--patch` test lift needs the ten camera functions.

Round 8 tightened the mouse regression (tests/mac_mouse_cameras.py): pitch is checked by the picture moving up or down instead of the hook's log line, the stick has to turn the picture the same way as the mouse (where a held key doesn't mask the scripted pad), and like mac_user_launch it checks its inputs first and won't clear a --work folder that isn't its own.

Round 9 checked which way up the mouse goes. Review saw strafe's picture move the other way from follow's, but that was strafe still settling over the shoulder: measured with that motion taken out, strafe's original sign is the one that matches follow's mouse, and the lift agrees (with the game's invert off, stick-down and mouse-down both raise strafe's pitch). So strafe stays as it was, and the test now measures pitch net of the camera's own motion. The lock camera's tilt only took the mouse with the stick pushed (the game multiplies it by the stick's magnitude afterwards) and the other way up, so it now goes on that product, the stick's way up. A lift patched with the earlier edits gets them replaced. patch_lift.py also migrates 494cfa6's follow hook now, which is what the shipped v1.0.0 USA Windows exes were built from, instead of stopping the build. The mouse test checks which way pitch goes too: like the stick's down, or where a held key masks the stick, like follow's mouse. Lock's direction is from reading the lift, it still isn't reached in a scripted run.

Round 7 added two more. The lock camera only took the mouse's yaw, now the mouse's pitch tilts it too. Its tilt isn't an angle you hold, it follows the stick's vertical axis and springs back to level, so the mouse goes into that axis right after the stick read, clamped to the stick's range, on 1.00 and 1.20. A lift patched with the older yaw-only lock hook gets it taken out and both put in (`tests/test_mouse_repatch.py`). And scripts/update_runtime_patch.sh and the setups run git with `core.autocrlf=false` and `core.eol=lf`, so a Windows git that defaults autocrlf to true doesn't make every runtime look edited. The test covers that with a global autocrlf=true config.

## Black window with sound on the Mac (#72, `fix/macos-guest-programs`)

On 1.0.0 the Mac game started from the .app showed a black window with sound. Windows and Linux turn on the guest's own shader programs (`PS3RECOMP_VK_GUEST_PROGRAMS=1`) and set `PS3_TITLE` before starting, the Mac start didn't, so the renderer took the vtable path and never uploaded a texture. Our test runs set both in the environment, which is why we never saw it. The Mac start now sets them too (the environment or settings.txt still win), and `tests/mac_user_launch.py` starts the packaged game the way a player does, without them, and checks the log for the register-file draw path and uploaded textures. The macOS 14 VM boot test now starts the game without them too. Round 8: the test checks its inputs before anything, only clears a --work folder that's empty or its own, and fails a game that stops by itself during the run. Round 9: it waits for the first drawn frame, watches from there and longer while the game is still loading, fails a game that stops drawing (a hang), and calls a run still loading at its cap INCONCLUSIVE so it gets run again with more time.

Tested: RESULTS_GUEST

## Windows checkouts and tests (`fix/windows-gitattributes-eol`)

The first Windows CI run found three Windows-only problems. With `* text=auto` a Windows clone checked out game/menu_keycaps.as with CRLF, so its hash stopped matching SOURCE_SHA and the menu keycaps script and its test failed. .gitattributes now keeps `*.as` LF and `*.bin` binary. The other two are test fixes on their own branches: test_update_runtime_patch runs the bash on PATH (a bare `bash` started WSL's, since Windows looks in System32 first, on `fix/mouse-direct-all-cameras`), and the comma test gives its harness programs a .exe name on Windows (on `fix/comma-binding`).

## hotfix/1.0.1 all together

RESULTS_COMBINED
