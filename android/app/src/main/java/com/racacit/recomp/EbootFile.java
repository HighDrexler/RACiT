package com.racacit.recomp;

import java.io.*;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/** Validates and stages a user's decrypted PS3 program without changing their source file. */
final class EbootFile {
    static final long MAX_BYTES = 512L << 20;
    static final String KNOWN_SHA256 = "406bded328d9dbdfa9684ed392554ca34c76f4ed95aa21685f622d3ab2b04592";

    static final class Candidate implements AutoCloseable {
        final File file;
        final String sha256;
        Candidate(File file, String sha256) { this.file = file; this.sha256 = sha256; }
        boolean known() { return KNOWN_SHA256.equals(sha256); }
        void install(File target) throws IOException {
            Files.move(file.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        }
        @Override public void close() { file.delete(); }
    }

    static Candidate stage(InputStream source, File directory) throws IOException {
        if (source == null) throw new IOException("The selected program could not be opened.");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Could not create the app's program folder.");
        File part = File.createTempFile(".eboot-", ".pending", directory);
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[65536]; long count = 0;
            try (FileOutputStream out = new FileOutputStream(part)) {
                int n;
                while ((n = source.read(buffer)) != -1) {
                    count += n;
                    if (count > MAX_BYTES) throw new IOException("The program exceeds the 512 MB size limit.");
                    out.write(buffer, 0, n); digest.update(buffer, 0, n);
                }
                out.getFD().sync();
            }
            validate(part);
            StringBuilder hash = new StringBuilder(64);
            for (byte b : digest.digest()) hash.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            return new Candidate(part, hash.toString());
        } catch (IOException | RuntimeException | NoSuchAlgorithmException e) {
            part.delete();
            if (e instanceof IOException) throw (IOException) e;
            throw new IOException("Could not validate the selected program.", e);
        }
    }

    static void validate(File file) throws IOException {
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            long size = in.length();
            if (size < 64 || size > MAX_BYTES) throw new IOException("The selected file is truncated or too large for a PS3 program.");
            byte[] h = new byte[64]; in.readFully(h);
            if (h[0] == 'S' && h[1] == 'C' && h[2] == 'E')
                throw new IOException("This is the encrypted EBOOT.BIN. Select the decrypted EBOOT.elf from your own dump.");
            if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') throw new IOException("The selected file is not an ELF program.");
            if (h[4] != 2 || h[5] != 2 || h[6] != 1 || u16(h, 18) != 21 || u16(h, 16) != 2)
                throw new IOException("This is not a big-endian 64-bit PowerPC PS3 executable.");
            long phoff = positive64(h, 32);
            int ehsize = u16(h, 52), stride = u16(h, 54), count = u16(h, 56);
            if (ehsize != 64 || stride < 56 || count < 1 || count > 4096 || phoff < 64 || phoff > size - (long) stride * count)
                throw new IOException("The ELF program-header table is invalid or truncated.");
            byte[] ph = new byte[56]; boolean load = false;
            for (int i = 0; i < count; i++) {
                in.seek(phoff + (long) i * stride); in.readFully(ph);
                if (u32(ph, 0) != 1) continue;
                load = true;
                long offset = positive64(ph, 8), address = positive64(ph, 16), filesz = positive64(ph, 32), memsz = positive64(ph, 40);
                if (filesz > memsz || offset > size || filesz > size - offset)
                    throw new IOException("The ELF has an invalid or truncated load segment.");
                if (address >= 0x1_0000_0000L || memsz > 0x1_0000_0000L - address)
                    throw new IOException("The ELF load segment extends outside the 32-bit PS3 address space.");
            }
            if (!load) throw new IOException("The ELF has no loadable program segment.");
        }
    }

    private static int u16(byte[] b, int p) { return (b[p] & 255) << 8 | b[p + 1] & 255; }
    private static long u32(byte[] b, int p) { return (long) u16(b, p) << 16 | u16(b, p + 2); }
    private static long positive64(byte[] b, int p) throws IOException {
        if (b[p] < 0) throw new IOException("The ELF contains an unsupported 64-bit offset.");
        return u32(b, p) << 32 | u32(b, p + 4);
    }
}
