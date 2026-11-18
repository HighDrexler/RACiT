# Modding Ratchet & Clank: Into the Nexus (RACiT launcher)

Into the Nexus uses the same mod system as RACiT's A Crack in Time. A mod is a folder in the
launcher's `nexus\mods\` folder (or a `.zip` installed from the Mods page with Into the Nexus
selected). It can do any mix of three things:

| Part | What it does |
|---|---|
| `files\` | Replaces game files. Laid out like the game folder (`files\PS3_GAME\USRDIR\packed\...`); the game reads a mod's copy instead of its own. *Inherited from RACiT's runtime; not yet tested with Into the Nexus.* |
| `"settings"` in mod.json | Sets game settings (the names in settings.txt) while the mod is on. |
| `"code"` in mod.json | A DLL the game loads before it starts: it can hook game functions, read and write game memory, and react to frames and level changes. *Tested 2026-11-18 with the example below.* |

```json
{
  "name": "My mod",
  "version": "1.0",
  "author": "you",
  "description": "What it does.",
  "code": "my_mod.dll",
  "settings": { "SOME_SETTING": "1" }
}
```

**Priority.** The Mods page lists the mods that are on, highest priority first. For files, the
higher mod's copy wins. For code, the higher mod's hooks run first: they wrap the hooks of the
mods below.

**A Crack in Time mods do not work here.** The API is the same, but every game function and
memory address is different in Into the Nexus. A mod has to be written for this game's addresses.

## Code mods

Include [`game/racit_mod.h`](../game/racit_mod.h). It is the whole API, in plain C, with no other
header needed. Build a Windows x86-64 DLL with any compiler:

```sh
cl /LD /O2 /I path\to\game my_mod.c                                       # MSVC
x86_64-w64-mingw32-gcc -O2 -shared -I path/to/game -o my_mod.dll my_mod.c # MinGW
zig cc -target x86_64-windows-gnu -O2 -shared -I path/to/game -o my_mod.dll my_mod.c
```

The DLL exports two functions:

```c
#include "racit_mod.h"

static const racit_api* api;
static racit_mod* self;

RACIT_EXPORT uint32_t racit_mod_api_version(void) { return RACIT_API_VERSION; }

RACIT_EXPORT int racit_mod_init(const racit_api* a, racit_mod* m)
{
    api = a; self = m;
    api->log(self, "loaded");
    return 0;   /* anything else: the mod is unloaded and the game goes on */
}
```

`racit_mod_init` runs once, on the main thread, before any of the game's code has run.

A complete example is [`examples/mods/nexus_hello`](../examples/mods/nexus_hello): it hooks the
function that shows the title's main menu, logs level changes, and reads the game's frame time
from memory every 10 s. `./build.sh` builds it; copy the folder (with `nexus_hello.dll` and
`mod.json`) into `nexus\mods\` and turn it on. Its lines in `log.txt` look like:

```
[mods] nexus_hello hooks 0x00530420
[mod nexus_hello] level 0 -> 1
[mod nexus_hello] main menu shown (frontend 0x00F213A0)
[mod nexus_hello] level 1 -> 5
[mod nexus_hello] frame 4200: game frame time 0.0167 s (60.0 fps mode)
```

### Hooking a function

The game is PS3 code recompiled to native code, with one native function per guest function.
Guest functions are named by their PS3 address: `func_00530420` in the recompiled source
(`game/src/recomp/`) is the function at `0x00530420`.

```c
static racit_func original;

static void my_hook(racit_ppu* ctx)
{
    /* arguments: ctx->gpr[3], gpr[4], ... (floats: fpr[1], ...) */
    original(ctx);              /* the game's own behaviour (or the next mod's hook) */
    /* result: ctx->gpr[3] (or fpr[1]) -- change it here to change what the caller gets */
}

api->hook(self, 0x00530420, my_hook, &original);
```

- If you skip `original(ctx)`, the hook replaces the function: set `gpr[3]` to what the caller expects.
- `api->hook` returns `0` installed, `-1` no function starts there, `-2` no hook pad (the game was
  built without them), `-3` already installed.
- Hooks can be installed at any time, from any thread. `api->unhook` removes yours, as long as no
  later mod's hook wraps it.
- `api->find_function(addr)` returns the game's own code for a function, to call from a hook on
  `ctx`'s registers (it changes them: save what you need first).

**Calls that skip a hook.** The compiler folds some small functions into their callers; those
calls do not pass through the function's entry. To make a function fully hookable, add it to
[`game/mod_hookable.h`](../game/mod_hookable.h) (`RACIT_HOOKABLE(00530420)`); the next build never
folds it. The list is empty in Into the Nexus so far.

Some functions known in this port:

| Address | What it is |
|---|---|
| `0x00530420` | Shows the title's main menu (r3 = the frontend) |
| `0x00611568` | Waits for a group of visibility test results (the occlusion reader) |
| `0x0069892C` | Sets the game's frame-rate mode (the port's 60 fps patch lives here) |
| `0x00619C70` | Builds the atmosphere colour tables (512-texel gamma curve) |
| `0x00588480` | Blends two packed ARGB colours by t |

### Memory

PS3 memory is big-endian; guest address `a` is at `api->guest_base + a`. The header's helpers read
and write it: `racit_rd8/16/32`, `racit_rdf`, `racit_wr8/16/32`, `racit_wrf`. Check
`api->guest_valid(addr, len)` before touching an address you did not get from the game.

Known addresses: `0x00F940EC` the game's frame time in seconds (float: 1/30 in menus, 1/60 in play).

### Events

- `api->on_frame(self, cb, user)` calls `cb(frame, user)` once per presented frame.
- `api->on_level(self, cb, user)` calls `cb(old, new, user)` when the level changes.
- `api->current_level()` returns the current level.

Level ids in Into the Nexus (from the level archive the game opens; the game's own level global is
not found yet): `0` starting up, `1` main menu, `2` Prison Transport, `3` Meridian City (Igliak),
`4` Silox, `5` Thram, `6` Kragg, `7` Yerek, `8` the Netherverse.

Both callbacks run on the frame thread, not a game thread: read and write memory there, but do
not call game functions.

### Other calls

- `api->log(self, fmt, ...)` writes a line to `log.txt`, prefixed with `[mod <id>]`.
- `api->mod_dir(self)` returns the mod's folder, so the mod can load its own files.
- `api->setting(self, "NAME")` reads a game setting, including the mod's own `"settings"`.
- `api->game_version` is the launcher's version (set by the launcher), or `Into the Nexus` when the
  game is started without it.

### Versions

`racit_mod_api_version` tells the game which API version the mod was built for. A game older than
that refuses the mod and says so in `log.txt`. Newer games only add fields to the end of
`racit_api`, so a mod built for API 1 keeps working.

## Safety and troubleshooting

- A code mod is a program, with the same access to the PC as the game. The launcher marks these
  mods **runs code** and asks before turning one on.
- **Safe mode** (Mods page -> Code mods) starts the game without any mod's code
  (`RACACIT_CODE_MODS_OFF=1`). Those mods' files and settings still apply.
- `log.txt` records every mod that loads, every hook with the mod's id and the address, and every
  mod refused and why.
- If the game crashes, the crash report lists the code mods that were in and the functions they
  hooked, and names a mod's DLL when the crash happened inside it.
