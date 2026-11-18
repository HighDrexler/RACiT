package com.racacit.recomp;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** Transactional extraction of an ISO's game folder; the selected ISO is always read-only. */
final class GameIsoImport {
    private static final Object COMMIT_LOCK = new Object();
    interface Progress { boolean cancelled(); void update(String path, long done, long total); }
    interface Validator { void validate(File stagedRoot) throws IOException; }
    static final class KeyRequired extends IOException { KeyRequired() { super("The image's game files are encrypted. Select the .dkey from your own dump."); } }

    static void recover(File base) throws IOException {
        synchronized (COMMIT_LOCK) {
            File previous = new File(base, ".iso-previous-game"), current = new File(base, "PS3_GAME");
            if (previous.exists() && !current.exists() && !previous.renameTo(current))
                throw new IOException("Could not recover the previous game folder from " + previous);
        }
    }

    static long importInto(IsoImage image, File base, Progress progress, Validator validator) throws IOException {
        inspect(image);
        recover(base);
        File previous = new File(base, ".iso-previous-game"), current = new File(base, "PS3_GAME");
        if (previous.exists()) {
            Store.deleteTree(previous);
            if (previous.exists()) throw new IOException("Could not clear an older completed game import.");
        }
        List<IsoImage.Entry> files = new ArrayList<>(); long bytes = 0;
        for (IsoImage.Entry entry : image.files()) {
            if (!entry.path.startsWith("PS3_GAME/")) continue;
            if (entry.size > Long.MAX_VALUE - bytes) throw new IOException("The image's total file size overflows.");
            bytes += entry.size; files.add(entry);
        }
        if (files.isEmpty()) throw new IOException("The image has no PS3_GAME files.");
        long free = base.getUsableSpace();
        if (bytes > free - (512L << 20)) throw new IOException("Not enough free space for a complete staged import: needs " + Store.size(bytes) + " plus 512 MB, available " + Store.size(free) + ".");
        final long total = bytes;
        File stage = Files.createTempDirectory(base.toPath(), ".iso-import-").toFile();
        long done = 0;
        try {
            for (IsoImage.Entry entry : files) {
                if (progress.cancelled()) throw new IOException("Import cancelled.");
                final long before = done;
                image.extractTo(entry, stage, new IsoImage.Progress() {
                    @Override public boolean cancelled() { return progress.cancelled(); }
                    @Override public void update(String path, long fileDone, long fileTotal) { progress.update(path, before + fileDone, total); }
                });
                done += entry.size;
                progress.update(entry.path, done, total);
            }
            if (progress.cancelled()) throw new IOException("Import cancelled.");
            if (validator != null) validator.validate(stage);
            if (progress.cancelled()) throw new IOException("Import cancelled.");
            File ready = new File(stage, "PS3_GAME");
            if (!ready.isDirectory()) throw new IOException("The staged image has no game folder.");
            synchronized (COMMIT_LOCK) {
                if (current.exists() && !current.renameTo(previous)) throw new IOException("Could not preserve the currently imported game.");
                if (!ready.renameTo(current)) {
                    if (previous.exists() && !previous.renameTo(current))
                        throw new IOException("Import interrupted. Your original game remains in " + previous);
                    throw new IOException("Could not publish the new game; the previous game was kept.");
                }
            }
            Store.deleteTree(previous);
            return done;
        } finally { Store.deleteTree(stage); }
    }

    static void inspect(IsoImage image) throws IOException {
        IsoImage.Entry sfo = image.find("PS3_GAME/PARAM.SFO");
        if (sfo == null) throw new IOException("This ISO has no PS3_GAME/PARAM.SFO.");
        byte[] sfoBytes = image.read(sfo, 65536);
        if (sfoBytes.length < 4 || sfoBytes[0] != 0 || sfoBytes[1] != 'P' || sfoBytes[2] != 'S' || sfoBytes[3] != 'F') encryptedOrInvalid(image);
        String title = titleId(sfoBytes);
        if (!"BCUS98124".equals(title)) throw new IOException("This ISO is " + (title == null ? "missing a valid title ID" : title) + "; this build requires A Crack in Time USA (BCUS98124).");
        IsoImage.Entry program = image.find("PS3_GAME/USRDIR/EBOOT.BIN");
        if (program == null) program = image.find("PS3_GAME/USRDIR/EBOOT.elf");
        if (program == null) throw new IOException("The ISO has no EBOOT.BIN or EBOOT.elf.");
        byte[] head = image.read(program, 4);
        if (!(head.length == 4 && ((head[0] == 'S' && head[1] == 'C' && head[2] == 'E' && head[3] == 0)
                || (head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F')))) encryptedOrInvalid(image);
        IsoImage.Entry archive = null; boolean built = false;
        for (IsoImage.Entry entry : image.files()) {
            if (archive == null && entry.path.startsWith("PS3_GAME/USRDIR/packed/") && entry.path.endsWith(".psarc")) archive = entry;
            if (entry.path.startsWith("PS3_GAME/USRDIR/built/")) built = true;
        }
        if (archive == null || !built) throw new IOException("The ISO is missing the game's packed archives or built files.");
        byte[] magic = image.read(archive, 4);
        if (!(magic.length == 4 && magic[0] == 'P' && magic[1] == 'S' && magic[2] == 'A' && magic[3] == 'R')) encryptedOrInvalid(image);
    }

    private static void encryptedOrInvalid(IsoImage image) throws IOException {
        if (image.hasEncryptionRegions() && !image.keyed()) throw new KeyRequired();
        throw new IOException(image.keyed() ? "The selected disc key does not decrypt this game's expected file headers, or the image is damaged."
                : "The image's game headers are unreadable; the ISO may be encrypted, incomplete or damaged.");
    }

    private static String titleId(byte[] d) throws IOException {
        if (d.length < 20) return null;
        int keys = le32(d, 8), data = le32(d, 12), count = le32(d, 16);
        if (count < 0 || count > 4096 || keys < 20 || data < keys || data > d.length || (long) count * 16 + 20 > d.length) throw new IOException("Invalid PARAM.SFO table.");
        for (int i = 0; i < count; i++) {
            int p = 20 + i * 16, ko = keys + (d[p] & 255 | (d[p + 1] & 255) << 8);
            int len = le32(d, p + 4), rel = le32(d, p + 12);
            if (ko < keys || ko >= data || len < 0 || rel < 0 || (long) data + rel + len > d.length) throw new IOException("Invalid PARAM.SFO entry.");
            int end = ko; while (end < data && d[end] != 0) end++;
            if (end == data) throw new IOException("Unterminated PARAM.SFO key.");
            if (!new String(d, ko, end - ko, StandardCharsets.US_ASCII).equals("TITLE_ID")) continue;
            int start = data + rel, z = start; while (z < start + len && d[z] != 0) z++;
            return new String(d, start, z - start, StandardCharsets.US_ASCII);
        }
        return null;
    }
    private static int le32(byte[] b, int p) { return b[p] & 255 | (b[p + 1] & 255) << 8 | (b[p + 2] & 255) << 16 | (b[p + 3] & 255) << 24; }
}
