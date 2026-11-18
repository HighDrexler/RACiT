package com.racacit.recomp;

import android.content.Context;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.HashSet;
import java.util.Set;
import java.util.Iterator;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;
import org.json.JSONObject;

/**
 * Everything the launcher keeps, under the app's external files folder
 * (Android/data/<application-id>/files -- reachable over USB):
 *
 *   PS3_GAME/              the imported game (your own copy; never bundled)
 *   settings/settings.txt  settings, NAME=value lines as on Windows; edit freely
 *   mods/<id>/mod.json     a mod: {"id","name","version","author","description"}
 *   mods/<id>/files/       ...and the files it replaces, laid out like the game
 *                          folder (PS3_GAME/USRDIR/packed/...)
 *   dev_hdd0/              the PS3 hard drive: saves under home/00000001/savedata
 *   backups/               save backups (one zip per launch, the last 8 kept)
 *   assets/glyphs/         button icons taken from the game's own font at import
 *   launch.env             written before each launch (enabled mods, game folder)
 *   debug/                 present = write log.txt
 */
final class Store {
    private static final Object MOD_COMMIT_LOCK = new Object();
    final File base;

    Store(Context c) {
        File f = c.getExternalFilesDir(null);
        base = f != null ? f : c.getFilesDir();
    }

    File gameRoot()   { String g = settings().get("GAME"); return g != null && !g.isEmpty() ? new File(g) : base; }
    File settingsFile() { return new File(base, "settings/settings.txt"); }
    File modsDir()    { return new File(base, "mods"); }
    File hdd()        { return new File(base, "dev_hdd0"); }
    File saves()      { return new File(base, "dev_hdd0/home/00000001/savedata"); }
    File backups()    { return new File(base, "backups"); }
    File glyphs()     { return new File(base, "assets/glyphs"); }
    File privateEboot() { return new File(base, "program/EBOOT.elf"); }
    File eboot()      { if (privateEboot().isFile()) return privateEboot(); File r = gameRoot(); File e = new File(r, "PS3_GAME/USRDIR/EBOOT.elf"); return e.exists() ? e : new File(r, "PS3_GAME/USRDIR/EBOOT.ELF"); }
    File debugFlag()  { return new File(base, "debug"); }
    File log()        { return new File(base, "log.txt"); }

    /** "ok", or what is wrong with the game folder. */
    String gameDataStatus() {
        try { GameIsoImport.recover(base); Importer.recover(base); } catch (IOException e) { return e.getMessage(); }
        File root = gameRoot();
        File sfo = new File(root, "PS3_GAME/PARAM.SFO");
        if (!sfo.exists()) return "No game imported yet.";
        String id = titleId(sfo);
        if (id != null && !id.equals("BCUS98124")) return "This is " + id + ", not A Crack in Time (BCUS98124).";
        if (!new File(root, "PS3_GAME/USRDIR/packed").isDirectory()) return "PS3_GAME/USRDIR/packed is missing.";
        return "ok";
    }

    String gameStatus() {
        String data = gameDataStatus();
        if (!data.equals("ok")) return data;
        File e = eboot();
        if (!e.exists()) return "EBOOT.elf is missing: use Import game > Add EBOOT.elf to select your decrypted program.";
        try { EbootFile.validate(e); } catch (IOException problem) { return "EBOOT.elf: " + problem.getMessage(); }
        return "ok";
    }

    static boolean isElf(File f) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[4];
            return in.read(b) == 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
        } catch (IOException e) { return false; }
    }

    /** TITLE_ID from a PARAM.SFO, or null. */
    static String titleId(File sfo) {
        try {
            byte[] d = readAll(sfo, 1 << 16);
            if (d.length < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') return null;
            int keyTab = le32(d, 8), dataTab = le32(d, 12), n = le32(d, 16);
            for (int i = 0; i < n; i++) {
                int e = 20 + i * 16;
                int ko = keyTab + (d[e] & 0xFF | (d[e + 1] & 0xFF) << 8);
                int len = le32(d, e + 4), off = dataTab + le32(d, e + 12);
                int k = ko; while (k < d.length && d[k] != 0) k++;
                String key = new String(d, ko, k - ko, StandardCharsets.US_ASCII);
                if (key.equals("TITLE_ID")) {
                    int z = off; while (z < off + len && z < d.length && d[z] != 0) z++;
                    return new String(d, off, z - off, StandardCharsets.US_ASCII);
                }
            }
        } catch (Exception e) { /* not readable */ }
        return null;
    }

    static int le32(byte[] d, int o) { return d[o] & 0xFF | (d[o + 1] & 0xFF) << 8 | (d[o + 2] & 0xFF) << 16 | (d[o + 3] & 0xFF) << 24; }

    static byte[] readAll(File f, int max) throws IOException {
        try (InputStream in = new FileInputStream(f)) { return readAll(in, max); }
    }

    static byte[] readAll(InputStream in, int max) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] b = new byte[16384]; int n;
        while ((n = in.read(b)) > 0) { out.write(b, 0, n); if (out.size() > max) throw new IOException("file too large"); }
        return out.toByteArray();
    }

    /* ---- settings.txt: NAME=value, comments and unknown keys kept ---------- */

    Map<String, String> settings() {
        Map<String, String> m = new LinkedHashMap<>();
        File f = settingsFile();
        if (!f.exists()) return m;
        try {
            for (String line : new String(readAll(f, 1 << 20), StandardCharsets.UTF_8).split("\n")) {
                line = line.replace("\r", "");
                int eq = line.indexOf('=');
                if (line.startsWith("#") || eq <= 0) continue;
                m.put(line.substring(0, eq).trim(), line.substring(eq + 1));
            }
        } catch (IOException e) { /* empty */ }
        return m;
    }

    String setting(String key, String def) { String v = settings().get(key); return v == null ? def : v; }

    /** Sets (or with null removes) keys, keeping every other line as written. */
    synchronized void putSettings(Map<String, String> changes) throws IOException {
        File f = settingsFile();
        f.getParentFile().mkdirs();
        List<String> lines = new ArrayList<>();
        if (f.exists()) lines.addAll(Arrays.asList(new String(readAll(f, 1 << 20), StandardCharsets.UTF_8).replace("\r", "").split("\n")));
        else {
            lines.add("# Ratchet & Clank: A Crack in Time -- Android settings.");
            lines.add("# NAME=value lines, the same names as the Windows build's settings.txt.");
            lines.add("# The launcher's Settings screen edits these; anything else here is kept.");
        }
        Map<String, String> left = new LinkedHashMap<>(changes);
        Set<String> changed = new HashSet<>();
        for (int i = 0; i < lines.size(); i++) {
            String l = lines.get(i); int eq = l.indexOf('=');
            if (l.startsWith("#") || eq <= 0) continue;
            String k = l.substring(0, eq).trim();
            if (!changes.containsKey(k)) continue;
            if (!changed.add(k)) { lines.remove(i--); continue; }
            String v = left.remove(k);
            if (v == null) { lines.remove(i--); } else lines.set(i, k + "=" + v);
        }
        for (Map.Entry<String, String> e : left.entrySet()) if (e.getValue() != null) lines.add(e.getKey() + "=" + e.getValue());
        while (!lines.isEmpty() && lines.get(lines.size() - 1).isEmpty()) lines.remove(lines.size() - 1);
        writeText(f, String.join("\n", lines) + "\n");
    }

    void putSetting(String key, String value) throws IOException {
        Map<String, String> m = new LinkedHashMap<>(); m.put(key, value); putSettings(m);
    }

    static void writeText(File f, String s) throws IOException {
        File parent = f.getAbsoluteFile().getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("could not create " + parent);
        File tmp = File.createTempFile(".write-", ".tmp", parent);
        try {
            try (FileOutputStream o = new FileOutputStream(tmp)) { o.write(s.getBytes(StandardCharsets.UTF_8)); o.getFD().sync(); }
            Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { tmp.delete(); }
    }

    /* ---- mods --------------------------------------------------------------- */

    static final class Mod {
        String id = "", name = "", version = "", author = "", description = "", problem;
        File dir;
        boolean builtin;
        final Map<String, String> settings = new LinkedHashMap<>();
    }

    static boolean allowedModSetting(String key) {
        if (key == null || !key.matches("[A-Za-z_][A-Za-z0-9_]*")) return false;
        switch (key.toUpperCase(Locale.ROOT)) {
            case "GAME": case "PS3_VFS_ROOT": case "PS3_HDD0_ROOT":
            case "RSX_SHADER_CACHE": case "RSX_PIPELINE_CACHE": case "RSX_PIPELINE_LIST":
            case "RACACIT_SETTINGS_FILE": case "RACACIT_LOG_FILE": case "RACACIT_MODS": case "RACACIT_EBOOT":
                return false;
            default: return true;
        }
    }

    static String modSettingValue(Object value) throws IOException {
        String text = value instanceof String ? (String) value : value instanceof Boolean ? ((Boolean) value ? "1" : "0")
                : value instanceof Number ? Long.toString(((Number) value).longValue()) : "";
        if (text.indexOf('\n') >= 0 || text.indexOf('\r') >= 0 || text.indexOf(0) >= 0)
            throw new IOException("Mod setting values must fit on one line.");
        return text;
    }

    private static Mod readMod(File d) {
        Mod m = new Mod(); m.dir = d; m.id = d.getName(); m.name = d.getName(); m.version = ""; m.author = ""; m.description = "";
        File j = new File(d, "mod.json");
        try {
            if (j.exists()) {
                JSONObject o = new JSONObject(new String(readAll(j, 65536), StandardCharsets.UTF_8));
                m.name = o.optString("name", m.name); m.version = o.optString("version", "");
                m.author = o.optString("author", ""); m.description = o.optString("description", "");
                JSONObject settings = o.optJSONObject("settings");
                if (settings != null) for (Iterator<String> keys = settings.keys(); keys.hasNext();) {
                    String key = keys.next();
                    if (allowedModSetting(key)) m.settings.put(key.toUpperCase(Locale.ROOT), modSettingValue(settings.opt(key)));
                }
            }
        } catch (Exception e) { m.problem = "mod.json could not be read: " + e.getMessage(); }
        if (m.problem == null && !new File(d, "files").isDirectory() && m.settings.isEmpty())
            m.problem = "No files/ folder and no settings: nothing to change.";
        return m;
    }

    private void recoverMods() throws IOException {
        synchronized (MOD_COMMIT_LOCK) {
            File[] entries = modsDir().listFiles();
            if (entries == null) return;
            for (File previous : entries) {
                String name = previous.getName();
                if (!previous.isDirectory() || !name.startsWith(".previous-")) continue;
                String id = name.substring(10);
                if (!validModId(id)) continue;
                File current = new File(modsDir(), id);
                if (!current.exists() && !previous.renameTo(current)) throw new IOException("Could not recover mod " + id);
            }
        }
    }

    List<Mod> mods() {
        List<Mod> out = new ArrayList<>();
        try { recoverMods(); } catch (IOException e) {
            Mod error = new Mod(); error.id = ".recovery-error"; error.name = "Mod recovery needed"; error.problem = e.getMessage(); out.add(error);
        }
        Mod dual = new Mod();
        dual.id = "dual-screen"; dual.name = "Dual-Screen Companion"; dual.version = "1.0"; dual.author = "built in";
        dual.description = "The second screen (AYN Thor and other dual-screen devices) becomes a Crack in Time "
                + "companion: quick select by touch, map and pause, and live performance. Applies on the next launch.";
        dual.builtin = true;
        out.add(dual);
        File[] dirs = modsDir().listFiles();
        if (dirs == null) return out;
        Arrays.sort(dirs);
        for (File d : dirs) {
            if (!d.isDirectory() || d.getName().startsWith(".")) continue;
            out.add(readMod(d));
        }
        return out;
    }

    List<String> enabledMods() {
        List<String> l = new ArrayList<>();
        for (String s : setting("RACACIT_MODS_ENABLED", "").split(",")) if (validModId(s.trim()) && !l.contains(s.trim())) l.add(s.trim());
        return l;
    }

    boolean modEnabled(Mod m) {
        if (m.builtin) return companionEnabled();
        return enabledMods().contains(m.id);
    }

    void setModEnabled(Mod m, boolean on) throws IOException {
        if (on && m.problem != null) throw new IOException(m.problem);
        if (m.builtin) { putSetting("RACACIT_COMPANION", on ? "1" : "0"); return; }
        List<String> l = enabledMods(); l.remove(m.id);
        if (on) l.add(0, m.id);
        putSetting("RACACIT_MODS_ENABLED", String.join(",", l));
    }

    boolean companionEnabled() { return !"0".equals(setting("RACACIT_COMPANION", "1")); }

    /** Unpacks a mod zip (mod.json at its root or one folder down) into mods/. */
    synchronized String installModZip(InputStream zin) throws IOException {
        recoverMods();
        if (!modsDir().isDirectory() && !modsDir().mkdirs()) throw new IOException("could not create mods folder");
        File tmp = Files.createTempDirectory(modsDir().toPath(), ".incoming-").toFile();
        try {
        unzip(zin, tmp, Math.max(0, modsDir().getUsableSpace() - (256L << 20)), 100000);
        File root = tmp;
        if (!new File(root, "mod.json").exists() && !new File(root, "files").isDirectory()) {
            File[] k = tmp.listFiles();
            if (k != null && k.length == 1 && k[0].isDirectory()) root = k[0];
        }
        String id = root == tmp ? "mod" : root.getName();
        File j = new File(root, "mod.json");
        if (j.exists()) {
            try { id = new JSONObject(new String(readAll(j, 65536), StandardCharsets.UTF_8)).optString("id", id); }
            catch (Exception e) { throw new IOException("mod.json is not valid JSON"); }
        }
        id = id.replaceAll("[^A-Za-z0-9._-]", "_");
        if (!validModId(id) || id.equals("dual-screen")) throw new IOException("invalid or reserved mod id");
        Mod candidate = readMod(root);
        if (candidate.problem != null) throw new IOException(candidate.problem);
        publishMod(root, id);
        return id;
        } finally { deleteTree(tmp); }
    }

    private static boolean validModId(String id) { return id != null && id.matches("[A-Za-z0-9_-][A-Za-z0-9._-]*"); }

    /** Only the short publish/rollback section holds the shared lock; extraction never blocks UI recovery. */
    void publishMod(File staged, String id) throws IOException {
        recoverMods();
        if (!validModId(id)) throw new IOException("invalid mod id");
        File dst = new File(modsDir(), id);
        if (!dst.getCanonicalFile().getParentFile().equals(modsDir().getCanonicalFile())) throw new IOException("invalid mod destination");
        File previous = new File(modsDir(), ".previous-" + id);
        deleteTree(previous);
        if (previous.exists()) throw new IOException("could not finish the prior mod installation");
        synchronized (MOD_COMMIT_LOCK) {
            if (dst.exists() && !dst.renameTo(previous)) throw new IOException("could not preserve the current mod");
            if (!staged.renameTo(dst)) {
                if (previous.exists() && !previous.renameTo(dst)) throw new IOException("mod installation interrupted; previous copy retained at " + previous);
                throw new IOException("could not install the mod; previous copy was kept");
            }
        }
        shareWithGroup(dst);
        deleteTree(previous);
    }

    /* ---- saves -------------------------------------------------------------- */

    List<File> saveSlots() {
        List<File> out = new ArrayList<>();
        File[] d = saves().listFiles();
        if (d == null) return out;
        Arrays.sort(d);
        for (File f : d) if (f.isDirectory() && f.getName().startsWith("BCUS98124")) out.add(f);
        return out;
    }

    /** Zips the whole savedata folder. */
    void exportSaves(OutputStream raw) throws IOException {
        try (ZipOutputStream z = new ZipOutputStream(new BufferedOutputStream(raw))) { zipTree(z, saves(), "savedata/"); }
    }

    /** Backs the current saves up, then unpacks a save zip into savedata. */
    synchronized void importSaves(InputStream in) throws IOException {
        recoverSaves();
        File parent = saves().getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("could not create save folder");
        File tmp = Files.createTempDirectory(parent.toPath(), ".save-import-").toFile();
        File stage = null;
        try {
        unzip(in, tmp, 512L << 20, 10000);
        File src = new File(tmp, "savedata").isDirectory() ? new File(tmp, "savedata") : tmp;
        File[] k = src.listFiles();
        List<File> slots = new ArrayList<>();
        if (k != null) for (File f : k) {
            if (f.isDirectory() && f.getName().matches("BCUS98124[A-Za-z0-9_-]*") && new File(f, "GAME.SAV").isFile()) slots.add(f);
        }
        if (slots.isEmpty()) throw new IOException("no valid A Crack in Time save (BCUS98124.../GAME.SAV) in that zip; nothing was changed");
        // Prepare a complete replacement before touching live saves. Unrelated ZIP entries never enter savedata.
        stage = Files.createTempDirectory(parent.toPath(), ".save-stage-").toFile();
        if (saves().exists()) copyTree(saves(), stage);
        for (File f : slots) { File dst = new File(stage, f.getName()); deleteTree(dst); copyTree(f, dst); }
        backupSaves("before-import");
        File old = new File(parent, ".savedata-previous");
        if (saves().exists() && !saves().renameTo(old)) throw new IOException("could not preserve current saves");
        if (!stage.renameTo(saves())) {
            if (old.exists() && !old.renameTo(saves())) throw new IOException("restore interrupted; original saves are retained in " + old);
            throw new IOException("could not install imported saves; original saves were kept");
        }
        shareWithGroup(saves());
        deleteTree(old);
        } finally { deleteTree(tmp); if (stage != null) deleteTree(stage); }
    }

    /** Recover a process interruption between the two directory renames. Called before every launch backup. */
    private void recoverSaves() throws IOException {
        File old = new File(saves().getParentFile(), ".savedata-previous");
        if (!old.exists()) return;
        if (!saves().exists()) {
            if (!old.renameTo(saves())) throw new IOException("could not recover saves from " + old);
        } else {
            deleteTree(old);
            if (old.exists()) throw new IOException("could not finish previous save restore");
        }
    }

    /** A zip of savedata in backups/, keeping the newest 8. Null if there are no saves. */
    synchronized File backupSaves(String why) throws IOException {
        recoverSaves();
        if (saveSlots().isEmpty()) return null;
        if (!backups().isDirectory() && !backups().mkdirs()) throw new IOException("could not create backups folder");
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date());
        File pending = File.createTempFile(".backup-", ".tmp", backups());
        String unique = pending.getName().substring(8, pending.getName().length() - 4);
        File out = new File(backups(), "saves-" + stamp + "-" + why.replaceAll("[^A-Za-z0-9_-]", "_") + "-" + unique + ".zip");
        try {
            try (OutputStream o = new FileOutputStream(pending)) { exportSaves(o); }
            try (FileOutputStream o = new FileOutputStream(pending, true)) { o.getFD().sync(); }
            Files.move(pending.toPath(), out.toPath(), StandardCopyOption.ATOMIC_MOVE);
        } finally { pending.delete(); }
        File[] all = backups().listFiles((d, n) -> n.startsWith("saves-") && n.endsWith(".zip"));
        if (all != null && all.length > 8) {
            Arrays.sort(all);
            for (int i = 0; i < all.length - 8; i++) all[i].delete();
        }
        return out;
    }

    File[] backupList() {
        File[] all = backups().listFiles((d, n) -> n.startsWith("saves-") && n.endsWith(".zip"));
        if (all == null) return new File[0];
        Arrays.sort(all); return all;
    }

    /* ---- launch -------------------------------------------------------------- */

    /** launch.env for the next run: the enabled mods' files folders, in priority order. */
    void writeLaunchEnv() throws IOException {
        recoverMods();
        writeLaunchEnv(mods());
    }

    /** launch.env for the shader prepare: the game brings up Vulkan only, builds every recorded
     *  pipeline on all cores, stamps shaders.stamp with {@code tag} and exits. */
    void writeLaunchEnvPrepare(String tag) throws IOException {
        recoverMods();
        writeLaunchEnv(mods());
        File f = new File(base, "launch.env");
        String s = new String(readAll(f, 1 << 20), StandardCharsets.UTF_8);
        writeText(f, s + "RACACIT_PREPARE=1\nRACACIT_PREPARE_TAG=" + tag + "\n");
    }

    /* ---- shaders: pipelinelist.bin, shaders.state (each start), shaders.stamp (each prepare) ---- */

    static Map<String, String> keyValues(File f) {
        Map<String, String> m = new LinkedHashMap<>();
        if (!f.isFile()) return m;
        try {
            for (String line : new String(readAll(f, 1 << 16), StandardCharsets.UTF_8).split("\n")) {
                int eq = line.indexOf('=');
                if (eq > 0) m.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (IOException e) { /* empty */ }
        return m;
    }

    /** Pipelines the game has recorded (pipelinelist.bin: 16-byte header, then fixed-size records). */
    int shaderVariants() {
        File f = new File(base, "pipelinelist.bin");
        if (!f.isFile() || f.length() < 16) return 0;
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] h = new byte[16];
            if (in.read(h) != 16) return 0;
            int rec = (h[8] & 0xFF) | (h[9] & 0xFF) << 8 | (h[10] & 0xFF) << 16 | (h[11] & 0xFF) << 24;
            return rec <= 0 ? 0 : (int) ((f.length() - 16) / rec);
        } catch (IOException e) { return 0; }
    }

    Map<String, String> shaderStamp() { return keyValues(new File(base, "shaders.stamp")); }
    Map<String, String> shaderState() { return keyValues(new File(base, "shaders.state")); }

    /** 0 ready, 1 nothing recorded yet, 2 never prepared, 3 driver changed, 4 other app version. */
    int shaderStatus(String version) {
        if (shaderVariants() == 0) return 1;
        Map<String, String> stamp = shaderStamp(), state = shaderState();
        if (stamp.isEmpty()) return 2;
        String drv = state.get("driver");
        if ("refused".equals(state.get("cache")) || (drv != null && !drv.equals(stamp.get("driver")))) return 3;
        if (!version.equals(stamp.get("build"))) return 4;
        return 0;
    }

    void writeLaunchEnv(List<Mod> all) throws IOException {
        StringBuilder mods = new StringBuilder();
        Map<String, String> settings = new LinkedHashMap<>();
        List<String> enabled = enabledMods();
        for (int i = enabled.size() - 1; i >= 0; i--) for (Mod mod : all) {
            if (!enabled.get(i).equals(mod.id) || mod.problem != null || mod.builtin) continue;
            for (Map.Entry<String, String> entry : mod.settings.entrySet()) {
                if (allowedModSetting(entry.getKey())) settings.put(entry.getKey().toUpperCase(Locale.ROOT), modSettingValue(entry.getValue()));
            }
        }
        for (String id : enabled) for (Mod mod : all) {
            if (!id.equals(mod.id) || mod.problem != null || mod.builtin || mod.dir == null) continue;
            File f = new File(mod.dir, "files");
            if (f.isDirectory()) { if (mods.length() > 0) mods.append(';'); mods.append(f.getAbsolutePath()); }
        }
        StringBuilder s = new StringBuilder("# Written by the launcher before each launch; edits here are replaced.\n");
        // Android reads launch.env after user settings. First enabled mod wins, matching Windows; user settings are preserved.
        for (Map.Entry<String, String> entry : settings.entrySet()) s.append(entry.getKey()).append('=').append(entry.getValue()).append('\n');
        s.append("RACACIT_MODS=").append(mods).append('\n');
        s.append("RACACIT_EBOOT=").append(eboot().getAbsolutePath()).append('\n');
        s.append("PS3_VFS_ROOT=").append(gameRoot().getAbsolutePath()).append('\n');
        writeText(new File(base, "launch.env"), s.toString());
    }

    /* ---- files ------------------------------------------------------------- */

    /**
     * Files.createTempDirectory makes a folder only the app can open (mode 0700), and a staged folder renamed into
     * place keeps it: after a save import the savedata folder could no longer be read over adb (USB debugging).
     * Give such a folder the mode the app's other folders have (rwxrws---).
     */
    static void shareWithGroup(File dir) {
        try { android.system.Os.chmod(dir.getPath(), 02770); } catch (Exception ignored) { }
    }

    /** Opens again the folders an earlier import left private (see shareWithGroup). A few stat calls; run at launcher start. */
    void repairFolderModes() {
        List<File> dirs = new ArrayList<>();
        dirs.add(saves());
        File[] mods = modsDir().listFiles(File::isDirectory);
        if (mods != null) dirs.addAll(Arrays.asList(mods));
        for (File d : dirs) {
            try { if ((android.system.Os.stat(d.getPath()).st_mode & 0050) != 0050) shareWithGroup(d); } catch (Exception ignored) { }
        }
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        File[] k = f.listFiles();
        if (k != null) for (File c : k) deleteTree(c);
        f.delete();
    }

    static void copyTree(File src, File dst) throws IOException {
        if (src.isDirectory()) {
            if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("could not create " + dst);
            File[] k = src.listFiles();
            if (k == null) throw new IOException("could not read " + src);
            for (File c : k) copyTree(c, new File(dst, c.getName()));
        } else {
            try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
        }
    }

    static long copy(InputStream in, OutputStream out) throws IOException {
        byte[] b = new byte[1 << 20]; int n; long t = 0;
        while ((n = in.read(b)) > 0) { out.write(b, 0, n); t += n; }
        return t;
    }

    static void zipTree(ZipOutputStream z, File dir, String prefix) throws IOException {
        File[] k = dir.listFiles();
        if (k == null) return;
        for (File f : k) {
            if (f.isDirectory()) zipTree(z, f, prefix + f.getName() + "/");
            else {
                z.putNextEntry(new ZipEntry(prefix + f.getName()));
                try (InputStream in = new BufferedInputStream(new FileInputStream(f))) { copy(in, z); }
                z.closeEntry();
            }
        }
    }

    static void unzip(InputStream raw, File dst) throws IOException {
        unzip(raw, dst, Long.MAX_VALUE, Integer.MAX_VALUE);
    }

    static void unzip(InputStream raw, File dst, long byteLimit, int entryLimit) throws IOException {
        String root = dst.getCanonicalPath() + File.separator;
        try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(raw))) {
            ZipEntry e;
            long total = 0; int entries = 0;
            byte[] buffer = new byte[65536];
            while ((e = z.getNextEntry()) != null) {
                if (++entries > entryLimit) throw new IOException("too many ZIP entries");
                File f = new File(dst, e.getName());
                if (!f.getCanonicalPath().startsWith(root)) throw new IOException("bad path in zip: " + e.getName());
                if (e.isDirectory()) { f.mkdirs(); continue; }
                f.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(f)) {
                    int n;
                    while ((n = z.read(buffer)) != -1) {
                        total += n;
                        if (total > byteLimit) throw new IOException("save ZIP exceeds size limit");
                        o.write(buffer, 0, n);
                    }
                }
            }
        }
    }

    static String size(long b) {
        if (b >= 1L << 30) return String.format(Locale.US, "%.1f GB", b / (double) (1L << 30));
        if (b >= 1L << 20) return String.format(Locale.US, "%.0f MB", b / (double) (1L << 20));
        return String.format(Locale.US, "%d KB", b >> 10);
    }
}
