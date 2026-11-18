package com.racacit.recomp;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import java.io.File;
import java.io.IOException;
import java.util.Arrays;

/**
 * A level's walkable ground, from the player's copy of the game, for the map
 * page. region.dat's section 0x19500 holds the navigation polygons the game's
 * AI paths over: 48-byte rows, a plane (normal, distance), then the polygon's
 * centre and radius (floats 8..11), y up. Drawn once as discs shaded by height
 * into a picture of the whole level; the page shows the part around the player.
 */
final class LevelMap {
    /** Level id (the word at 0x00EF7C90) -> its folder under packed/levels. */
    private static final String[] FOLDERS = { "", "great_clock_a", "zolar_forest", "space_sector_1", "galacton_ship",
        "great_clock_b", "space_sector_2", "molonoth", "axiom_city", "great_clock_c", "space_sector_3", "krell_canyon",
        "agorian_arena", "tombli", "great_clock_d", "space_sector_4", "valkyrie_fleet", "nefarious_station",
        "space_sector_5", "gimlick_valley", "great_clock_e", "insomniac_museum" };
    private static final String[] NAMES = { "", "THE GREAT CLOCK", "ZOLAR FOREST", "SPACE", "VORSELON'S SHIP",
        "THE GREAT CLOCK", "SPACE", "MOLONOTH FIELDS", "AXIOM CITY", "THE GREAT CLOCK", "SPACE", "KRELL CANYON",
        "AGORIAN BATTLEPLEX", "TOMBLI OUTPOST", "THE GREAT CLOCK", "SPACE", "VALKYRIE FLEET", "NEFARIOUS STATION",
        "SPACE", "GIMLICK VALLEY", "THE GREAT CLOCK", "INSOMNIAC MUSEUM" };
    private static final int MAX_SIDE = 1024;
    /* low ground to high, the quick select's navy towards a pale blue */
    private static final int LOW = 0xFF2A3F78, HIGH = 0xFF8CC4EE;

    final int level;
    final Bitmap image;
    /** World x/z of the picture's top left, and its pixels per world unit. */
    final float x0, z0, scale;

    private LevelMap(int level, Bitmap image, float x0, float z0, float scale) {
        this.level = level; this.image = image; this.x0 = x0; this.z0 = z0; this.scale = scale;
    }

    static String name(int level) { return level > 0 && level < NAMES.length ? NAMES[level] : ""; }

    static LevelMap load(File gameRoot, int level) throws IOException {
        if (level <= 0 || level >= FOLDERS.length) throw new IOException("no level " + level);
        String folder = FOLDERS[level];
        byte[] d;
        try (Psarc p = new Psarc(new File(gameRoot, "PS3_GAME/USRDIR/packed/levels/" + folder + "/level_cached.psarc"))) {
            d = p.read("/built/levels/" + folder + "/default/region.dat");
        }
        if (d == null) throw new IOException(folder + " has no region.dat");
        int[] s = GlyphCore.section(d, 0x19500);
        if (s == null || s[1] < 48 || s[2] == 0) throw new IOException(folder + ": no navigation polygons");
        int n = s[2];
        float[] x = new float[n], y = new float[n], z = new float[n], r = new float[n];
        float minX = Float.MAX_VALUE, maxX = -Float.MAX_VALUE, minZ = Float.MAX_VALUE, maxZ = -Float.MAX_VALUE;
        for (int i = 0; i < n; i++) {
            int o = s[0] + i * s[1];
            if (o + 48 > d.length) { n = i; break; }
            x[i] = f(d, o + 32); y[i] = f(d, o + 36); z[i] = f(d, o + 40); r[i] = f(d, o + 44);
            if (!(Math.abs(x[i]) < 1e5f && Math.abs(z[i]) < 1e5f && r[i] > 0 && r[i] < 1e3f)) { r[i] = 0; continue; }
            minX = Math.min(minX, x[i] - r[i]); maxX = Math.max(maxX, x[i] + r[i]);
            minZ = Math.min(minZ, z[i] - r[i]); maxZ = Math.max(maxZ, z[i] + r[i]);
        }
        if (!(maxX > minX && maxZ > minZ)) throw new IOException(folder + ": empty map");
        float pad = 4, scale = MAX_SIDE / Math.max(maxX - minX + 2 * pad, maxZ - minZ + 2 * pad);
        float x0 = minX - pad, z0 = minZ - pad;
        Bitmap b = Bitmap.createBitmap(Math.max(1, Math.round((maxX - minX + 2 * pad) * scale)),
                                       Math.max(1, Math.round((maxZ - minZ + 2 * pad) * scale)), Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        /* height shading between the 2nd and 98th percentile, lowest drawn first */
        Integer[] order = new Integer[n];
        for (int i = 0; i < n; i++) order[i] = i;
        final float[] ys = y;
        Arrays.sort(order, (a, bb) -> Float.compare(ys[a], ys[bb]));
        float lo = y[order[Math.min(n - 1, n / 50)]], hi = y[order[Math.max(0, n - 1 - n / 50)]];
        Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG); edge.setColor(0xFF101A30);
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        for (int k : order) if (r[k] > 0) c.drawCircle((x[k] - x0) * scale, (z[k] - z0) * scale, r[k] * scale + 1.5f, edge);
        for (int k : order) {
            if (r[k] <= 0) continue;
            float t = hi > lo ? Math.max(0, Math.min(1, (y[k] - lo) / (hi - lo))) : 0.5f;
            fill.setColor(mix(LOW, HIGH, t));
            c.drawCircle((x[k] - x0) * scale, (z[k] - z0) * scale, r[k] * scale, fill);
        }
        return new LevelMap(level, b, x0, z0, scale);
    }

    private static float f(byte[] d, int o) { return Float.intBitsToFloat(GlyphCore.be32(d, o)); }

    private static int mix(int a, int b, float t) {
        int r = Math.round((a >> 16 & 0xFF) + ((b >> 16 & 0xFF) - (a >> 16 & 0xFF)) * t);
        int g = Math.round((a >> 8 & 0xFF) + ((b >> 8 & 0xFF) - (a >> 8 & 0xFF)) * t);
        int bl = Math.round((a & 0xFF) + ((b & 0xFF) - (a & 0xFF)) * t);
        return 0xFF000000 | r << 16 | g << 8 | bl;
    }
}
