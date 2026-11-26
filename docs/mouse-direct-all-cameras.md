# Mouse turns every camera directly (hotfix)

This is meant for a hotfix release on top of v1.0.0. Players were promised keyboard and mouse and right now only the follow camera takes the mouse directly (#66). Every other camera still gets the mouse as a fake right stick push, which is why it feels like a thumbstick and turns too slow (#68). This drops the stick emulation for the cameras completely. Screens that aren't a camera, like the map and the rotate-object puzzles, still get the mouse through the right stick, so both the mouse and the I/J/K/L keys work there.

## What changed

- `scripts/patch_lift.py` step 22 is now a table, `MOUSE_CAMERAS`. Each camera that the right stick turns gets a small hook in its update that adds the mouse motion (radians, used once) to that camera's own yaw and pitch step, after the stick's. So a controller still works with the mouse captured, and the game's own pitch limits still apply.
- `MOUSE_CAMERA_FUNCS` says where each camera is per profile: NA 1.00, EU BCES00511 (same names, `retarget.py` converts them) and NA 1.20. The 1.20 path runs the same table after its JSON hooks, so the old follow cam entry in `versions/BCUS98124-1.20-hooks.json` is gone.
- The build stops if any camera is missing its hook for the profile being built, or if any profile in `versions/profiles.json` has no full camera table.
- The runtime patch (`cellPad.c`) hands the motion to `game/desktop_mouse.cpp` (`racit_mouse_feed`). While some hooked camera is updating it goes to the cameras only and never reaches the stick, so a camera never feels like a thumbstick and never gets a move twice. When no hooked camera has updated for 250 ms (the map, puzzles, menus, cutscenes) the mouse is a right stick push instead, from its speed like 1.0.0's emulation, with the same sensitivity and invert settings. Motion the cameras had is never pushed into the stick later, and the push lets go right away when a camera comes back, the mouse is let go, the quick select ring opens or the polls stop for a moment.
- The camera keys and the mouse add up on the right stick, kept inside its range. Keys alone push it like before.
- Same "Mouse sensitivity" and "Invert mouse Y" settings for every camera, 0.003 rad per count at 100% like #66.

The cameras, by the function that updates them on NA 1.00 / 1.20:

| Camera | 1.00 | 1.20 | Checked |
| --- | --- | --- | --- |
| Follow (default third person) | `func_000459D4` | same | in game, yaw and pitch |
| Strafe (L2, over the shoulder) | `func_0004833C` | same | in game, yaw and pitch |
| Free look | `func_0003DD98` | same | build only |
| Free look 2 | `func_00044050` | same | build only |
| Orbit | `func_00047104` | same | build only |
| Swapped-axes look | `func_00048B38` | same | build only |
| Lock camera | `func_00041E34` | same | build only (pitch tilts it, it springs back like with the stick) |
| First person look | `func_001AFF7C` | `func_001AFF80` | build only |
| Turrets | `func_0037F18C` | `func_0037F4C4` | build only |
| Hand-back camera | `func_00049450` | same | build only (a mouse move hands back to the default camera, like the stick) |

EU uses the 1.00 names through `retarget.py`. I don't have the EU EBOOT so the EU build isn't checked at all.

Kept on the right stick on purpose: the keyboard camera keys from Settings > Controls are keys, so they still push the right stick. The quick select ring pointer still uses the mouse through the left stick.

Not cameras, so not hooked: a few things read the right stick for other stuff, like rotating a puzzle object, the map pan and zoom, radial UI selection and the ship's dodge moves. Those get the mouse as a stick push now, plus the keys. I looked for a guest flag that tells the pause menu apart from the map and puzzles to gate on, but there isn't one I'd trust (nothing confirmed, and nothing checked on 1.20), so it's just "no camera active". The pause menu is checked in game not to move with the mouse instead.

## Tests

- `tests/desktop_mouse_test.cpp`: unit test for `game/desktop_mouse.cpp`, no game data needed (`cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests`). Covers motion used once, sensitivity, invert, nothing outside gameplay and the hand-back peek. For the stick: no push while a camera is active, a push past the 250 ms window, sensitivity, invert, keys and mouse adding up and clamped, and no stale motion getting into the stick (from the cameras, the quick select ring, a released mouse or a gap in the polls). Passes.
- The hook check in `patch_lift.py`: the build stops if any camera is missing its hook for the profile it builds, or a profile has no full table. I checked it fails by breaking one anchor.
- `tests/mac_mouse_cameras.py`: headless run on a Mac (offscreen, no audio). Loads a save, then for follow and strafe a scripted mouse move has to reach that camera's hook and turn the picture the same way the stick does, and a mouse move in the pause menu must turn nothing and not move the menu. With `--map` it also opens the map (SELECT) and checks steady mouse motion pans it with no camera turning. Passes on 1.00 and 1.20 here (Zolar Forest save).

## Tested / not tested

Tested on this Mac (M1 Pro), offscreen, on NA 1.00 and NA 1.20: both build, every camera hook lands in both lifts, and in Zolar Forest the mouse turns the follow camera (yaw and pitch) and the L2 strafe camera, the same direction as the stick, with nothing turning in the pause menu or after leaving it.

Not tested in game: free look, orbit, the swapped-axes look, the lock camera, first person look, turrets and the hand-back camera. They're hooked from reading the code and only build-checked, I couldn't reach them from the saves I have. EU BCES00511 isn't built since I don't have its EBOOT.

Windows isn't built here (I only have a Mac), so HighDrexler needs to build and check Windows before a release. The C++ side is plain and shared, but I haven't compiled it with MSVC.
