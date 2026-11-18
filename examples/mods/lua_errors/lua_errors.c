/* An example RACiT code mod: logs every Lua error the game raises and every
 * level change. Build: ./build.sh (or any Windows x86-64 compiler, see
 * docs/MODDING.md), then copy this folder into the launcher's mods\ folder. */
#include "racit_mod.h"

static const racit_api* api;
static racit_mod* self;
static racit_func luaD_throw_original;
static volatile long errors;

/* luaD_throw(lua_State* L, int errcode) at 0x50A83C. It never returns to its
 * caller (it unwinds to the Lua error handler), so log before passing it on. */
static void my_luaD_throw(racit_ppu* ctx)
{
    const uint32_t L = (uint32_t)ctx->gpr[3];
    const int code = (int)ctx->gpr[4];
    api->log(self, "Lua error #%ld: code %d, state 0x%08X", ++errors, code, L);
    luaD_throw_original(ctx);
}

static void level_changed(uint32_t old_level, uint32_t new_level, void* user)
{
    (void)user;
    api->log(self, "level %u -> %u", old_level, new_level);
}

RACIT_EXPORT uint32_t racit_mod_api_version(void) { return RACIT_API_VERSION; }

RACIT_EXPORT int racit_mod_init(const racit_api* a, racit_mod* m)
{
    api = a; self = m;
    api->log(self, "hello from %s (game %s, API %u)", api->mod_dir(self), api->game_version, api->api_version);
    if (api->hook(self, 0x0050A83C, my_luaD_throw, &luaD_throw_original) != 0)
        return 1;   /* not this game build: unload */
    api->on_level(self, level_changed, 0);
    return 0;
}
