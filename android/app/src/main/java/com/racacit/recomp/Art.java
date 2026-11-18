package com.racacit.recomp;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The game's own art from PS3_GAME, as the Windows launcher uses it
 * (launcher/src/store.cpp refresh_art): PIC1.PNG, the key art behind the
 * launcher and the companion, and ICON0.PNG, the game's tile. Copied into
 * assets/art at import (the game folder may move or be a card that is out),
 * decoded at the size of the screen, kept for the process.
 */
final class Art {
    private Art() {}
    private static volatile Bitmap pic1, icon0;
    private static int pic1Max;

    static File dir(Store s) { return new File(s.base, "assets/art"); }

    // UI peeks must not take the decoder's monitor or touch storage. The full
    // resolution loaders below run on a worker and publish completed bitmaps.
    static Bitmap cachedKeyArt() { return pic1; }
    static Bitmap cachedIcon() { return icon0; }

    /** Copies PIC1.PNG and ICON0.PNG from the game into assets/art; a line for the import report. */
    static String refresh(Store s) {
        File pg = new File(s.gameRoot(), "PS3_GAME"), out = dir(s);
        int n = 0;
        if (out.isDirectory() || out.mkdirs()) {
            for (String[] f : new String[][] { { "PIC1.PNG", "pic1.png" }, { "ICON0.PNG", "icon0.png" } }) {
                File src = new File(pg, f[0]);
                if (!src.isFile()) continue;
                try (InputStream in = new FileInputStream(src); OutputStream o = new FileOutputStream(new File(out, f[1]))) {
                    Store.copy(in, o); n++;
                } catch (IOException ignored) { }
            }
        }
        synchronized (Art.class) { pic1 = null; icon0 = null; }
        return n == 0 ? "The game folder has no PIC1.PNG / ICON0.PNG (no key art)." : "The game's key art and icon taken for the launcher.";
    }

    /** True once the art is in assets/art, or there is none in the game to take. */
    static boolean present(Store s) {
        return new File(dir(s), "pic1.png").isFile() || !new File(s.gameRoot(), "PS3_GAME/PIC1.PNG").isFile();
    }

    /** The key art, decoded no larger than about maxSide pixels on its longer side; null before an import. */
    static synchronized Bitmap keyArt(Store s, int maxSide) {
        if (pic1 != null && pic1Max >= maxSide) return pic1;
        pic1 = decode(new File(dir(s), "pic1.png"), maxSide);
        pic1Max = maxSide;
        return pic1;
    }

    /** The game's tile (ICON0, 320x176); null before an import. */
    static synchronized Bitmap icon(Store s) {
        if (icon0 == null) icon0 = decode(new File(dir(s), "icon0.png"), 640);
        return icon0;
    }

    private static Bitmap decode(File f, int maxSide) {
        if (!f.isFile()) return null;
        maxSide = Math.max(1, Math.min(2048, maxSide));
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(f.getPath(), o);
        if (o.outWidth <= 0 || o.outHeight <= 0 || o.outWidth > 32768 || o.outHeight > 32768) return null;
        int side = Math.max(o.outWidth, o.outHeight), k = 1;
        while (side / (k * 2) >= maxSide) k *= 2;
        BitmapFactory.Options d = new BitmapFactory.Options();
        d.inSampleSize = k;
        try { return BitmapFactory.decodeFile(f.getPath(), d); } catch (OutOfMemoryError e) { return null; }
    }
}
