/*
 * racit_mod.h -- the RACiT code mod API (version 1).
 *
 * The one header a code mod needs. Plain C, no other RACiT header, any
 * Windows x86-64 compiler (MSVC, clang, MinGW). See docs/MODDING.md.
 *
 * A code mod is a DLL in a mod folder, named by mod.json:
 *
 *     { "name": "My mod", "version": "1.0", "code": "my_mod.dll" }
 *
 * It exports two functions:
 *
 *     RACIT_EXPORT uint32_t racit_mod_api_version(void) { return RACIT_API_VERSION; }
 *     RACIT_EXPORT int racit_mod_init(const racit_api* api, racit_mod* self);
 *
 * racit_mod_init runs once, after the game is in memory and before its first
 * instruction runs, on the main thread. Return 0 to stay loaded; anything
 * else unloads the mod (its hooks are removed first) and the game goes on.
 *
 * Mods load lowest priority first, so a mod higher in the launcher's list
 * hooks a function after (outside) the mods below it: its hook runs first and
 * decides whether the others run at all.
 */
#ifndef RACIT_MOD_H
#define RACIT_MOD_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RACIT_API_VERSION 1u

#ifdef __cplusplus
#  define RACIT_EXPORT extern "C" __declspec(dllexport)
#else
#  define RACIT_EXPORT __declspec(dllexport)
#endif

/* The guest (PS3 PPU) register file, as the recompiled game holds it. Only
 * the game creates these: a hook is handed the calling game thread's own.
 * Arguments are in gpr[3..10] (and fpr[1..13]), the result goes in gpr[3]
 * (or fpr[1]), gpr[1] is the guest stack, gpr[2] the TOC. */
typedef struct racit_vr { union { uint8_t b[16]; uint16_t h[8]; uint32_t w[4]; uint64_t d[2]; float f[4]; }; } racit_vr;
typedef struct racit_ppu {
    uint64_t gpr[32];
    double   fpr[32];
#ifdef _MSC_VER
    __declspec(align(16)) racit_vr vr[32];
#else
    racit_vr vr[32] __attribute__((aligned(16)));
#endif
    uint32_t cr;
    uint64_t lr;
    uint64_t ctr;
    uint32_t xer;
    uint32_t fpscr;
    uint32_t vscr;
    uint64_t cia;
    /* more fields follow that belong to the game: never copy this struct */
} racit_ppu;

/* A recompiled game function: one guest function, called with its thread's
 * registers. */
typedef void (*racit_func)(racit_ppu* ctx);

typedef struct racit_mod racit_mod;   /* this mod, as the game knows it */

typedef struct racit_api {
    uint32_t api_version;   /* RACIT_API_VERSION of the game (>= the mod's) */
    uint32_t size;          /* sizeof(racit_api) in the game: newer games append only */
    const char* game_version;   /* e.g. "0.9.9" */

    /* A line in the game's log.txt, prefixed with the mod's id. */
    void (*log)(racit_mod* self, const char* fmt, ...);

    /* The mod's own folder (UTF-8, no trailing slash) and id. */
    const char* (*mod_dir)(racit_mod* self);
    const char* (*mod_id)(racit_mod* self);

    /* A game setting: settings.txt, then mod.json "settings" of the enabled
     * mods (the launcher puts them in the environment). NULL when unset. */
    const char* (*setting)(racit_mod* self, const char* name);

    /* ---- hooks -------------------------------------------------------- */
    /* Make every call of the guest function at guest_addr call fn instead.
     * *original receives what to call for the function's normal behaviour
     * (the next hook in the chain, or the game's own code). Returns 0, or
     * -1 (no function starts there), -2 (that function cannot be hooked:
     * see docs/MODDING.md on hookable functions), -3 (fn already hooks it).
     *
     * Calls the recompiler folded into their caller (small functions, at
     * the call sites it chose) skip the hook: the functions listed in
     * game/mod_hookable.txt are never folded, so every call is seen. */
    int (*hook)(racit_mod* self, uint32_t guest_addr, racit_func fn, racit_func* original);
    /* Remove a hook this mod installed. 0, or -1 if it was not installed. */
    int (*unhook)(racit_mod* self, uint32_t guest_addr, racit_func fn);
    /* The game's own code of a guest function (no hooks), or NULL. Calling it
     * runs that function on ctx's registers: save what you need first. */
    racit_func (*find_function)(uint32_t guest_addr);

    /* ---- guest memory ---------------------------------------------------- */
    /* host pointer = guest_base + guest address. PS3 memory is big-endian:
     * use the racit_rd / racit_wr helpers below. */
    uint8_t* guest_base;
    /* 1 if [addr, addr+len) is mapped memory, 0 if touching it would crash. */
    int (*guest_valid)(uint32_t addr, uint32_t len);

    /* ---- events ---------------------------------------------------------- */
    /* Called once per presented frame, on the frame thread (not a game
     * thread: read memory here, but do not call game functions). */
    int (*on_frame)(racit_mod* self, void (*cb)(uint32_t frame, void* user), void* user);
    /* Called (on the frame thread) when the current level changes. Level ids
     * are the game's own: 0 = loading, 22 = the menus. */
    int (*on_level)(racit_mod* self, void (*cb)(uint32_t old_level, uint32_t new_level, void* user), void* user);
    uint32_t (*current_level)(void);
} racit_api;

/* Big-endian guest memory helpers (check racit_api.guest_valid first for
 * addresses you did not get from the game). */
static inline uint32_t racit_bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
static inline uint8_t  racit_rd8 (const racit_api* a, uint32_t p) { return a->guest_base[p]; }
static inline uint16_t racit_rd16(const racit_api* a, uint32_t p) { const uint8_t* q = a->guest_base + p; return (uint16_t)((q[0] << 8) | q[1]); }
static inline uint32_t racit_rd32(const racit_api* a, uint32_t p) { uint32_t v; memcpy(&v, a->guest_base + p, 4); return racit_bswap32(v); }
static inline float    racit_rdf (const racit_api* a, uint32_t p) { uint32_t v = racit_rd32(a, p); float f; memcpy(&f, &v, 4); return f; }
static inline void racit_wr8 (const racit_api* a, uint32_t p, uint8_t v)  { a->guest_base[p] = v; }
static inline void racit_wr16(const racit_api* a, uint32_t p, uint16_t v) { uint8_t* q = a->guest_base + p; q[0] = (uint8_t)(v >> 8); q[1] = (uint8_t)v; }
static inline void racit_wr32(const racit_api* a, uint32_t p, uint32_t v) { v = racit_bswap32(v); memcpy(a->guest_base + p, &v, 4); }
static inline void racit_wrf (const racit_api* a, uint32_t p, float f)    { uint32_t v; memcpy(&v, &f, 4); racit_wr32(a, p, v); }

#ifdef __cplusplus
}
#endif
#endif /* RACIT_MOD_H */
