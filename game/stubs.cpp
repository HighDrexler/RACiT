/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "rsx_host_ui.h"   /* "Clank levels" in the Settings menu (racacit_level_change) */

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

/* A Crack in Time: real libsre (cellSpurs) via LLE, when scripts/lift_libsre.py
 * has produced game/prx/. The symbols are weak so the port still builds and
 * runs on HLE SPURS without the (firmware-derived, local-only) lift. */
#include "prx_loader.h"

typedef struct { uint64_t addr; void (*func)(void*); const char* name; } prx_fe;
extern "C" {
    extern const uint8_t    libsre_image[]          __attribute__((weak));
    extern const uint32_t   libsre_image_size       __attribute__((weak));
    extern const uint32_t   libsre_base             __attribute__((weak));
    extern const prx_export libsre_exports[]        __attribute__((weak));
    extern const uint32_t   libsre_export_count     __attribute__((weak));
    extern const prx_fe     libsre_function_table[] __attribute__((weak));
    extern const uint64_t   libsre_function_table_count __attribute__((weak));
    void ppu_register_function(uint64_t addr, void (*fn)(void*));
}

static void racacit_prx_register(uint32_t guest_addr, void (*host)(void*))
{
    ppu_register_function(guest_addr, host);
}

extern "C" void racacit_discord_start(void);   /* game/discord_rpc.cpp */
extern "C" void racacit_prompts_start(void);   /* game/button_prompts.cpp */

extern "C" void ps3_load_prx_modules(void)
{
    racacit_discord_start();   /* optional Rich Presence; off without a client id */
    racacit_prompts_start();   /* button prompts follow the controller layout */
    if (!&libsre_image_size || !&libsre_function_table_count) {
        printf("[prx] libsre not lifted -- using HLE cellSpurs\n");
        return;
    }
    if (getenv("RACACIT_HLE_SPURS")) {
        printf("[prx] RACACIT_HLE_SPURS set -- skipping real libsre\n");
        return;
    }
    prx_module m;
    memset(&m, 0, sizeof m);
    m.name         = "libsre";
    m.base         = libsre_base;
    m.image        = libsre_image;
    m.image_size   = libsre_image_size;
    m.funcs        = (const prx_func_entry*)libsre_function_table;
    m.func_count   = libsre_function_table_count;
    m.exports      = libsre_exports;
    m.export_count = libsre_export_count;
    prx_load_result r = prx_load_module(&m, racacit_prx_register);
    printf("[prx] libsre LLE: ok=%d funcs=%u base=0x%08X size=0x%X exports=%u\n",
           r.ok, r.funcs_registered, r.base, r.image_size, prx_export_registry_count());
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */

/* ---------------------------------------------------------------------------
 * Guest setjmp/longjmp (Lua error handling) -- see scripts/patch_lift.py.
 * A per-thread stack of host jmp_bufs keyed by the 16-byte-aligned guest
 * jmp_buf address (the guest routines align it the same way).
 * -----------------------------------------------------------------------*/
#include "ps3_setjmp.h"   /* non-unwinding on Windows (tools/ps3recomp/runtime/platform) */
typedef struct RacJbFrame { jmp_buf env; uint32_t guest_buf; struct RacJbFrame* prev; } RacJbFrame;
static thread_local RacJbFrame* t_jb_top = nullptr;

extern "C" void racacit_jb_push(RacJbFrame* f, uint32_t guest_buf)
{
    f->guest_buf = (guest_buf + 15u) & ~15u;
    f->prev = t_jb_top;
    t_jb_top = f;
}

extern "C" void racacit_jb_pop(RacJbFrame* f)
{
    /* Normal exit of the protected frame: drop it (and anything above it,
     * which can only be stale if an inner frame was skipped). */
    t_jb_top = f->prev;
}

extern "C" void racacit_longjmp(uint32_t guest_buf)
{
    uint32_t g = (guest_buf + 15u) & ~15u;
    for (RacJbFrame* f = t_jb_top; f; f = f->prev) {
        if (f->guest_buf == g) {
            t_jb_top = f;        /* frames above the target are unwound */
            longjmp(f->env, 1);
        }
    }
    fprintf(stderr, "[racacit] guest longjmp to 0x%08X with no live setjmp frame\n", guest_buf);
    abort();
}

/* ---------------------------------------------------------------------------
 * sceNpDrmIsAvailable / sceNpDrmIsAvailable2 (sceNp). Unregistered, the HLE
 * layer answered CELL_OK -- "licence present" -- for every path the title asked
 * about, so it went on to use content that is not there. Answer from the VFS:
 * no file, no licence. (There are no .rap licences in a disc dump, so an
 * .edat that does exist cannot be decrypted either; report it as unlicensed.)
 * -----------------------------------------------------------------------*/
#include "../tools/ps3recomp/runtime/ppu/ppu_context.h"
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint8_t* vm_base;
static void racacit_npdrm_is_available(ppu_context* ctx)
{
    uint32_t p = (uint32_t)ctx->gpr[4];
    char path[512] = {0};
    if (p) { for (int i = 0; i < 511 && vm_base[p + i]; i++) path[i] = (char)vm_base[p + i]; }
    static int n = 0;
    if (n++ < 16) fprintf(stderr, "[sceNpDrm] IsAvailable('%s') -> LICENSE_NOT_FOUND\n", path);
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80029521;   /* SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND */
}
__attribute__((constructor)) static void racacit_register_npdrm(void)
{
    ps3_hle_register_ctx(0xF042B14Fu, "sceNpDrmIsAvailable2", racacit_npdrm_is_available);
}

/* ---------------------------------------------------------------------------
 * Language (RACACIT_LANGUAGE: en, fr, es -- the launcher's Settings > Game).
 * The title has no language menu of its own: it reads the PS3's system
 * language (cellSysutilGetSystemParamInt, CELL_SYSUTIL_SYSTEMPARAM_ID_LANG)
 * and loads that translation from the disc. The runtime's answer is always
 * 1, English (US). This handler gives the chosen one instead and answers
 * every other id exactly as the runtime's table does (v91: LANG and
 * ENTER_BUTTON_ASSIGN 1 = Cross, 0x141 and 0x152 1, the rest of its known
 * ids 0, unknown ids logged and 0). It is registered only for a language
 * other than English, so an English game runs the runtime's own handler.
 * -----------------------------------------------------------------------*/
extern "C" void vm_write32(uint64_t a, uint32_t v);
static uint32_t s_sys_lang = 1;   /* CELL_SYSUTIL_LANG_ENGLISH_US */
static void racacit_get_system_param_int(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3], ea = (uint32_t)ctx->gpr[4];
    if (!ea) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8002B002; return; }   /* CELL_SYSUTIL_ERROR_VALUE */
    uint32_t v = 0;
    switch (id) {
    case 0x0111: v = s_sys_lang; break;                         /* LANG */
    case 0x0112: case 0x0141: case 0x0152: v = 1; break;        /* ENTER_BUTTON_ASSIGN (Cross), 0x141, 0x152 */
    case 0x0114: case 0x0115: case 0x0116: case 0x0117:
    case 0x0121: case 0x0123: case 0x0151: case 0x0153: case 0x0156: v = 0; break;
    default: fprintf(stderr, "[cellSysutil] GetSystemParamInt: unknown id 0x%04X\n", id); break;
    }
    vm_write32(ea, v);
    ctx->gpr[3] = 0;
}
extern "C" void racacit_register_language(void)   /* main.cpp, after ppu_hle_init */
{
    const char* e = getenv("RACACIT_LANGUAGE");
    if (!e || !*e) return;
    char l[16] = {0};
    for (int i = 0; e[i] && i < 15; i++) l[i] = (char)((e[i] >= 'A' && e[i] <= 'Z') ? e[i] + 32 : e[i]);
    /* CELL_SYSUTIL_LANG_*: 1 English (US), 2 French, 3 Spanish. */
    if (!strcmp(l, "fr") || !strcmp(l, "french") || !strcmp(l, "2")) s_sys_lang = 2;
    else if (!strcmp(l, "es") || !strcmp(l, "spanish") || !strcmp(l, "3")) s_sys_lang = 3;
    else return;   /* English, or not one the disc has */
    ps3_hle_register_ctx(0x40E895D3u, "cellSysutilGetSystemParamInt", racacit_get_system_param_int);
    fprintf(stderr, "[racacit] language: %s (system language %u)\n", s_sys_lang == 2 ? "French" : "Spanish", s_sys_lang);
}

extern "C" const char* racacit_lua_last_exec(void);
/* racacit: luaD_throw (0x50A83C) hook -- print the error object (Lua 5.1
 * layout: L->top at +8, 16-byte TValue with tt at +8, TString body at +16). */
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" void racacit_lua_throw_hook(ppu_context* ctx)
{
    static int n = 0;
    if (n >= 40) return;
    uint32_t L = (uint32_t)ctx->gpr[3], code = (uint32_t)ctx->gpr[4];
    uint32_t top = vm_read32(L + 8);
    for (int sz = 16; sz >= 8; sz -= 8) {
        uint32_t o = top - sz, tt = vm_read32(o + (sz == 16 ? 8 : 4));
        if (tt == 4) {
            uint32_t ts = vm_read32(o), len = vm_read32(ts + 12);
            char buf[400]; uint32_t k = 0;
            for (; k < len && k < sizeof buf - 1; k++) buf[k] = (char)vm_base[ts + 16 + k];
            buf[k] = 0;
            fprintf(stderr, "[lua] error %u (tv%d): %s\n      last execute(\"%s\")\n", code, sz, buf, racacit_lua_last_exec()); n++;
            return;
        }
    }
    fprintf(stderr, "[lua] error %u (non-string error object, L=0x%08X top=0x%08X)\n", code, L, top); n++;
}

/* racacit: execute(<lua string>) entry (0x5069C) -- log what the scripts run. */
static thread_local char t_last_exec[600];
static void racacit_lua_alltrace_init(void);
extern "C" void racacit_lua_exec_hook(ppu_context* ctx)
{
    { static int once = 0; if (!once) { once = 1; racacit_lua_alltrace_init(); } }
    uint32_t p = (uint32_t)ctx->gpr[3];
    int k = 0;
    if (p) for (; k < 599 && vm_base[p + k]; k++) t_last_exec[k] = (char)vm_base[p + k];
    t_last_exec[k] = 0;
    static int trace = -1;   /* the environment once, not a getenv per script call */
    if (trace < 0) trace = getenv("RACACIT_LUA_EXEC_TRACE") ? 1 : 0;
    if (trace) fprintf(stderr, "[lua] execute(\"%s\")\n", t_last_exec);
}
extern "C" const char* racacit_lua_last_exec(void) { return t_last_exec; }

/* racacit: RACACIT_LUA_ALLTRACE=<map file> (diagnostic): every call from a
 * script into the engine -- the map lists each binding's code address and
 * name (out/lua_cfuncs.txt, from the EBOOT's registration tables) -- with its
 * arguments and what it returned. Thinned per name like RACACIT_LUA_CTRACE. */
extern "C" void (*g_ps3_icall_observer)(ppu_context*, uint32_t, int);
static uint32_t s_lmap_addr[4096]; static char s_lmap_name[4096][40]; static unsigned s_lmap_n;
static void racacit_lua_val(char* line, int& n, int cap, uint32_t o)
{
    const uint32_t v = vm_read32(o), tt = vm_read32(o + 4);
    if (tt == 4) { const uint32_t len = vm_read32(v + 12); n += snprintf(line + n, cap - n, "\"");
                   for (uint32_t j = 0; j < len && j < 48 && n < cap - 8; j++) line[n++] = (char)vm_base[v + 16 + j];
                   n += snprintf(line + n, cap - n, "\""); }
    else if (tt == 3) { float f; memcpy(&f, &v, 4); n += snprintf(line + n, cap - n, "%g", f); }
    else if (tt == 1) n += snprintf(line + n, cap - n, v ? "true" : "false");
    else if (tt == 0) n += snprintf(line + n, cap - n, "nil");
    else n += snprintf(line + n, cap - n, "<t%u %08X>", tt, v);
}
static void racacit_lua_observer(ppu_context* ctx, uint32_t tgt, int phase)
{
    unsigned lo = 0, hi = s_lmap_n;
    while (lo < hi) { const unsigned m = (lo + hi) / 2; if (s_lmap_addr[m] < tgt) lo = m + 1; else hi = m; }
    if (lo >= s_lmap_n || s_lmap_addr[lo] != tgt) return;
    static thread_local uint32_t t_L;
    static unsigned counts[4096];
    if (phase == 0) {
        const unsigned c = ++counts[lo];
        if (c > 8 && (c & (c - 1))) { t_L = 0; return; }
        const uint32_t L = (uint32_t)ctx->gpr[3], top = vm_read32(L + 8), base = vm_read32(L + 12);
        t_L = L;
        char line[480]; int n = snprintf(line, sizeof line, "[lcall] %s #%u (", s_lmap_name[lo], c);
        for (uint32_t o = base, i = 0; o < top && i < 5 && n < 380; o += 8, i++) {
            if (i) n += snprintf(line + n, sizeof line - n, ", ");
            racacit_lua_val(line, n, sizeof line, o);
        }
        n += snprintf(line + n, sizeof line - n, ")");
        fprintf(stderr, "%s%c", line, 10);
    } else if (t_L) {
        const int nres = (int)(int32_t)ctx->gpr[3];
        if (nres > 0 && nres < 8) {
            const uint32_t top = vm_read32(t_L + 8);
            char line[300]; int n = snprintf(line, sizeof line, "[lcall]   -> %s:", s_lmap_name[lo]);
            for (int i = nres; i >= 1 && n < 240; i--) { n += snprintf(line + n, sizeof line - n, " "); racacit_lua_val(line, n, sizeof line, top - 8u * (uint32_t)i); }
            fprintf(stderr, "%s%c", line, 10);
        }
        t_L = 0;
    }
}
static void racacit_lua_alltrace_init(void)   /* first script execute(): settings are in the environment by then */
{
    const char* f = getenv("RACACIT_LUA_ALLTRACE");
    if (!f) return;
    FILE* fp = fopen(f, "r");
    if (!fp) return;
    char nm[64]; unsigned a;
    while (s_lmap_n < 4096 && fscanf(fp, "%x %63s", &a, nm) == 2) { s_lmap_addr[s_lmap_n] = a; strncpy(s_lmap_name[s_lmap_n], nm, 39); s_lmap_n++; }
    fclose(fp);
    g_ps3_icall_observer = racacit_lua_observer;
    fprintf(stderr, "[lcall] tracing %u script bindings%c", s_lmap_n, 10);
}

/* racacit: RACACIT_LUA_CTRACE=1 (diagnostic) -- the entry of the script
 * functions patch_lift.py lists (scenes, speech, cameras, control), with up to
 * four arguments. Lua 5.1 with 8-byte TValues (value, then type; numbers are
 * floats): L->top at +8, L->base at +12. Repeats of one name are thinned. */
extern "C" void vm_write32(uint64_t a, uint32_t v);
static void racacit_lua_note_state(uint32_t L);
static void racacit_lua_file(ppu_context* ctx, uint32_t L);
extern "C" void racacit_luac_trace(ppu_context* ctx, const char* name)
{
    racacit_lua_note_state((uint32_t)ctx->gpr[3]);   /* RACACIT_LUA_FILE (test hook) */
    racacit_lua_file(ctx, (uint32_t)ctx->gpr[3]);
    /* RACACIT_SPACE_FREE_LANDING (default 1): every moon and planet in space
     * can be landed on. The space scripts gate them on story progress --
     * space_enable_moon / space_enable_planet(clue, enable, show), all false
     * until Vorselon's ships and Vorselon are beaten (space_sector_1
     * functions.lua) -- and this turns those arguments to true. 0 = the
     * original story lock. */
    if (name[0] == 's' && (!strcmp(name, "space_enable_moon") || !strcmp(name, "space_enable_planet"))) {
        static int free_land = -1;
        if (free_land < 0) { const char* e = getenv("RACACIT_SPACE_FREE_LANDING"); free_land = !(e && *e == '0'); }
        if (free_land) {
            const uint32_t L = (uint32_t)ctx->gpr[3], top = vm_read32(L + 8), base = vm_read32(L + 12);
            for (uint32_t o = base + 8; o < top && o < base + 24; o += 8) {   /* args 2 and 3: enable, show */
                vm_write32(o, 1); vm_write32(o + 4, 1);                      /* boolean true */
            }
            static int said = 0;
            if (said++ < 4) fprintf(stderr, "[space] %s: landing allowed (RACACIT_SPACE_FREE_LANDING)%c", name, 10);
        }
    }
    static int on = -1;
    if (on < 0) on = getenv("RACACIT_LUA_CTRACE") ? 1 : 0;
    if (!on) return;
    static const char* names[64]; static unsigned counts[64];
    unsigned k = 0;
    for (; k < 64 && names[k] && names[k] != name; k++) {}
    if (k == 64) return;
    names[k] = name;
    const unsigned c = ++counts[k];
    if (c > 8 && (c & (c - 1))) return;   /* the first 8, then powers of two */
    const uint32_t L = (uint32_t)ctx->gpr[3], top = vm_read32(L + 8), base = vm_read32(L + 12);
    char line[400]; int n = snprintf(line, sizeof line, "[luac] %s #%u (", name, c);
    for (uint32_t o = base, i = 0; o < top && i < 4 && n < 300; o += 8, i++) {
        const uint32_t v = vm_read32(o), tt = vm_read32(o + 4);
        if (i) n += snprintf(line + n, sizeof line - n, ", ");
        if (tt == 4) {
            const uint32_t len = vm_read32(v + 12);
            n += snprintf(line + n, sizeof line - n, "\"");
            for (uint32_t j = 0; j < len && j < 60 && n < 380; j++) line[n++] = (char)vm_base[v + 16 + j];
            n += snprintf(line + n, sizeof line - n, "\"");
        } else if (tt == 3) { float f; memcpy(&f, &v, 4); n += snprintf(line + n, sizeof line - n, "%g", f); }
        else if (tt == 1) n += snprintf(line + n, sizeof line - n, v ? "true" : "false");
        else if (tt == 0) n += snprintf(line + n, sizeof line - n, "nil");
        else if (tt == 7)   /* full userdata: the words after the 16-byte header */
            n += snprintf(line + n, sizeof line - n, "<ud 0x%08X: %08X %08X %08X %08X>", v,
                          vm_read32(v + 16), vm_read32(v + 20), vm_read32(v + 24), vm_read32(v + 28));
        else n += snprintf(line + n, sizeof line - n, "<t%u 0x%08X>", tt, v);
    }
    fprintf(stderr, "%s)\n", line);
}

/* ---------------------------------------------------------------------------
 * Per-object shadow maps: a caster's view-space box must be current
 *
 * func_005EDEE8 fits each shadow-casting moby's shadow map to the box at
 * moby+0xA0/+0xB0 -- the moby's bounds in CAMERA space (the bounds SPU job,
 * GUID 429de002, rotates them by the camera quaternion in the block at
 * 0x00F5D680). That job only revisits mobies that queued themselves for an
 * update this frame (the list func_00647F60 builds), so a static object keeps
 * the box of the camera it was first seen from. In Zolar Forest the crashed
 * ship's box was computed at load; once the camera moved its 1024x1024 shadow
 * map was fitted to empty space and its sun shadow never appeared.
 *
 * Its world-space bounding sphere (moby+0x70, x y z r -- kept current by the
 * same job) says where the box must be: when the stored box's centre is more
 * than a radius from the sphere's centre in today's camera space, the box is
 * rebuilt around that point, and from then on follows the sphere every frame
 * for as long as the game leaves it untouched. A box the game keeps current
 * is left exactly as computed. patch_lift.py calls this before the box is read.
 * RACACIT_KEEP_STALE_BOXES=1 turns it off.
 * -----------------------------------------------------------------------*/
extern "C" void vm_write32(uint64_t a, uint32_t v);
static inline float racacit_rdf(uint32_t ea) { uint32_t u = vm_read32(ea); float f; memcpy(&f, &u, 4); return f; }
static inline void  racacit_wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); vm_write32(ea, u); }

extern "C" void racacit_caster_box(uint32_t moby)
{
    static int off = -1;
    if (off < 0) { const char* e = getenv("RACACIT_KEEP_STALE_BOXES"); off = (e && *e == '1') ? 1 : 0; }
    if (off || !moby) return;
    const float sx = racacit_rdf(moby + 0x70), sy = racacit_rdf(moby + 0x74),
                sz = racacit_rdf(moby + 0x78), r = racacit_rdf(moby + 0x7C);
    if (!(r > 0.0f) || r > 1.0e4f) return;
    /* The camera rotation the bounds job uses (x y z w at 0x00F5D690). */
    const float x = racacit_rdf(0x00F5D690), y = racacit_rdf(0x00F5D694),
                z = racacit_rdf(0x00F5D698), w = racacit_rdf(0x00F5D69C);
    const float n2 = x * x + y * y + z * z + w * w;
    if (!(n2 > 0.98f && n2 < 1.02f)) return;
    const float cx = (1 - 2 * (y * y + z * z)) * sx + 2 * (x * y - z * w) * sy + 2 * (x * z + y * w) * sz;
    const float cy = 2 * (x * y + z * w) * sx + (1 - 2 * (x * x + z * z)) * sy + 2 * (y * z - x * w) * sz;
    const float cz = 2 * (x * z - y * w) * sx + 2 * (y * z + x * w) * sy + (1 - 2 * (x * x + y * y)) * sz;
    const float bx = 0.5f * (racacit_rdf(moby + 0xA0) + racacit_rdf(moby + 0xB0));
    const float by = 0.5f * (racacit_rdf(moby + 0xA4) + racacit_rdf(moby + 0xB4));
    const float bz = 0.5f * (racacit_rdf(moby + 0xA8) + racacit_rdf(moby + 0xB8));
    const float dx = bx - cx, dy = by - cy, dz = bz - cz;
    /* A box this function wrote and the game has not rewritten since is
     * stale by definition: follow the sphere every frame. Waiting for a
     * radius of drift (the old rule) let a moving caster leave its box
     * behind -- the map is fitted to the box, so the part outside was cut
     * off in a straight line and the shadow jumped back each rebuild: the
     * owner's boxy shadows (worst on the moons' large static casters). */
    enum { OWN = 256 };
    static struct { uint32_t moby; float lo_x, hi_z; } own[OWN];
    const uint32_t k = (moby >> 4) & (OWN - 1);
    const int ours = own[k].moby == moby && own[k].lo_x == racacit_rdf(moby + 0xA0) && own[k].hi_z == racacit_rdf(moby + 0xB8);
    if (!ours && dx * dx + dy * dy + dz * dz <= r * r) return;   /* the game's current box */
    /* RACACIT_CASTER_LOG=1: each rebuild, with the box the game had (size) and the cube it gets. */
    { static int lg = -1; if (lg < 0) { const char* e = getenv("RACACIT_CASTER_LOG"); lg = (e && *e == '1') ? 1 : 0; }
      static unsigned n; if (lg && !ours && n < 400) { n++;
        fprintf(stderr, "[caster-box] moby 0x%08X r=%.1f old %.1fx%.1fx%.1f at (%.1f %.1f %.1f) -> cube %.1f at (%.1f %.1f %.1f)%c",
                moby, r, racacit_rdf(moby + 0xB0) - racacit_rdf(moby + 0xA0), racacit_rdf(moby + 0xB4) - racacit_rdf(moby + 0xA4),
                racacit_rdf(moby + 0xB8) - racacit_rdf(moby + 0xA8), bx, by, bz, 2 * r, cx, cy, cz, 10); } }
    racacit_wrf(moby + 0xA0, cx - r); racacit_wrf(moby + 0xA4, cy - r); racacit_wrf(moby + 0xA8, cz - r);
    racacit_wrf(moby + 0xB0, cx + r); racacit_wrf(moby + 0xB4, cy + r); racacit_wrf(moby + 0xB8, cz + r);
    own[k].moby = moby; own[k].lo_x = racacit_rdf(moby + 0xA0); own[k].hi_z = racacit_rdf(moby + 0xB8);
}

/* ---------------------------------------------------------------------------
 * Start skips any pre-rendered movie (RACACIT_START_SKIPS_MOVIES, default on)
 *
 * Insomniac's movie layer keeps a table of 32-byte slots: slot+0 is the BINK*,
 * slot+8 "stopped", slot+9 "loop". func_006511BC is the per-frame update for a
 * slot and returns 1 while the movie is still playing, 0 once it has ended --
 * its caller then stops and closes it. scripts/patch_lift.py routes that
 * return value and every BinkOpen (func_009A8798, called from func_0065282C)
 * through the two hooks below. A START press during a one-shot movie makes
 * the update report "ended", so the game's own end-of-movie path runs exactly
 * as when the last frame plays. Looping movies (the title screen's backdrop)
 * and the weapon-vendor previews (HOW_*_FMV) are left alone: those are part
 * of a menu, where START already means something.
 * -----------------------------------------------------------------------*/
extern "C" uint8_t  vm_read8(uint64_t a);
extern "C" int  ps3_pad_take_start_press(void);
extern "C" void ps3_pad_clear_start_press(void);

namespace {
struct MovieRec { uint32_t bink; int skippable; char name[48]; };
MovieRec g_movies[8];
int      g_movie_next;

int start_skips_enabled()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RACACIT_START_SKIPS_MOVIES"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}

MovieRec* movie_find(uint32_t bink)
{
    for (auto& m : g_movies) if (m.bink && m.bink == bink) return &m;
    return nullptr;
}
}  // namespace

extern "C" void racacit_bink_opened(uint32_t bink, uint32_t path_ea)
{
    if (!bink) return;
    char path[160] = {0};
    for (unsigned i = 0; path_ea && i + 1 < sizeof path; i++) {
        char c = (char)vm_read8(path_ea + i);
        if (!c) break;
        path[i] = c;
    }
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char low[48] = {0};
    for (unsigned i = 0; base[i] && i + 1 < sizeof low; i++)
        low[i] = (char)((base[i] >= 'A' && base[i] <= 'Z') ? base[i] + 32 : base[i]);
    MovieRec& m = g_movies[g_movie_next++ & 7];
    m.bink = bink;
    m.skippable = strncmp(low, "how_", 4) != 0 && !strstr(low, "title");
    snprintf(m.name, sizeof m.name, "%s", low);
    ps3_pad_clear_start_press();   /* a press from before the movie must not skip it */
    fprintf(stderr, "[movie] open %s (bink 0x%08X)%s\n", m.name, bink,
            m.skippable && start_skips_enabled() ? " -- START skips" : "");
}

/* Frames a one-shot movie has played -- game/discord_rpc.cpp shows "Watching a
 * cutscene" while it moves. */
extern "C" volatile unsigned g_racacit_movie_frames = 0;

extern "C" uint32_t racacit_movie_update_hook(uint32_t slot, uint32_t playing)
{
    if (playing && vm_read32(slot) && !vm_read8(slot + 9)) g_racacit_movie_frames++;
    if (!playing) {                       /* played to the end: say so (test drivers) */
        MovieRec* m = movie_find(vm_read32(slot));
        if (m) { if (m->skippable) fprintf(stderr, "[movie] ended %s\n", m->name); m->bink = 0; }
        return playing;
    }
    if (!start_skips_enabled()) return playing;
    uint32_t bink = vm_read32(slot);
    if (!bink || vm_read8(slot + 9)) return playing;          /* no movie / looping */
    MovieRec* m = movie_find(bink);
    if (!m || !m->skippable) return playing;
    /* RACACIT_AUTOSKIP_MOVIES=1 (test drivers): every skippable movie ends on
     * its first frame, no START needed -- the test scripts do not sit through
     * cutscenes. Off by default; players skip with START. */
    static int s_auto = -1;
    if (s_auto < 0) { const char* e = getenv("RACACIT_AUTOSKIP_MOVIES"); s_auto = (e && *e == '1') ? 1 : 0; }
    if (!s_auto && !ps3_pad_take_start_press()) return playing;
    fprintf(stderr, "[movie] START: skipping %s\n", m->name);
    m->bink = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Every cutscene skippable (RACACIT_ALL_CUTSCENES_SKIPPABLE, default on)
 *
 * The level scripts queue movies, in-engine cinematics and scripted scenes
 * through queue_movie / queue_cinematic / queue_scene with an options table;
 * `abort = true` there is what lets the player skip it (great_clock_a's
 * main.lua: queue_movie("GC1_INTRO_FMV", { abort = true })). The parsed
 * options reach the three queue functions (func_0004E7B8 movie, 0004E598
 * cinematic, 0004E6F0 scene) as r5, with the abort flag at +0x61 -- the
 * option defaults to false, which is why some cutscenes could never be
 * skipped. patch_lift.py calls this at each queue function's entry.
 * -----------------------------------------------------------------------*/
extern "C" void vm_write8(uint64_t a, uint8_t v);
extern "C" void racacit_force_abort(uint32_t opts, int kind)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RACACIT_ALL_CUTSCENES_SKIPPABLE"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !opts) return;
    if (!vm_read8(opts + 0x61)) {
        static int n;
        if (n++ < 32)
            fprintf(stderr, "[cutscene] %s queued without abort -- made skippable\n",
                    kind == 0 ? "movie" : kind == 1 ? "cinematic" : "scene");
        vm_write8(opts + 0x61, 1);
    }
}

/* ---------------------------------------------------------------------------
 * End-of-frame blit: the RSX must take the return JUMP before the scratch
 * packet is reused
 *
 * func_005F3E50 ends every level frame like this (addresses from a trace):
 *   - a cellGcmSetTransferImage packet is built in scratch memory
 *     (0x2003C780, 0x68 bytes) with a return JUMP after it (0x2003C7E8);
 *   - the word at the RSX's parked position in one main command buffer is
 *     patched to JUMP into the packet, put = the packet's end;
 *   - it waits for the RSX to go idle (func_0066117C);
 *   - put = the other main buffer + 0x24, which is where the return JUMP
 *     leads, so the RSX takes it and is idle again;
 *   - it returns WITHOUT waiting, and the scratch is handed to the next user
 *     (func_0054CBC4 builds SPU job descriptors at the same address).
 * A real RSX takes that JUMP microseconds after the put write. Our walker ran
 * milliseconds later, read the next tenant's data where the JUMP had been (a
 * pointer 0x20044980 -- which decodes as JUMP 0x44980, a pointer table) and
 * walked off into vertex data: "FIFO JUMP to unmapped IO ... 0x3F800000", the
 * resync, the game's "GPU sit and spin" watchdog, and a level that never
 * appeared while its audio played.
 *
 * patch_lift.py calls this after each of the two put writes: wait = 0 wakes
 * the walker (the title waits for idle itself), wait = 1 blocks until the
 * walker has caught up, which is what the hardware guarantees here.
 * -----------------------------------------------------------------------*/
extern "C" void cellGcm_fifo_wake(void);
extern "C" int  cellGcm_fifo_sync(uint32_t timeout_us);
extern "C" void ps3_tl(char ev, uint32_t a, uint32_t b);
extern "C" void racacit_gcm_put_moved(int wait)
{
    ps3_tl('K', (uint32_t)wait, 0);
    if (wait) cellGcm_fifo_sync(50000);
    else      cellGcm_fifo_wake();
}

/* -------------------------------------------------------------------------
 * Test hook: RACACIT_START_LEVEL=<id> (dev testing only, unset by default).
 * func_0029A4F4(level, ...) is the title's level change: it stores the id in
 * the current-level global (0xEF7C90, via func_0032A1B0) and starts the load.
 * The first change to a real level (id 1..0x15) after boot is redirected to
 * the given id, so LOAD on any save goes straight to that level -- e.g. 2 =
 * zolar_forest, the first Ratchet level. Ids come from the name table at
 * 0xDE1820 (1 great_clock_a, 2 zolar_forest, 3 space_sector_1, ...).
 * patch_lift.py calls this at the function's entry.
 * -----------------------------------------------------------------------*/
/* "Clank levels: Skip" in the Settings menu (RACACIT_SKIP_CLANK=1): a load into
 * one of Clank's Great Clock levels goes to the level that Great Clock level
 * itself exits to (its set_level in the level's main.lua): A -> zolar_forest,
 * B -> space_sector_2, C -> space_sector_3, D -> space_sector_4.
 * Checked in the level scripts (2026-10-18, all 21 levels' Lua): no level
 * outside the Great Clock reads a GC?_LEVEL_COMPLETE flag or a Great Clock
 * mission, so a skipped A-D cannot stall the story; it only leaves that
 * level's own flags unset and skips its space recap text (set_story_tag).
 * great_clock_e (20) is NEVER skipped: it is the final boss, the ending
 * movie, the credits and the beat-the-game trophy (end.lua). Skipping it used
 * to send the player to the Insomniac Museum with no ending. */
static uint32_t skip_clank_level(uint32_t level)
{
    rsx_host_options o; rsx_host_ui_options(&o);
    if (!o.skip_clank) return level;
    /* great_clock_a..d and the level each one exits to. */
    static const uint32_t from[] = { 1, 5, 9, 14 }, to[] = { 2, 6, 10, 15 };
    for (int i = 0; i < 4; i++)
        if (level == from[i]) {
            fprintf(stderr, "[racacit] Clank level %u skipped -> %u (Settings: Clank levels = Skip)\n", level, to[i]);
            return to[i];
        }
    return level;
}

/* Counts level loads (game/button_prompts.cpp rescans after each). */
extern "C" { volatile unsigned g_racacit_level_changes = 0; }
extern "C" uint32_t racacit_level_change(uint32_t level)
{
    g_racacit_level_changes++;
    static int done = 0;
    static long want = -1;
    if (want < 0) {
        const char* e = getenv("RACACIT_START_LEVEL");
        want = e ? strtol(e, nullptr, 0) : 0;
    }
    if (!done && want > 0 && level >= 1 && level <= 0x15) {
        done = 1;
        fprintf(stderr, "[racacit] level change %u -> %ld (RACACIT_START_LEVEL)\n", level, want);
        return (uint32_t)want;
    }
    return skip_clank_level(level);
}

/* ---------------------------------------------------------------------------
 * Collision command record: a late "done" write-back must not land on the
 * next command (the Great Clock D freeze, v91 and v93; docs/dev/HANDOFF.md
 * "RE-DIAGNOSIS").
 *
 * The PPU writes a 160-byte collision command record (0x00F3CE00 in the
 * owner's logs) and the broad-phase job (B9B580) posts work items for it. The
 * narrow-phase job CollNarrow (B8A980) runs on several SPU lanes at once: each
 * claims an item, fetches the record, and ends the item by writing the
 * record's +0x10 quad back from that fetched copy with bit 15 ("done") set in
 * the type halfword at +0x18 (its F5E8; the broad phase does the same at
 * 0x1790 when it posted nothing). On a PS3 that write lands long before the
 * PPU writes the next command. Here a lane thread the OS deschedules for a few
 * ms can write it AFTER: the new command then reads "old type 4, done"
 * (0x8004), the lane that claims its item indexes the finish-callback table at
 * LS 0x14F2C with 0x8004, branches into data and dies, and the main thread
 * waits on the record forever.
 *
 * The record's line is marked in the coherence bitmap, so every PPU store into
 * it takes the lock-line lock and reaches spu_coh_notify_write; a store into
 * the +0x10 quad bumps the record's generation. A lane's fetch of the record
 * (copied under the same lock) remembers the generation, and its write-back is
 * dropped if the PPU has written the quad since -- the state a PS3 ends in
 * (its write lands first and the PPU's overwrites it). Logged as "[collrec]
 * stale done-mark dropped"; RACACIT_COLLREC_KEEP=1 logs it but lets the write
 * through (the old behaviour). A narrow-phase fetch of a record already marked
 * done is logged as well, with which phase marked it and in which command.
 * ------------------------------------------------------------------------- */
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);
extern "C" void spu_coh_reserve(void* ctx, uint32_t ea);
extern "C" void (*g_coh_write_hook)(uint32_t ea);
extern "C" int  (*g_spu_get_copy_hook)(void*, const uint8_t*, uint32_t, uint32_t, uint32_t, uint8_t*, const uint8_t*);
extern "C" int  (*g_spu_put_skip_hook)(void*, const uint8_t*, uint32_t, uint32_t, uint32_t);
static const uint32_t kCollGuid[2] = { 0x43593402u, 0x4248D602u };   /* job GUID at LS 0x100: narrow, broad */
static const uint32_t kCollCopy[2] = { 0x14F60u, 0xCA00u };          /* where each keeps its copy of the record */
static const char* const kCollWho[2] = { "narrow-phase", "broad-phase" };
extern "C" int  spu_coh_is_reserved(uint32_t addr);
extern "C" void spu_coh_unreserve_line(uint32_t ea);
struct CollRec  { uint32_t ea, gen, done_gen, inc; int done_by, owned, marked; unsigned long long last_prod; };
struct CollSnap { const void* spu; uint32_t ea, gen, inc; };
struct CollLog  { int kind, k, done_by; uint32_t ea, type, gen_was, gen_now, done_gen; };
/* Besides a few fixed records (0x00F3CE00, the one in both owner freezes, is
 * rewritten every frame) the game cycles through rings of them (0x205DF780,
 * +0xA0, ...): ~4100 distinct ones in Great Clock D, each reused about every
 * 2 s -- far longer than any lane is held up, so they cannot race. Only a
 * record the broad phase fetches again within 500 ms (a new command) gets its
 * line marked and its write-backs checked. v94 marked every one: ~3,000 lines,
 * and 160-180k PPU stores a second into records took the locked path ([mem],
 * 2026-10-30). The rest stay in the table for that timing only. Kept at most
 * half full; when new pools fill it, it starts over and the lines it marked
 * go back to the lock-free path. `inc` tells a lane's copy from before a
 * restart from the record's new entry: that one write goes unchecked. */
enum { kCollSlots = 16384, kCollMax = kCollSlots / 2 };
static CollRec s_crec[kCollSlots];      /* this block's state: written under the lock-line lock; ea 0 = free */
static int s_crec_n;
static uint32_t s_crec_inc;
static long s_crec_resets, s_crec_drops, s_crec_fetched_done;
static thread_local CollSnap t_csnap[2];
static thread_local CollLog t_clog;     /* filled under the lock, printed after it */
static int collrec_keep(void) { static int v = -1; if (v < 0) { const char* e = getenv("RACACIT_COLLREC_KEEP"); v = (e && *e == '1') ? 1 : 0; } return v; }
static inline uint32_t collrec_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int collrec_find(uint32_t ea, bool add)
{
    if (!ea) return -1;
    for (uint32_t h = ((ea >> 4) * 0x9E3779B1u) >> 18, p = 0; p < kCollSlots; p++) {
        const int i = (int)((h + p) & (kCollSlots - 1));
        if (s_crec[i].ea == ea) return i;
        if (s_crec[i].ea) continue;
        if (!add) return -1;
        s_crec[i] = CollRec{ ea, 0u, 0u, ++s_crec_inc, -1, 0, 0, 0ull };
        s_crec_n++;
        return i;
    }
    return -1;
}
/* From here on the PPU's stores into the record come through the lock. */
static void collrec_mark(CollRec& r)
{
    r.owned = !spu_coh_is_reserved(r.ea + 0x10u);
    spu_coh_reserve(nullptr, r.ea + 0x10u);
    r.marked = 1;
}
static void collrec_reset(void)
{
    for (int i = 0; i < kCollSlots; i++)
        if (s_crec[i].ea && s_crec[i].marked && s_crec[i].owned) spu_coh_unreserve_line(s_crec[i].ea + 0x10u);
    memset(s_crec, 0, sizeof s_crec);
    s_crec_n = 0;
    s_crec_resets++;
}

static void racacit_collrec_print(void)
{
    const CollLog l = t_clog;
    t_clog.kind = 0;
    if (l.kind == 1) {
        if (s_crec_drops <= 16 || (s_crec_drops & 63) == 0)
            fprintf(stderr, "[collrec] stale done-mark %s (#%ld): a %s lane wrote back record 0x%08X (type 0x%04X) "
                            "after the PPU had written the next command (gen %u -> %u)\n",
                    collrec_keep() ? "KEPT (RACACIT_COLLREC_KEEP)" : "dropped", s_crec_drops, kCollWho[l.k],
                    l.ea, l.type, l.gen_was, l.gen_now);
    } else if (l.kind == 3) {
        fprintf(stderr, "[collrec] protecting collision record 0x%08X (a new command %u us after the last)\n",
                l.ea, l.gen_now);
    } else if (l.kind == 4) {
        fprintf(stderr, "[collrec] record table full (%d records): restarted (#%ld) at record 0x%08X\n",
                (int)kCollMax, s_crec_resets, l.ea);
    } else if (l.kind == 2) {
        if (s_crec_fetched_done <= 16 || (s_crec_fetched_done & 255) == 0)
            fprintf(stderr, "[collrec] narrow-phase lane fetched record 0x%08X already marked done (type 0x%04X, #%ld); "
                            "last done-mark: %s, %s\n", l.ea, l.type, s_crec_fetched_done,
                    l.done_by >= 0 ? kCollWho[l.done_by] : "none seen",
                    l.done_by < 0 ? "-" : l.done_gen == l.gen_now ? "this command" : "an EARLIER command");
    }
}

/* Every DMA GET. A protected record's fetch is copied here, under the lock. */
extern "C" unsigned long long ps3_qpc_us(void);
static int racacit_collrec_get(void* spu, const uint8_t* ls, uint32_t lsa, uint32_t ea, uint32_t size,
                               uint8_t* dst, const uint8_t* src)
{
    if (size != 0xA0u) return 0;
    if (t_clog.kind) racacit_collrec_print();
    const int k = lsa == kCollCopy[0] ? 0 : lsa == kCollCopy[1] ? 1 : -1;
    if (k < 0 || collrec_be32(ls + 0x100) != kCollGuid[k]) return 0;
    spu_lockline_lock();
    int i = collrec_find(ea, false);
    if (i < 0) {
        if (s_crec_n >= kCollMax) { collrec_reset(); t_clog = CollLog{ 4, k, -1, ea, 0u, 0u, 0u, 0u }; }
        i = collrec_find(ea, true);
        if (i < 0) { spu_lockline_unlock(); return 0; }
    }
    CollRec& r = s_crec[i];
    if (k == 1) {                                   /* the broad phase: once per command */
        const unsigned long long now = ps3_qpc_us();
        if (!r.marked && r.last_prod && now - r.last_prod < 500000ull) {
            collrec_mark(r);
            static int s_told;
            if (s_told++ < 8) t_clog = CollLog{ 3, k, -1, ea, 0u, 0u, (uint32_t)(now - r.last_prod), 0u };
        }
        r.last_prod = now;
    }
    if (!r.marked) {                                /* reused too rarely to race: the plain copy */
        t_csnap[k].spu = nullptr;
        spu_lockline_unlock();
        if (k == 0 && (src[0x18] & 0x80u)) {
            s_crec_fetched_done++;
            t_clog = CollLog{ 2, k, r.done_by, ea, (uint32_t)src[0x18] << 8 | src[0x19], 0u, r.gen, r.done_gen };
            racacit_collrec_print();
        }
        return 0;
    }
    memcpy(dst, src, size);
    t_csnap[k] = CollSnap{ spu, ea, r.gen, r.inc };
    if (k == 0 && (dst[0x18] & 0x80u)) {
        s_crec_fetched_done++;
        t_clog = CollLog{ 2, k, s_crec[i].done_by, ea, (uint32_t)dst[0x18] << 8 | dst[0x19], 0u, s_crec[i].gen, s_crec[i].done_gen };
    }
    spu_lockline_unlock();
    if (t_clog.kind) racacit_collrec_print();
    return 1;
}

/* A DMA PUT into a reserved line, lock-line lock held: 1 drops the copy. */
static int racacit_collrec_put(void* spu, const uint8_t* ls, uint32_t lsa, uint32_t ea, uint32_t size)
{
    if (size != 16u) return 0;
    const int k = lsa == kCollCopy[0] + 0x10u ? 0 : lsa == kCollCopy[1] + 0x10u ? 1 : -1;
    if (k < 0) return 0;
    const CollSnap s = t_csnap[k];
    if (s.spu != spu || ea != s.ea + 0x10u || collrec_be32(ls + 0x100) != kCollGuid[k]) return 0;
    const int i = collrec_find(s.ea, false);
    if (i < 0 || s_crec[i].inc != s.inc) return 0;     /* table restarted since this copy: unchecked, as before */
    CollRec& r = s_crec[i];
    const uint8_t* q = ls + lsa;
    if (r.gen == s.gen || collrec_keep()) {
        if (q[8] & 0x80u) { r.done_gen = s.gen; r.done_by = k; }
        if (r.gen == s.gen) return 0;
    }
    s_crec_drops++;
    t_clog = CollLog{ 1, k, -1, s.ea, (uint32_t)q[8] << 8 | q[9], s.gen, r.gen, 0u };
    return collrec_keep() ? 0 : 1;
}

/* A PPU (or SPU atomic) write into a reserved line, lock-line lock held. The
 * +0x10 quad is 16-aligned, so a store into it names its record by address
 * (the PPU writes it in halfwords and words, none straddling the quad). */
static void racacit_collrec_write(uint32_t ea)
{
    if (!s_crec_n) return;
    const int i = collrec_find((ea & ~15u) - 0x10u, false);
    if (i >= 0) s_crec[i].gen++;
}

__attribute__((constructor)) static void racacit_collrec_install(void)
{
    g_coh_write_hook    = racacit_collrec_write;
    g_spu_get_copy_hook = racacit_collrec_get;
    g_spu_put_skip_hook = racacit_collrec_put;
}

/* ---------------------------------------------------------------------------
 * Frame-wait hooks (scripts/patch_lift.py step 10): the entry and the exit
 * of the game's SPU waits -- job_wait func_00696614 and batch_wait
 * func_00660924. render_build_views (func_004DFDF0) makes ~9 submit/wait
 * round trips per view per frame. With SPU_KICK_ON_WAIT=1 the entry also
 * wakes the parked SPU lanes (racacit_spu_kick; measured: no effect, so off by
 * default). RACACIT_WAIT_STATS=1 times every wait by its
 * caller (ctx->lr: tie_wait, particle_wait, ...; names in out/names.json)
 * and prints the top ones every 5 s as ms per second of play.
 * ------------------------------------------------------------------------- */
#include <chrono>
#include <mutex>
extern "C" void racacit_spu_kick(void);
static int rac_wait_stats(void) { static int v = -1; if (v < 0) v = getenv("RACACIT_WAIT_STATS") ? 1 : 0; return v; }
static unsigned long long rac_us(void)
{
    return (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static thread_local unsigned long long t_wait_t0;
static thread_local uint32_t t_wait_lr;
static thread_local int t_wait_depth;
extern "C" volatile uint32_t g_racacit_watch_ea;
extern "C" volatile int g_racacit_watch_img;
extern "C" volatile unsigned long long g_racacit_watch_t;
static thread_local uint32_t t_watch_obj;
/* The wait the main thread is in (its key), 0 = none: PS3_SAMPLE_INWAIT. */
#ifdef __ANDROID__
/* Defined by the Windows sampler (ppu_loader.cpp); here only this file uses it. */
extern "C" { volatile uint32_t g_racacit_main_wait_lr = 0; }
#else
extern "C" volatile uint32_t g_racacit_main_wait_lr;
#endif
extern "C" { ppu_context* volatile g_racacit_main_ctx = nullptr; }
extern "C" unsigned long long ps3_qpc_us(void);
extern "C" void ps3_tl_at(unsigned long long t, char ev, uint32_t a, uint32_t b);
static struct { long lr; long long us; long n; } s_ws[32];
static long long s_ws_win;
static std::mutex s_ws_lock;

/* RACACIT_LUA_FILE=<path> (test hook): when the file exists, its text runs
 * as a Lua chunk on the level scripts' own Lua state, then the file is
 * deleted -- e.g. "on_elapsed(1, function() go(9) end)" in a level whose
 * scripts have a debug go() (Axiom: 9 = the boss battle;
 * scripts/gameplay/gameplay_0_global.lua in level_cached.psarc), or
 * "warp(get_hero(), teleporter_ship)". It goes through the game's
 * load-and-pcall func_005004BC (its execute() uses a config state that is
 * gone after loading): at a script's call into the engine (racacit_luac_trace,
 * r3 = the calling Lua thread), or -- since a level can go a long time
 * without one -- at the main thread's per-frame wait, outside any Lua code,
 * on the level's main Lua thread (found from the last traced call: Lua 5.1
 * L->l_G at +16, G->mainthread at +100, checked by M->l_G == G). The chunk
 * sits below the guest stack pointer during the call and every register is
 * put back after it. */
void func_005004BC(ppu_context* ctx);
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
static volatile uint32_t s_lua_main;
static const char* racacit_lua_file_path(void)
{
    static const char* path = (const char*)1;
    if (path == (const char*)1) path = getenv("RACACIT_LUA_FILE");
    return path;
}
static void racacit_lua_note_state(uint32_t L)
{
    if (!racacit_lua_file_path()) return;   /* only the test hook needs it: no guest reads per script call otherwise */
    if (!L || L >= 0x40000000u) return;
    const uint32_t G = vm_read32(L + 16u);
    if (!G || G >= 0x40000000u) return;
    const uint32_t M = vm_read32(G + 100u);
    if (!M || M >= 0x40000000u || vm_read32(M + 16u) != G) return;
    if (M != s_lua_main) { s_lua_main = M; fprintf(stderr, "[lua-file] level Lua main thread 0x%08X (global 0x%08X)%c", M, G, 10); }
}
static void racacit_lua_file(ppu_context* ctx, uint32_t L)
{
    const char* path = racacit_lua_file_path();
    if (!path || !L) return;
    static unsigned long long next;
    const unsigned long long now = ps3_qpc_us();
    if (now < next) return;
    next = now + 500000ull;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char code[1024];
    const size_t n = fread(code, 1, sizeof code - 1, f);
    fclose(f); remove(path);
    code[n] = 0;
    if (!n) return;
    static uint64_t gpr[32], lr, ctr; static double fpr[32]; static u128 vr[32]; static uint32_t cr, xer, fpscr, vscr;
    memcpy(gpr, ctx->gpr, sizeof gpr); memcpy(fpr, ctx->fpr, sizeof fpr); memcpy(vr, ctx->vr, sizeof vr);
    cr = ctx->cr; lr = ctx->lr; ctr = ctx->ctr; xer = ctx->xer; fpscr = ctx->fpscr; vscr = ctx->vscr;
    const uint32_t sp = (uint32_t)ctx->gpr[1], str = (sp - 0x1000u) & ~15u, nsp = (sp - 0x1400u) & ~15u;
    memcpy(vm_base + str, code, n + 1);
    vm_write32(nsp, 0); vm_write32(nsp + 4, sp);          /* back chain */
    ctx->gpr[1] = nsp; ctx->gpr[3] = L; ctx->gpr[4] = str;
    func_005004BC(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(ctx); }
    const int status = (int)(int32_t)ctx->gpr[3];
    memcpy(ctx->gpr, gpr, sizeof gpr); memcpy(ctx->fpr, fpr, sizeof fpr); memcpy(ctx->vr, vr, sizeof vr);
    ctx->cr = cr; ctx->lr = lr; ctx->ctr = ctr; ctx->xer = xer; ctx->fpscr = fpscr; ctx->vscr = vscr;
    fprintf(stderr, "[lua-file] ran \"%s\" on 0x%08X: status %d%c", code, L, status, 10);
}

extern "C" void racacit_wait_begin(ppu_context* ctx)
{
    racacit_spu_kick();
    /* The main game thread's context, for the stall watch (main.cpp): it is
     * the one that waits on MobyPreRender every frame. */
    if ((uint32_t)ctx->lr == 0x006459D0u) { g_racacit_main_ctx = ctx; racacit_lua_file(ctx, s_lua_main); }
    if (!rac_wait_stats()) return;
    if (t_wait_depth++ == 0) {
        t_wait_t0 = rac_us(); t_wait_lr = (uint32_t)ctx->lr;
        /* func_00929FFC (resolve a handle and wait until its object is
         * ready) has dozens of callers: key on its caller, whose return
         * address it saved at its frame + 0x90. */
        if (t_wait_lr == 0x0092A018u) t_wait_lr = vm_read32((uint32_t)ctx->gpr[1] + 0x94u);
        /* ...and func_004E7D60 is a one-line wrapper of it: its caller. */
        if (t_wait_lr == 0x004E7D70u) t_wait_lr = vm_read32((uint32_t)ctx->gpr[1] + 0x80u + 0x94u);
        g_racacit_main_wait_lr = t_wait_lr;   /* PS3_SAMPLE_INWAIT */
        /* RACACIT_SPIN_WATCH=1: for the 6 ms-a-frame wait in func_002A7E88,
         * who sets the object's ready bit (halfword +0x18). */
        static int sw = -1; if (sw < 0) sw = getenv("RACACIT_SPIN_WATCH") ? 1 : 0;
        if (sw && t_wait_lr == 0x002A7F1Cu) {
            g_racacit_watch_img = -1;
            t_watch_obj = (uint32_t)ctx->gpr[3];
            g_racacit_watch_ea = t_watch_obj + 0x18u;
        }
    }
}

extern "C" void racacit_wait_end(void)
{
    if (!rac_wait_stats() || t_wait_depth <= 0 || --t_wait_depth != 0) return;
    const unsigned long long now = rac_us(), d = now - t_wait_t0;
    g_racacit_main_wait_lr = 0;
    /* PS3_TIMELINE: the wait as a 'Q' span (lr, us) -- tlan2.py pairs it
     * with the jobs and lane waits around it. */
    { const unsigned long long q = ps3_qpc_us();
      if (d > 50) ps3_tl_at(q - d, 'Q', t_wait_lr, (uint32_t)d); }
    if (t_watch_obj) {
        static int logs;
        const int img = g_racacit_watch_img;
        const unsigned long long hit = g_racacit_watch_t, q = ps3_qpc_us();
        g_racacit_watch_ea = 0;
        if (logs++ < 60)
            fprintf(stderr, "[spin-watch] obj 0x%08X waited %llu us; ready bit written by %s img %d %lld us before the end; +0x18=%04X +0x30=%08X\n",
                    t_watch_obj, d, img >= 0 ? "SPU" : "no SPU write seen,", img,
                    img >= 0 ? (long long)(q - hit) : -1ll, vm_read32(t_watch_obj + 0x18u) >> 16, vm_read32(t_watch_obj + 0x30u));
        t_watch_obj = 0;
    }
    std::lock_guard<std::mutex> g_(s_ws_lock);
    int k = 0;
    for (; k < 32; k++) if (s_ws[k].lr == (long)t_wait_lr || !s_ws[k].lr) break;
    if (k < 32) { s_ws[k].lr = (long)t_wait_lr; s_ws[k].us += (long long)d; s_ws[k].n++; }
    if (!s_ws_win) s_ws_win = (long long)now;
    else if ((long long)now - s_ws_win >= 5000000) {
        const double secs = (double)((long long)now - s_ws_win) / 1e6;
        long long tot = 0; for (int i = 0; i < 32 && s_ws[i].lr; i++) tot += s_ws[i].us;
        fprintf(stderr, "[frame-waits] %.0f ms/s waiting on SPU jobs:", (double)tot / 1000.0 / secs);
        for (int r = 0; r < 8; r++) {
            int b = -1;
            for (int i = 0; i < 32 && s_ws[i].lr; i++)
                if (s_ws[i].n > 0 && (b < 0 || s_ws[i].us > s_ws[b].us)) b = i;
            if (b < 0) break;
            fprintf(stderr, " lr=%08lX %.1fms/s (%ldx, %.0fus)", (unsigned long)s_ws[b].lr,
                    (double)s_ws[b].us / 1000.0 / secs, s_ws[b].n, (double)s_ws[b].us / (double)s_ws[b].n);
            s_ws[b].n = -s_ws[b].n;   /* mark printed */
        }
        fprintf(stderr, "\n");
        for (int i = 0; i < 32; i++) { s_ws[i].us = 0; s_ws[i].n = 0; }
        s_ws_win = (long long)now;
    }
}

/* racacit: sound starts (scripts/patch_lift.py #12): tag 0 = func_00577ED8
 * (play a sound spec), 1 = func_0057AEC0 (the backend virtual it ends in,
 * also called directly by the sound-instance layer). RACACIT_SND_TRACE=1
 * logs each new (tag, r4, caller chain) the first time it plays, with the
 * flip count, and the busiest every 5 s -- which code plays what, to pick
 * the sounds the DualSense speaker should carry. */
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" unsigned long long ps3_qpc_us(void);
static void racacit_guest_chain(ppu_context* ctx, uint32_t* out, int n)
{
    /* PPC64 ELF ABI: back chain at 0(r1), the caller's saved LR at 16(frame). */
    uint32_t fp = (uint32_t)ctx->gpr[1];
    for (int k = 0; k < n; k++) {
        out[k] = 0;
        if (!fp || fp >= 0xF0000000u) break;
        const uint32_t next = vm_read32(fp + 4);          /* low word of the 64-bit slot */
        if (!next || next <= fp || next - fp > 0x100000u) break;
        out[k] = vm_read32(next + 16 + 4);
        fp = next;
    }
}
/* DualSense speaker routing: which sounds the controller carries, by name.
 * A Crack in Time names every sound in the level's sound table (64-byte
 * entries, [[instance+0xC]]): wep_* weapons, cha_* characters, ratchet_*,
 * gad_* gadgets, amb_*, ui_*, the Zoni pickups (cha_zoni_capture_heb,
 * item_collected_zoni_cro, ...). RACACIT_DS_ROUTE: comma list of substrings
 * that route, ^ anchoring one to the start (default: wep_* weapons -- the
 * Mag-Net Launcher's sounds are mag_net_*, the Sonic Eruptor's sub_new_* --
 * and their projectiles (proj_pistol_*, proj_ryno_*, proj_rift_*,
 * proj_dynamo_*, proj_buzz_blade_*, proj_xbox_*: the Plasma Striker's),
 * the Zoni, the Aphelion's guns (its fire, missile, kinetic, swarm and plasma
 * sounds; v42-v48 routed every veh_aphelion_* sound, the engine loops too),
 * the comms chime); RACACIT_DS_SKIP: substrings that do not (default:
 * enemies' weapons that carry wep_* names -- the turrets (wep_turret_*, the
 * Valkyries' among them, wep_krell*, wep_pirateturret_*), Vorselon's attacks
 * (wep_vis_*), the Nefarious station's laser (wep_nef_*), thrown enemy bombs
 * (wep_thrown_*); since v52, after the owner heard the controller carry
 * "all sounds" in fights --; the wrench and Clank's staff; Mr. Zurkon's hover
 * hum (a loop for as long as he is out; his lines and shots stay); since v54
 * the loops that only hum -- the time bomb held and active and its ticking
 * (Clank's time bomb hummed on and off through Orvus' lines in Great Clock
 * B, owner v53: "Orvus ... in and out of the controller"; its
 * throw/activate/deactivate stay), the Constructo Bomb's lava pool, the
 * Cryomine's beep/motor/wind -- but not loops that are the firing itself
 * (proj_ryno_rock_loop, a laser's beam loop); a deployed
 * minion's damage and death cries; idle loops of an equipped or deployed
 * weapon).
 * Since v61 the Groovitron's music: its dance tracks are streams named
 * disconapoli_127inf_ab_loop, copkiller_145inf_ab_loop, ... (Axiom's and the
 * global sound tables), followed from the streaming file like _stream sounds
 * ("stick with weapons, their effects, and mr zurkon dialogue, as well as
 * groovitron music", owner, v60). NOT verified in game: no test save has it.
 * Since v50 the player's own hits and explosions route too: the Constructo
 * Pistol's shockwave mod is mostly its blast where the round lands
 * (proj_pistol_explode_dko), and without it the pistol sounded cut off on
 * the controller (owner, v49).
 * v63: a routed sound is played by the controller's own player
 * (tools/ps3recomp/libs/audio/ds_player.c) from the bank's grain script, at
 * one steady level, and ends when the game stops its instance
 * (racacit_snd_stop_hook). v42-v62 marked its samples' addresses and copied
 * whichever synth voices read them (owner, v62: "random noises ... nothing is
 * consistent"). */
extern "C" int  ds_player_enabled(void);
extern "C" void ds_player_scream(uint32_t bank_ea, uint32_t sound, float spk, float felt, uint32_t owner, const char* name);
extern "C" void ds_player_stop(uint32_t owner);
extern "C" void ds_player_bind(uint32_t owner, uint32_t handle, uint32_t snd_globals);
extern "C" void racacit_ds_stream_expect(uint32_t owner, float level, const char* name);   /* ppu_fs.cpp */
extern "C" volatile unsigned long long g_racacit_fire_ms;   /* cellPad.c: fire last pressed (ms) */
extern "C" volatile int g_racacit_fire_held;                /* ...and whether it is down now */
static thread_local int t_ds_cat = 0, t_ds_in = 0;
static thread_local uint32_t t_ds_inst;               /* the sound instance being started (the player's stop key) */
static thread_local char t_ds_name[48];               /* the routed sound being started */
static int racacit_ds_match(const char* name, const char* list)
{
    char tok[64];
    for (const char* p = list; *p; ) {
        const char* q = strchr(p, ',');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (n && n < sizeof tok) {
            memcpy(tok, p, n); tok[n] = 0;
            if (tok[0] == '^' ? !strncmp(name, tok + 1, n - 1) : strstr(name, tok) != nullptr) return 1;   /* ^ = at the start */
        }
        if (!q) break;
        p = q + 1;
    }
    return 0;
}
static int racacit_ds_classify(const char* name)
{
    static const char* route = nullptr; static const char* skip = nullptr;
    if (!route) { const char* e = getenv("RACACIT_DS_ROUTE");
                  route = e ? e : "^wep_,^mag_net_,^sub_new_,^proj_pistol_,^proj_ryno_,^proj_rift_,^proj_dynamo_,"
                                  "^proj_buzz_blade_,^proj_xbox_,zoni,^veh_aphelion_fire,^veh_aphelion_missilefire,"
                                  "^veh_aphelion_kinetic,^veh_aphelion_swarm,^veh_aphelion_plasma,helpdesk,"
                                  "^disconapoli_,^restlessdancer_,^teknokrat_,^chainfree_,^checkeredflag_,^copkiller_,^girlrules_,^metalfuture_,"
                                  "^ui_upgrade_"; }
    if (!skip) { const char* e = getenv("RACACIT_DS_SKIP");
                 skip = e ? e : "wep_turret_,wep_krell,wep_pirateturret_,wep_vis_,wep_nef_,wep_thrown_,mrzurkon_hover,"
                                 "handloop,activeloop,lavaloop,beeploop,motorloop,windloop,alarm_clock,"
                                 "wep_wrench_,wep_clank_,damage,death,idle,flyby,ricochet,debris,"
                                 "proximity_beep,proximitylatch,zoom_amb,_amb_,tracking,connector"; }
    return racacit_ds_match(name, route) && !racacit_ds_match(name, skip) ? 1 : 0;
}
/* The speaker level of a routed sound (v69): one ceiling per kind instead of
 * every sound at 1.0 into the limiter ("a lot of the weapon noises are still
 * overly loud ... there needs to be a clean baseline for how loud certain
 * volumes of weapons can be, with the only exception being zurkon having
 * slightly higher volume", owner, v68). ds_player.c also holds each sample
 * to a fixed loudness first, so these are the levels of equally loud
 * sounds. Since v69 the Constructo Bomb's proximity beep and latch (they
 * repeat for as long as a mine is armed) are not routed at all.
 * RACACIT_DS_LEVEL_SCALE scales them all (default 1). */
static float racacit_ds_level(const char* name)
{
    static float scale = -1.0f;
    if (scale < 0.0f) { const char* e = getenv("RACACIT_DS_LEVEL_SCALE"); scale = e ? (float)atof(e) : 1.0f; }
    static const struct { const char* k; float lv; } t[] = {
        { "zurk", 0.70f },                                       /* Mr. Zurkon: the one above the guns */
        { "beep", 0.15f }, { "latch", 0.15f }, { "equip", 0.15f }, { "_arm", 0.15f }, { "click", 0.15f },
        { "reload", 0.15f }, { "emptied", 0.15f }, { "^wep_gen_", 0.15f }, { "helpdesk", 0.25f },
        /* the Zoni and the weapon level-up popup: rewards, just above a shot
         * (v77; 0.35 / 0.30 since v70 left them at a third to a sixth of a
         * shot's loudness, measured from the bank -- "still not coming
         * through", owner, v76); quiet samples lifted (ds_player.c) */
        { "zoni", 0.60f }, { "^ui_upgrade_", 0.60f },
        { "explo", 0.45f }, { "impact", 0.45f }, { "blast", 0.45f }, { "shockwave", 0.45f }, { "_hit", 0.45f },
        { "inf_ab_loop", 0.40f },                                /* the Groovitron's music */
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (racacit_ds_match(name, t[i].k)) return t[i].lv * scale;
    return 0.55f * scale;                                        /* a gun's shot */
}
static thread_local float t_ds_lvl = 1.0f;
/* Audio haptics (v57): what the DualSense's actuators play -- the player's
 * guns and projectiles, impacts, explosions, Ratchet's landings and hits --
 * category 4 next to the speaker's 1: the controller's player plays them at
 * its actuator level and cellAudio.c low-passes that into channels 3-4.
 * RACACIT_DS_HAPTIC_ROUTE / RACACIT_DS_HAPTIC_SKIP, same list syntax.
 * v61: never characters' or enemy projectiles' sounds (cha_*, hca_*): enemy
 * robots' hits and explosions matched _hit / explo ("ditch the idea of enemy
 * robots coming through the controller", owner, v60). */
static thread_local int t_ds_hcat = 0;
static int racacit_ds_haptic(const char* name)
{
    static const char* route = nullptr; static const char* skip = nullptr;
    if (!route) { const char* e = getenv("RACACIT_DS_HAPTIC_ROUTE");
                  route = e ? e : "^wep_,^mag_net_,^sub_new_,^proj_,^veh_aphelion_fire,^veh_aphelion_missile,^veh_aphelion_kinetic,"
                                  "^veh_aphelion_swarm,^veh_aphelion_plasma,explo,impact,_hit,hit_,land,crash,smash,break"; }
    if (!skip) { const char* e = getenv("RACACIT_DS_HAPTIC_SKIP");
                 skip = e ? e : "wep_turret_,wep_krell,wep_pirateturret_,wep_vis_,wep_nef_,wep_thrown_,mrzurkon_hover,"
                                 "loop,idle,flyby,ricochet,equip,emptied,wep_gen_,wep_wrench_whoosh,_vox,vocal,ui_,^cha_,^hca_,"
                                 "zoom_amb,_amb_,tracking,connector,proximity_beep,proximitylatch"; }
    return racacit_ds_match(name, route) && !racacit_ds_match(name, skip) ? 4 : 0;
}
/* The start returned: the instance holds its SCREAM handle at +0x14 (0 if
 * the sound did not start); SCREAM's globals are the word at r2 - 0x48D8. */
static uint32_t s_ds_snd_globals;
extern "C" void racacit_snd_start_end(void)
{
    if (t_ds_inst && s_ds_snd_globals) ds_player_bind(t_ds_inst, vm_read32(t_ds_inst + 0x14), s_ds_snd_globals);
    t_ds_cat = 0; t_ds_hcat = 0; t_ds_in = 0; t_ds_inst = 0;
}

/* A sound instance stopped (scripts/patch_lift.py #12: func_006DE478 sets the
 * instance's stopping flag and stops its SCREAM sound, func_006DDC58 stops it
 * at once; r3 = the instance): the controller's copy ends with it. */
static void racacit_hb_remove(uint32_t inst);
extern "C" void racacit_snd_stop_hook(ppu_context* ctx, int tag)
{
    (void)tag;
    racacit_hb_remove((uint32_t)ctx->gpr[3]);
    ds_player_stop((uint32_t)ctx->gpr[3]);
}
/* Weapon-aware DualSense triggers (tools/ps3recomp/libs/input/cellPad.c): the
 * equipped weapon id is the word at 0x00E24E78 (the previous one follows it);
 * a weapon's family comes from its fire sounds' names and is remembered per id
 * (seeded with the ids seen on the test save: 17 the Constructo Bomb Glove,
 * 16 the Constructo Pistol, 5 the Sonic Eruptor). */
extern "C" volatile int g_racacit_ds_pad;
extern "C" void racacit_ds_weapon_profile(int p);
extern "C" void racacit_ds_weapon_shot(void);
extern "C" void racacit_ds_weapon_empty(void);
extern "C" void racacit_ds_weapon_shot_left(void);
enum { DSW_MEDIUM, DSW_LIGHT, DSW_HEAVY, DSW_THROW, DSW_BEAM, DSW_DEPLOY };
static int racacit_dsw_family(const char* n)
{
    static const struct { const char* k; int f; } t[] = {
        { "zurkon", DSW_DEPLOY }, { "groovitron", DSW_DEPLOY }, { "chimp", DSW_DEPLOY }, { "dynamo", DSW_DEPLOY }, { "turret", DSW_DEPLOY },
        { "spiral", DSW_BEAM }, { "beam", DSW_BEAM }, { "torch", DSW_BEAM }, { "flame", DSW_BEAM }, { "cryo", DSW_BEAM },
        { "bomb", DSW_THROW }, { "grenade", DSW_THROW }, { "mine", DSW_THROW }, { "spike", DSW_THROW }, { "tesla", DSW_THROW }, { "throw", DSW_THROW },
        { "shotgun", DSW_HEAVY }, { "eruptor", DSW_HEAVY }, { "sonic", DSW_HEAVY }, { "sub_new", DSW_HEAVY }, { "ryno", DSW_HEAVY }, { "launch", DSW_HEAVY },
        { "cannon", DSW_HEAVY }, { "rift", DSW_HEAVY }, { "magnet", DSW_HEAVY }, { "mag_", DSW_HEAVY },
        { "pistol", DSW_LIGHT }, { "blaster", DSW_LIGHT }, { "combust", DSW_LIGHT }, { "rifle", DSW_LIGHT }, { "striker", DSW_LIGHT },
        { "plasma", DSW_LIGHT }, { "laser", DSW_LIGHT }, { "gun", DSW_LIGHT } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (strstr(n, t[i].k)) return t[i].f;
    return -1;
}
static void racacit_dsw_event(const char* name)
{
    static signed char fam[256];
    static int init = 0; static uint32_t last_id = 0xFFFFFFFFu;
    if (!init) { init = 1; memset(fam, -1, sizeof fam); fam[17] = DSW_THROW; fam[16] = DSW_LIGHT; fam[5] = DSW_HEAVY; }
    const uint32_t id = vm_read32(0x00E24E78u);
    if (id != last_id) { last_id = id; racacit_ds_weapon_profile(id < 256 && fam[id] >= 0 ? fam[id] : DSW_MEDIUM); }
    if (!strncmp(name, "veh_aphelion_fire", 17)) {                  /* the ship: blasters on the fire trigger */
        static int ship = 0; if (!ship) { ship = 1; racacit_ds_weapon_profile(DSW_LIGHT); }
        racacit_ds_weapon_shot(); return; }
    if (!strncmp(name, "veh_aphelion_missile", 20)) return;   /* missiles: L1 since v70 (v52-v69: L2, kicked) */
    if (strncmp(name, "wep_", 4) && strncmp(name, "mag_net_", 8) && strncmp(name, "sub_new_", 8)) return;   /* Mag-Net, Sonic Eruptor */
    if (strstr(name, "gen_emptied")) { racacit_ds_weapon_empty(); return; }
    if (!strncmp(name, "wep_gen_", 8)) return;                    /* equip, ammo and the like */
    static const char* const not_shot[] = { "explode", "impact", "_hit", "hit_", "damage", "death", "_vox", "vocal",
                                            "flyby", "ricochet", "debris", "reload", "pickup", "idle", "upgrade",
                                            "equip", "flying", "projectile", "out_of_ammo", "hover" };
    for (unsigned i = 0; i < sizeof not_shot / sizeof not_shot[0]; i++) if (strstr(name, not_shot[i])) return;
    /* The player's shots only: Mr. Zurkon fires on his own and enemies' guns
     * are wep_* too -- in the owner's v54 log Zurkon's shots kicked R2 and
     * flipped the equipped pistol's profile back and forth (5 <-> 1). A shot
     * counts while fire is held or within 250 ms of a press. */
    static const char* const not_mine[] = { "wep_mrzurkon_fire", "wep_turret_", "wep_krell", "wep_pirateturret_",
                                            "wep_vis_", "wep_nef_", "wep_thrown_" };
    for (unsigned i = 0; i < sizeof not_mine / sizeof not_mine[0]; i++) if (!strncmp(name, not_mine[i], strlen(not_mine[i]))) return;
    { const unsigned long long fp = g_racacit_fire_ms, now = ps3_qpc_us() / 1000ull;
      if (!g_racacit_fire_held && !(fp && now - fp < 250ull)) return; }
    const int f = racacit_dsw_family(name);
    if (f >= 0 && id < 256 && fam[id] != f) { fam[id] = (signed char)f; racacit_ds_weapon_profile(f);
        fprintf(stderr, "[ds-trigger] weapon %u plays %s: profile %d%c", id, name, f, 10); }
    racacit_ds_weapon_shot();
}

extern "C" volatile int g_racacit_ctx_hoverboots;   /* cellPad.c */
static uint32_t s_hb_inst[8]; static volatile long s_hb_mu;
static void racacit_hb_add(uint32_t inst)
{
    while (__sync_lock_test_and_set(&s_hb_mu, 1)) { }
    int n = 0, have = 0;
    for (int i = 0; i < 8; i++) { if (s_hb_inst[i] == inst) have = 1; }
    if (!have) for (int i = 0; i < 8; i++) if (!s_hb_inst[i]) { s_hb_inst[i] = inst; break; }
    for (int i = 0; i < 8; i++) n += s_hb_inst[i] != 0;
    __sync_lock_release(&s_hb_mu);
    if (!g_racacit_ctx_hoverboots && n) fprintf(stderr, "[cellPad] hoverboots on: L2 boosts%c", 10);
    g_racacit_ctx_hoverboots = n > 0;
}
static void racacit_hb_remove(uint32_t inst)
{
    while (__sync_lock_test_and_set(&s_hb_mu, 1)) { }
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (s_hb_inst[i] == inst) s_hb_inst[i] = 0;
        if (s_hb_inst[i]) {   /* reused for another sound, or stopped without our hook: gone */
            const uint32_t d = vm_read32(s_hb_inst[i] + 0xC);
            if (!d || d >= 0x40000000u || memcmp(vm_base + d, "gad_hb_", 7) || !vm_read32(s_hb_inst[i] + 0x14)) s_hb_inst[i] = 0;
        }
        n += s_hb_inst[i] != 0;
    }
    __sync_lock_release(&s_hb_mu);
    g_racacit_ctx_hoverboots = n > 0;
}
extern "C" void racacit_snd_play_hook(ppu_context* ctx, int tag)
{
    const int ds_on = ds_player_enabled();
    if (tag == 2 && !ds_on && g_racacit_ds_pad) {        /* triggers only (no speaker): just the weapon events */
        const uint32_t inst = (uint32_t)ctx->gpr[3], desc = vm_read32(inst + 0xC);
        char name[64] = { 0 };
        if (desc && desc < 0x40000000u)
            for (int k = 0; k < 63 && vm_base[desc + k]; k++) name[k] = (char)vm_base[desc + k];
        racacit_dsw_event(name);
    }
    /* Hoverboots on (v72): their loops (gad_hb_super_new_idle / _moving /
     * _thrusting) play for as long as they are, so the instances of gad_hb_*
     * sounds alive say so -- cellPad.c then lets L2 boost (the game's R2;
     * "hoverboot l2", owner). Ended in racacit_snd_stop_hook. */
    if (tag == 2) {
        const uint32_t inst = (uint32_t)ctx->gpr[3], desc = vm_read32(inst + 0xC);
        if (desc && desc < 0x40000000u && !memcmp(vm_base + desc, "gad_hb_", 7)) racacit_hb_add(inst);
        else if (g_racacit_ctx_hoverboots) racacit_hb_remove(0);   /* re-check the ones held */
    }
    /* RACACIT_FRAMELOG_ON_SOUND=<substring> (test hook): a sound with that in
     * its name arms the draw engine's frame log (RSX_ENG_FRAMELOG_GATE) --
     * the frames of an effect that comes with a sound, logged as it plays. */
    { static const char* fs = (const char*)1; static const char* gate;
      if (fs == (const char*)1) { fs = getenv("RACACIT_FRAMELOG_ON_SOUND"); gate = getenv("RSX_ENG_FRAMELOG_GATE"); }
      if (tag == 2 && fs && gate) {
          const uint32_t desc = vm_read32((uint32_t)ctx->gpr[3] + 0xC);
          char nm[64] = { 0 };
          if (desc && desc < 0x40000000u) for (int k = 0; k < 63 && vm_base[desc + k]; k++) nm[k] = (char)vm_base[desc + k];
          if (strstr(nm, fs)) { FILE* g = fopen(gate, "wb"); if (g) fclose(g);
                                fprintf(stderr, "[framelog] armed by sound %s%c", nm, 10); }
      } }
    if (tag == 2 && ds_on) {                             /* instance start: classify by name */
        const uint32_t inst = (uint32_t)ctx->gpr[3], desc = vm_read32(inst + 0xC);
        char name[64] = { 0 };
        if (desc && desc < 0x40000000u)
            for (int k = 0; k < 63 && vm_base[desc + k]; k++) name[k] = (char)vm_base[desc + k];
        t_ds_in = 1; t_ds_inst = inst;
        if (!s_ds_snd_globals) s_ds_snd_globals = vm_read32((uint32_t)ctx->gpr[2] - 0x48D8u);
        t_ds_cat = name[0] ? racacit_ds_classify(name) : 0;
        t_ds_hcat = name[0] ? racacit_ds_haptic(name) : 0;
        if (t_ds_cat || t_ds_hcat) { strncpy(t_ds_name, name, sizeof t_ds_name - 1); t_ds_name[sizeof t_ds_name - 1] = 0; }
        if (t_ds_cat) t_ds_lvl = racacit_ds_level(name);
        {   /* What each gun plays: every sound (but characters' cha_*) that
             * starts within 60 ms of a fire press -- a shot starts within ~10 --
             * once per (equipped weapon, name): a gun whose shots never reach
             * the controller shows up as "stays on the TV". */
            const unsigned long long f = g_racacit_fire_ms, now = ps3_qpc_us() / 1000ull;
            if (f && now - f < 60ull && name[0] && strncmp(name, "amb_", 4) && strncmp(name, "ui_", 3) && strncmp(name, "cha_", 4)) {
                const uint32_t wid = vm_read32(0x00E24E78u);
                static uint32_t said[256]; static unsigned ns; static volatile long mg;
                uint32_t h = wid * 2654435761u; for (const char* q = name; *q; q++) h = h * 31u + (uint8_t)*q;
                while (__sync_lock_test_and_set(&mg, 1)) { }
                int seen = 0; for (unsigned k = 0; k < ns; k++) if (said[k] == h) { seen = 1; break; }
                if (!seen && ns < 256) said[ns++] = h;
                __sync_lock_release(&mg);
                if (!seen && ns < 256)
                    fprintf(stderr, "[ds-gun] weapon %u: %s %llu ms after fire -> %s%c", wid, name, now - f,
                            t_ds_cat ? "controller" : "stays on the TV", 10);
            }
        }
        if (g_racacit_ds_pad) racacit_dsw_event(name);
        if (t_ds_cat) { const size_t L = strlen(name);          /* a streamed sound: its stream's data is read next */
            if ((L > 7 && !strcmp(name + L - 7, "_stream")) || (L > 11 && !strcmp(name + L - 11, "inf_ab_loop")))   /* the Groovitron's tracks (v61) */
                racacit_ds_stream_expect(inst, t_ds_lvl, name); }
        { static int names = -1; if (names < 0) { const char* e = getenv("RACACIT_DS_NAMES"); names = e && *e == '1'; }
          if (names) {                                   /* every distinct sound once (choosing routes) */
              static uint32_t seen[4096]; static unsigned ns; static volatile long mn;
              uint32_t h = 2166136261u; for (const char* q = name; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
              while (__sync_lock_test_and_set(&mn, 1)) { }
              int dup = 0; for (unsigned k = 0; k < ns; k++) if (seen[k] == h) { dup = 1; break; }
              if (!dup && ns < 4096) seen[ns++] = h;
              __sync_lock_release(&mn);
              if (!dup) fprintf(stderr, "[ds-name] %s%s%c", name, t_ds_cat ? "  (routed)" : "", 10);
          } }
        if (!strncmp(name, "wep_", 4) || !strncmp(name, "mag_net_", 8) || !strncmp(name, "sub_new_", 8)) {   /* which weapon makes which sounds */
            const uint32_t wid = vm_read32(0x00E24E78u);   /* equipped weapon id (A Crack in Time) */
            static uint32_t said[512]; static unsigned ns; static volatile long mw;
            uint32_t h = wid * 2654435761u; for (const char* q = name; *q; q++) h = h * 31u + (uint8_t)*q;
            while (__sync_lock_test_and_set(&mw, 1)) { }
            int seen = 0; for (unsigned k = 0; k < ns; k++) if (said[k] == h) { seen = 1; break; }
            if (!seen && ns < 512) said[ns++] = h;
            __sync_lock_release(&mw);
            if (!seen) fprintf(stderr, "[ds-weapon] equipped %u: %s%c", wid, name, 10);
        }
        if (t_ds_cat) {
            static char said[256][40]; static unsigned nsaid; static volatile long mu;
            while (__sync_lock_test_and_set(&mu, 1)) { }
            int seen = 0;
            for (unsigned k = 0; k < nsaid; k++) if (!strncmp(said[k], name, 39)) { seen = 1; break; }
            if (!seen && nsaid < 256) { strncpy(said[nsaid], name, 39); said[nsaid][39] = 0; nsaid++;
                fprintf(stderr, "[ds-route] %s -> controller speaker%c", name, 10); }
            __sync_lock_release(&mu);
        }
    }
    /* SCREAM start (func_00979CF0: r3 bank, r4 sound) inside a routed
     * instance's start: the controller plays that sound itself. */
    if (tag == 3 && ds_on && t_ds_in && (t_ds_cat || t_ds_hcat))
        ds_player_scream((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], t_ds_cat ? t_ds_lvl : 0.0f, t_ds_hcat ? 0.5f : 0.0f,
                         t_ds_inst, t_ds_name);
    static int on = -1;
    if (on < 0) { const char* e = getenv("RACACIT_SND_TRACE"); on = e && *e == '1'; }
    if (!on) return;
    if (tag == 2) {   /* sound instance start: dump the instance and its spec once per (index, spec) */
        const uint32_t inst = (uint32_t)ctx->gpr[3], spec = vm_read32(inst + 0x8);
        const int idx = (int16_t)(vm_read32(inst + 0x4) & 0xFFFF);
        static uint32_t seen[2048]; static unsigned nseen; static volatile long mu2;
        const uint32_t key = (spec << 12) ^ (uint32_t)(idx & 0xFFF);
        while (__sync_lock_test_and_set(&mu2, 1)) { }
        int dup = 0; for (unsigned k = 0; k < nseen; k++) if (seen[k] == key) { dup = 1; break; }
        if (!dup && nseen < 2048) seen[nseen++] = key;
        __sync_lock_release(&mu2);
        if (dup) return;
        uint32_t chn[6]; racacit_guest_chain(ctx, chn, 6);
        fprintf(stderr, "[snd-inst] flip %u idx=%d inst=%08X spec=%08X chain %06X<%06X<%06X<%06X<%06X<%06X |", cellGcm_flip_request_count(),
                idx, inst, spec, chn[0], chn[1], chn[2], chn[3], chn[4], chn[5]);
        for (uint32_t o = 0; o < 0x60; o += 4) fprintf(stderr, " %08X", vm_read32(inst + o));
        fprintf(stderr, " | spec");
        for (uint32_t o = 0; o < 0xA0; o += 4) fprintf(stderr, " %08X", vm_read32(spec + o));
        fputc(10, stderr);
        return;
    }
    struct Ent { uint32_t tag, a4, c0, c1, c2, n; };
    static Ent tab[2048]; static unsigned ntab; static volatile long mu; static unsigned long long t_last;
    uint32_t ch[7];
    ch[0] = (uint32_t)ctx->lr;
    racacit_guest_chain(ctx, ch + 1, 6);
    const uint32_t a4 = (uint32_t)ctx->gpr[4];
    while (__sync_lock_test_and_set(&mu, 1)) { }
    unsigned i = 0;
    for (; i < ntab; i++)
        if (tab[i].tag == (uint32_t)tag && tab[i].a4 == a4 && tab[i].c0 == ch[0] && tab[i].c1 == ch[1] && tab[i].c2 == ch[2]) break;
    if (i == ntab && ntab < 2048) {
        tab[ntab++] = { (uint32_t)tag, a4, ch[0], ch[1], ch[2], 0 };
        fprintf(stderr, "[snd-new] flip %u t%d r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X chain %06X<%06X<%06X<%06X<%06X<%06X<%06X%c",
                cellGcm_flip_request_count(), tag, (uint32_t)ctx->gpr[3], a4, (uint32_t)ctx->gpr[5],
                (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7], ch[0], ch[1], ch[2], ch[3], ch[4], ch[5], ch[6], 10);
    }
    if (i < ntab) tab[i].n++;
    const unsigned long long now = ps3_qpc_us() / 1000;
    if (!t_last) t_last = now;
    if (now - t_last >= 5000) {
        t_last = now;
        fprintf(stderr, "[snd-top] flip %u:", cellGcm_flip_request_count());
        for (int k = 0; k < 16; k++) {
            unsigned b = ntab;
            for (unsigned q = 0; q < ntab; q++) if (tab[q].n && (b == ntab || tab[q].n > tab[b].n)) b = q;
            if (b == ntab) break;
            fprintf(stderr, " t%u/%08X/%06X<%06X x%u", tab[b].tag, tab[b].a4, tab[b].c0, tab[b].c1, tab[b].n);
            tab[b].n = 0;
        }
        for (unsigned q = 0; q < ntab; q++) tab[q].n = 0;
        fputc(10, stderr);
    }
    __sync_lock_release(&mu);
}
