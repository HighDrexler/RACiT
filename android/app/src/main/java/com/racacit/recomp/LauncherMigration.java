package com.racacit.recomp;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.zip.*;

/** Reads only an explicitly selected archive. No source directory discovery or executable import. */
final class LauncherMigration {
    static final long MAX_BYTES = 512L << 20;
    static final int MAX_ENTRIES = 10000;
    interface SettingFilter { String portableValue(String key, String value); }

    static final class Preview implements AutoCloseable {
        final File directory, savesZip;
        final Map<String, String> settings = new LinkedHashMap<>();
        int slots, ignoredSettings, ignoredFiles;
        Preview(File directory) { this.directory = directory; savesZip = new File(directory, "saves.zip"); }
        @Override public void close() { Store.deleteTree(directory); }
        void importSettings(Store store) throws IOException {
            if (settings.isEmpty()) throw new IOException("No supported settings in this archive.");
            File original = store.settingsFile();
            if (original.isFile()) Store.writeText(new File(store.base, "settings/before-migration.txt"),
                    new String(Store.readAll(original, 1 << 20), StandardCharsets.UTF_8));
            store.putSettings(settings);
        }
        void importSaves(Store store) throws IOException {
            if (slots == 0) throw new IOException("No valid BCUS98124 saves in this archive.");
            try (InputStream in = new FileInputStream(savesZip)) { store.importSaves(in); }
        }
    }

    static Preview read(InputStream input, File cache, SettingFilter filter) throws IOException {
        return read(input, cache, filter, MAX_BYTES, MAX_ENTRIES);
    }

    static Preview read(InputStream input, File cache, SettingFilter filter, long maxBytes, int maxEntries) throws IOException {
        if (input == null) throw new IOException("The selected document cannot be opened.");
        if (!cache.isDirectory() && !cache.mkdirs()) throw new IOException("Cannot create import staging folder.");
        Preview result = new Preview(Files.createTempDirectory(cache.toPath(), "migration-").toFile());
        File savedata = new File(result.directory, "savedata");
        Set<String> outputs = new HashSet<>();
        Map<String, String> slotOrigins = new HashMap<>();
        boolean settingsSeen = false;
        long total = 0; int entries = 0;
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(input))) {
            byte[] buffer = new byte[32768];
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > maxEntries) throw new IOException("Archive has too many entries.");
                String path = checkedPath(entry.getName());
                // Even directory entries must be drained through our byte counter; a hostile
                // archive can attach a compressed payload to a directory-shaped name.
                boolean settings = !entry.isDirectory() && (path.equals("settings.txt") || path.endsWith("/settings.txt"));
                String save = entry.isDirectory() ? null : savePath(path);
                if (settings && settingsSeen) throw new IOException("Several settings.txt files found. Select an archive containing just one launcher configuration.");
                if (settings) settingsSeen = true;
                ByteArrayOutputStream text = settings ? new ByteArrayOutputStream() : null;
                File output = save == null ? null : new File(savedata, save);
                if (save != null) {
                    String slot = save.substring(0, save.indexOf('/')).toLowerCase(Locale.ROOT);
                    String origin = path.substring(0, path.length() - save.length());
                    String previous = slotOrigins.put(slot, origin);
                    if (previous != null && !previous.equals(origin)) throw new IOException("The same save slot appears in several source folders.");
                }
                if (output != null && !outputs.add(save.toLowerCase(Locale.ROOT))) throw new IOException("Duplicate save files in archive; nothing imported.");
                if (output != null && !output.getParentFile().isDirectory() && !output.getParentFile().mkdirs()) throw new IOException("Cannot stage save.");
                if (!entry.isDirectory() && !settings && output == null) result.ignoredFiles++;
                try (OutputStream out = output == null ? null : new BufferedOutputStream(new FileOutputStream(output))) {
                    int count;
                    while ((count = zip.read(buffer)) != -1) {
                        if (count == 0) continue;
                        total += count;
                        if (total > maxBytes) throw new IOException("Archive expands beyond the 512 MB import limit. Include only settings and saves.");
                        if (text != null) {
                            if (text.size() + count > (1 << 20)) throw new IOException("Settings file is too large.");
                            text.write(buffer, 0, count);
                        }
                        if (out != null) out.write(buffer, 0, count);
                    }
                }
                if (text != null) parseSettings(result, text.toString("UTF-8"), filter);
            }
            File[] slots = savedata.listFiles();
            if (slots != null) for (File slot : slots) {
                if (new File(slot, "GAME.SAV").isFile()) result.slots++;
                else Store.deleteTree(slot);
            }
            if (result.slots == 0 && result.settings.isEmpty()) throw new IOException("No supported settings or BCUS98124 saves found. Use settings/settings.txt and savedata/BCUS98124.../GAME.SAV.");
            if (result.slots > 0) try (ZipOutputStream out = new ZipOutputStream(new BufferedOutputStream(new FileOutputStream(result.savesZip)))) {
                Store.zipTree(out, savedata, "savedata/");
            }
            Store.deleteTree(savedata);
            return result;
        } catch (IOException | RuntimeException error) { result.close(); throw error; }
    }

    private static String checkedPath(String input) throws IOException {
        String path = input.replace('\\', '/');
        if (path.length() > 1024 || path.startsWith("/") || path.indexOf(':') >= 0 || path.indexOf(0) >= 0)
            throw new IOException("Unsafe archive path.");
        for (String part : path.split("/")) if (part.equals("..") || part.equals(".")) throw new IOException("Unsafe archive path.");
        return path;
    }

    private static String savePath(String path) {
        String[] parts = path.split("/");
        if (parts.length < 2) return null;
        String slot = parts[parts.length - 2], file = parts[parts.length - 1];
        if (!slot.matches("BCUS98124[A-Za-z0-9_-]*")) return null;
        // Save payload/metadata only: never import a DLL, script, cache, mod or launcher program.
        if (!Arrays.asList("GAME.SAV", "PARAM.SFO", "ICON0.PNG", "PIC0.PNG", "PIC1.PNG", "SND0.AT3").contains(file)) return null;
        if (parts.length > 2 && !parts[parts.length - 3].equals("savedata")) return null;
        return slot + "/" + file;
    }

    private static void parseSettings(Preview preview, String text, SettingFilter filter) {
        if (text.startsWith("\uFEFF")) text = text.substring(1);
        for (String line : text.split("\n")) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int eq = line.indexOf('=');
            if (eq <= 0) { preview.ignoredSettings++; continue; }
            String key = line.substring(0, eq).trim(), value = line.substring(eq + 1).trim();
            String portable = value.indexOf(0) < 0 && value.indexOf('\r') < 0 ? filter.portableValue(key, value) : null;
            if (portable == null) preview.ignoredSettings++; else preview.settings.put(key, portable);
        }
    }
}
