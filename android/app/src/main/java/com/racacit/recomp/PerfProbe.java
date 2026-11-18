package com.racacit.recomp;

import android.content.Context;
import android.os.Build;
import android.os.PowerManager;
import android.system.Os;
import android.system.OsConstants;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Worker-owned diagnostic sampling. No files are opened for logging unless profiling is enabled. */
final class PerfProbe {
    static final class Busy {
        final String name; final float pct;
        /** Of pct, the share spent waiting (the PPU main thread's SPU job waits); NaN = not known. */
        final float wait;
        Busy(String name, float pct) { this(name, pct, Float.NaN); }
        Busy(String name, float pct, float wait) { this.name = name; this.pct = pct; this.wait = wait; }
    }
    static final class Snapshot {
        final List<Busy> top;
        final float processPct, headroom;
        final String clocks;
        final int gpuBusy, gpuMhz, thermal;
        final long sampledAtMs;
        Snapshot(List<Busy> top, float processPct, String clocks, int gpuBusy, int gpuMhz, int thermal, float headroom, long sampledAtMs) {
            this.top = Collections.unmodifiableList(new ArrayList<>(top)); this.processPct = processPct;
            this.clocks = clocks; this.gpuBusy = gpuBusy; this.gpuMhz = gpuMhz; this.thermal = thermal;
            this.headroom = headroom; this.sampledAtMs = sampledAtMs;
        }
    }
    static final Snapshot EMPTY = new Snapshot(Collections.emptyList(), Float.NaN, "unavailable", -1, -1, -1, Float.NaN, 0);
    static final String[] THERMAL = { "none", "light", "moderate", "severe", "critical", "emergency", "shutdown" };
    private static final long MAX_LOG_BYTES = 32L << 20;
    private static final Set<String> initializedLogs = new HashSet<>();
    private static long lastHeadroomAt = -10000;
    private static float recentHeadroom = Float.NaN;
    private final PowerManager power;
    private final File log;
    private final long ticksPerSecond;
    private final List<File> policies = new ArrayList<>();
    private final byte[] readBuffer = new byte[2048];
    private Map<Integer, ProcStat> lastThreads = new HashMap<>(), nextThreads = new HashMap<>();
    private ProcStat lastProcess;
    private long lastMainWaitUs = -1;
    private long lastAt, writtenBytes, lastFlush;
    private boolean logFailed;
    private BufferedWriter out;

    PerfProbe(Context context, File filesDir) {
        power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
        log = new File(filesDir, "perf_log.csv");
        long ticks;
        try { ticks = Os.sysconf(OsConstants._SC_CLK_TCK); } catch (Exception unsupported) { ticks = -1; }
        ticksPerSecond = ticks;
        File[] entries;
        try { entries = new File("/sys/devices/system/cpu/cpufreq").listFiles((dir, name) -> name.startsWith("policy")); }
        catch (SecurityException denied) { entries = null; }
        if (entries != null) {
            Arrays.sort(entries, Comparator.comparing(File::getName));
            for (int i = 0; i < Math.min(32, entries.length); i++) policies.add(new File(entries[i], "scaling_cur_freq"));
        }
    }

    /** Reset interval baselines when Status closes and profiling is disabled. */
    void pause() { lastAt = 0; lastProcess = null; lastThreads.clear(); nextThreads.clear(); lastMainWaitUs = -1; }

    Snapshot sample(long now) { return sample(now, null); }

    /** The PPU main thread spins (WFE) in its SPU job waits on a core of its own, which counts as
     * running: its row says how much of its share was that wait, from the native snapshot. */
    Snapshot sample(long now, long[] nativeState) {
        long elapsed = lastAt > 0 ? now - lastAt : 0;
        int mainTid = 0; long mainWaitUs = -1;
        if (nativeState != null && nativeState.length > NativeBridge.MAIN_TID) {
            mainTid = (int) nativeState[NativeBridge.MAIN_TID]; mainWaitUs = nativeState[NativeBridge.MAIN_WAIT_US];
        }
        float mainWaitPct = lastMainWaitUs >= 0 && mainWaitUs >= lastMainWaitUs && elapsed > 0
                ? (mainWaitUs - lastMainWaitUs) / 10f / elapsed : Float.NaN;
        lastMainWaitUs = mainWaitUs;
        ProcStat process = ProcStat.parse(read(new File("/proc/self/stat")));
        float processPct = ProcStat.percent(process, lastProcess, elapsed, ticksPerSecond);
        lastProcess = process; lastAt = now;
        List<Busy> top = new ArrayList<>(6);
        nextThreads.clear();
        File[] tasks;
        try { tasks = new File("/proc/self/task").listFiles(); } catch (SecurityException denied) { tasks = null; }
        if (tasks != null) for (int i = 0; i < Math.min(256, tasks.length); i++) {
            File task = tasks[i]; int id;
            try { id = Integer.parseInt(task.getName()); } catch (NumberFormatException ignored) { continue; }
            ProcStat stat = ProcStat.parse(read(new File(task, "stat")));
            if (stat == null) continue;
            nextThreads.put(id, stat);
            float pct = ProcStat.percent(stat, lastThreads.get(id), elapsed, ticksPerSecond);
            if (!Float.isFinite(pct)) continue;
            int index = 0; while (index < top.size() && top.get(index).pct >= pct) index++;
            final boolean main = id == mainTid && mainTid != 0;
            Busy busy = main ? new Busy("PPU main", pct, Float.isFinite(mainWaitPct) ? Math.min(mainWaitPct, pct) : Float.NaN)
                             : new Busy(stat.name, pct);
            if (index < 6) { top.add(index, busy); if (top.size() > 6) top.remove(6); }
        }
        Map<Integer, ProcStat> previous = lastThreads; lastThreads = nextThreads; nextThreads = previous;
        StringBuilder clocks = new StringBuilder();
        for (File policy : policies) {
            long frequency = readLong(policy);
            if (frequency > 0) clocks.append(clocks.length() == 0 ? "" : "/").append(String.format(Locale.US, "%.1f", frequency / 1e6));
        }
        long busy = readLong(new File("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage"));
        long hz = readLong(new File("/sys/class/kgsl/kgsl-3d0/gpuclk"));
        int gpuBusy = busy >= 0 && busy <= 100 ? (int) busy : -1;
        int gpuMhz = hz > 0 && hz / 1000000 < Integer.MAX_VALUE ? (int) (hz / 1000000) : -1;
        int thermal = -1;
        try {
            if (power != null && Build.VERSION.SDK_INT >= 29) thermal = power.getCurrentThermalStatus();
            // One process-wide worker owns this rate limit, including after presentation recreation.
            if (power != null && Build.VERSION.SDK_INT >= 30 && now - lastHeadroomAt >= 10000) {
                lastHeadroomAt = now; recentHeadroom = power.getThermalHeadroom(0);
            }
        } catch (RuntimeException unavailable) { thermal = -1; recentHeadroom = Float.NaN; }
        return new Snapshot(top, processPct, clocks.length() == 0 ? "unavailable" : clocks + " GHz", gpuBusy, gpuMhz,
                thermal, recentHeadroom, now);
    }

    /** Optional bounded diagnostics, called only on the telemetry worker with real interval samples. */
    void log(FrameTelemetry.Snapshot frames, Snapshot facts, long now) {
        if (logFailed || !frames.hasFrameSample) return;
        try {
            if (out == null) {
                synchronized (initializedLogs) {
                    boolean append = initializedLogs.contains(log.getAbsolutePath()) && log.length() > 0;
                    writtenBytes = append ? log.length() : 0;
                    if (writtenBytes >= MAX_LOG_BYTES) { logFailed = true; return; }
                    out = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(log, append), StandardCharsets.UTF_8), 16384);
                    if (!append) {
                        String header = "seconds,fps,frame_ms,process_pct,thermal,headroom,gpu_busy,gpu_mhz,cpu_ghz,top_threads\n";
                        out.write(header); writtenBytes += header.length();
                    }
                    initializedLogs.add(log.getAbsolutePath()); lastFlush = now;
                }
            }
            StringBuilder threads = new StringBuilder();
            for (Busy thread : facts.top) threads.append(threads.length() == 0 ? "" : ";").append(thread.name.replace(',', ' ').replace(';', ' ').replace('"', '\'').replace('\n', ' ').replace('\r', ' ')).append(':').append(Math.round(thread.pct))
                    .append(Float.isFinite(thread.wait) ? " wait " + Math.round(thread.wait) : "");
            String heat = facts.thermal >= 0 && facts.thermal < THERMAL.length ? THERMAL[facts.thermal] : "unavailable";
            String row = String.format(Locale.US, "%d,%.1f,%.1f,%.0f,%s,%.2f,%d,%d,%s,%s\n", now / 1000,
                    frames.fps, frames.frameMs, facts.processPct, heat, facts.headroom, facts.gpuBusy, facts.gpuMhz,
                    facts.clocks.replace(" GHz", ""), threads);
            writtenBytes += row.getBytes(StandardCharsets.UTF_8).length;
            if (writtenBytes > MAX_LOG_BYTES) { close(); logFailed = true; return; }
            out.write(row);
            if (now - lastFlush >= 5000) { out.flush(); lastFlush = now; }
        } catch (IOException | SecurityException unavailable) { close(); logFailed = true; }
    }

    void close() { try { if (out != null) out.close(); } catch (IOException ignored) { } out = null; }
    private long readLong(File file) {
        String value = read(file);
        if (value == null) return -1;
        value = value.trim(); int end = 0;
        while (end < value.length() && Character.isDigit(value.charAt(end))) end++;
        if (end == 0) return -1;
        try { return Long.parseLong(value.substring(0, end)); } catch (NumberFormatException invalid) { return -1; }
    }
    private String read(File file) {
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            int count = input.read(readBuffer);
            return count > 0 ? new String(readBuffer, 0, count, StandardCharsets.UTF_8) : null;
        } catch (IOException | SecurityException unavailable) { return null; }
    }
}
