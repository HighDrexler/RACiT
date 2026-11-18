package com.racacit.recomp;

import android.content.Context;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;

/**
 * Is the imported game complete? Compares it with the file list of the USA
 * release (assets/acit_bcus98124_files.txt: size and path of each of its 790
 * files, no game data). A missing file is a broken copy -- a phone once had
 * everything but packed/game/game.psarc and the game sat on a black screen,
 * while the launcher said "ready". A file of another size is only noted: a
 * different dump or region can differ. EBOOT.elf need only exist (its size
 * depends on the tool that decrypted it).
 */
final class GameCheck {
    final List<String> missing = new ArrayList<>(), different = new ArrayList<>();
    long expectedBytes, missingBytes;
    int expectedFiles;

    static GameCheck run(Context c, File root) {
        return run(c, root, null);
    }

    static GameCheck run(Context c, File root, File program) {
        GameCheck g = new GameCheck();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open("acit_bcus98124_files.txt")))) {
            String line;
            while ((line = r.readLine()) != null) {
                int sp = line.indexOf(' ');
                if (sp <= 0) continue;
                long size = Long.parseLong(line.substring(0, sp));
                String path = line.substring(sp + 1);
                g.expectedFiles++; g.expectedBytes += size;
                File f = program != null && path.equals("PS3_GAME/USRDIR/EBOOT.elf") ? program : new File(root, path);
                if (!f.isFile()) { g.missing.add(path); g.missingBytes += size; }
                else if (f.length() != size && !path.endsWith("/EBOOT.elf")) g.different.add(path);
            }
        } catch (Exception e) { g.expectedFiles = -1; }
        return g;
    }

    boolean known() { return expectedFiles > 0; }
    boolean complete() { return known() && missing.isEmpty(); }

    /** One line for the home screen. */
    String line() {
        if (!known()) return "Could not check the game's files.";
        if (complete()) return "All " + expectedFiles + " game files present" + (different.isEmpty() ? "." : " (" + different.size() + " of another size).");
        return missing.size() + " of " + expectedFiles + " game files missing (" + Store.size(missingBytes) + "), e.g. "
                + shortPath(missing.get(0)) + ". The game cannot load without them.";
    }

    /** The full report. */
    String report(int most) {
        StringBuilder b = new StringBuilder(line());
        if (!missing.isEmpty()) {
            b.append("\n\nMissing:\n");
            for (int i = 0; i < Math.min(most, missing.size()); i++) b.append("  ").append(shortPath(missing.get(i))).append('\n');
            if (missing.size() > most) b.append("  ... and ").append(missing.size() - most).append(" more\n");
        }
        if (!different.isEmpty()) {
            b.append("\nAnother size than the USA release (fine if your copy is another version):\n");
            for (int i = 0; i < Math.min(most, different.size()); i++) b.append("  ").append(shortPath(different.get(i))).append('\n');
            if (different.size() > most) b.append("  ... and ").append(different.size() - most).append(" more\n");
        }
        return b.toString();
    }

    static String shortPath(String p) { return p.startsWith("PS3_GAME/USRDIR/") ? p.substring(16) : p; }
}
