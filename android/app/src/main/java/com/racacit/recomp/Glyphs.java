package com.racacit.recomp;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;

/** The game's button icons as PNGs in assets/glyphs (GlyphCore extracts them at import). */
final class Glyphs {
    private Glyphs() {}
    private static final Map<Integer, Bitmap> cache = new HashMap<>();

    /** Extracts every button icon from the game into dir; returns how many. */
    static int extract(File gameRoot, File dir) throws IOException {
        Map<Integer, GlyphCore.Image> m = GlyphCore.extract(gameRoot);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        for (Map.Entry<Integer, GlyphCore.Image> e : m.entrySet()) {
            GlyphCore.Image im = e.getValue();
            Bitmap b = Bitmap.createBitmap(im.argb, im.w, im.h, Bitmap.Config.ARGB_8888);
            try (OutputStream o = new FileOutputStream(new File(dir, GlyphCore.fileName(e.getKey())))) {
                b.compress(Bitmap.CompressFormat.PNG, 100, o);
            }
            b.recycle();
        }
        synchronized (cache) { cache.clear(); }
        return m.size();
    }

    /** The icon for a button code point (GlyphCore.CROSS ...), or null before an import. */
    static Bitmap get(File dir, int cp) {
        synchronized (cache) {
            if (cache.containsKey(cp)) return cache.get(cp);
            File f = new File(dir, GlyphCore.fileName(cp));
            Bitmap b = f.exists() ? BitmapFactory.decodeFile(f.getPath()) : null;
            cache.put(cp, b);
            return b;
        }
    }

    static boolean present(File dir) { return new File(dir, GlyphCore.fileName(GlyphCore.CROSS)).exists(); }
}
