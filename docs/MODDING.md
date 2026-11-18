# Modding RACiT

A mod is a folder in the launcher's `mods\` folder (or a `.zip` installed from the Mods page). It can do any mix of three things:

| Part | What it does |
|---|---|
| `files\` | Replaces game files. Laid out like the game folder (`files\PS3_GAME\USRDIR\packed\...`); the game reads a mod's copy instead of its own. |
| `"settings"` in mod.json | Sets game settings (the names in settings.txt) while the mod is on. |
| `"code"` in mod.json | A DLL the game loads before it starts: it can hook game functions, read and write game memory, and react to frames and level changes. |

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

**Priority.** The Mods page lists the mods that are on, highest priority first. For files, the higher mod's copy wins. For code, the higher mod's hooks run first: they wrap the hooks of the mods below.

## Code mods

Include [`game/racit_mod.h`](../game/racit_mod.h). It is the whole API, in plain C, with no other RACiT header needed. Build a Windows x86-64 DLL with any compiler:

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

`racit_mod_init` runs once, on the main thread. At that point the game's code and data are in memory, but none of the game's code has run yet.

A complete example is [`examples/mods/lua_errors`](../examples/mods/lua_errors). It hooks a function, uses the level event, and comes with a build script.

### Hooking a function

The game is PS3 code recompiled to native code, with one native function per guest function. Guest functions are identified by their PS3 address: `func_0050A83C` in the recompiled source is the function at `0x0050A83C`.

```c
static racit_func original;

static void my_hook(racit_ppu* ctx)
{
    /* arguments: ctx->gpr[3], gpr[4], ... (floats: fpr[1], ...) */
    original(ctx);              /* the game's own behaviour (or the next mod's hook) */
    /* result: ctx->gpr[3] (or fpr[1]) -- change it here to change what the caller gets */
}

api->hook(self, 0x0050A83C, my_hook, &original);
```

- If you skip `original(ctx)`, the hook replaces the function. In that case, set `gpr[3]` to the result the caller expects.
- `api->hook` returns one of these:
  - `0`: the hook is installed.
  - `-1`: no function starts at that address.
  - `-2`: the function has no hook pad. That means the game was built without them, which shouldn't happen in a release.
  - `-3`: this hook is already installed on that function.
- Hooks can be installed at any time, from any thread. `api->unhook` removes yours, as long as no later mod's hook wraps it.
- `api->find_function(addr)` returns the game's own code for a function. You can call it from inside a hook to run another guest function on `ctx`'s registers. It changes those registers, so save the ones you need first.

**Calls that skip a hook.** The compiler folds some small functions into their callers, and the calls it folded don't pass through the function's entry. To make a function fully hookable, add it to [`game/mod_hookable.h`](../game/mod_hookable.h) and open a PR. The next build then never folds that function, so every call is seen. Functions already on that list:

| Address | What it is |
|---|---|
| `0x0050A83C` | `luaD_throw`: a Lua error is raised |
| `0x0029A4F4` | Stores the current level id |

### Memory

PS3 memory is big-endian, and guest address `a` is at `api->guest_base + a`. The header has helpers for reading and writing it: `racit_rd8/16/32`, `racit_rdf`, `racit_wr8/16/32` and `racit_wrf`. Before you touch an address you didn't get from the game itself, check `api->guest_valid(addr, len)`.

### Events

- `api->on_frame(self, cb, user)` calls `cb(frame, user)` once per presented frame.
- `api->on_level(self, cb, user)` calls `cb(old, new, user)` when the level changes. Level `0` means loading and `22` means the menus.
- `api->current_level()` returns the current level.

Both callbacks run on the frame thread, not a game thread. Read and write memory there, but don't call game functions.

### Other calls

- `api->log(self, fmt, ...)` writes a line to `log.txt`, prefixed with `[mod <id>]`.
- `api->mod_dir(self)` returns the mod's folder, so the mod can load its own files.
- `api->setting(self, "NAME")` reads a game setting, including the mod's own `"settings"`.
- `api->game_version` is the game's version string.

### Versions

`racit_mod_api_version` tells the game which API version the mod was built for. A game older than that refuses to load the mod and says so in `log.txt`. Newer games only add fields to the end of `racit_api` and never change existing ones, so a mod built for API 1 keeps working.

## Safety and troubleshooting

- A code mod is a program, with the same access to the PC as the game. The launcher marks these mods **runs code** and asks before turning one on.
- **Safe mode** (Mods page → Code mods) starts the game without any mod's code. Those mods' files and settings still apply.
- `log.txt` records:
  - every mod that loads;
  - every hook installed, with the mod's id and the function's address;
  - every mod refused, and why.
- If the game crashes, the crash report lists the code mods that were loaded and the functions they hooked. When the crash happened inside a mod, the report also names that mod's DLL.
