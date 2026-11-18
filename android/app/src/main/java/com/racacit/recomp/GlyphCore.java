package com.racacit.recomp;

import java.io.File;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The button icons out of the game's own font (no Android types here, so it
 * also runs on a desktop JVM for testing). A Crack in Time's text carries
 * buttons as private-use characters (E001 circle ... E00C R2, see
 * game/button_prompts.cpp) and its fonts draw them as pictures:
 *
 *   font.dat         IGHW; section table at 0x20 (16-byte rows: id, offset,
 *                    flags << 24 | count, row size); glyph records in section
 *                    0x29400, 16 bytes: code point, x, y, page (u16 each),
 *                    u16, width u8, height u8
 *   font_header.dat  IGHW; section 0x5200: one 0x20-byte RSX texture row per
 *                    page -- texel offset u32, format byte at +6 (0x86 DXT1,
 *                    0x87 DXT3, 0x88 DXT5), width and height u16 at +0x18
 *   font_texel.dat   IGHW; section 0x5300 holds the pages (linear DXT
 *                    blocks), the header's offsets counting from its start
 */
final class GlyphCore {
    private GlyphCore() {}

    static final String FONT = "/built/tuids/947/947468b6d1bfbc96/";
    static final String ARCHIVE = "PS3_GAME/USRDIR/packed/game/global_cached.psarc";

    static final class Image { final int w, h; final int[] argb; Image(int w, int h) { this.w = w; this.h = h; argb = new int[w * h]; } }

    static int be16(byte[] d, int o) { return (d[o] & 0xFF) << 8 | d[o + 1] & 0xFF; }
    static int be32(byte[] d, int o) { return (d[o] & 0xFF) << 24 | (d[o + 1] & 0xFF) << 16 | (d[o + 2] & 0xFF) << 8 | d[o + 3] & 0xFF; }

    /** {offset, row size, count} of an IGHW section, or null. */
    static int[] section(byte[] d, int id) {
        int n = be32(d, 8);
        for (int i = 0; i < n && 0x20 + i * 16 + 16 <= d.length; i++) {
            int o = 0x20 + i * 16;
            if (be32(d, o) == id) return new int[] { be32(d, o + 4), be32(d, o + 12), be32(d, o + 8) & 0xFFFFFF };
        }
        return null;
    }

    /** Every glyph U+E001..U+E01F of the game's font, by code point. */
    static Map<Integer, Image> extract(File gameRoot) throws IOException {
        byte[] font, header, texel;
        try (Psarc p = new Psarc(new File(gameRoot, ARCHIVE))) {
            font = p.read(FONT + "font.dat"); header = p.read(FONT + "font_header.dat"); texel = p.read(FONT + "font_texel.dat");
        }
        if (font == null || header == null || texel == null) throw new IOException("the game's font was not found in global_cached.psarc");
        int[] g = section(font, 0x29400), t = section(header, 0x5200), tx = section(texel, 0x5300);
        if (g == null || t == null) throw new IOException("unexpected font layout");
        final int texBase = tx != null ? tx[0] : 0;
        Map<Integer, Image> pages = new LinkedHashMap<>(), out = new LinkedHashMap<>();
        for (int i = 0; i < g[1] / 16 && g[0] + i * 16 + 16 <= font.length; i++) {
            int r = g[0] + i * 16, cp = be16(font, r);
            if (cp < 0xE001 || cp > 0xE01F) continue;
            int x = be16(font, r + 2), y = be16(font, r + 4), page = be16(font, r + 6);
            int w = font[r + 10] & 0xFF, h = font[r + 11] & 0xFF;
            if (w == 0 || h == 0 || page >= t[2]) continue;
            Image pg = pages.get(page);
            if (pg == null) { pg = decodePage(header, t[0] + page * 0x20, texel, texBase); pages.put(page, pg); }
            Image im = new Image(w, h);
            for (int yy = 0; yy < h; yy++)
                for (int xx = 0; xx < w; xx++) {
                    int sx = x + xx, sy = y + yy;
                    im.argb[yy * w + xx] = sx < pg.w && sy < pg.h ? pg.argb[sy * pg.w + sx] : 0;
                }
            out.put(cp, im);
        }
        if (out.isEmpty()) throw new IOException("no button glyphs in the font");
        return out;
    }

    static Image decodePage(byte[] hdr, int row, byte[] texel, int texBase) throws IOException {
        int off = texBase + be32(hdr, row), fmt = hdr[row + 6] & 0x9F, w = be16(hdr, row + 0x18), h = be16(hdr, row + 0x1A);
        int bs = fmt == 0x86 ? 8 : 16;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) throw new IOException("bad font page size");
        if (off + (long) ((w + 3) / 4) * ((h + 3) / 4) * bs > texel.length) throw new IOException("font page outside its file");
        Image im = new Image(w, h);
        int p = off;
        int[] col = new int[4], al = new int[8];
        for (int by = 0; by < (h + 3) / 4; by++)
            for (int bx = 0; bx < (w + 3) / 4; bx++, p += bs) {
                int cb = bs == 16 ? p + 8 : p;
                long alphaBits = 0; int a0 = 255, a1 = 255;
                if (fmt == 0x88) {                       /* DXT5: interpolated alpha */
                    a0 = texel[p] & 0xFF; a1 = texel[p + 1] & 0xFF;
                    for (int k = 0; k < 6; k++) alphaBits |= (long) (texel[p + 2 + k] & 0xFF) << (8 * k);
                    al[0] = a0; al[1] = a1;
                    if (a0 > a1) for (int k = 1; k < 7; k++) al[k + 1] = (a0 * (7 - k) + a1 * k) / 7;
                    else { for (int k = 1; k < 5; k++) al[k + 1] = (a0 * (5 - k) + a1 * k) / 5; al[6] = 0; al[7] = 255; }
                }
                int c0 = texel[cb] & 0xFF | (texel[cb + 1] & 0xFF) << 8, c1 = texel[cb + 2] & 0xFF | (texel[cb + 3] & 0xFF) << 8;
                int bits = texel[cb + 4] & 0xFF | (texel[cb + 5] & 0xFF) << 8 | (texel[cb + 6] & 0xFF) << 16 | (texel[cb + 7] & 0xFF) << 24;
                col[0] = rgb565(c0); col[1] = rgb565(c1);
                if (bs == 16 || c0 > c1) { col[2] = mix(col[0], col[1], 2, 1, 3); col[3] = mix(col[0], col[1], 1, 2, 3); }
                else { col[2] = mix(col[0], col[1], 1, 1, 2); col[3] = 0; }
                for (int k = 0; k < 16; k++) {
                    int x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
                    if (x >= w || y >= h) continue;
                    int ci = bits >>> (2 * k) & 3, a;
                    if (fmt == 0x88) a = al[(int) (alphaBits >>> (3 * k) & 7)];
                    else if (fmt == 0x87) a = ((texel[p + k / 2] & 0xFF) >> (4 * (k & 1)) & 15) * 17;
                    else a = bs == 8 && c0 <= c1 && ci == 3 ? 0 : 255;
                    im.argb[y * w + x] = a << 24 | col[ci] & 0xFFFFFF;
                }
            }
        return im;
    }

    static int rgb565(int c) {
        int r = (c >> 11 & 31) * 255 / 31, g = (c >> 5 & 63) * 255 / 63, b = (c & 31) * 255 / 31;
        return 0xFF000000 | r << 16 | g << 8 | b;
    }

    static int mix(int a, int b, int wa, int wb, int d) {
        int r = ((a >> 16 & 255) * wa + (b >> 16 & 255) * wb) / d, g = ((a >> 8 & 255) * wa + (b >> 8 & 255) * wb) / d,
            bl = ((a & 255) * wa + (b & 255) * wb) / d;
        return 0xFF000000 | r << 16 | g << 8 | bl;
    }

    /** File names the launcher saves them under and the companion loads. */
    static String fileName(int cp) { return String.format("e%03x.png", cp & 0xFFF); }

    static final int CIRCLE = 0xE001, SQUARE = 0xE002, CROSS = 0xE003, TRIANGLE = 0xE004, SELECT = 0xE005, START = 0xE006,
            L1 = 0xE007, L2 = 0xE008, L3 = 0xE009, LSTICK = 0xE00A, R1 = 0xE00B, R2 = 0xE00C, R3 = 0xE00D, RSTICK = 0xE00E;
}
