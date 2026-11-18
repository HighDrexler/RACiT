package com.racacit.recomp;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.GeneralSecurityException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

/**
 * Seekable ISO9660/Joliet reader, including level-3 multi-extent files and PS3
 * sector decryption. No mounting, shell commands, Android APIs or bundled keys.
 *
 * The image owns its Source/channel, including on an open/parse failure. An
 * Android caller can supply an AssetFileDescriptor's seekable FileChannel,
 * start offset and length. Pipes/cloud streams must first be copied locally.
 * Entries are immutable; the image itself is intended for one worker thread.
 *
 * A PS3 region table can survive predecryption. Without a supplied disc key,
 * bytes are returned as stored. A key enables AES-128-CBC for the indicated
 * sectors, each with its sector number as a big-endian 128-bit IV. CBC has no
 * authentication: callers must validate known file magic/title before import.
 */
public final class IsoImage implements Closeable {
    public interface Cancellation { boolean cancelled(); }
    public interface Progress extends Cancellation {
        void update(String path, long fileBytesDone, long fileBytesTotal);
    }
    public interface Source extends Closeable {
        long size() throws IOException;
        void readFully(long position, byte[] buffer, int offset, int length) throws IOException;
    }
    public static final class Entry {
        public final String path;
        public final long size;
        private final List<Extent> extents;
        private final Object owner;
        private Entry(String path, long size, List<Extent> extents, Object owner) {
            this.path = path; this.size = size; this.owner = owner;
            this.extents = Collections.unmodifiableList(new ArrayList<>(extents));
        }
    }
    private static final class Extent {
        final long sector, bytes;
        Extent(long sector, long bytes) { this.sector = sector; this.bytes = bytes; }
    }
    private static final class Record {
        String name;
        long sector, bytes;
        int flags;
        boolean special;
    }
    private static final class ChannelSource implements Source {
        private final FileChannel channel;
        private final long offset, length;
        ChannelSource(FileChannel channel, long offset, long length) throws IOException {
            this.channel = channel;
            long channelSize = channel.size();
            if (offset < 0 || offset > channelSize) throw new IOException("Invalid image descriptor offset");
            long available = channelSize - offset;
            if (length < 0) length = available;
            if (length > available) throw new EOFException("Image descriptor extends beyond the file");
            this.offset = offset; this.length = length;
        }
        public long size() { return length; }
        public void readFully(long position, byte[] buffer, int start, int count) throws IOException {
            if (position < 0 || count < 0 || position > length - count) throw new EOFException("Image ends early");
            ByteBuffer destination = ByteBuffer.wrap(buffer, start, count);
            long at = offset + position;
            while (destination.hasRemaining()) {
                int got = channel.read(destination, at);
                if (got < 0) throw new EOFException("Image ends early");
                if (got == 0) throw new IOException("Image source is not providing seekable file data");
                at += got;
            }
        }
        public void close() throws IOException { channel.close(); }
    }

    private static final int SECTOR = 2048;
    private static final int CHUNK_SECTORS = 512;
    private static final int MAX_FILES = 100000, MAX_DIRECTORIES = 10000, MAX_DEPTH = 32, MAX_EXTENTS = 64;
    private static final long MAX_DIRECTORY_BYTES = 64L << 20, MAX_TOTAL_BYTES = 1L << 40;
    private final Source source;
    private final Cancellation cancellation;
    private final Object owner = new Object();
    private final List<Entry> entries = new ArrayList<>();
    private final Map<String, Entry> lookup = new HashMap<>();
    private final Set<String> paths = new HashSet<>();
    private final Set<Long> directories = new HashSet<>();
    private final List<long[]> encrypted = new ArrayList<>();
    private long imageBytes, volumeBytes, totalBytes;
    private byte[] discKey;
    private boolean closed, joliet;

    private IsoImage(Source source, Cancellation cancellation) {
        this.source = source; this.cancellation = cancellation;
    }
    public static IsoImage open(File file) throws IOException { return open(file, null); }
    public static IsoImage open(File file, Cancellation cancellation) throws IOException {
        FileChannel channel = new FileInputStream(file).getChannel();
        return open(channel, 0, -1, cancellation);
    }
    public static IsoImage open(FileChannel channel, long offset, long length) throws IOException {
        return open(channel, offset, length, null);
    }
    public static IsoImage open(FileChannel channel, long offset, long length, Cancellation cancellation) throws IOException {
        Source source;
        try { source = new ChannelSource(channel, offset, length); }
        catch (IOException | RuntimeException error) {
            try { channel.close(); } catch (IOException close) { error.addSuppressed(close); }
            throw error;
        }
        return open(source, cancellation);
    }
    public static IsoImage open(Source source, Cancellation cancellation) throws IOException {
        if (source == null) throw new NullPointerException("source");
        IsoImage image = new IsoImage(source, cancellation);
        try { image.scan(); return image; }
        catch (IOException | RuntimeException error) {
            try { image.close(); } catch (IOException close) { error.addSuppressed(close); }
            throw error;
        }
    }
    public List<Entry> files() { return Collections.unmodifiableList(entries); }
    public Entry find(String path) { return path == null ? null : lookup.get(path.toLowerCase(Locale.ROOT)); }
    public boolean hasEncryptionRegions() { return !encrypted.isEmpty(); }
    public boolean keyed() { return discKey != null; }
    public long totalFileBytes() { return totalBytes; }

    /** Set a user's textual .dkey. Never stores key material on disk or logs it. */
    public void setDiscKey(String text) throws IOException {
        ensureOpen();
        if (text == null) throw new IOException("A disc key must contain 32 hexadecimal digits");
        StringBuilder hex = new StringBuilder(32);
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f') continue;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) || hex.length() == 32)
                throw new IOException("A disc key must contain exactly 32 hexadecimal digits");
            hex.append(c);
        }
        if (hex.length() != 32) throw new IOException("A disc key must contain exactly 32 hexadecimal digits");
        byte[] key = new byte[16];
        for (int i = 0; i < key.length; i++) key[i] = (byte)Integer.parseInt(hex.substring(2 * i, 2 * i + 2), 16);
        if (discKey != null) Arrays.fill(discKey, (byte)0);
        discKey = key;
    }
    public void clearDiscKey() {
        if (discKey != null) Arrays.fill(discKey, (byte)0);
        discKey = null;
    }
    public byte[] read(Entry entry, int maxBytes) throws IOException {
        checkEntry(entry);
        if (maxBytes < 0 || maxBytes > (64 << 20)) throw new IllegalArgumentException("Read limit must be between 0 and 64 MiB");
        ByteArrayOutputStream output = new ByteArrayOutputStream((int)Math.min(entry.size, maxBytes));
        copyLimited(entry, output, null, maxBytes);
        return output.toByteArray();
    }
    /** Stream one file. Caller owns output and any partial output on failure. */
    public long copy(Entry entry, OutputStream output, Progress progress) throws IOException {
        return copyLimited(entry, output, progress, Long.MAX_VALUE);
    }
    /**
     * Extract under a destination root. A private temporary file is promoted
     * only after successful completion; cancellation never removes an old file.
     * Callers can stage an entire import in a separate directory before commit.
     */
    public File extractTo(Entry entry, File destinationRoot, Progress progress) throws IOException {
        checkEntry(entry);
        File root = destinationRoot.getCanonicalFile();
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create extraction root");
        File target = new File(root, entry.path).getCanonicalFile();
        requireChild(root, target);
        File parent = target.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create directory for " + entry.path);
        requireChild(root, parent.getCanonicalFile());
        File part = File.createTempFile("racit-iso-", ".part", parent);
        boolean committed = false;
        try {
            try (OutputStream output = new FileOutputStream(part)) { copy(entry, output, progress); }
            checkCancelled(progress);
            requireChild(root, target.getCanonicalFile());
            try {
                Files.move(part.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException unsupported) {
                Files.move(part.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
            committed = true;
            return target;
        } finally { if (!committed) Files.deleteIfExists(part.toPath()); }
    }
    private static void requireChild(File root, File child) throws IOException {
        if (!child.equals(root) && !child.toPath().startsWith(root.toPath())) throw new IOException("ISO path escapes extraction root");
    }
    private void ensureOpen() throws IOException { if (closed) throw new IOException("Image is closed"); }
    private void checkEntry(Entry entry) throws IOException {
        ensureOpen();
        if (entry == null || entry.owner != owner) throw new IOException("File entry does not belong to this image");
    }
    private void checkCancelled(Cancellation operation) throws InterruptedIOException {
        if (Thread.currentThread().isInterrupted() || (cancellation != null && cancellation.cancelled()) ||
            (operation != null && operation.cancelled())) throw new InterruptedIOException("ISO import cancelled");
    }
    private long copyLimited(Entry entry, OutputStream output, Progress progress, long maximum) throws IOException {
        checkEntry(entry);
        checkCancelled(progress);
        long limit = Math.min(entry.size, maximum), done = 0;
        if (progress != null) progress.update(entry.path, 0, limit);
        if (limit == 0) { checkCancelled(progress); return 0; }
        byte[] buffer = new byte[CHUNK_SECTORS * SECTOR];
        Cipher cipher = null;
        byte[] plaintext = null;
        try {
            if (discKey != null && !encrypted.isEmpty()) {
                cipher = Cipher.getInstance("AES/ECB/NoPadding");
                cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(discKey, "AES"));
                plaintext = new byte[buffer.length];
            }
            for (Extent extent : entry.extents) {
                long remaining = Math.min(extent.bytes, limit - done), sector = extent.sector;
                while (remaining > 0) {
                    checkCancelled(progress);
                    int count = (int)Math.min(CHUNK_SECTORS, (remaining + SECTOR - 1) / SECTOR);
                    readSectors(sector, count, buffer);
                    if (cipher != null) decryptSectors(sector, count, buffer, plaintext, cipher);
                    int take = (int)Math.min(remaining, (long)count * SECTOR);
                    output.write(buffer, 0, take);
                    done += take; remaining -= take; sector += count;
                    if (progress != null) progress.update(entry.path, done, limit);
                    checkCancelled(progress);
                }
                if (done == limit) break;
            }
            return done;
        } catch (GeneralSecurityException error) { throw new IOException("AES sector decryption failed", error); }
        finally {
            Arrays.fill(buffer, (byte)0);
            if (plaintext != null) Arrays.fill(plaintext, (byte)0);
        }
    }
    private void decryptSectors(long first, int count, byte[] bytes, byte[] plain, Cipher cipher) throws GeneralSecurityException {
        long endExclusive = first + count;
        for (long[] range : encrypted) {
            long start = Math.max(first, range[0]), end = Math.min(endExclusive, range[1] + 1);
            if (start >= end) continue;
            int offset = (int)(start - first) * SECTOR, length = (int)(end - start) * SECTOR;
            int written = cipher.doFinal(bytes, offset, length, plain, 0);
            if (written != length) throw new GeneralSecurityException("Unexpected AES output length");
            for (long sector = start; sector < end; sector++) {
                int p = (int)(sector - start) * SECTOR, c = offset + p;
                for (int i = 0; i < 4; i++) plain[p + 12 + i] ^= (byte)(sector >>> (24 - 8 * i));
                for (int i = 16; i < SECTOR; i++) plain[p + i] ^= bytes[c + i - 16];
            }
            System.arraycopy(plain, 0, bytes, offset, length);
        }
    }

    private void scan() throws IOException {
        imageBytes = source.size();
        if (imageBytes < 18L * SECTOR || imageBytes % SECTOR != 0) throw new IOException("Not a complete 2048-byte-sector ISO image");
        volumeBytes = imageBytes;
        byte[] sector = new byte[SECTOR], primary = null, supplementary = null;
        readSectors(0, 1, sector);
        readRegions(sector);
        boolean terminated = false;
        for (int index = 16; index < 64; index++) {
            checkCancelled(null);
            readSectors(index, 1, sector);
            if (sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' || sector[4] != '0' || sector[5] != '1' || sector[6] != 1)
                throw new IOException("Invalid ISO9660 volume descriptor");
            int type = sector[0] & 255;
            if (type == 255) { terminated = true; break; }
            if (type == 1 && primary == null) primary = sector.clone();
            if (type == 2 && sector[88] == '%' && sector[89] == '/' &&
                (sector[90] == '@' || sector[90] == 'C' || sector[90] == 'E')) supplementary = sector.clone();
        }
        if (primary == null || !terminated) throw new IOException("No complete ISO9660 volume descriptor set");
        long primarySize = volumeSize(primary);
        byte[] descriptor = supplementary != null ? supplementary : primary;
        long selectedSize = volumeSize(descriptor);
        if (selectedSize != primarySize) throw new IOException("Conflicting ISO volume sizes");
        volumeBytes = selectedSize;
        joliet = supplementary != null;
        Record root = record(descriptor, 156, SECTOR - 156, false);
        if ((root.flags & 2) == 0 || !root.special) throw new IOException("Invalid ISO root directory");
        walk(root.sector, root.bytes, "", 0);
        if (entries.isEmpty()) throw new IOException("ISO contains no files");
    }
    private long volumeSize(byte[] descriptor) throws IOException {
        int blockSize = both16(descriptor, 128);
        long sectors = both32(descriptor, 80);
        if (blockSize != SECTOR || sectors < 18 || sectors > imageBytes / SECTOR)
            throw new IOException("Unsupported or truncated ISO logical volume");
        return sectors * SECTOR;
    }
    private void readRegions(byte[] sector) throws IOException {
        long count = big32(sector, 0);
        if (count < 1 || count > 200 || big32(sector, 4) != 0) return; // Ordinary ISO, not a PS3 region table.
        long previous = -1, priorEnd = -1;
        for (int i = 0; i < count; i++) {
            long first = big32(sector, 8 + i * 8), last = big32(sector, 12 + i * 8);
            if ((i == 0 && first != 0) || first < previous || last < first || last >= imageBytes / SECTOR)
                throw new IOException("Invalid PS3 unencrypted-sector table");
            if (i > 0 && first > priorEnd + 1) encrypted.add(new long[] { priorEnd + 1, first - 1 });
            previous = last; priorEnd = last;
        }
    }
    private void readSectors(long first, int count, byte[] destination) throws IOException {
        long length = (long)count * SECTOR;
        if (first < 0 || count < 0 || first > volumeBytes / SECTOR || length > volumeBytes - first * SECTOR || length > destination.length)
            throw new EOFException("ISO extent is outside the image");
        source.readFully(first * SECTOR, destination, 0, (int)length);
    }
    private void validateExtent(long sector, long bytes) throws IOException {
        long padded = ((bytes + SECTOR - 1) / SECTOR) * SECTOR;
        if (sector < 0 || sector > volumeBytes / SECTOR || padded > volumeBytes - sector * SECTOR)
            throw new EOFException("ISO file extent is outside its volume");
    }
    private Record record(byte[] data, int offset, int available, boolean decodeName) throws IOException {
        int length = data[offset] & 255;
        if (length < 34 || length > available || offset + length > data.length) throw new IOException("Malformed ISO directory record");
        int nameLength = data[offset + 32] & 255;
        if (nameLength < 1 || 33 + nameLength > length) throw new IOException("Invalid ISO filename length");
        if (data[offset + 26] != 0 || data[offset + 27] != 0) throw new IOException("Interleaved ISO files are not supported");
        if (both16(data, offset + 28) != 1) throw new IOException("Multi-volume ISO files are not supported");
        Record result = new Record();
        result.sector = both32(data, offset + 2) + (data[offset + 1] & 255);
        result.bytes = both32(data, offset + 10);
        result.flags = data[offset + 25] & 255;
        result.special = nameLength == 1 && (data[offset + 33] == 0 || data[offset + 33] == 1);
        validateExtent(result.sector, result.bytes);
        if (decodeName && !result.special) {
            try {
                if (joliet) {
                    if ((nameLength & 1) != 0) throw new IOException("Odd-length Joliet name");
                    result.name = StandardCharsets.UTF_16BE.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(data, offset + 33, nameLength)).toString();
                } else {
                    for (int i = 0; i < nameLength; i++) if ((data[offset + 33 + i] & 255) > 127) throw new IOException("Non-ASCII ISO9660 name without Joliet");
                    result.name = new String(data, offset + 33, nameLength, StandardCharsets.US_ASCII);
                }
            } catch (CharacterCodingException error) { throw new IOException("Malformed Joliet name", error); }
            int semicolon = result.name.indexOf(';');
            if (semicolon >= 0) result.name = result.name.substring(0, semicolon);
            if (!joliet && result.name.endsWith(".")) result.name = result.name.substring(0, result.name.length() - 1);
            validateName(result.name);
        }
        return result;
    }
    private static void validateName(String name) throws IOException {
        if (name.isEmpty() || name.equals(".") || name.equals("..")) throw new IOException("Unsafe ISO filename");
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':') throw new IOException("Unsafe character in ISO filename");
        }
    }
    private void walk(long sector, long bytes, String prefix, int depth) throws IOException {
        checkCancelled(null);
        if (depth > MAX_DEPTH || bytes <= 0 || bytes > MAX_DIRECTORY_BYTES || directories.size() >= MAX_DIRECTORIES)
            throw new IOException("ISO directory traversal limit exceeded");
        if (!directories.add(sector)) throw new IOException("Cyclic or aliased ISO directory extent");
        byte[] data = new byte[SECTOR];
        String pendingPath = null;
        List<Extent> pendingExtents = new ArrayList<>();
        long pendingBytes = 0;
        for (long at = 0; at < bytes; at += SECTOR) {
            checkCancelled(null);
            readSectors(sector + at / SECTOR, 1, data);
            int available = (int)Math.min(SECTOR, bytes - at), offset = 0;
            while (offset < available) {
                checkCancelled(null);
                int length = data[offset] & 255;
                if (length == 0) break;
                Record entry = record(data, offset, available - offset, true);
                offset += length;
                if (entry.special) continue;
                String path = prefix + entry.name;
                if (path.length() > 4096) throw new IOException("ISO path exceeds supported length");
                if ((entry.flags & 2) != 0) {
                    if (pendingPath != null || (entry.flags & 128) != 0) throw new IOException("Invalid multi-extent directory sequence");
                    registerPath(path);
                    walk(entry.sector, entry.bytes, path + "/", depth + 1);
                    continue;
                }
                if (pendingPath != null && !pendingPath.equals(path)) throw new IOException("Interrupted multi-extent ISO file");
                if (pendingPath == null) pendingPath = path;
                if (pendingExtents.size() >= MAX_EXTENTS) throw new IOException("Too many extents in one ISO file");
                pendingExtents.add(new Extent(entry.sector, entry.bytes));
                pendingBytes += entry.bytes;
                if ((entry.flags & 128) != 0) {
                    if (entry.bytes == 0 || entry.bytes % SECTOR != 0) throw new IOException("Non-final ISO extent is not sector-aligned");
                    continue;
                }
                if (entries.size() >= MAX_FILES || pendingBytes > MAX_TOTAL_BYTES - totalBytes) throw new IOException("ISO file list exceeds supported limits");
                registerPath(pendingPath);
                Entry completed = new Entry(pendingPath, pendingBytes, pendingExtents, owner);
                entries.add(completed); lookup.put(pendingPath.toLowerCase(Locale.ROOT), completed);
                totalBytes += pendingBytes;
                pendingPath = null; pendingBytes = 0; pendingExtents.clear();
            }
        }
        if (pendingPath != null) throw new IOException("Unterminated multi-extent ISO file");
    }
    private void registerPath(String path) throws IOException {
        if (!paths.add(path.toLowerCase(Locale.ROOT))) throw new IOException("Ambiguous duplicate ISO path: " + path);
    }
    private static long little32(byte[] b, int p) {
        return (b[p] & 255L) | (b[p + 1] & 255L) << 8 | (b[p + 2] & 255L) << 16 | (b[p + 3] & 255L) << 24;
    }
    private static long big32(byte[] b, int p) {
        return (b[p] & 255L) << 24 | (b[p + 1] & 255L) << 16 | (b[p + 2] & 255L) << 8 | (b[p + 3] & 255L);
    }
    private static long both32(byte[] b, int p) throws IOException {
        long value = little32(b, p);
        if (value != big32(b, p + 4)) throw new IOException("Conflicting ISO byte-order fields");
        return value;
    }
    private static int both16(byte[] b, int p) throws IOException {
        int value = (b[p] & 255) | (b[p + 1] & 255) << 8;
        if (value != ((b[p + 2] & 255) << 8 | (b[p + 3] & 255))) throw new IOException("Conflicting ISO byte-order fields");
        return value;
    }
    public void close() throws IOException {
        if (!closed) { closed = true; clearDiscKey(); source.close(); }
    }
}
