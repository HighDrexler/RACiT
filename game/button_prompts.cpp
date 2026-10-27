/*
 * Button prompts that name the buttons the player presses.
 *
 * With a DualSense (or another pad in the swapped layout, cellPad.c pad_swap)
 * the port swaps R1<->R2 and L1<->L2: R2 fires, L2 aims. The game's text
 * still says "R1 to fire". Its strings carry each button as a private-use
 * character (core_text_tags.<lang>.pkg, UTF-8): E001 circle, E002 square,
 * E003 cross, E004 triangle, E005 select, E006 start, E007 L1, E008 L2,
 * E009 L3, E00A left stick, E00B R1, E00C R2, E00D R3, E00E right stick,
 * E00F-E014 d-pad, E015-E01A radar icons. The fonts draw them as pictures:
 * each font.dat (IGHW, glyph records of 16 bytes in section 0x29400: code
 * point u16, x, y, page, ..., width/height bytes at +10/+11) has the four
 * shoulder buttons side by side on one texture row (87x52 each: L1 x 0x3C,
 * L2 x 0x97, R1 x 0xF2, R2 x 0x14D, y 0x74) -- the same atlas layout in every
 * font, only the page differs.
 *
 * So instead of editing every string (and the strings the scripts build),
 * the fonts' L1/L2 and R1/R2 records trade their pictures (everything but
 * the code point) while a swapped layout is active: " to fire" then
 * draws R2. The game uses its loaded font data in place, as it does its
 * other IGHW assets. A thread finds the records in guest memory (a few
 * scans while the game starts and loads levels) and keeps them in step with
 * the layout; a record's own x says which picture it holds, so nothing has
 * to be remembered and a re-scan cannot swap twice.
 *
 * An Xbox-style pad (layout 2) gets the same swap; the fonts have no Xbox
 * glyphs (A/B/X/Y, RB/RT), so it reads R2 where it presses RT.
 * RACACIT_PROMPT_SWAP=0 turns it off; RACACIT_PROMPT_LAYOUT=<n> forces a layout (tests).
 *
 * v69: the prompt that floats over what the Kinetic Tether can grab (an
 * Agorian's exoshield, a Hydra Tank's fuses, the mine cave's pads) is not
 * text but one of the HUD's own pictures: "R2 + [square]", a 183x52 DXT5
 * texture in main memory, listed in the HUD's texture table (32-byte
 * entries: RSX offset, format at +6, width/height at +0x18, pitch at +0x1E)
 * next to the face buttons' pictures -- the HUD has no R1 version ("it says
 * r2+square like on the ps3, however with our dualsense config it SHOULD say
 * R1+square", owner, v68). Its key is the font's R2 picture, pixel for pixel,
 * so while the layout is swapped the key's 22 DXT block columns are
 * replaced by the font's R1 picture (decoded from the font's own page in RSX
 * memory -- the page the glyph records name, from the font's IGHW texture
 * header -- and encoded again), and put back for the PS3 layout.
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
/* VirtualQuery, CreateThread, Sleep: <windows.h> on Windows; off it the
 * runtime's shim gives the same names over /proc/self/maps and pthreads. */
#include "win32_compat.h"

extern "C" uint8_t* vm_base;
extern "C" volatile int g_racacit_pad_layout;   /* cellPad.c racacit_pad_layout at the last poll: 0 PS3 layout, 1 PS pad swapped, 2 other pad swapped */
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);   /* cellGcmSys.c: RSX offset -> guest address */
extern "C" volatile int g_racacit_ctx_quickselect;   /* cellPad.c: the quick select ring is open (TRIANGLE held) */
extern "C" volatile unsigned g_racacit_level_changes;   /* stubs.cpp: level loads so far */

namespace {

const uint16_t kL1x = 0x003C, kL2x = 0x0097, kR1x = 0x00F2, kR2x = 0x014D, kRowY = 0x0074;

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* The E007 record of a font at guest address `ea`: the four shoulder records
 * (E007 at +0, E008 +16, E00B +64, E00C +80) are there, in either state.
 * Returns 0 not a font, else 1 + (L1/L2 traded) + 2 * (R1/R2 traded). */
int font_state(uint32_t ea)
{
    const uint8_t* r = vm_base + ea;
    if (be16(r) != 0xE007 || be16(r + 16) != 0xE008 || be16(r + 64) != 0xE00B || be16(r + 80) != 0xE00C) return 0;
    static const int kRec[4] = { 0, 16, 64, 80 };
    for (int k : kRec)
        if (be16(r + k + 4) != kRowY || r[k + 10] != 0x57 || r[k + 11] != 0x34) return 0;
    const uint16_t l1 = be16(r + 2), l2 = be16(r + 18), r1 = be16(r + 66), r2 = be16(r + 82);
    const int ls = l1 == kL1x && l2 == kL2x ? 0 : l1 == kL2x && l2 == kL1x ? 1 : -1;
    const int rs = r1 == kR1x && r2 == kR2x ? 0 : r1 == kR2x && r2 == kR1x ? 1 : -1;
    return ls < 0 || rs < 0 ? 0 : 1 + ls + 2 * rs;
}

void swap_records(uint32_t a, uint32_t b)
{
    uint8_t t[14];
    memcpy(t, vm_base + a + 2, 14);
    memcpy(vm_base + a + 2, vm_base + b + 2, 14);
    memcpy(vm_base + b + 2, t, 14);
}

uint32_t s_font[16];
int s_nfont;

/* ---- the HUD's "R2 + square" picture ----------------------------------- */

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Whether guest [ea, ea + n) is committed and readable. */
bool readable(uint32_t ea, uint32_t n)
{
    for (uint64_t a = ea; a < (uint64_t)ea + n; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        a = (uint64_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
    }
    return true;
}

/* Where the readable, committed memory that holds `ea` ends (0: none). One
 * VirtualQuery per region, not one per address: the scans' per-16-byte
 * readable() calls (v73) took the process's address-space lock ~100,000
 * times a scan, and the write-watch poll (GetWriteWatch, rsx_writewatch.c)
 * waited on it -- the game froze up to 9 s (owner, v75). */
uint32_t readable_end(uint32_t ea)
{
    uint64_t a = ea;
    for (;;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return a == ea ? 0u : (uint32_t)a;
        a = (uint64_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
        if (a >= 0x100000000ull) return 0xFFFFFFFFu;
        if (a - ea >= 0x20000u) return (uint32_t)a;   /* far enough for every caller */
    }
}

void dxt5_decode(const uint8_t* b, uint8_t px[16][4])
{
    const int a0 = b[0], a1 = b[1];
    int al[8] = { a0, a1 };
    if (a0 > a1) for (int i = 1; i < 7; i++) al[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; i++) al[i + 1] = ((5 - i) * a0 + i * a1) / 5; al[6] = 0; al[7] = 255; }
    uint64_t ab = 0; for (int i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
    const int c0 = b[8] | b[9] << 8, c1 = b[10] | b[11] << 8;
    int cl[4][3] = { { (c0 >> 11) * 255 / 31, ((c0 >> 5) & 63) * 255 / 63, (c0 & 31) * 255 / 31 },
                     { (c1 >> 11) * 255 / 31, ((c1 >> 5) & 63) * 255 / 63, (c1 & 31) * 255 / 31 } };
    for (int k = 0; k < 3; k++) { cl[2][k] = (2 * cl[0][k] + cl[1][k]) / 3; cl[3][k] = (cl[0][k] + 2 * cl[1][k]) / 3; }
    const uint32_t ci = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
    for (int i = 0; i < 16; i++) {
        const int* c = cl[(ci >> (2 * i)) & 3];
        px[i][0] = (uint8_t)c[0]; px[i][1] = (uint8_t)c[1]; px[i][2] = (uint8_t)c[2];
        px[i][3] = (uint8_t)al[(ab >> (3 * i)) & 7];
    }
}

/* Endpoints from the block's extremes (the pictures are black and white
 * with grey edges), nearest palette entry per pixel. */
void dxt5_encode(const uint8_t px[16][4], uint8_t* b)
{
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) { if (px[i][3] < amin) amin = px[i][3]; if (px[i][3] > amax) amax = px[i][3]; }
    b[0] = (uint8_t)amax; b[1] = (uint8_t)amin;
    int al[8] = { amax, amin };
    for (int i = 1; i < 7; i++) al[i + 1] = ((7 - i) * amax + i * amin) / 7;
    uint64_t ab = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        if (amax > amin) for (int k = 1; k < 8; k++) if (abs(al[k] - px[i][3]) < abs(al[best] - px[i][3])) best = k;
        ab |= (uint64_t)best << (3 * i);
    }
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(ab >> (8 * i));
    /* colour: the two most distant visible pixels (any pixel if none is) */
    int lo = -1, hi = -1, dmax = -1;
    for (int i = 0; i < 16; i++) for (int j = i; j < 16; j++) {
        if ((px[i][3] == 0 || px[j][3] == 0) && amax > 0) continue;
        const int d = (px[i][0] - px[j][0]) * (px[i][0] - px[j][0]) + (px[i][1] - px[j][1]) * (px[i][1] - px[j][1]) +
                      (px[i][2] - px[j][2]) * (px[i][2] - px[j][2]);
        if (d > dmax) { dmax = d; lo = i; hi = j; }
    }
    if (lo < 0) lo = hi = 0;
    const int c0 = (px[hi][0] >> 3) << 11 | (px[hi][1] >> 2) << 5 | px[hi][2] >> 3;
    const int c1 = (px[lo][0] >> 3) << 11 | (px[lo][1] >> 2) << 5 | px[lo][2] >> 3;
    b[8] = (uint8_t)c0; b[9] = (uint8_t)(c0 >> 8); b[10] = (uint8_t)c1; b[11] = (uint8_t)(c1 >> 8);
    int cl[4][3] = { { (c0 >> 11) * 255 / 31, ((c0 >> 5) & 63) * 255 / 63, (c0 & 31) * 255 / 31 },
                     { (c1 >> 11) * 255 / 31, ((c1 >> 5) & 63) * 255 / 63, (c1 & 31) * 255 / 31 } };
    for (int k = 0; k < 3; k++) { cl[2][k] = (2 * cl[0][k] + cl[1][k]) / 3; cl[3][k] = (cl[0][k] + 2 * cl[1][k]) / 3; }
    uint32_t ci = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) {
            const int d = (cl[k][0] - px[i][0]) * (cl[k][0] - px[i][0]) + (cl[k][1] - px[i][1]) * (cl[k][1] - px[i][1]) +
                          (cl[k][2] - px[i][2]) * (cl[k][2] - px[i][2]);
            if (d < bd) { bd = d; best = k; }
        }
        ci |= (uint32_t)best << (2 * i);
    }
    b[12] = (uint8_t)ci; b[13] = (uint8_t)(ci >> 8); b[14] = (uint8_t)(ci >> 16); b[15] = (uint8_t)(ci >> 24);
}

/* A pixel of a DXT5 picture `pitch` bytes per block row. */
void dxt5_pixel(const uint8_t* tex, uint32_t pitch, int x, int y, uint8_t out[4])
{
    uint8_t px[16][4];
    dxt5_decode(tex + (uint32_t)(y / 4) * pitch + (uint32_t)(x / 4) * 16u, px);
    memcpy(out, px[(y & 3) * 4 + (x & 3)], 4);
}

const int kKeyW = 87, kKeyH = 52, kKeyCols = 22, kHudW = 183, kHudPitch = 736;
const int kHudRows = (kKeyH + 3) / 4, kKeyBytes = kHudRows * kKeyCols * 16;

/* The font's page holding the shoulder pictures (page `page` of the IGHW
 * texture header the game loads right after the font's records: section
 * 0x5200, 32-byte entries, RSX local offset at +0, format at +6, width
 * at +0x18). Host pointer to a DXT5 page and its pitch, or NULL. */
const uint8_t* font_page(uint32_t rec, uint32_t* pitch)
{
    const int page = be16(vm_base + rec + 64 + 6);   /* the E00B record's page */
    const uint32_t a0 = (rec + 255u) & ~255u, re = readable_end(a0);
    for (uint32_t a = a0, end = rec + 0x10000u; a < end; a += 16) {
        if (a + 0x30u > re) return nullptr;
        const uint8_t* h = vm_base + a;
        if (be32(h) != 0x49474857u || be32(h + 0x20) != 0x5200u) continue;   /* "IGHW", textures */
        const uint32_t n = be32(h + 0x28) & 0xFFFFu, e = a + be32(h + 0x24) + 32u * (uint32_t)page;
        if ((uint32_t)page >= n || !readable(e, 32)) return nullptr;
        const uint8_t* t = vm_base + e;
        const int w = be16(t + 0x18), hh = be16(t + 0x1A);
        if (t[6] != 0x88 || w < kR2x + kKeyW || hh < kRowY + kKeyH) return nullptr;   /* DXT5, holds the row */
        const uint32_t ea = cellGcmResolveLocated(1, be32(t));
        *pitch = (uint32_t)w / 4u * 16u;
        if (!ea || !readable(ea, *pitch * (uint32_t)(hh / 4))) return nullptr;
        return vm_base + ea;
    }
    return nullptr;
}

struct {
    uint32_t entry, ea;                 /* the table entry; the picture's guest address */
    uint8_t  orig[kKeyBytes], mine[kKeyBytes];
    int      have;                      /* orig/mine hold this picture's blocks */
} s_hud;

void hud_blocks(const uint8_t* tex, uint8_t* out)
{
    for (int r = 0; r < kHudRows; r++) memcpy(out + r * kKeyCols * 16, tex + r * kHudPitch, kKeyCols * 16);
}

/* Keep the picture in step with the layout (want: 1 = R1 drawn in). */
void hud_sync(int want)
{
    if (!s_hud.ea || !s_nfont) return;
    if (!readable(s_hud.entry, 32) || !readable(s_hud.ea, kHudRows * kHudPitch)) { s_hud.ea = 0; return; }
    const uint8_t* t = vm_base + s_hud.entry;
    if (be16(t + 0x18) != kHudW || be16(t + 0x1A) != kKeyH || t[6] != 0xA8) { s_hud.ea = 0; return; }   /* memory reused */
    uint8_t* tex = vm_base + s_hud.ea;
    uint8_t cur[kKeyBytes];
    hud_blocks(tex, cur);
    const bool is_mine = s_hud.have && !memcmp(cur, s_hud.mine, sizeof cur);
    bool is_orig = s_hud.have && !memcmp(cur, s_hud.orig, sizeof cur);
    if (!is_mine && !is_orig) {         /* first sight, or loaded again: is it the R2 key? */
        uint32_t pitch = 0;
        const uint8_t* pg = font_page(s_font[0], &pitch);
        if (!pg) return;
        long err = 0;
        for (int y = 0; y < kKeyH; y++)
            for (int x = 0; x < kKeyW - 3; x++) {
                uint8_t a[4], b[4];
                dxt5_pixel(pg, pitch, kR2x + x, kRowY + y, a);
                dxt5_pixel(tex, kHudPitch, x, y, b);
                for (int k = 0; k < 4; k++) err += abs(a[k] - b[k]);
            }
        if (err > 40L * kKeyH * (kKeyW - 3)) {
            fprintf(stderr, "[prompts] HUD picture at %08X: not the font's R2 (difference %ld), left alone%c", s_hud.ea,
                    err / (kKeyH * (kKeyW - 3)), 10);
            s_hud.ea = 0; return; }
        memcpy(s_hud.orig, cur, sizeof cur);
        for (int r = 0; r < kHudRows; r++)
            for (int c = 0; c < kKeyCols; c++) {
                uint8_t px[16][4];
                for (int i = 0; i < 16; i++) {
                    const int x = c * 4 + (i & 3), y = r * 4 + (i >> 2);
                    if (x < kKeyW && y < kKeyH) dxt5_pixel(pg, pitch, kR1x + x, kRowY + y, px[i]);
                    else dxt5_pixel(tex, kHudPitch, x, y, px[i]);
                }
                dxt5_encode(px, s_hud.mine + (r * kKeyCols + c) * 16);
            }
        s_hud.have = 1; is_orig = true;
    }
    const uint8_t* put = want && is_orig ? s_hud.mine : !want && is_mine ? s_hud.orig : nullptr;
    if (!put) return;
    for (int r = 0; r < kHudRows; r++) memcpy(tex + r * kHudPitch, put + r * kKeyCols * 16, kKeyCols * 16);
    fprintf(stderr, "[prompts] HUD picture \"R2 + square\" at %08X: %s%c", s_hud.ea, want ? "R1 drawn in" : "R2 put back", 10);
}

/* ---- the quick select ring's "L1" / "R1" badges -------------------------
 * Beside the ring sit two HUD pictures, 48x44 DXT5 in RSX local memory,
 * listed in the HUD's texture table like the "R2 + square" one (not text:
 * the ring's two-glyph text draws are the hint's "or"). The HUD has no
 * L2/R2 versions; with the layout swapped the ring pages with L2 / R2, so
 * each badge's "1" is redrawn as a "2" taken from the game's own font page
 * (the '2' glyph record of the in-game letter font), and put back for the
 * PS3 layout. */

struct Glyph { uint32_t page; int pw, ph, fmt, x, y, w, h; };
Glyph s_two;                            /* the font's '2'; page 0 = not found */

/* The font whose records hold '1', '2', '3' from `p`: its '2' glyph and page,
 * from the IGHW texture header that follows the records (font_page). The
 * tallest such glyph is kept (the badge digit is scaled down from it). */
static int s_lf_calls;
void letter_font(uint32_t p)
{
    s_lf_calls++;
    const uint8_t* two = vm_base + p + 16;
    if (!two[10] || !two[11]) return;
    const int page = be16(two + 6);
    const uint32_t a0 = (p + 255u) & ~255u, re = readable_end(a0);
    for (uint32_t a = a0, end = p + 0x10000u; a < end; a += 16) {
        if (a + 0x30u > re) return;
        const uint8_t* h = vm_base + a;
        if (be32(h) != 0x49474857u || be32(h + 0x20) != 0x5200u) continue;   /* "IGHW", textures */
        const uint32_t n = be32(h + 0x28) & 0xFFFFu, e = a + be32(h + 0x24) + 32u * (uint32_t)page;
        if ((uint32_t)page >= n || !readable(e, 32)) return;
        const uint8_t* t = vm_base + e;
        const Glyph g = { be32(t), be16(t + 0x18), be16(t + 0x1A), t[6] & 0x9F, be16(two + 2), be16(two + 4), two[10], two[11] };
        if ((g.fmt != 0x86 && g.fmt != 0x88) || g.x + g.w > g.pw || g.y + g.h > g.ph || g.h <= s_two.h) return;
        if (!cellGcmResolveLocated(1, g.page)) return;
        s_two = g;
        fprintf(stderr, "[prompts] font '2' for the ring badges: page %08X (%dx%d fmt %02X) at %d,%d %dx%d%c",
                g.page, g.pw, g.ph, g.fmt, g.x, g.y, g.w, g.h, 10);
        return;
    }
}

void dxt1_decode(const uint8_t* b, uint8_t px[16][4])
{
    const int c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    int cl[4][4] = { { (c0 >> 11) * 255 / 31, ((c0 >> 5) & 63) * 255 / 63, (c0 & 31) * 255 / 31, 255 },
                     { (c1 >> 11) * 255 / 31, ((c1 >> 5) & 63) * 255 / 63, (c1 & 31) * 255 / 31, 255 } };
    for (int k = 0; k < 3; k++) {
        if (c0 > c1) { cl[2][k] = (2 * cl[0][k] + cl[1][k]) / 3; cl[3][k] = (cl[0][k] + 2 * cl[1][k]) / 3; }
        else { cl[2][k] = (cl[0][k] + cl[1][k]) / 2; cl[3][k] = 0; }
    }
    cl[2][3] = 255; cl[3][3] = c0 > c1 ? 255 : 0;
    const uint32_t ci = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) for (int k = 0; k < 4; k++) px[i][k] = (uint8_t)cl[(ci >> (2 * i)) & 3][k];
}

/* The '2' glyph's coverage at page texel (x, y): alpha times brightness
 * (white letters). */
float glyph_cov(const uint8_t* pg, int x, int y)
{
    const Glyph& g = s_two;
    if (x < g.x || y < g.y || x >= g.x + g.w || y >= g.y + g.h) return 0.0f;
    const uint32_t bb = g.fmt == 0x86 ? 8u : 16u, pitch = (uint32_t)g.pw / 4u * bb;
    const uint8_t* blk = pg + (uint32_t)(y / 4) * pitch + (uint32_t)(x / 4) * bb;
    uint8_t px[16][4];
    if (g.fmt == 0x86) dxt1_decode(blk, px); else dxt5_decode(blk, px);
    const uint8_t* q = px[(y & 3) * 4 + (x & 3)];
    return (float)q[3] / 255.0f * (float)(q[0] + q[1] + q[2]) / 765.0f;
}

const int kBadgeW = 48, kBadgeH = 44, kBadgePitch = 192, kBadgeBytes = 11 * kBadgePitch;

struct Badge {
    uint32_t entry, ea;
    uint8_t  orig[kBadgeBytes], mine[kBadgeBytes];
    int      have;                      /* 1: orig/mine hold this picture */
};
Badge s_badge[2];

/* The badge with its "1" as a "2": false when it does not look like a
 * two-character badge or the font's '2' is not known. */
bool badge_redraw(const uint8_t* tex, uint8_t* out)
{
    if (!s_two.page) return false;
    const uint32_t pea = cellGcmResolveLocated(1, s_two.page);
    const uint32_t pbytes = (uint32_t)s_two.ph / 4u * ((uint32_t)s_two.pw / 4u * (s_two.fmt == 0x86 ? 8u : 16u));
    if (!pea || !readable(pea, pbytes)) return false;
    const uint8_t* pg = vm_base + pea;
    static uint8_t img[kBadgeH][kBadgeW][4];
    for (int by = 0; by < kBadgeH / 4; by++)
        for (int bx = 0; bx < kBadgeW / 4; bx++) {
            uint8_t px[16][4];
            dxt5_decode(tex + by * kBadgePitch + bx * 16, px);
            for (int i = 0; i < 16; i++) memcpy(img[by * 4 + (i >> 2)][bx * 4 + (i & 3)], px[i], 4);
        }
    /* The characters: bright, opaque pixels; the "1" is the rightmost run of
     * columns that hold any, the letter the run before it. */
    auto ink = [&](int x, int y) { const uint8_t* q = img[y][x]; return q[3] > 128 && q[0] + q[1] + q[2] > 3 * 170; };
    int col[kBadgeW] = {};
    for (int x = 0; x < kBadgeW; x++) for (int y = 0; y < kBadgeH; y++) col[x] += ink(x, y);
    int x1 = kBadgeW - 1; while (x1 > 0 && !col[x1]) x1--;
    int x0 = x1; while (x0 > 0 && col[x0 - 1]) x0--;
    int lx = x0 - 1; while (lx > 0 && !col[lx]) lx--;
    if (x1 - x0 < 1 || lx <= 0 || x0 - lx < 2) return false;
    int y0 = kBadgeH, y1 = -1;
    for (int y = 0; y < kBadgeH; y++) for (int x = x0; x <= x1; x++) if (ink(x, y)) { if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (y1 - y0 < 5) return false;
    /* The colours: the text's fill (the brightest pixel of the characters),
     * its outline (the darkest opaque pixel round the "1") and, column by
     * column, the badge face (sampled clear of the characters, above them). */
    uint8_t fg[4] = { 0, 0, 0, 0 }, ol[4] = { 255, 255, 255, 0 };
    for (int y = y0 - 3; y <= y1 + 3; y++)
        for (int x = x0 - 3; x <= x1 + 3; x++) {
            if (y < 0 || y >= kBadgeH || x <= lx || x >= kBadgeW) continue;
            const uint8_t* q = img[y][x];
            if (q[0] + q[1] + q[2] > fg[0] + fg[1] + fg[2]) memcpy(fg, q, 4);
            if (q[3] > 128 && q[0] + q[1] + q[2] < ol[0] + ol[1] + ol[2]) memcpy(ol, q, 4);
        }
    if (!ol[3]) return false;
    const int ys = y0 - 5 >= 0 ? y0 - 5 : (y1 + 5 < kBadgeH ? y1 + 5 : -1);
    if (ys < 0) return false;
    /* The glyph's inked box on the page. */
    int gx0 = s_two.x + s_two.w, gx1 = s_two.x - 1, gy0 = s_two.y + s_two.h, gy1 = s_two.y - 1;
    for (int y = s_two.y; y < s_two.y + s_two.h; y++)
        for (int x = s_two.x; x < s_two.x + s_two.w; x++)
            if (glyph_cov(pg, x, y) > 0.5f) { if (x < gx0) gx0 = x; if (x > gx1) gx1 = x; if (y < gy0) gy0 = y; if (y > gy1) gy1 = y; }
    if (gx1 <= gx0 || gy1 <= gy0) return false;
    /* The "2" starts where the "1" did, so the gap after the letter stays the
     * HUD's own: a "2" is wider than a "1", and centred on it, it ran into the
     * "L" and was clipped there (owner, v81: "overlapping ... sloppy"). Too wide
     * for the face to its right edge, it is narrowed to fit. */
    const float sc = (float)(gy1 - gy0 + 1) / (float)(y1 - y0 + 1);
    int fr = x1;   /* the face's last opaque column in the digit's rows */
    for (int y = y0; y <= y1; y++)
        for (int x = x1; x < kBadgeW; x++) if (img[y][x][3] > 128 && x > fr) fr = x;
    const int OL = 2;   /* outline width, as the HUD's own characters */
    const float left = (float)x0, room = (float)(fr - 1 - OL) - left + 1.0f;
    float w = (float)(gx1 - gx0 + 1) / sc;
    if (w > room && room > 2.0f) w = room;
    const float scx = (float)(gx1 - gx0 + 1) / w;
    /* Coverage of the "2" over the area it and its outline take. */
    const int ex0 = x0 - 3 > lx + OL + 1 ? x0 - 3 : lx + OL + 1, ex1 = std::min(kBadgeW - 1, (int)ceilf(left + w) + OL + 1);
    static float cov[kBadgeH][kBadgeW];
    for (int y = 0; y < kBadgeH; y++) for (int x = 0; x < kBadgeW; x++) cov[y][x] = 0.0f;
    for (int y = y0; y <= y1; y++)
        for (int x = (int)floorf(left); x <= (int)ceilf(left + w) && x < kBadgeW; x++) {
            float c = 0.0f;   /* 3x3 samples per pixel */
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++)
                    c += glyph_cov(pg, (int)(gx0 + (x - left + (sx + 0.5f) / 3.0f) * scx),
                                   (int)(gy0 + (y - y0 + (sy + 0.5f) / 3.0f) * sc));
            cov[y][x] = c / 9.0f;
        }
    /* Erase the "1" (face colour from the clear row), then outline, then fill. */
    for (int y = y0 - 3; y <= y1 + 3; y++)
        for (int x = ex0; x <= ex1; x++) {
            if (y < 0 || y >= kBadgeH) continue;
            uint8_t* q = img[y][x];
            memcpy(q, img[ys][x], 4);
            float o = 0.0f;
            for (int dy = -OL; dy <= OL; dy++)
                for (int dx = -OL; dx <= OL; dx++) {
                    const int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && yy < kBadgeH && xx >= 0 && xx < kBadgeW && cov[yy][xx] > o) o = cov[yy][xx];
                }
            o = std::min(1.0f, o * 1.5f);
            for (int k = 0; k < 4; k++) q[k] = (uint8_t)(q[k] + (ol[k] - q[k]) * o + 0.5f);
            const float c = cov[y][x];
            for (int k = 0; k < 4; k++) q[k] = (uint8_t)(q[k] + (fg[k] - q[k]) * c + 0.5f);
        }
    for (int by = 0; by < kBadgeH / 4; by++)
        for (int bx = 0; bx < kBadgeW / 4; bx++) {
            uint8_t px[16][4];
            for (int i = 0; i < 16; i++) memcpy(px[i], img[by * 4 + (i >> 2)][bx * 4 + (i & 3)], 4);
            dxt5_encode(px, out + by * kBadgePitch + bx * 16);
        }
    return true;
}

/* Keep both badges in step with the layout (want: 1 = "2" drawn in). */
/* RACACIT_BADGE_DUMP=<dir>: each badge as found and as redrawn, side by side
 * at 4x (badge_<ea>.ppm) -- to look at the "2" without opening the ring. */
void badge_dump(uint32_t ea, const uint8_t* orig, const uint8_t* mine)
{
    const char* dir = getenv("RACACIT_BADGE_DUMP");
    if (!dir || !*dir) return;
    char path[512]; snprintf(path, sizeof path, "%s/badge_%08X.ppm", dir, ea);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const int S = 4, W = kBadgeW * 2 * S + S, H = kBadgeH * S;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    static uint8_t img[2][kBadgeH][kBadgeW][4];
    for (int k = 0; k < 2; k++) {
        const uint8_t* t = k ? (mine ? mine : orig) : orig;
        for (int by = 0; by < kBadgeH / 4; by++)
            for (int bx = 0; bx < kBadgeW / 4; bx++) {
                uint8_t px[16][4];
                dxt5_decode(t + by * kBadgePitch + bx * 16, px);
                for (int i = 0; i < 16; i++) memcpy(img[k][by * 4 + (i >> 2)][bx * 4 + (i & 3)], px[i], 4);
            }
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int k = x >= kBadgeW * S + S, xx = (k ? x - kBadgeW * S - S : x) / S, yy = y / S;
            uint8_t c[3] = { 40, 40, 40 };   /* grey behind, so alpha shows */
            if (xx < kBadgeW && (k || x < kBadgeW * S)) {
                const uint8_t* q = img[k][yy][xx];
                for (int i = 0; i < 3; i++) c[i] = (uint8_t)((q[i] * q[3] + c[i] * (255 - q[3])) / 255);
            }
            fwrite(c, 1, 3, f);
        }
    fclose(f);
}

void badge_sync(int want)
{
    for (Badge& b : s_badge) {
        if (!b.ea) continue;
        if (!readable(b.entry, 32) || !readable(b.ea, kBadgeBytes)) { b.ea = 0; b.have = 0; continue; }
        const uint8_t* t = vm_base + b.entry;
        if (be16(t + 0x18) != kBadgeW || be16(t + 0x1A) != kBadgeH || t[6] != 0xA8) { b.ea = 0; b.have = 0; continue; }
        uint8_t* tex = vm_base + b.ea;
        const bool is_mine = b.have && !memcmp(tex, b.mine, kBadgeBytes);
        bool is_orig = b.have && !memcmp(tex, b.orig, kBadgeBytes);
        if (!is_mine && !is_orig) {     /* first sight, or loaded again */
            memcpy(b.orig, tex, kBadgeBytes);
            const bool ok = badge_redraw(tex, b.mine);
            fprintf(stderr, "[prompts] quick select badge at %08X: \"2\" %s%c", b.ea, ok ? "made" : "NOT made (no two-character badge or no font '2')", 10);
            badge_dump(b.ea, b.orig, ok ? b.mine : nullptr);
            if (!ok) { b.have = 0; continue; }
            b.have = 1; is_orig = true;
        }
        const uint8_t* put = want && is_orig ? b.mine : !want && is_mine ? b.orig : nullptr;
        if (!put) continue;
        memcpy(tex, put, kBadgeBytes);
        fprintf(stderr, "[prompts] quick select badge at %08X: %s%c", b.ea, want ? "\"2\" drawn in" : "put back", 10);
    }
}

/* Every committed page of guest memory, 16-byte steps: the records sit at
 * 16-byte offsets in a font.dat that the game loads 16-byte aligned. */
void scan(void)
{
    const unsigned long long t0 = GetTickCount64(); s_lf_calls = 0;
    int n = 0, nb = 0;
    uint32_t hud = 0, badges[2] = {};
    for (uint32_t a = 0x10000u; a < 0x40000000u; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) break;
        uint32_t end = (uint32_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
        if (end > 0x40000000u || end <= a) end = 0x40000000u;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && end - a >= 96)
            for (uint32_t p = (a + 15u) & ~15u; p + 96 <= end; p += 16) {
                const uint8_t* q = vm_base + p;
                if (q[0x19] == 0xB7 && q[0x1B] == 0x34 && q[0x18] == 0 && q[0x1A] == 0 && q[6] == 0xA8 &&
                    be16(q + 0x1E) == kHudPitch && !hud) hud = p;   /* the HUD's "R2 + square" entry */
                if (q[0] == 0 && q[1] == '1' && q[16] == 0 && q[17] == '2' && q[32] == 0 && q[33] == '3') letter_font(p);
                if (q[0x18] == 0 && q[0x19] == kBadgeW && q[0x1A] == 0 && q[0x1B] == kBadgeH && q[6] == 0xA8 &&
                    be16(q + 0x1E) == kBadgePitch && nb < 2) badges[nb++] = p;   /* the ring's two badges */
                if (q[0] != 0xE0 || q[1] != 0x07 || q[4] != 0x00 || q[5] != 0x74) continue;
                if (font_state(p) && n < 16) {
                    int dup = 0;
                    for (int i = 0; i < n; i++) dup |= s_font[i] == p;
                    if (!dup) s_font[n++] = p;
                }
            }
        a = end;
    }
    fprintf(stderr, "[prompts] memory scan: %llu ms, %d letter-font candidates%c", GetTickCount64() - t0, s_lf_calls, 10);
    if (n != s_nfont) fprintf(stderr, "[prompts] button glyphs found in %d font(s)%c", n, 10);
    s_nfont = n;
    for (int k = 0; k < nb; k++) {
        Badge& bd = s_badge[k];
        if (badges[k] == bd.entry && bd.ea) continue;
        const uint32_t ea = cellGcmResolveLocated(1, be32(vm_base + badges[k]));
        if (!ea) continue;
        bd.entry = badges[k]; bd.ea = ea; bd.have = 0;
        fprintf(stderr, "[prompts] quick select badge found at %08X (table %08X)%c", ea, badges[k], 10);
    }
    if (hud && hud != s_hud.entry) {
        const uint32_t ea = cellGcmResolveLocated(0, be32(vm_base + hud));
        if (ea) { s_hud.entry = hud; s_hud.ea = ea;
                  fprintf(stderr, "[prompts] HUD picture \"R2 + square\" found at %08X (table %08X)%c", ea, hud, 10); } }
}

DWORD WINAPI prompts_thread(LPVOID)
{
    /* Fonts load at boot and with a level's own assets: scan a few times
     * after boot and after each level load, never during play. (v73-v75
     * also rescanned once a minute; the owner's v75 session froze every
     * ~62 s, for up to 9 s, in step with it.) The layout is followed every
     * 50 ms: the quick select ring's page prompts change as it opens (v70). */
    static const unsigned kScanAt[] = { 5, 10, 20, 40, 60 };
    unsigned t = 0, next = 0, last_layout = ~0u, ms = 0, levels = g_racacit_level_changes;
    for (;; Sleep(50), ms += 50) {
        if (levels != g_racacit_level_changes) { levels = g_racacit_level_changes; t = 0; next = 0; }
        if (ms >= 1000) { ms = 0; t++;
            if (next < sizeof kScanAt / sizeof kScanAt[0] && t == kScanAt[next]) { scan(); next++; } }
        static int force = -2;   /* RACACIT_PROMPT_LAYOUT=<0|1|2>: a layout for tests */
        if (force == -2) { const char* e = getenv("RACACIT_PROMPT_LAYOUT"); force = e && *e ? atoi(e) : -1; }
        const int layout = force >= 0 ? force : g_racacit_pad_layout;
        /* R1 <-> R2 with the layout swapped (R2 fires); L1 <-> L2 only while
         * the quick select ring is open, where L2 / R2 page (cellPad.c). */
        static int force_qs = -1;   /* RACACIT_PROMPT_QS=1: the quick select state on (tests: scripted input bypasses cellPad's check) */
        if (force_qs < 0) { const char* e = getenv("RACACIT_PROMPT_QS"); force_qs = e && *e == '1'; }
        const int want_r = layout != 0, want_l = layout != 0 && (g_racacit_ctx_quickselect || force_qs);
        int changed = 0;
        for (int i = 0; i < s_nfont; i++) {
            const int st = font_state(s_font[i]);
            if (!st) { s_font[i--] = s_font[--s_nfont]; continue; }   /* memory reused */
            const int has_l = (st - 1) & 1, has_r = ((st - 1) >> 1) & 1;
            if (has_l != want_l) { swap_records(s_font[i], s_font[i] + 16); changed = 1; }        /* L1 <-> L2 */
            if (has_r != want_r) { swap_records(s_font[i] + 64, s_font[i] + 80); changed = 1; }   /* R1 <-> R2 */
        }
        if (!ms) { hud_sync(want_r); badge_sync(want_r); }
        if ((changed && !want_l) || (unsigned)layout != last_layout)
            if (s_nfont) fprintf(stderr, "[prompts] controller layout %d: shoulder buttons in the text %s%c", layout,
                                 want_r ? "R1<->R2 (L1<->L2 while quick select is open)" : "as the PS3 layout", 10);
        if (s_nfont) last_layout = (unsigned)layout;
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
