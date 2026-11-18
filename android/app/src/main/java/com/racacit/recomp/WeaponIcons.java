package com.racacit.recomp;

import android.graphics.Bitmap;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * The quick select's own icons, read from the player's copy of the game. The
 * menu asks for img://icon_<id>; those are 64x64 DXT1 pictures, white on black
 * for the game to tint, in one texture pack in global_cached.psarc. Kept here
 * as alpha masks, so a Paint's colour does the same job.
 */
final class WeaponIcons {
    private WeaponIcons() {}

    private static final String PACK = "/built/tuids/340/34068c590791cf44/";
    /** Weapon id -> row of the pack's texture table (the game's item records, +4, through its image table). */
    private static final int[] ROWS = { 26, 117, 45, 21, 23, 116, 22, 112, 118, 109, 111, 107, 119, 114, 24, 110,
                                        113, 108, 115, 25, 121, 120, 196 };
    /** The US names, at level 1. WPN_BOOPER's name is not known; it was not seen on a quick select. */
    private static final String[] NAMES = { "OMNIWRENCH", "TIME BOMB", "MR. ZURKON", "BUZZ BLADES", "NEGOTIATOR", "SONIC ERUPTOR",
        "MAG-NET LAUNCHER", "CRYOMINE GLOVE", "PLASMA STRIKER", "DYNAMO OF DOOM", "RIFT INDUCER 5000", "TESLA SPIKES",
        "GROOVITRON GLOVE", "CHIMP-O-MATIC", "RYNO V", "SPIRAL OF DEATH", "CONSTRUCTO PISTOL", "CONSTRUCTO BOMB",
        "CONSTRUCTO SHOTGUN", "SWINGSHOT", "OMNISOAKER", "HOVERBOOTS", "BOOPER" };

    static String name(int weapon) { return weapon >= 0 && weapon < NAMES.length ? NAMES[weapon] : ""; }

    /** One mask per weapon id, null where a picture could not be read. */
    static Bitmap[] load(File gameRoot) throws IOException {
        byte[] header, texel;
        try (Psarc p = new Psarc(new File(gameRoot, GlyphCore.ARCHIVE))) {
            header = p.read(PACK + "header.dat"); texel = p.read(PACK + "texel.dat");
        }
        if (header == null || texel == null) throw new IOException("the game's icons were not found in global_cached");
        int[] rows = GlyphCore.section(header, 0x5200), pages = GlyphCore.section(texel, 0x5300);
        if (rows == null || pages == null) throw new IOException("unexpected icon layout");
        Bitmap[] out = new Bitmap[ROWS.length];
        for (int id = 0; id < ROWS.length; id++) {
            if (ROWS[id] >= rows[2]) continue;
            out[id] = mask(GlyphCore.decodePage(header, rows[0] + ROWS[id] * 0x20, texel, pages[0]));
        }
        return out;
    }

    private static Bitmap mask(GlyphCore.Image im) {
        byte[] a = new byte[im.w * im.h];
        for (int i = 0; i < a.length; i++) {
            int c = im.argb[i];
            a[i] = (byte) Math.max(c >> 16 & 0xFF, Math.max(c >> 8 & 0xFF, c & 0xFF));
        }
        Bitmap b = Bitmap.createBitmap(im.w, im.h, Bitmap.Config.ALPHA_8);
        b.copyPixelsFromBuffer(ByteBuffer.wrap(a));
        return b;
    }
}
