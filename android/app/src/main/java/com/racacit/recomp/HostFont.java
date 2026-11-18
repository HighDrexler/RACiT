package com.racacit.recomp;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import java.nio.ByteBuffer;

/**
 * The font of the game's own overlays (the save/load screen, the settings
 * panel, the FPS counter), drawn by Android's text engine for the native side
 * (rsx_host_ui.c font_build_android, via android_companion.cpp). Laid out as
 * the Windows build lays out Segoe UI with GDI: printable ASCII in rows of a
 * 1024-wide alpha atlas, each glyph's cell with a little padding either side.
 */
final class HostFont {
    private HostFont() { }

    /** meta: atlas w, h, line height, ascent, then x, y, w, h, xoff, advance
     *  for characters 32..126. Returns the atlas (w*h bytes of coverage). */
    static byte[] build(int px, int[] meta) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        p.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        p.setTextSize(px);
        p.setColor(0xFFFFFFFF);
        Paint.FontMetrics fm = p.getFontMetrics();
        final int asc = (int) Math.ceil(-fm.ascent), lh = (int) Math.ceil(fm.descent - fm.ascent) + 2;
        final int pad = px / 6 + 2, aw = 1024, rowH = lh + 2;
        int x = 8, y = 0;
        int[] gx = new int[95], gy = new int[95], gw = new int[95], adv = new int[95];
        for (int c = 0; c < 95; c++) {
            String ch = String.valueOf((char) (32 + c));
            adv[c] = Math.round(p.measureText(ch));
            int w = adv[c] + 2 * pad;
            if (x + w > aw) { x = 8; y += rowH; }
            gx[c] = x; gy[c] = y; gw[c] = w; x += w;
        }
        final int ah = y + rowH + 8;
        Bitmap b = Bitmap.createBitmap(aw, ah, Bitmap.Config.ALPHA_8);
        Canvas cv = new Canvas(b);
        for (int c = 0; c < 95; c++) {
            cv.drawText(String.valueOf((char) (32 + c)), gx[c] + pad, gy[c] + 1 + asc, p);
            int o = 4 + c * 6;
            meta[o] = gx[c]; meta[o + 1] = gy[c]; meta[o + 2] = gw[c]; meta[o + 3] = lh + 2;
            meta[o + 4] = -pad; meta[o + 5] = adv[c];
        }
        meta[0] = aw; meta[1] = ah; meta[2] = lh + 2; meta[3] = asc + 1;
        byte[] out = new byte[aw * ah];
        if (b.getRowBytes() == aw) {
            b.copyPixelsToBuffer(ByteBuffer.wrap(out));
        } else {   /* padded rows: copy row by row */
            ByteBuffer buf = ByteBuffer.allocate(b.getRowBytes() * ah);
            b.copyPixelsToBuffer(buf);
            byte[] all = buf.array();
            for (int r = 0; r < ah; r++) System.arraycopy(all, r * b.getRowBytes(), out, r * aw, aw);
        }
        b.recycle();
        return out;
    }
}
