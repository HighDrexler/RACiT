/* Desktop keycaps for the game's private-use text glyphs. Included inside
 * button_prompts.cpp's namespace, after its existing DXT5/page helpers.
 * No game textures are distributed; original blocks are retained for restore.
 */
struct DesktopGlyph {
    uint32_t rec, page, pitch; int x, y, w, h, bx, by, cols, rows;
    uint8_t original[8192], keys[8192];
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
    };
    for (const auto& letter : font) if (letter.ch == ch) return letter.rows[row];
    return 0;
}
static void desktop_blocks(const DesktopGlyph& g, uint8_t* blocks, bool write)
{
    for (int row = 0; row < g.rows; ++row) {
        uint8_t* texture = vm_base + g.page + (g.by + row) * g.pitch + g.bx * 16;
        if (write) memcpy(texture, blocks + row * g.cols * 16, g.cols * 16);
        else memcpy(blocks + row * g.cols * 16, texture, g.cols * 16);
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
static void desktop_prompts(bool keyboard)
{
    static const char* const labels[20] = {
        "E", "F", "SPC", "Q", "TAB", "ESC", "CTRL", "RMB", "V", "WASD",
        "LMB", "SHIFT", "B", "IJKL", "UP", "DOWN", "LEFT", "RIGHT", "", ""
    };
    static const char* const bindings[18] = {
        "CIRCLE", "SQUARE", "CROSS", "TRIANGLE", "SELECT", "START", "L1", "L2", "L3", "LS_UP",
        "R1", "R2", "R3", "RS_UP", "UP", "DOWN", "LEFT", "RIGHT"
    };
    /* Directional picture variants need a separate atlas mapping; do not guess. */
    for (int font = 0; font < s_nfont; ++font) for (unsigned key = 0; key < 14; ++key) {
        const uint32_t rec = s_font[font] - 6 * 16 + key * 16;
        if (!readable(rec, 16) || be16(vm_base + rec) != 0xE001 + key) continue;
        uint32_t pitch = 0;
        const uint8_t* page = font_page_at(s_font[font], be16(vm_base + rec + 6), &pitch);
        if (!page) continue;
        const uint32_t ea = (uint32_t)(page - vm_base);
        DesktopGlyph* glyph = nullptr;
        for (unsigned i = 0; i < desktop_count; ++i)
            if (desktop_glyphs[i].rec == rec && desktop_glyphs[i].page == ea) { glyph = &desktop_glyphs[i]; break; }
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
            char environment[48], label[8];
            snprintf(environment, sizeof environment, "RACACIT_BIND_%s", bindings[key]);
            const char* custom = getenv(environment);
            snprintf(label, sizeof label, "%s", labels[key]);
            if (custom && *custom) {
                if (!strncmp(custom,"Mouse1",6)) snprintf(label,sizeof label,"LMB");
                else if (!strncmp(custom,"Mouse2",6)) snprintf(label,sizeof label,"MMB");
                else if (!strncmp(custom,"Mouse3",6)) snprintf(label,sizeof label,"RMB");
                else if (!strncmp(custom,"Space",5)) snprintf(label,sizeof label,"SPC");
                else if (!strncmp(custom,"Return",6)) snprintf(label,sizeof label,"ENT");
                else if (!strncmp(custom,"Left Ctrl",9)) snprintf(label,sizeof label,"CTRL");
                else if (!strncmp(custom,"Left Shift",10)) snprintf(label,sizeof label,"SHIFT");
                else {
                    unsigned i = 0;
                    while (i < 5 && custom[i] && custom[i] != ',') {
                        const char c = custom[i]; label[i++] = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
                    }
                    label[i] = 0;
                }
            }
            desktop_make(*glyph, label);
            fprintf(stderr, "[prompts] desktop glyph E%03X: %s\n", key + 1, label);
        }
        uint8_t present[8192]; desktop_blocks(*glyph, present, false);
        const size_t bytes = (size_t)glyph->cols * glyph->rows * 16;
        if (!memcmp(present, keyboard ? glyph->original : glyph->keys, bytes))
            desktop_blocks(*glyph, keyboard ? glyph->keys : glyph->original, true);
    }
}
