/* Keycaps for the HUD's own button pictures. Included inside
 * button_prompts.cpp's namespace, after desktop_prompts.h.
 *
 * Not every button the game shows is a font glyph. The HUD's IGHW texture
 * table (section 0x5200, 32-byte entries: RSX offset at +0, format at +6,
 * width/height at +0x18/+0x1A, pitch at +0x1E) holds, right after the
 * 183x52 "R2 + square" picture, three 52x52 DXT5 face-button pictures. Those
 * are what the game draws for its in-world prompts: the "hold [square]"
 * bubble over a bolt crank, the "[triangle] Shop for weapons" bubble at a
 * vendor, and the like. The quick select ring's "L1"/"R1" badges (48x44,
 * button_prompts.cpp) are two more.
 *
 * With keyboard prompts on, each picture is replaced in guest memory by a
 * key (keycap_rgba) labelled with its binding, and put back for a
 * controller, exactly as the font glyphs are. Which button a face picture
 * shows is read from the picture itself: it is compared with the font's
 * E001-E004 glyphs (the same artwork at the same size), with the colour of
 * the symbol (circle red, square pink, cross blue, triangle green) as the
 * fallback. No game textures are distributed. */

/* A DXT5 picture `cols` x `rows` blocks, `pitch` bytes per block row, as
 * RGBA (cols*4 x rows*4). */
static void dxt5_to_rgba(const uint8_t* tex, uint32_t pitch, int cols, int rows, uint8_t* rgba)
{
    const int w = cols * 4;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            uint8_t px[16][4];
            dxt5_decode(tex + r * pitch + c * 16, px);
            for (int i = 0; i < 16; i++) memcpy(rgba + 4 * ((r * 4 + i / 4) * w + c * 4 + i % 4), px[i], 4);
        }
}

/* Encode the blocks of an RGBA picture (cols*4 wide) whose columns fall in
 * [c0, c1); the others are copied from `keep` (the original blocks). */
static void rgba_to_dxt5(const uint8_t* rgba, int cols, int rows, uint32_t pitch, int c0, int c1,
                         const uint8_t* keep, uint8_t* out)
{
    const int w = cols * 4;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            uint8_t* b = out + r * pitch + c * 16;
            if (c < c0 || c >= c1) { memcpy(b, keep + r * pitch + c * 16, 16); continue; }
            uint8_t px[16][4];
            for (int i = 0; i < 16; i++) memcpy(px[i], rgba + 4 * ((r * 4 + i / 4) * w + c * 4 + i % 4), 4);
            dxt5_encode(px, b);
        }
}

/* RACACIT_PROMPT_DUMP=<dir>: each picture as found and as replaced, side by
 * side at 4x over grey (hudkey_<ea>.ppm). */
static void hudkey_dump(uint32_t ea, const uint8_t* a, const uint8_t* b, int w, int h)
{
    const char* dir = getenv("RACACIT_PROMPT_DUMP");
    if (!dir || !*dir) return;
    char path[512]; snprintf(path, sizeof path, "%s/hudkey_%08X.ppm", dir, ea);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const int S = 4, W = (2 * w + 1) * S, H = h * S;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int k = x >= (w + 1) * S, xx = (k ? x - (w + 1) * S : x) / S, yy = y / S;
            uint8_t c[3] = { 40, 40, 40 };
            if (xx < w) {
                const uint8_t* q = (k ? b : a) + 4 * (yy * w + xx);
                for (int i = 0; i < 3; i++) c[i] = (uint8_t)((q[i] * q[3] + c[i] * (255 - q[3])) / 255);
            }
            fwrite(c, 1, 3, f);
        }
    fclose(f);
}

/* Pixel (x, y) of the font's button glyph E001 + `key` as the game drew it
 * (the desktop glyph's saved original blocks once keycaps were made). */
static bool glyph_pixel(unsigned key, int x, int y, uint8_t out[4])
{
    if (!s_nfont) return false;
    const uint32_t rec = s_font[0] - 6 * 16 + key * 16;
    if (!readable(rec, 16) || be16(vm_base + rec) != 0xE001 + key) return false;
    const int gx = be16(vm_base + rec + 2), gy = be16(vm_base + rec + 4), gw = vm_base[rec + 10], gh = vm_base[rec + 11];
    if (x >= gw || y >= gh) return false;
    for (unsigned i = 0; i < desktop_count; ++i) {
        const DesktopGlyph& g = desktop_glyphs[i];
        if (g.rec != rec) continue;
        const int px_ = g.x + x, py_ = g.y + y, c = px_ / 4 - g.bx, r = py_ / 4 - g.by;
        uint8_t px[16][4];
        dxt5_decode(g.original + (r * g.cols + c) * 16, px);
        memcpy(out, px[(py_ & 3) * 4 + (px_ & 3)], 4);
        return true;
    }
    uint32_t pitch = 0; int pw = 0, ph = 0;
    const uint8_t* page = font_page_at(s_font[0], be16(vm_base + rec + 6), &pitch, &pw, &ph);
    if (!page) return false;
    dxt5_pixel(page, pitch, gx + x, gy + y, out);
    return true;
}

/* Which face button (0 circle, 1 square, 2 cross, 3 triangle) a 52x52
 * picture shows; -1 if it does not look like one. */
static int face_key(const uint8_t* rgba, int w, int h)
{
    long best = -1; int key = -1;
    for (unsigned k = 0; k < 4; k++) {
        long err = 0; int n = 0;
        for (int y = 0; y < h; y += 2)
            for (int x = 0; x < w; x += 2) {
                uint8_t g[4];
                if (!glyph_pixel(k, x, y, g)) { n = 0; goto next; }
                const uint8_t* p = rgba + 4 * (y * w + x);
                for (int i = 0; i < 4; i++) err += abs(g[i] - p[i]);
                n++;
            }
        if (n && (best < 0 || err < best)) { best = err; key = (int)k; }
    next:;
    }
    const long per = best >= 0 ? best / ((w / 2) * (h / 2)) : -1;
    if (key >= 0 && per <= 60) return key;
    /* The symbol's colour: saturated, opaque pixels. */
    long sr = 0, sg = 0, sb = 0, n = 0;
    for (int i = 0; i < w * h; i++) {
        const uint8_t* p = rgba + 4 * i;
        const int mx = std::max(p[0], std::max(p[1], p[2])), mn = std::min(p[0], std::min(p[1], p[2]));
        if (p[3] > 128 && mx - mn > 60) { sr += p[0]; sg += p[1]; sb += p[2]; n++; }
    }
    int hue = -1;
    if (n > 20) {
        sr /= n; sg /= n; sb /= n;
        if (sg >= sr && sg >= sb) hue = 3;
        else if (sb >= sr && sb >= sg) hue = 2;
        else hue = sb * 100 > sr * 55 ? 1 : 0;
    }
    fprintf(stderr, "[prompts] HUD face picture: font match %d (difference %ld), colour %ld,%ld,%ld -> %d%c", key, per, sr, sg, sb, hue, 10);
    return hue;
}

const int kFaceW = 52, kFaceCols = 13, kFacePitch = 208, kFaceBytes = 13 * kFacePitch;
struct HudFace {
    uint32_t entry, ea;
    uint8_t  orig[kFaceBytes], mine[kFaceBytes];
    int      have;          /* orig/mine hold this picture */
};
HudFace s_face[8];
int s_nface;

/* The face pictures listed with the "R2 + square" entry `hud`: 52x52 DXT5
 * entries of the same table, read from the same memory. */
static void hud_faces_find(uint32_t hud)
{
    int n = 0;
    for (int k = -96; k <= 96 && n < 8; k++) {
        const uint32_t e = hud + 32 * k;
        if (!readable(e, 32)) continue;
        const uint8_t* t = vm_base + e;
        if (be16(t + 0x18) != kFaceW || be16(t + 0x1A) != kFaceW || t[6] != 0xA8 || be16(t + 0x1E) != kFacePitch) continue;
        const uint32_t ea = cellGcmResolveLocated(0, be32(t));
        if (!ea) continue;
        HudFace& f = s_face[n++];
        if (f.entry != e || f.ea != ea) {
            f.entry = e; f.ea = ea; f.have = 0;
            fprintf(stderr, "[prompts] HUD face picture found at %08X (table %08X)%c", ea, e, 10);
        }
    }
    s_nface = n;
}

static void hud_faces_sync(bool keyboard)
{
    for (int i = 0; i < s_nface; i++) {
        HudFace& f = s_face[i];
        if (!f.ea) continue;
        if (!readable(f.entry, 32) || !readable(f.ea, kFaceBytes)) { f.ea = 0; continue; }
        const uint8_t* t = vm_base + f.entry;
        if (be16(t + 0x18) != kFaceW || be16(t + 0x1A) != kFaceW || t[6] != 0xA8) { f.ea = 0; continue; }   /* memory reused */
        uint8_t* tex = vm_base + f.ea;
        const bool is_mine = f.have && !memcmp(tex, f.mine, kFaceBytes);
        bool is_orig = f.have && !memcmp(tex, f.orig, kFaceBytes);
        if (!is_mine && !is_orig) {
            if (!keyboard) continue;    /* made on first need, from the game's own picture */
            static uint8_t a[kFaceW * kFaceW * 4], b[kFaceW * kFaceW * 4];
            dxt5_to_rgba(tex, kFacePitch, kFaceCols, kFaceCols, a);
            const int key = face_key(a, kFaceW, kFaceW);
            if (key < 0) { fprintf(stderr, "[prompts] HUD face picture at %08X: not a face button, left alone%c", f.ea, 10); f.ea = 0; continue; }
            char label[8]; desktop_label((unsigned)key, label);
            keycap_rgba(kFaceW, kFaceW, label, b);
            memcpy(f.orig, tex, kFaceBytes);
            rgba_to_dxt5(b, kFaceCols, kFaceCols, kFacePitch, 0, kFaceCols, f.orig, f.mine);
            dxt5_to_rgba(f.mine, kFacePitch, kFaceCols, kFaceCols, b);
            hudkey_dump(f.ea, a, b, kFaceW, kFaceW);
            fprintf(stderr, "[prompts] HUD face picture at %08X: E%03X, key %s%c", f.ea, key + 1, label, 10);
            f.have = 1; is_orig = true;
        }
        const uint8_t* put = keyboard && is_orig ? f.mine : !keyboard && is_mine ? f.orig : nullptr;
        if (!put) continue;
        memcpy(tex, put, kFaceBytes);
        fprintf(stderr, "[prompts] HUD face picture at %08X: %s%c", f.ea, keyboard ? "keycap drawn in" : "put back", 10);
    }
}

