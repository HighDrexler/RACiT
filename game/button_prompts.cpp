/*
 * Button prompts that name the buttons the player presses (Into the Nexus).
 *
 * Ported from RACiT game/button_prompts.cpp (2026-11-11), font part only.
 * With a DualSense in the DualSense Plus layout (or another pad in the swapped
 * layout, cellPad.c pad_swap) the port swaps R1<->R2 and L1<->L2: R2 fires,
 * L2 aims. The game's text still says R1. Its strings carry each button as a
 * private-use character (E007 L1, E008 L2, E00B R1, E00C R2, ...) and its
 * fonts (font.dat, IGHW, 16-byte glyph records in section 0x29400: code point
 * u16, x u16, y u16, page u16, ..., width/height bytes at +10/+11) draw them
 * as pictures. The E007..E00C records are consecutive (+0, +16, ..., +80).
 * While the layout is swapped the L1/L2 and R1/R2 records trade their
 * pictures (everything but the code point), so "R1" text draws R2.
 *
 * Into the Nexus has two such fonts (global_cached.psarc, tuids 243c96e8...
 * and a31bfeab...), the button pictures 54x32 at different places than in A
 * Crack in Time (87x52 on one row):
 *   243c96e8: L1 0x88,0x04  L2 0xC2,0x04  R1 0x04,0x28  R2 0x3E,0x28
 *   a31bfeab: L1 0x04,0x10  L2 0x3E,0x10  R1 0x78,0x10  R2 0xB2,0x10
 * A record's own x/y say which picture it holds, so a re-scan never swaps
 * twice and nothing is remembered across a reload.
 *
 * Not ported: RACiT's HUD pictures (the Kinetic Tether's "R2 + square", the
 * quick select ring's badges) and its letter-font fix -- A Crack in Time's own.
 * The menus are Scaleform (gfxfontlib*.swf); their button pictures are not
 * these records and are not swapped.
 * RACACIT_PROMPT_SWAP=0 turns it off; RACACIT_PROMPT_LAYOUT=<n> forces a layout (tests).
 */
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "win32_compat.h"   /* VirtualQuery, CreateThread, Sleep (the runtime's shim off Windows) */

extern "C" uint8_t* vm_base;
extern "C" volatile int g_racacit_pad_layout;        /* cellPad.c: 0 PS3 layout, 1 PS pad swapped, 2 other pad swapped */
extern "C" volatile int g_racacit_ctx_quickselect;   /* cellPad.c: the quick select ring is open (TRIANGLE held) */

namespace {

struct Layout { uint16_t l1x, l1y, l2x, l2y, r1x, r1y, r2x, r2y; uint8_t w, h; };
const Layout kLayouts[] = {
    { 0x88, 0x04, 0xC2, 0x04, 0x04, 0x28, 0x3E, 0x28, 0x36, 0x20 },   /* font 243c96e8... */
    { 0x04, 0x10, 0x3E, 0x10, 0x78, 0x10, 0xB2, 0x10, 0x36, 0x20 },   /* font a31bfeab... */
};

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* Which picture a record holds: 0 its own (a), 1 its partner's (b), -1 neither. */
int holds(const uint8_t* r, uint16_t ax, uint16_t ay, uint16_t bx, uint16_t by)
{
    const uint16_t x = be16(r + 2), y = be16(r + 4);
    return x == ax && y == ay ? 0 : x == bx && y == by ? 1 : -1;
}

/* A font's E007 record at guest address `ea`, read against layout L. Returns 0
 * not that font (any more), else 1 + (L1/L2 traded) + 2 * (R1/R2 traded). */
int font_state_in(uint32_t ea, const Layout& L)
{
    const uint8_t* r = vm_base + ea;
    if (be16(r) != 0xE007 || be16(r + 16) != 0xE008 || be16(r + 64) != 0xE00B || be16(r + 80) != 0xE00C) return 0;
    for (int k : { 0, 16, 64, 80 }) if (r[k + 10] != L.w || r[k + 11] != L.h) return 0;
    const int l1 = holds(r, L.l1x, L.l1y, L.l2x, L.l2y), l2 = holds(r + 16, L.l2x, L.l2y, L.l1x, L.l1y);
    const int r1 = holds(r + 64, L.r1x, L.r1y, L.r2x, L.r2y), r2 = holds(r + 80, L.r2x, L.r2y, L.r1x, L.r1y);
    if (l1 < 0 || l1 != l2 || r1 < 0 || r1 != r2) return 0;
    return 1 + l1 + 2 * r1;
}

/* A font not in kLayouts (2026-11-18: the owner's vendor / upgrade screens kept
 * the PS3 R1/R2 pictures with DualSense Plus on; only one of the two known fonts
 * was ever found, scanned in the first minute only). Any E007/E008 ... E00B/E00C
 * block whose L1/L2 and R1/R2 pictures are the same size is taken, its pictures
 * as found (unswapped: nothing swaps a font before it is in s_font). */
bool font_layout_new(uint32_t ea, Layout* out)
{
    const uint8_t* r = vm_base + ea;
    if (be16(r) != 0xE007 || be16(r + 16) != 0xE008 || be16(r + 64) != 0xE00B || be16(r + 80) != 0xE00C) return false;
    for (const Layout& L : kLayouts) if (font_state_in(ea, L)) { *out = L; return true; }
    if (r[10] != r[26] || r[11] != r[27] || r[74] != r[90] || r[75] != r[91] || r[10] != r[74] || r[11] != r[75] || !r[10] || !r[11]) return false;
    Layout L = { be16(r + 2), be16(r + 4), be16(r + 18), be16(r + 20), be16(r + 66), be16(r + 68), be16(r + 82), be16(r + 84), r[10], r[11] };
    if ((L.l1x == L.l2x && L.l1y == L.l2y) || (L.r1x == L.r2x && L.r1y == L.r2y)) return false;
    *out = L; return true;
}

void swap_records(uint32_t a, uint32_t b)
{
    uint8_t t[14];
    memcpy(t, vm_base + a + 2, 14);
    memcpy(vm_base + a + 2, vm_base + b + 2, 14);
    memcpy(vm_base + b + 2, t, 14);
}

uint32_t s_font[16];
Layout s_lay[16];
int s_nfont;
int font_state(int i) { return font_state_in(s_font[i], s_lay[i]); }

/* Every committed page of guest memory, 16-byte steps (one VirtualQuery per
 * region: RACiT v75 froze up to 9 s on per-address queries). */
void scan(void)
{
    const unsigned long long t0 = GetTickCount64();
    int n = s_nfont;   /* keep the fonts already followed (their state is known) */
    for (uint32_t a = 0x10000u; a < 0x40000000u; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) break;
        uint32_t end = (uint32_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
        if (end > 0x40000000u || end <= a) end = 0x40000000u;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && end - a >= 96)
            for (uint32_t p = (a + 15u) & ~15u; p + 96 <= end; p += 16) {
                const uint8_t* q = vm_base + p;
                if (q[0] != 0xE0 || q[1] != 0x07 || q[16] != 0xE0 || q[17] != 0x08) continue;
                int dup = 0;
                for (int i = 0; i < n; i++) dup |= s_font[i] == p;
                Layout L;
                if (!dup && n < 16 && font_layout_new(p, &L)) { s_font[n] = p; s_lay[n] = L; n++; }
            }
        a = end;
    }
    if (n != s_nfont) fprintf(stderr, "[prompts] button glyphs found in %d font(s) (scan %llu ms)%c", n, GetTickCount64() - t0, 10);
    s_nfont = n;
}

DWORD WINAPI prompts_thread(LPVOID)
{
    /* The fonts are global (loaded at boot, kept): a few scans while the game
     * starts, never during play. The layout is followed every 50 ms. */
    /* Every 5 s for the whole session (a scan is ~16 ms on this thread): fonts
     * a screen loads later (vendor, upgrades) are found too. */
    unsigned t = 0, last_layout = ~0u, ms = 0;
    for (;; Sleep(50), ms += 50) {
        if (ms >= 1000) { ms = 0; t++; if (t % 5 == 0) scan(); }
        static int force = -2;
        if (force == -2) { const char* e = getenv("RACACIT_PROMPT_LAYOUT"); force = e && *e ? atoi(e) : -1; }
        const int layout = force >= 0 ? force : g_racacit_pad_layout;
        /* R1 <-> R2 with the layout swapped (R2 fires); L1 <-> L2 only while
         * the quick select ring is open, where L2 / R2 page (cellPad.c). */
        const int want_r = layout != 0, want_l = layout != 0 && g_racacit_ctx_quickselect;
        for (int i = 0; i < s_nfont; i++) {
            const int st = font_state(i);
            if (!st) { --s_nfont; s_font[i] = s_font[s_nfont]; s_lay[i] = s_lay[s_nfont]; i--; continue; }   /* memory reused */
            const int has_l = (st - 1) & 1, has_r = ((st - 1) >> 1) & 1;
            if (has_l != want_l) swap_records(s_font[i], s_font[i] + 16);        /* L1 <-> L2 */
            if (has_r != want_r) swap_records(s_font[i] + 64, s_font[i] + 80);   /* R1 <-> R2 */
        }
        if (s_nfont && (unsigned)layout != last_layout) {
            fprintf(stderr, "[prompts] controller layout %d: shoulder buttons in the text %s%c", layout,
                    want_r ? "R1<->R2 (L1<->L2 while quick select is open)" : "as the PS3 layout", 10);
            last_layout = (unsigned)layout;
        }
    }
    return 0;
}

}  // namespace

extern "C" void racacit_prompts_start(void)
{
    const char* e = getenv("RACACIT_PROMPT_SWAP");
    if (e && *e == '0') return;
    CreateThread(NULL, 0, prompts_thread, NULL, 0, NULL);
}
