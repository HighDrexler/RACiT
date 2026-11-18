package com.racacit.recomp;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * A PSARC archive (the game's packed/ files), read only: the manifest is
 * entry 0 (the names, one per line), every entry is split into blocks that
 * are zlib-compressed unless storing them was smaller. Only the requested
 * entry's block table is paged in; importing a font need not retain the
 * entire game's archive index or a second copy of its decoded texture data.
 */
final class Psarc implements AutoCloseable {
    private final RandomAccessFile f;
    private static final int MAX_ENTRY = 256 << 20, MAX_MANIFEST = 16 << 20;
    private final int tocEnt, block, sizeBytes, nFiles;
    private final long fileLength, tocEnd, sizesOffset, sizeCount;
    final List<String> names = new ArrayList<>();

    Psarc(File file) throws IOException {
        f = new RandomAccessFile(file, "r");
        try {
            fileLength = f.length();
            byte[] h = new byte[32];
            f.readFully(h);
            if (h[0] != 'P' || h[1] != 'S' || h[2] != 'A' || h[3] != 'R') throw new IOException("not a PSARC: " + file);
            if (h[8] != 'z' || h[9] != 'l' || h[10] != 'i' || h[11] != 'b') throw new IOException("unsupported PSARC compression");
            tocEnd = Integer.toUnsignedLong(be(h, 12, 4));
            tocEnt = be(h, 16, 4); nFiles = be(h, 20, 4); block = be(h, 24, 4);
            if (tocEnt < 30 || nFiles < 1 || block < 1 || block > (16 << 20)) throw new IOException("invalid PSARC header");
            sizesOffset = 32L + (long) nFiles * tocEnt;
            sizeBytes = block <= 0x10000 ? 2 : block <= 0x1000000 ? 3 : 4;
            if (tocEnd < sizesOffset || tocEnd > fileLength || (tocEnd - sizesOffset) % sizeBytes != 0)
                throw new IOException("invalid PSARC index bounds");
            sizeCount = (tocEnd - sizesOffset) / sizeBytes;
            String manifest = new String(read(0, MAX_MANIFEST), StandardCharsets.ISO_8859_1);
            if (!manifest.isEmpty()) for (String s : manifest.split("\n")) names.add(s);
            if (names.size() > nFiles - 1) throw new IOException("PSARC manifest exceeds file count");
        } catch (IOException | RuntimeException | Error failure) {
            // A failed constructor cannot be closed by the caller's try-with-resources.
            try { f.close(); } catch (IOException close) { failure.addSuppressed(close); }
            throw failure;
        }
    }

    static int be(byte[] d, int o, int n) { int v = 0; for (int i = 0; i < n; i++) v = v << 8 | (d[o + i] & 0xFF); return v; }
    static long be5(byte[] d, int o) { long v = 0; for (int i = 0; i < 5; i++) v = v << 8 | (d[o + i] & 0xFF); return v; }

    /** Entry i (0 = the manifest; file k of names is entry k + 1), whole. */
    byte[] read(int i) throws IOException {
        return read(i, MAX_ENTRY);
    }

    private byte[] read(int i, int limit) throws IOException {
        if (i < 0 || i >= nFiles) throw new IOException("PSARC entry outside index");
        byte[] entry = new byte[30];
        f.seek(32L + (long) i * tocEnt); f.readFully(entry);
        long idx = Integer.toUnsignedLong(be(entry, 16, 4));
        long ulen = be5(entry, 20), off = be5(entry, 25);
        long count = (ulen + block - 1) / block;
        if (ulen > limit) throw new IOException("PSARC entry too large");
        if (idx > sizeCount || count > sizeCount - idx || off < tocEnd || off > fileLength)
            throw new IOException("PSARC entry outside archive");
        if (count == 0) return new byte[0];

        // One destination, one reusable compressed block, and at most 64 KiB
        // of size records. In particular, no ByteArrayOutputStream final copy.
        byte[] out = new byte[(int) ulen];
        int pageEntries = (int) Math.min(count, 65536 / sizeBytes);
        byte[] sizes = new byte[pageEntries * sizeBytes];
        byte[] compressed = null, extra = new byte[1];
        Inflater inf = new Inflater();
        try {
            int at = 0;
            for (long b = 0; b < count; b++) {
                int slot = (int) (b % pageEntries);
                if (slot == 0) {
                    f.seek(sizesOffset + (idx + b) * sizeBytes);
                    f.readFully(sizes, 0, (int) Math.min(pageEntries, count - b) * sizeBytes);
                    f.seek(off);
                }
                int csz = be(sizes, slot * sizeBytes, sizeBytes);
                if (csz == 0) csz = block;
                int want = Math.min(block, out.length - at);
                if (csz < 0 || csz > block || csz > fileLength - off) throw new IOException("invalid PSARC block bounds");
                if (csz == want || csz == block) {
                    f.readFully(out, at, want);
                    if (csz > want) f.seek(off + csz); // Stored full block with final-block padding.
                } else {
                    if (compressed == null) compressed = new byte[block];
                    f.readFully(compressed, 0, csz);
                    inf.reset(); inf.setInput(compressed, 0, csz);
                    int got = 0;
                    try {
                        while (got < want) {
                            int n = inf.inflate(out, at + got, want - got);
                            if (n == 0) throw new IOException("incomplete PSARC compressed block");
                            got += n;
                        }
                        // Verify checksum/end and reject an overlong decoded block.
                        if ((!inf.finished() && inf.inflate(extra) != 0) || !inf.finished() || inf.getRemaining() != 0)
                            throw new IOException("invalid PSARC compressed block length");
                    } catch (DataFormatException bad) { throw new IOException("damaged PSARC compressed block", bad); }
                }
                at += want; off += csz;
            }
        } finally { inf.end(); }
        return out;
    }

    /** The file named exactly (with or without the leading '/'), or null. */
    byte[] read(String name) throws IOException {
        String n = name.startsWith("/") ? name : "/" + name;
        for (int k = 0; k < names.size(); k++) if (names.get(k).equals(n) || names.get(k).equals(name)) return read(k + 1);
        return null;
    }

    @Override public void close() throws IOException { f.close(); }
}
