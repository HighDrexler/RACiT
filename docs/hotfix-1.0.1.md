# 1.0.1 hotfix PRs

These are the PR descriptions for the 1.0.1 hotfix. Each one is its own branch on main, and `hotfix/1.0.1` just merges them all so they can be tested together. Windows isn't built here (I only have a Mac), so everything below was built and run on macOS, and the Windows side is only checked by the data-free tests and by reading the code.

The data-free tests run anywhere with Python 3, no game files needed:

    python -m unittest discover -s tests

## Menus still show controller buttons with a keyboard (#22)

Branches: `fix/menu-keycaps-actions` (#70) and `feat/menu-keycaps-auto` (#76), with `fix/keycaps-everywhere` on top.

In 1.0.0 the menus kept the PS3 buttons with a keyboard for a few reasons. The compiled menu actions file got dropped by the `*.BIN` rule in .gitignore (#70), and even with it the keyboard menus needed a Python step players never run, so the game now makes the menu archive itself (#76). This branch turns that on by default on Windows, macOS and Linux (`RACACIT_MENU_KEYCAPS=0` turns it off, Android stays opt-in) and adds the rest of the keycaps: the HUD's face button pictures, the Kinetic Tether picture, the quick select L1/R1 badges, the d-pad glyphs, and the font's smaller mip levels, which is what the small hint text in vendors and quick select uses.

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

The lift is generated once and patch_lift.py patches it, so a lift patched before a hook was added builds fine without it. That already shipped a Mac build with no mouse camera and no menu keycaps once. Now the build re-runs patch_lift.py when it, its inputs or the lift changed, and stops with the list of missing hooks if any are still missing. versions/*-hook-symbols.txt list every hook for 1.00 and 1.20. I checked both lists call for call against freshly patched 1.00 and 1.20 lifts, and the 1.00 one now has the cheat menu's three hooks too. Also fixes 1.20 chunks patched by an older recipe getting two preambles. `RACACIT_CHECK_HOOKS=OFF` skips it.

`tests/test_lift_hooks.py` checks the lists cover everything patch_lift.py and the 1.20 recipe insert, and that the check passes a complete fake lift and fails one missing racit_publish_menu_keyboard.

Tested: RESULTS_HOOKS

Not tested: Windows (it runs Python from CMake there too, with the venv's interpreter).

## Mac build needs macOS 27 (`fix/macos-14`)

The 1.0.0 Mac zip (RACiT-0.1.0-macos.zip) only starts on macOS 27.2 or newer: the game and the bundled SDL, Vulkan loader, glslang and SPIRV-Tools all have minos 27, because nothing set a deployment target and Homebrew's libraries are built for the build machine. The game now targets macOS 14 (with a build error for any unguarded newer API), mac/build_macos_libs.sh builds the bundled libraries for 14 from their hash-checked source tarballs, and packaging fails if anything in the zip needs a newer macOS.

The draft #74 has the same target change, but it sits on the ps3recomp v0.19 stack, so it can't go in a hotfix on main. This one is the same two changes on main.

Tested: RESULTS_MAC14

Not tested: actually running on a macOS 14 machine (only checked with vtool), Windows (no change there).

## Mouse feels like a thumbstick outside the follow camera (#68)

Branch: `fix/mouse-direct-all-cameras`, see its own PR. Merged here so it gets tested with the rest.

## hotfix/1.0.1 all together

RESULTS_COMBINED
