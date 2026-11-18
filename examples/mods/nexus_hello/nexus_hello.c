/* An example Into the Nexus code mod: logs when the title's main menu is shown
 * (a hook), each level change (an event) and, every 10 s, the game's own frame
 * time read from guest memory. Build: ./build.sh (or any Windows x86-64 C
 * compiler, see docs/MODDING.md), then copy this folder into the launcher's
 * nexus\mods\ folder and turn it on. */
#include "racit_mod.h"
#include <string.h>

static const racit_api* api;
static racit_mod* self;
static racit_func show_menu_original;

/* func_00530420(frontend*): "show the main menu" (found while porting).
 * A hook gets the guest registers; call the original to keep the game's behaviour. */
static void my_show_menu(racit_ppu* ctx)
{
    api->log(self, "main menu shown (frontend 0x%08X)", (uint32_t)ctx->gpr[3]);
    show_menu_original(ctx);
}

static void level_changed(uint32_t old_level, uint32_t new_level, void* user)
{
    (void)user;
    api->log(self, "level %u -> %u", old_level, new_level);
}

/* The game's frame time (seconds, a big-endian float) lives at 0x00F940EC. */
static void each_frame(uint32_t frame, void* user)
{
    (void)user;
    if (frame % 600 != 0 || !api->guest_valid(0x00F940EC, 4)) return;
    const float dt = racit_rdf(api, 0x00F940EC);   /* big-endian read from guest memory */
    api->log(self, "frame %u: game frame time %.4f s (%.1f fps mode)", frame, dt, dt > 0 ? 1.0f / dt : 0.0f);
}

RACIT_EXPORT uint32_t racit_mod_api_version(void) { return RACIT_API_VERSION; }

RACIT_EXPORT int racit_mod_init(const racit_api* a, racit_mod* m)
{
    api = a; self = m;
    api->log(self, "hello from %s (game %s, API %u)", api->mod_dir(self), api->game_version, api->api_version);
    if (api->hook(self, 0x00530420, my_show_menu, &show_menu_original) != 0)
        return 1;   /* not this game: unload */
    api->on_level(self, level_changed, 0);
    api->on_frame(self, each_frame, 0);
    return 0;
}
