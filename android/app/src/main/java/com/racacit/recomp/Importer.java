package com.racacit.recomp;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.StatFs;
import android.provider.DocumentsContract;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BooleanSupplier;

/**
 * Brings the player's own copy of the game in: the folder that holds
 * PS3_GAME (a decrypted disc dump with PS3_GAME/USRDIR/EBOOT.elf), picked
 * with the system folder picker. Either copied into the app's folder, or
 * used where it is (needs "All files access"; the launcher then writes
 * GAME=<path> to settings.txt).
 */
final class Importer {
    interface Progress { void update(String what, long done, long total); boolean cancelled(); }
    interface FileMove { boolean move(File from, File to); }
    private static final Object COMMIT_LOCK = new Object();
    private static final String PREVIOUS_GAME = ".folder-previous-game";

    private static final class Doc { String id, name, mime; long size; Doc(String i, String n, String m, long s) { id = i; name = n; mime = m; size = s; } }

    private final Context ctx;
    private final ContentResolver cr;
    private final Uri tree;
    private final FileMove fileMove;

    Importer(Context c, Uri tree) { this(c, tree, File::renameTo); }
    Importer(Context c, Uri tree, FileMove fileMove) {
        ctx = c; cr = c.getContentResolver(); this.tree = tree; this.fileMove = fileMove;
    }

    static void recover(File base) throws IOException { recover(base, File::renameTo); }
    private static void recover(File base, FileMove move) throws IOException {
        synchronized (COMMIT_LOCK) {
            File previous = new File(base, PREVIOUS_GAME), current = new File(base, "PS3_GAME");
            if (previous.exists() && !current.exists() && !move.move(previous, current))
                throw new IOException("Could not restore the previous game. It remains in " + previous);
        }
    }

    /** Discard only a genuinely abandoned partial copy, never failed recovery data. */
    static void cleanupInterruptedCopy(File base, BooleanSupplier importBusy) {
        synchronized (COMMIT_LOCK) {
            // Read the live guard here, not when the launcher queues the task.
            // A new copy sets busy before starting and takes this same lock in
            // recover() before it can touch staging; cleanup therefore finishes
            // first or observes that copy in progress. Do not try recovery here.
            if (importBusy.getAsBoolean() || new File(base, PREVIOUS_GAME).exists()) return;
            Store.deleteTree(new File(base, "PS3_GAME.incoming"));
        }
    }

    private void publish(File base, File incoming) throws IOException {
        synchronized (COMMIT_LOCK) {
            File current = new File(base, "PS3_GAME"), previous = new File(base, PREVIOUS_GAME);
            if (previous.exists()) throw new IOException("An unresolved previous game remains in " + previous);
            boolean hadPrevious = current.exists();
            if (hadPrevious && !fileMove.move(current, previous))
                throw new IOException("Could not preserve the previous game. It is still in " + current);
            boolean published = false;
            RuntimeException moveFailure = null;
            try { published = fileMove.move(incoming, current); }
            catch (RuntimeException failure) { moveFailure = failure; }
            if (!published) {
                boolean restored = !hadPrevious;
                try { if (hadPrevious) restored = fileMove.move(previous, current); }
                catch (RuntimeException failure) { if (moveFailure == null) moveFailure = failure; }
                if (!restored)
                    throw new IOException("Import publication and recovery failed. Your previous game remains in "
                            + previous + "; the new copy remains in " + incoming + ". Open the launcher again to retry recovery.", moveFailure);
                throw new IOException("Could not publish the new copy. " + (hadPrevious ? "The previous game was kept; " : "No installed game was changed; ")
                        + "the new copy remains in " + incoming, moveFailure);
            }
            // Delete only after the complete incoming copy is at its final name.
            Store.deleteTree(previous);
        }
    }

    private List<Doc> children(String parentId) throws IOException {
        List<Doc> out = new ArrayList<>();
        String[] proj = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                          DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE };
        try (Cursor c = cr.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId), proj, null, null, null)) {
            if (c == null) throw new IOException("the folder could not be listed");
            while (c.moveToNext()) out.add(new Doc(c.getString(0), c.getString(1), c.getString(2), c.isNull(3) ? 0 : c.getLong(3)));
        }
        return out;
    }

    private static boolean isDir(Doc d) { return DocumentsContract.Document.MIME_TYPE_DIR.equals(d.mime); }

    /** "global_uncached.psarc.001": a piece of a split transfer, not a game file. */
    private static boolean splitPiece(Doc d) { return d.name.matches(".*\\.psarc\\.\\d{3}$"); }

    /** The PS3_GAME folder inside the pick (or the pick itself), or null. */
    private Doc findPs3Game() throws IOException {
        String root = DocumentsContract.getTreeDocumentId(tree);
        String rootName = root.substring(root.lastIndexOf('/') + 1);
        if (rootName.endsWith("PS3_GAME") || rootName.equals("PS3_GAME")) return new Doc(root, "PS3_GAME", DocumentsContract.Document.MIME_TYPE_DIR, 0);
        for (Doc d : children(root)) if (isDir(d) && d.name.equals("PS3_GAME")) return d;
        /* one level further down: "<pick>/<game name>/PS3_GAME" */
        for (Doc d : children(root)) if (isDir(d)) for (Doc e : children(d.id)) if (isDir(e) && e.name.equals("PS3_GAME")) return e;
        return null;
    }

    private long measure(String id, Progress p) throws IOException {
        long t = 0;
        for (Doc d : children(id)) {
            if (p.cancelled()) throw new IOException("cancelled");
            if (!isDir(d) && splitPiece(d)) continue;
            t += isDir(d) ? measure(d.id, p) : d.size;
        }
        return t;
    }

    /** Copies PS3_GAME into dest/PS3_GAME. Returns the bytes copied. */
    long copyInto(File dest, Progress p) throws IOException {
        recover(dest, fileMove);
        // Both names can remain after a successful commit interrupted during
        // cleanup. Recovery above never deletes a stranded previous install.
        File previous = new File(dest, PREVIOUS_GAME);
        if (previous.exists()) {
            Store.deleteTree(previous);
            if (previous.exists()) throw new IOException("Could not clear the previous completed import in " + previous);
        }
        Doc g = findPs3Game();
        if (g == null) throw new IOException("No PS3_GAME folder in the folder you picked. Pick the folder that contains PS3_GAME.");
        p.update("Measuring your copy...", 0, 0);
        long total = measure(g.id, p);
        File target = new File(dest, "PS3_GAME.incoming");
        Store.deleteTree(target);   /* before measuring the free space: an earlier copy's leftovers */
        long free = new StatFs(dest.getPath()).getAvailableBytes();
        if (total + (512L << 20) > free)
            throw new IOException("Not enough space: the game needs " + Store.size(total) + ", " + Store.size(free) + " is free. "
                                  + "Free some space, or use the game where it is instead.");
        long[] done = { 0 };
        /* cancelled or failed: the part copied so far (up to 25 GB) goes, the game there before stays */
        // One transfer buffer for the entire operation, including nested folders.
        // Holding 4 MiB in each recursive frame multiplied peak heap by depth.
        try {
            copyTree(g.id, target, total, done, p, new byte[4 << 20]);
            if (p.cancelled()) throw new IOException("cancelled");
        }
        catch (IOException | RuntimeException e) { Store.deleteTree(target); throw e; }
        publish(dest, target);
        return done[0];
    }

    /**
     * Copies into dest/PS3_GAME only what is missing there or of another size:
     * the repair of an incomplete copy, without copying 25 GB again. Returns
     * the bytes copied (0: nothing was missing).
     */
    long repairInto(File dest, Progress p) throws IOException {
        recover(dest, fileMove);
        Doc g = findPs3Game();
        if (g == null) throw new IOException("No PS3_GAME folder in the folder you picked. Pick the folder that contains PS3_GAME.");
        p.update("Comparing your copy with the game here...", 0, 0);
        List<Object[]> todo = new ArrayList<>();
        long total = collectMissing(g.id, new File(dest, "PS3_GAME"), todo, p);
        if (todo.isEmpty()) return 0;
        long free = new StatFs(dest.getPath()).getAvailableBytes();
        if (total + (256L << 20) > free)
            throw new IOException("Not enough space: the missing files need " + Store.size(total) + ", " + Store.size(free) + " is free.");
        long[] done = { 0 };
        byte[] buf = new byte[4 << 20];
        for (Object[] t : todo) {
            Doc d = (Doc) t[0]; File f = (File) t[1];
            File parent = f.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            File part = new File(f.getPath() + ".part");
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id));
                 OutputStream out = new FileOutputStream(part)) {
                if (in == null) throw new IOException("cannot read " + d.name);
                copyBytes(in, out, buf, d.name, total, done, p);
            } catch (IOException | RuntimeException e) { part.delete(); throw e; }
            if (f.exists() && !f.delete()) throw new IOException("cannot replace " + f);
            if (!part.renameTo(f)) throw new IOException("cannot move " + part.getName() + " into place");
            p.update(d.name, done[0], total);
        }
        return done[0];
    }

    private long collectMissing(String id, File dst, List<Object[]> todo, Progress p) throws IOException {
        long t = 0;
        for (Doc d : children(id)) {
            if (p.cancelled()) throw new IOException("cancelled");
            File f = new File(dst, d.name);
            if (isDir(d)) { t += collectMissing(d.id, f, todo, p); continue; }
            if (splitPiece(d)) continue;
            if (f.isFile() && f.length() == d.size) continue;
            todo.add(new Object[] { d, f });
            t += d.size;
        }
        return t;
    }

    private void copyTree(String id, File dst, long total, long[] done, Progress p, byte[] buf) throws IOException {
        if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("cannot create " + dst);
        for (Doc d : children(id)) {
            if (p.cancelled()) throw new IOException("cancelled");
            File f = new File(dst, d.name);
            if (isDir(d)) { copyTree(d.id, f, total, done, p, buf); continue; }
            if (splitPiece(d)) continue;
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id));
                 OutputStream out = new FileOutputStream(f)) {
                if (in == null) throw new IOException("cannot read " + d.name);
                copyBytes(in, out, buf, d.name, total, done, p);
            }
            p.update(d.name, done[0], total);
        }
    }

    private static void copyBytes(InputStream in, OutputStream out, byte[] buf, String name,
                                  long total, long[] done, Progress p) throws IOException {
        long last = done[0];
        while (true) {
            // Check each buffer, even for an unknown-length or slow provider.
            if (p.cancelled()) throw new IOException("cancelled");
            int n = in.read(buf);
            if (n < 0) break;
            if (n == 0) {
                int one = in.read();
                if (one < 0) break;
                out.write(one); done[0]++;
            } else { out.write(buf, 0, n); done[0] += n; }
            if (done[0] - last > (32 << 20)) { last = done[0]; p.update(name, done[0], total); }
        }
    }

    /**
     * The real path of the picked folder (the one holding PS3_GAME), for
     * using it in place, or null when the provider is not local storage.
     */
    String folderPath() throws IOException {
        Doc g = findPs3Game();
        if (g == null) throw new IOException("No PS3_GAME folder in the folder you picked.");
        String id = g.id;                          /* e.g. "primary:Games/ACiT/PS3_GAME" */
        if (!"com.android.externalstorage.documents".equals(tree.getAuthority())) return null;
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String vol = id.substring(0, colon), rel = id.substring(colon + 1);
        String base = vol.equals("primary") ? Environment.getExternalStorageDirectory().getPath() : "/storage/" + vol;
        File ps3game = new File(base, rel);
        return ps3game.getParent();
    }
}
