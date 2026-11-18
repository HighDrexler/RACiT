/*
 * Into the Nexus -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * Started from RACiT's (rac-acit/game/stubs.cpp, v117) with everything keyed to
 * A Crack in Time's addresses left out: sound routing and the DualSense speaker,
 * Lua hooks, movie/cutscene skips, the camera sliders, the collision-record
 * coherence hooks, frame-wait hooks, skins, bonuses, cheats, level fixes and the
 * bolt-trophy rule. Each comes back from RACiT once its Nexus function is found
 * (docs/dev/HANDOFF.md). Never copy an ACiT func_ address in here: in this
 * executable it is some other function.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <thread>   /* std::this_thread::yield (nexus_gpu_idle_get) */

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. The real libsre (cellSpurs) via LLE,
 * when scripts/lift_libsre.py has produced game/prx/. The symbols are weak so
 * the port still builds and runs on HLE SPURS without the (firmware-derived,
 * local-only) lift. RACACIT_HLE_SPURS (main.cpp's default: 1) keeps HLE SPURS.
 * -----------------------------------------------------------------------*/
#include "prx_loader.h"

/* Held title-menu event: see scripts/patch_lift_nexus.py (menu-event). Set and
 * cleared by the two patched lifted functions, on the main thread only. */
uint32_t g_nexus_menu_evt = 0, g_nexus_menu_obj = 0;

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

static void nexus_prx_register(uint32_t guest_addr, void (*host)(void*))
{
    ppu_register_function(guest_addr, host);
}

extern "C" void racacit_discord_start(void);   /* game/discord_rpc.cpp */
extern "C" void racacit_prompts_start(void);   /* game/button_prompts.cpp */

extern "C" void ps3_load_prx_modules(void)
{
    racacit_discord_start();   /* Discord status (Settings: Discord status) */
    racacit_prompts_start();   /* button prompts follow the controller layout (DualSense Plus) */
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
    prx_load_result r = prx_load_module(&m, nexus_prx_register);
    printf("[prx] libsre LLE: ok=%d funcs=%u base=0x%08X size=0x%X exports=%u\n",
           r.ok, r.funcs_registered, r.base, r.image_size, prx_export_registry_count());
}

/* ---------------------------------------------------------------------------
 * Guest setjmp/longjmp (Lua error handling). scripts/patch_lift_nexus.py #5
 * "lua-jmp" rewrites the guest's setjmp call sites into these (RACiT's
 * patch_lift.py #1). A per-thread stack of host jmp_bufs keyed by the
 * 16-byte-aligned guest jmp_buf address. NEXUS_NO_LUA_JMP=1: the lifted longjmp
 * returns to its caller as it did before the patch.
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
    t_jb_top = f->prev;
}

extern "C" void racacit_longjmp(uint32_t guest_buf)
{
    static int off = -1;
    if (off < 0) off = getenv("NEXUS_NO_LUA_JMP") ? 1 : 0;
    static int said = 0;
    uint32_t g = (guest_buf + 15u) & ~15u;
    if (off) return;
    for (RacJbFrame* f = t_jb_top; f; f = f->prev) {
        if (f->guest_buf == g) {
            if (said < 20) { said++;
                fprintf(stderr, "[nexus] guest longjmp to 0x%08X (a Lua error unwinds to its protected call)\n", guest_buf); }
            t_jb_top = f;        /* frames above the target are unwound */
            longjmp(f->env, 1);
        }
    }
    /* No frame: a setjmp site the lift patch does not cover. Returning lets the
     * lifted longjmp go back to its caller, as every longjmp did before the patch. */
    if (said < 20) { said++;
        fprintf(stderr, "[nexus] guest longjmp to 0x%08X with no live setjmp frame\n", guest_buf); }
}

/* ---------------------------------------------------------------------------
 * sceNpDrmIsAvailable2 (sceNp). Unregistered, the HLE layer answers CELL_OK --
 * "licence present" -- for every path, and a title then goes on to use content
 * that is not there. Answer from the VFS view: a disc dump has no .rap
 * licences, so report every such file as unlicensed.
 * -----------------------------------------------------------------------*/
#include "../tools/ps3recomp/runtime/ppu/ppu_context.h"
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint8_t* vm_base;
static void nexus_npdrm_is_available(ppu_context* ctx)
{
    uint32_t p = (uint32_t)ctx->gpr[4];
    char path[512] = {0};
    if (p) { for (int i = 0; i < 511 && vm_base[p + i]; i++) path[i] = (char)vm_base[p + i]; }
    static int n = 0;
    if (n++ < 16) fprintf(stderr, "[sceNpDrm] IsAvailable('%s') -> LICENSE_NOT_FOUND\n", path);
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80029521;   /* SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND */
}
__attribute__((constructor)) static void nexus_register_npdrm(void)
{
    ps3_hle_register_ctx(0xF042B14Fu, "sceNpDrmIsAvailable2", nexus_npdrm_is_available);
}

/* ---------------------------------------------------------------------------
 * Language (RACACIT_LANGUAGE: en, fr, es). The title reads the PS3's system
 * language (cellSysutilGetSystemParamInt, CELL_SYSUTIL_SYSTEMPARAM_ID_LANG);
 * the runtime's answer is always 1, English (US). This handler gives the
 * chosen one instead and answers every other id as the runtime's table does.
 * Registered only for a language other than English. The USA disc is
 * "(En,Fr,Es)"; whether the title accepts more (it ships 17 sound banks) is
 * not checked yet, so only those three are offered.
 * -----------------------------------------------------------------------*/
extern "C" void vm_write32(uint64_t a, uint32_t v);
static uint32_t s_sys_lang = 1;   /* CELL_SYSUTIL_LANG_ENGLISH_US */
static void nexus_get_system_param_int(ppu_context* ctx)
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
    static const struct { const char* code; const char* name; uint32_t id; } langs[] = {
        { "fr", "french", 2 }, { "es", "spanish", 3 },
    };
    for (const auto& lang : langs) {
        if (strcmp(l, lang.code) && strcmp(l, lang.name) && (uint32_t)atoi(l) != lang.id) continue;
        s_sys_lang = lang.id;
        ps3_hle_register_ctx(0x40E895D3u, "cellSysutilGetSystemParamInt", nexus_get_system_param_int);
        fprintf(stderr, "[nexus] language: %s (system language %u)\n", lang.name, s_sys_lang);
        return;
    }
}

/* ---------------------------------------------------------------------------
 * Trophies. The set and the unlocks live in the runtime (libs/network/
 * sceNpTrophy.c). No Nexus-specific rule yet (RACiT's bolt-tier rule was
 * ACiT's trophy ids).
 * -----------------------------------------------------------------------*/
extern "C" void racacit_register_trophies(void)   /* main.cpp, after ppu_hle_init */
{
}

/* ---------------------------------------------------------------------------
 * The main guest thread's context, for main.cpp's stall report (guest stacks
 * when no flip came for 5 s). RACiT set it from a frame-wait hook on ACiT's
 * main loop; Nexus's is not hooked yet, so the report shows the other threads
 * only (ppu_threads_dump_stacks skips a null main context).
 * -----------------------------------------------------------------------*/
extern "C" { ppu_context* volatile g_racacit_main_ctx = nullptr; }

/* ---------------------------------------------------------------------------
 * The current level id, for code mods' level events (mods.cpp). Nexus's own
 * level global is not found; the id is discord_rpc.cpp's, from the level
 * archive the game opens: 0 starting up, 1 main menu, 2 Prison Transport,
 * 3 Meridian City, 4 Silox, 5 Thram, 6 Kragg, 7 Yerek, 8 Netherverse.
 * -----------------------------------------------------------------------*/
uint32_t nexus_level_index();   /* discord_rpc.cpp */
uint32_t racit_current_level()
{
    return nexus_level_index();
}

/* ---------------------------------------------------------------------------
 * The game's clock: two hooks (scripts/patch_lift_nexus.py #6 and #7).
 *
 * The game keeps two frame times. 0xF940EC is the nominal step of its frame-
 * rate mode (1/60 in mode 1, at 0xF91404); the frame end's limiter waits
 * until 0.9 x that since the last frame. 0xF940F8 is a clock the game updates
 * every frame from the timebase (func_0074EFD8): +4 the frame's measured
 * length, +8 its inverse, +0xC the previous one, +0x14 the total. Movement
 * follows the MEASURED one -- checked 2026-11-13 (runs 151/152): Ratchet ran
 * 2 s left at 60 fps and 2 s back at ~40 fps and stopped on the same pixel
 * whether 0xF940EC stayed 1/60 or followed the frame time.
 *
 * #7 clock-smooth (nexus_clock_smooth, at the end of func_0074EFD8, main
 * clock only): on a PS3 the frame starts on a vblank, so the measured length
 * is 16.7 ms frame after frame; here the main thread's frame start wobbles
 * around the paced presents, and the measured length ran 15.0-19.4 ms while
 * every present was 16.7 ms apart -- the world moved unevenly on an even
 * display ("hitches in the timing of the game itself", owner, 0.5). In the 60
 * fps mode the step handed to the game is now 1/60, and the difference to
 * the measured time is carried as a debt: past a 4 ms dead band it is paid
 * into the next steps (at most one extra 1/60 a step, two frames' worth held
 * in all), so real slowdowns still move the game in real time (a steady 22 ms
 * frame gets a 22 ms step) and a lone hitch is spread over two frames instead
 * of one jump. Frames over 250 ms (a load, a pause) are not caught up; other
 * modes are left alone. NEXUS_CLOCK_SMOOTH=0: the game's measured step.
 * With RACACIT_PACE_STATS=1, every 10 s: the measured steps (avg, range,
 * spread) and how many were handed over as exactly 1/60.
 *
 * #6 frame end (nexus_varstep, top of func_007631F8 / its copy func_007631EC):
 * NEXUS_STEP_TEST=<from s>,<to s>,<ms> (diagnostic) makes every frame in that
 * window <ms> longer (busy wait on the main thread; the seconds are
 * PAD_STICK's clock, cellpad_script_seconds) and logs the clock every 2 s.
 * NEXUS_VARSTEP_EC=1 also makes the nominal step 0xF940EC follow the frame
 * time (kept 1/60..1/30, limiter on the game's own value through
 * nexus_varstep_limit) -- off: it did not change movement and what else reads
 * it is not known.
 * -----------------------------------------------------------------------*/
extern "C" unsigned long long ps3_qpc_us(void);
extern "C" double cellpad_script_seconds(void);
static uint32_t s_vs_nominal;    /* the game's own step (bits), as it last wrote it */
static uint32_t s_vs_written;    /* the bits this hook last wrote (0 = none) */
static inline uint32_t nx_rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
extern "C" uint32_t nexus_varstep_limit(uint32_t v)
{
    return (s_vs_written && v == s_vs_written) ? s_vs_nominal : v;
}
extern "C" void nexus_varstep(void)
{
    static int on = -1;
    static double t_from, t_to; static long t_ms;
    static unsigned long long last, rep;
    static double debt;
    static unsigned n_fr, n_str; static double sum_step, max_el;
    const unsigned long long now = ps3_qpc_us();
    if (on < 0) {
        const char* e = getenv("NEXUS_VARSTEP_EC");
        on = e && *e == '1';
        const char* t = getenv("NEXUS_STEP_TEST");
        if (t) sscanf(t, "%lf,%lf,%ld", &t_from, &t_to, &t_ms);
        rep = now;
        if (on || t_ms > 0)
            fprintf(stderr, "[nexus] frame end hook:%s%s\n", on ? " nominal step follows the frame time (NEXUS_VARSTEP_EC=1)" : "",
                    t_ms > 0 ? " step test on" : "");
    }
    double el = last ? (double)(now - last) / 1e6 : 0.0;
    last = now;
    if (t_ms > 0) {                                     /* diagnostic: this frame made longer */
        const double ts = cellpad_script_seconds();
        if (ts >= t_from && ts < t_to) while (ps3_qpc_us() - now < (unsigned long long)t_ms * 1000ull) { }
        /* ... and what the clock does, every 2 s of the test run */
        static unsigned long long dbg; static unsigned calls;
        calls++;
        if (now - dbg >= 2000000ull) {
            float ec, fc; uint32_t b = nx_rd32(0xF940ECu), c = nx_rd32(0xF940FCu);
            memcpy(&ec, &b, 4); memcpy(&fc, &c, 4);
            fprintf(stderr, "[nexus] step test: t=%.1f s, %u frame ends in %.1f s, mode %u, step %.4f, measured %.4f\n",
                    ts, calls, dbg ? (double)(now - dbg) / 1e6 : 0.0, nx_rd32(0x00F91404u), (double)ec, (double)fc);
            dbg = now; calls = 0;
        }
    }
    const uint32_t cur = nx_rd32(0xF940ECu);
    if (cur != s_vs_written) s_vs_nominal = cur;        /* the game set it: start, a mode change */
    float nominal; memcpy(&nominal, &s_vs_nominal, 4);
    const uint32_t mode = nx_rd32(0x00F91404u);
    if (!on || mode != 1u || !(nominal > 0.0160f && nominal < 0.0174f) || el <= 0.0) {
        if (s_vs_written && cur == s_vs_written && cur != s_vs_nominal) vm_write32(0xF940ECu, s_vs_nominal);
        s_vs_written = 0; debt = 0.0;
        return;
    }
    if (el > 0.25) el = nominal;                        /* a load, a pause: nothing to catch up */
    debt += el - (double)nominal;
    if (debt < -(double)nominal) debt = -(double)nominal;
    if (debt > 2.0 * nominal) debt = 2.0 * nominal;
    double extra = 0.0;
    if (debt > 0.001) { extra = debt < nominal ? debt : nominal; debt -= extra; }
    uint32_t bits = s_vs_nominal;
    if (extra > 0.0) { const float st = (float)((double)nominal + extra); memcpy(&bits, &st, 4); }
    if (bits != cur) vm_write32(0xF940ECu, bits);
    s_vs_written = bits;
    n_fr++; sum_step += (double)nominal + extra; if (extra > 0.0) n_str++; if (el > max_el) max_el = el;
    if (now - rep >= 10000000ull) {                     /* every 10 s that had a longer step */
        if (n_str)
            fprintf(stderr, "[nexus] time step: %u frames in 10 s, %u longer than 1/60 (step %.1f ms avg, longest frame %.1f ms)\n",
                    n_fr, n_str, sum_step * 1e3 / n_fr, max_el * 1e3);
        rep = now; n_fr = n_str = 0; sum_step = max_el = 0.0;
    }
}

/* #7 clock-smooth: see the comment above. `base` is func_0074EFD8's clock
 * (r3); only the main game clock at 0xF940F8 is touched. Main thread. */
static inline void nx_wrf(uint32_t a, float f) { uint32_t b; memcpy(&b, &f, 4); vm_write32(a, b); }
extern "C" void nexus_clock_smooth(uint32_t base)
{
    if (base != 0xF940F8u) return;
    static int on = -1, stats = 0;
    static double debt;
    static unsigned long long rep;
    static unsigned n, n_flat; static double s1, s2, lo = 1e9, hi;
    if (on < 0) {
        const char* e = getenv("NEXUS_CLOCK_SMOOTH"); on = !(e && *e == '0');
        const char* p = getenv("RACACIT_PACE_STATS"); stats = p && *p == '1';
        rep = ps3_qpc_us();
        fprintf(stderr, "[nexus] game clock: %s\n", on ? "steps of 1/60 at 60 fps, real time carried over (NEXUS_CLOCK_SMOOTH=0 = the measured steps)"
                                                       : "the game's measured steps (NEXUS_CLOCK_SMOOTH=0)");
    }
    const uint32_t rb = nx_rd32(base + 4u);
    float rawf; memcpy(&rawf, &rb, 4);
    const double raw = (double)rawf, nom = 1.0 / 60.0;
    const int live = nx_rd32(0x00F91404u) == 1u && raw > 0.0 && raw < 0.25;
    if (stats && live) {
        n++; s1 += raw; s2 += raw * raw; if (raw < lo) lo = raw; if (raw > hi) hi = raw;
        const unsigned long long now = ps3_qpc_us();
        if (now - rep >= 10000000ull) {
            const double m = s1 / n, sd = s2 / n - m * m > 0.0 ? __builtin_sqrt(s2 / n - m * m) : 0.0;
            fprintf(stderr, "[nexus] game clock, 10 s: %u frames measured %.2f ms avg (%.2f-%.2f, spread %.2f); %u handed over as 1/60%s\n",
                    n, m * 1e3, lo * 1e3, hi * 1e3, sd * 1e3, n_flat, on ? "" : " (smoothing off)");
            rep = now; n = n_flat = 0; s1 = s2 = hi = 0.0; lo = 1e9;
        }
    }
    if (!on) return;
    if (!live) { debt = 0.0; return; }
    debt += raw - nom;
    if (debt > 2.0 * nom) debt = 2.0 * nom;
    if (debt < -nom) debt = -nom;
    const double band = 0.004;
    double step = nom;
    if (debt > band) { double x = debt - band; if (x > nom) x = nom; step += x; debt -= x; }
    else if (debt < -band) { double x = -band - debt; if (x > 0.5 * nom) x = 0.5 * nom; step -= x; debt += x; }
    if (step == nom) n_flat++;
    nx_wrf(base + 4u, (float)step);
    nx_wrf(base + 8u, (float)(1.0 / step));
}

/* #11 gpu-idle-wait (scripts/patch_lift_nexus.py; 2026-11-16, owner's 0.9.2
 * stadium freeze). At the frame end (func_005F6F78 and the lifter's copy
 * func_005F6F6C, loop at 0x005F7120) the game reads the GCM control block's
 * `get` ONCE and then spins until `put` equals it:
 *     lwz r27,4(r3) ; loop: lwz r4,0(r3) ; cmpw r4,r27 ; bne loop
 * -- the only loop of that shape in the EBOOT (the game's own idle wait at
 * 0x00697E0C re-reads both). On a PS3 `get` is the RSX's fetch pointer and has
 * reached `put` long before this runs; here it is published at the end of each
 * walker pass, so it can still be a few commands short. Then the loop can
 * never end: owner log 2026-11-16 14:30, ~30 s into a Kragg arena challenge,
 * main spinning with r27 = 0x048E5A34 while get had long since reached put
 * 0x048E5A44 ("no guest flip for 5 s", window closed by hand).
 * The patch re-reads `get` every pass, so the loop waits for the walker to
 * catch up, which it does. NEXUS_NO_GPU_IDLE_FIX=1 = the game's loop as it is. */
extern "C" uint32_t nexus_gpu_idle_get(uint32_t ctrl, uint32_t snap)
{
    static int off = -1;
    if (off < 0) { const char* e = getenv("NEXUS_NO_GPU_IDLE_FIX"); off = e && *e == '1'; }
    if (off) return snap;
    const uint32_t put = nx_rd32(ctrl), get = nx_rd32(ctrl + 4u);
    if (snap != put) {
        static uint32_t said_put = ~0u;
        static unsigned long long n;
        if (put != said_put) {
            said_put = put; n++;
            if (n <= 8 || !(n & (n - 1)))
                fprintf(stderr, "[nexus] frame-end GPU wait: get 0x%08X behind put 0x%08X -- waiting for it "
                                "(the game's loop read get once and would have frozen; %llu so far)\n", snap, put, n);
        }
        static unsigned spins;
        if (!(++spins & 255u)) std::this_thread::yield();
    }
    return get;
}
