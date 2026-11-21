/* Desktop keycaps for the game's private-use text glyphs. Included inside
 * button_prompts.cpp's namespace, after its existing DXT5/page helpers.
 * No game textures are distributed; original blocks are retained for restore.
 */
/* The glyph in the page's smaller mip levels: text drawn small (the hint
 * and prompt boxes: "Use [L] to select", a vendor's "[triangle] Shop for
 * weapons") samples those, not level 0. Up to 7 levels, their blocks one
 * after another in morig/mkeys. */
struct DesktopMip { uint32_t ea, pitch; int bx, by, cols, rows; };
struct DesktopGlyph {
    uint32_t rec, page, pitch; int x, y, w, h, bx, by, cols, rows;
    uint8_t original[8192], keys[8192];
    int nmips; DesktopMip mip[7];
    uint8_t morig[4096], mkeys[4096];
};
static DesktopGlyph desktop_glyphs[16 * 20];
static unsigned desktop_count;
static uint8_t desktop_letter(char ch, int row)
{
    /* Five-bit rows. All labels use this small, original bitmap alphabet. */
    static const struct { char ch; uint8_t rows[7]; } font[] = {
        {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
        {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
        {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
        {'G',{14,17,16,23,17,17,15}},
        {'H',{17,17,17,31,17,17,17}}, {'I',{31,4,4,4,4,4,31}},
        {'J',{7,2,2,2,18,18,12}}, {'K',{17,18,20,24,20,18,17}},
        {'L',{16,16,16,16,16,16,31}}, {'M',{17,27,21,21,17,17,17}},
        {'N',{17,25,25,21,19,19,17}}, {'O',{14,17,17,17,17,17,14}},
        {'P',{30,17,17,30,16,16,16}}, {'Q',{14,17,17,17,21,18,13}},
        {'R',{30,17,17,30,20,18,17}}, {'S',{15,16,16,14,1,1,30}},
        {'T',{31,4,4,4,4,4,4}}, {'U',{17,17,17,17,17,17,14}},
        {'V',{17,17,17,17,17,10,4}}, {'W',{17,17,17,21,21,21,10}},
        {'X',{17,17,10,4,10,17,17}}, {'Z',{31,1,2,4,8,16,31}},
        {'Y',{17,17,10,4,4,4,4}}, {'1',{4,12,4,4,4,4,14}},
        {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
        {'4',{2,6,10,18,31,2,2}},
        {'0',{14,17,19,21,25,17,14}}, {'5',{31,16,16,30,1,1,30}},
        {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
        {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
        /* The punctuation keys (SDL names them by the character). */
        {'[',{14,8,8,8,8,8,14}}, {']',{14,2,2,2,2,2,14}},
        {';',{0,4,4,0,4,4,8}}, {'\'',{4,4,8,0,0,0,0}},
        {',',{0,0,0,0,4,4,8}}, {'.',{0,0,0,0,0,12,12}},
        {'/',{1,1,2,4,8,16,16}}, {'\\',{16,16,8,4,2,1,1}},
        {'-',{0,0,0,31,0,0,0}}, {'=',{0,0,31,0,31,0,0}},
        {'`',{8,4,2,0,0,0,0}}, {'+',{0,4,4,31,4,4,0}},
        {'*',{0,21,14,31,14,21,0}},
    };
    for (const auto& letter : font) if (letter.ch == ch) return letter.rows[row];
    static const uint8_t unknown[7]={14,17,1,2,4,0,4};
    return unknown[row];
}
static bool desktop_has_letter(char ch)
{
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return true;
    return ch && strchr("[];',./\\-=`+*", ch) != nullptr;
}
static void desktop_blocks(const DesktopGlyph& g, uint8_t* blocks, bool write)
{
    for (int row = 0; row < g.rows; ++row) {
        uint8_t* texture = vm_base + g.page + (g.by + row) * g.pitch + g.bx * 16;
        if (write) memcpy(texture, blocks + row * g.cols * 16, g.cols * 16);
        else memcpy(blocks + row * g.cols * 16, texture, g.cols * 16);
    }
}
static void desktop_mip_blocks(DesktopGlyph& g, const uint8_t* src, uint8_t* dst, bool write)
{
    size_t o = 0;
    for (int m = 0; m < g.nmips; m++) {
        const DesktopMip& d = g.mip[m];
        for (int r = 0; r < d.rows; r++) {
            uint8_t* t = vm_base + d.ea + (uint32_t)(d.by + r) * d.pitch + (uint32_t)d.bx * 16;
            if (write) memcpy(t, src + o, (size_t)d.cols * 16); else memcpy(dst + o, t, (size_t)d.cols * 16);
            o += (size_t)d.cols * 16;
        }
    }
}
/* The keycap (level 0, g.keys) filtered down into each smaller level of a
 * `levels`-level DXT5 page `width` x `height` at `page_ea`: each texel
 * takes the key's average over the texels it covers, blended with the
 * page's own by how much of it the glyph covers. */
static void desktop_make_mips(DesktopGlyph& g, uint32_t page_ea, int width, int height, int levels)
{
    g.nmips = 0;
    uint32_t off = 0; size_t used = 0;
    static uint8_t key0[256 * 256][4];
    if (g.w > 256 || g.h > 256) return;
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w; x++) {
            const int px_ = g.x + x, py_ = g.y + y;
            uint8_t px[16][4];
            dxt5_decode(g.keys + ((py_ / 4 - g.by) * g.cols + (px_ / 4 - g.bx)) * 16, px);
            memcpy(key0[y * g.w + x], px[(py_ & 3) * 4 + (px_ & 3)], 4);
        }
    for (int n = 0; n < levels && g.nmips < 7; n++) {
        const int lw = std::max(1, width >> n), lh = std::max(1, height >> n);
        const uint32_t bw = (uint32_t)(lw + 3) / 4, bh = (uint32_t)(lh + 3) / 4, lp = bw * 16;
        const uint32_t lea = page_ea + off;
        off += bw * bh * 16;
        if (n == 0) continue;
        const int s = 1 << n, x0 = g.x >> n, y0 = g.y >> n, x1 = (g.x + g.w + s - 1) >> n, y1 = (g.y + g.h + s - 1) >> n;
        if (x1 - x0 < 2 || y1 - y0 < 2) break;
        DesktopMip d = { lea, lp, x0 / 4, y0 / 4, (x1 + 3) / 4 - x0 / 4, (y1 + 3) / 4 - y0 / 4 };
        const size_t bytes = (size_t)d.cols * d.rows * 16;
        if (used + bytes > sizeof g.morig || !readable(lea + (uint32_t)d.by * lp, (uint32_t)d.rows * lp)) break;
        uint8_t* ko = g.mkeys + used;
        for (int r = 0; r < d.rows; r++)
            for (int c = 0; c < d.cols; c++) {
                const uint8_t* blk = vm_base + lea + (uint32_t)(d.by + r) * lp + (uint32_t)(d.bx + c) * 16;
                memcpy(g.morig + used + (r * d.cols + c) * 16, blk, 16);
                uint8_t px[16][4];
                dxt5_decode(blk, px);
                for (int i = 0; i < 16; i++) {
                    const int tx = (d.bx + c) * 4 + (i & 3), ty = (d.by + r) * 4 + (i >> 2);
                    long acc[4] = {}; int cnt = 0;
                    for (int sy = ty * s; sy < (ty + 1) * s; sy++)
                        for (int sx = tx * s; sx < (tx + 1) * s; sx++) {
                            if (sx < g.x || sy < g.y || sx >= g.x + g.w || sy >= g.y + g.h) continue;
                            const uint8_t* q = key0[(sy - g.y) * g.w + (sx - g.x)];
                            for (int k = 0; k < 4; k++) acc[k] += q[k];
                            cnt++;
                        }
                    if (!cnt) continue;
                    const float cov = (float)cnt / (float)(s * s);
                    for (int k = 0; k < 4; k++) px[i][k] = (uint8_t)(px[i][k] + ((float)acc[k] / cnt - px[i][k]) * cov + 0.5f);
                }
                dxt5_encode(px, ko + (r * d.cols + c) * 16);
            }
        g.mip[g.nmips++] = d;
        used += bytes;
    }
}
static void desktop_make(DesktopGlyph& g, const char* label)
{
    memcpy(g.keys, g.original, sizeof g.keys);
    const int len = (int)strlen(label), letters_w = len * 6 - 1;
    const float scale = std::min((g.w - 8.f) / letters_w, (g.h - 8.f) / 7.f);
    const float left = (g.w - letters_w * scale) / 2, top = (g.h - 7 * scale) / 2;
    for (int row = 0; row < g.rows; ++row) for (int col = 0; col < g.cols; ++col) {
        uint8_t pixels[16][4]; uint8_t* block = g.keys + (row * g.cols + col) * 16;
        dxt5_decode(block, pixels);
        for (int i = 0; i < 16; ++i) {
            const int x = (g.bx + col) * 4 + i % 4 - g.x;
            const int y = (g.by + row) * 4 + i / 4 - g.y;
            if (x < 0 || x >= g.w || y < 0 || y >= g.h) continue;
            const bool edge = x < 2 || y < 2 || x >= g.w - 2 || y >= g.h - 2;
            bool ink = false;
            if (x >= left && y >= top) {
                const int tx = (int)((x - left) / scale), ty = (int)((y - top) / scale);
                if (ty < 7 && tx / 6 < len && tx % 6 < 5)
                    ink = desktop_letter(label[tx / 6], ty) & (16 >> (tx % 6));
            }
            const uint8_t color = edge || ink ? 255 : 20;
            pixels[i][0] = pixels[i][1] = pixels[i][2] = color; pixels[i][3] = 255;
        }
        dxt5_encode(pixels, block);
    }
}
/* A RACACIT_BIND_<name> setting's first key as a short label; `fallback` when unset. */
static void desktop_binding_label(const char* name, const char* fallback, char label[8])
{
    char environment[48];
    snprintf(environment, sizeof environment, "RACACIT_BIND_%s", name);
    const char* custom = *name ? getenv(environment) : nullptr;
    snprintf(label, 8, "%s", fallback);
    if (custom && *custom) {
        if (!strncmp(custom,"Mouse1",6)) snprintf(label,8,"LMB");
        else if (!strncmp(custom,"Mouse2",6)) snprintf(label,8,"MMB");
        else if (!strncmp(custom,"Mouse3",6)) snprintf(label,8,"RMB");
        else if (!strncmp(custom,"Mouse4",6)) snprintf(label,8,"M4");
        else if (!strncmp(custom,"Mouse5",6)) snprintf(label,8,"M5");
        else if (!strncmp(custom,"Space",5)) snprintf(label,8,"SPC");
        else if (!strncmp(custom,"Return",6)) snprintf(label,8,"ENT");
        else if (!strncmp(custom,"Left Ctrl",9)) snprintf(label,8,"CTRL");
        else if (!strncmp(custom,"Left Shift",10)) snprintf(label,8,"SHIFT");
        else {
            /* The first key's SDL name, shortened to five characters that have a letter. */
            static const struct { const char* name; const char* label; } shorter[] = {
                {"Escape","ESC"}, {"Backspace","BKSP"}, {"Delete","DEL"}, {"Insert","INS"},
                {"Page Up","PGUP"}, {"PageUp","PGUP"}, {"Page Down","PGDN"}, {"PageDown","PGDN"},
                {"CapsLock","CAPS"}, {"Caps Lock","CAPS"}, {"Right Ctrl","RCTRL"}, {"Right Shift","RSHFT"},
                {"Left Alt","ALT"}, {"Right Alt","RALT"}, {"Left GUI","CMD"}, {"Right GUI","RCMD"},
                {"Keypad Enter","KPENT"}, {"WheelUp","WHUP"}, {"WheelDown","WHDN"},
            };
            char name[48]; unsigned n = 0;
            while (n + 1 < sizeof name && custom[n] && custom[n] != ',') { name[n] = custom[n]; n++; }
            name[n] = 0;
            const char* use = name;
            for (const auto& s : shorter) if (!strcmp(name, s.name)) { use = s.label; break; }
            char tmp[48];
            if (use == name && !strncmp(name, "Keypad ", 7)) { snprintf(tmp, sizeof tmp, "KP%s", name + 7); use = tmp; }
            unsigned i = 0;
            for (const char* c = use; *c && i < 5; c++) {
                const char u = *c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c;
                if (desktop_has_letter(u)) label[i++] = u;   /* spaces and the like are left out */
            }
            label[i] = 0;
            if (!i) snprintf(label, 8, "%s", fallback);   /* nothing drawable: the default key */
        }
    }
}
/* The key bound to button glyph E001 + `key` (0..13), as a short label. */
static void desktop_label(unsigned key, char label[8])
{
    static const char* const labels[20] = {
        "E", "F", "SPC", "Q", "TAB", "ESC", "CTRL", "RMB", "V", "WASD",
        "LMB", "SHIFT", "B", "IJKL", "UP", "DOWN", "LEFT", "RIGHT", "", "ARROW"
    };
    static const char* const bindings[20] = {
        "CIRCLE", "SQUARE", "CROSS", "TRIANGLE", "SELECT", "START", "L1", "L2", "L3", "LS_UP",
        "R1", "R2", "R3", "RS_UP", "UP", "DOWN", "LEFT", "RIGHT", "", ""
    };
    desktop_binding_label(bindings[key], labels[key], label);
}
static void desktop_prompts(bool keyboard)
{
    /* E00F-E012: the d-pad with up, down, left or right lit; E014: the whole
     * d-pad (E013 is the grenade picture, not a button). */
    /* Smaller levels share blocks between neighbouring glyphs, and each
     * glyph's blocks were saved with its predecessors' keys in them: keys go
     * in front to back, the originals back to front. */
    for (int ff = 0; ff < s_nfont; ++ff) for (unsigned kk = 0; kk < 20; ++kk) {
        const int font = keyboard ? ff : s_nfont - 1 - ff;
        const unsigned key = keyboard ? kk : 19 - kk;
        if (key == 18) continue;
        const uint32_t rec = s_font[font] - 6 * 16 + key * 16;
        if (!readable(rec, 16) || be16(vm_base + rec) != 0xE001 + key) continue;
        uint32_t pitch = 0; int width=0,height=0,levels=1;
        const uint8_t* page = font_page_at(s_font[font], be16(vm_base + rec + 6), &pitch,&width,&height,&levels);
        if (!page) continue;
        const int x=be16(vm_base+rec+2),y=be16(vm_base+rec+4),w=vm_base[rec+10],h=vm_base[rec+11];
        if (w<12 || h<12 || x+w>width || y+h>height) continue;
        const uint32_t ea = (uint32_t)(page - vm_base);
        DesktopGlyph* glyph = nullptr;
        for (unsigned i = 0; i < desktop_count; ++i)
            if (desktop_glyphs[i].rec == rec && desktop_glyphs[i].page == ea && desktop_glyphs[i].pitch==pitch &&
                desktop_glyphs[i].x==x && desktop_glyphs[i].y==y && desktop_glyphs[i].w==w && desktop_glyphs[i].h==h) { glyph = &desktop_glyphs[i]; break; }
        if (!glyph) {
            if (!keyboard || desktop_count == sizeof desktop_glyphs / sizeof *desktop_glyphs) continue;
            glyph = &desktop_glyphs[desktop_count++];
            *glyph = {};
            glyph->rec = rec; glyph->page = ea; glyph->pitch = pitch;
            glyph->x = be16(vm_base + rec + 2); glyph->y = be16(vm_base + rec + 4);
            glyph->w = vm_base[rec + 10]; glyph->h = vm_base[rec + 11];
            glyph->bx = glyph->x / 4; glyph->by = glyph->y / 4;
            glyph->cols = (glyph->x + glyph->w + 3) / 4 - glyph->bx;
            glyph->rows = (glyph->y + glyph->h + 3) / 4 - glyph->by;
            if (glyph->w < 12 || glyph->h < 12 || glyph->cols * glyph->rows * 16 > 8192 ||
                !readable(ea + glyph->by * pitch + glyph->bx * 16,
                          (glyph->rows - 1) * pitch + glyph->cols * 16)) { --desktop_count; continue; }
            desktop_blocks(*glyph, glyph->original, false);
            char label[8];
            desktop_label(key, label);
            desktop_make(*glyph, label);
            desktop_make_mips(*glyph, ea, width, height, levels);
            fprintf(stderr, "[prompts] desktop glyph E%03X: %s (%d smaller levels of %d)\n", key + 1, label, glyph->nmips, levels);
        }
        uint8_t present[8192]; desktop_blocks(*glyph, present, false);
        const size_t bytes = (size_t)glyph->cols * glyph->rows * 16;
        if (!memcmp(present, keyboard ? glyph->original : glyph->keys, bytes)) {
            desktop_blocks(*glyph, keyboard ? glyph->keys : glyph->original, true);
            desktop_mip_blocks(*glyph, keyboard ? glyph->mkeys : glyph->morig, nullptr, true);
        }
    }
}

/* ---- keycap pictures for the HUD's own button pictures ------------------
 * A key drawn into an RGBA picture of any size: a rounded dark key with a
 * light rim, the label in the bitmap alphabet above, centred and as large as
 * fits. 4x4 samples per pixel for smooth edges. Alpha 0 outside the key. */
static void keycap_rgba(int w, int h, const char* label, uint8_t* rgba)
{
    const int len = (int)strlen(label), letters_w = len > 0 ? len * 6 - 1 : 1;
    const float m = std::max(1.0f, std::min(w, h) * 0.04f);       /* margin round the key */
    const float x0 = m, y0 = m, x1 = w - m, y1 = h - m;
    const float r = std::min(x1 - x0, y1 - y0) * 0.22f;             /* corner radius */
    const float rim = std::max(1.0f, std::min(w, h) * 0.06f);
    const float tw = (x1 - x0) - 2 * rim - 2 * std::max(1.0f, w * 0.06f), th = (y1 - y0) - 2 * rim - 2 * std::max(1.0f, h * 0.12f);
    const float scale = std::max(0.5f, std::min(tw / letters_w, th / 7.0f));
    const float lx = (w - letters_w * scale) / 2, ly = (h - 7 * scale) / 2;
    auto inside = [&](float x, float y, float inset) {
        const float ax = x0 + inset, ay = y0 + inset, bx = x1 - inset, by = y1 - inset, rr = std::max(0.0f, r - inset);
        if (x < ax || y < ay || x > bx || y > by) return false;
        const float cx = x < ax + rr ? ax + rr : x > bx - rr ? bx - rr : x;
        const float cy = y < ay + rr ? ay + rr : y > by - rr ? by - rr : y;
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr;
    };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float a = 0, lum = 0;
            for (int s = 0; s < 16; s++) {
                const float sx = x + (s % 4 + 0.5f) / 4, sy = y + (s / 4 + 0.5f) / 4;
                if (!inside(sx, sy, 0)) continue;
                a += 1;
                bool ink = !inside(sx, sy, rim);
                if (!ink && sx >= lx && sy >= ly) {
                    const int tx = (int)((sx - lx) / scale), ty = (int)((sy - ly) / scale);
                    if (ty < 7 && tx / 6 < len && tx % 6 < 5) ink = desktop_letter(label[tx / 6], ty) & (16 >> (tx % 6));
                }
                lum += ink ? 245 : 28;
            }
            uint8_t* p = rgba + 4 * (y * w + x);
            const uint8_t v = a ? (uint8_t)(lum / a) : 0;
            p[0] = p[1] = p[2] = v; p[3] = (uint8_t)(a * 255 / 16);
        }
}
