package com.racacit.recomp;

/** Native counter deltas. These are interval averages, never individual frame-time measurements. */
final class FrameTelemetry {
    private static final int CAPACITY = 120;
    private final float[] values = new float[CAPACITY];
    private final long[] times = new long[CAPACITY];
    private int first, count;
    private long flips = -1, presents = -1, micros;
    private boolean bridge, valid;
    private float fps = Float.NaN, presentedFps = Float.NaN, frameMs = Float.NaN;
    private long weapon = -1, sampleWindowMs;

    static final class Snapshot {
        final boolean bridge, hasFrameSample;
        final float fps, presentedFps, frameMs, sampleLowFps10s;
        final long weapon, sampleWindowMs;
        private final float[] history;
        private final long[] historyAgeMs;
        Snapshot(boolean bridge, boolean valid, float fps, float presentedFps, float frameMs, long weapon,
                 long sampleWindowMs, float low, float[] history, long[] historyAgeMs) {
            this.bridge = bridge; hasFrameSample = valid; this.fps = fps; this.presentedFps = presentedFps;
            this.frameMs = frameMs; this.weapon = weapon; this.sampleWindowMs = sampleWindowMs;
            sampleLowFps10s = low; this.history = history; this.historyAgeMs = historyAgeMs;
        }
        int historySize() { return history.length; }
        float historyAt(int index) { return history[index]; }
        long historyAgeMs(int index) { return historyAgeMs[index]; }
    }
    void unavailable() { bridge = false; reset(); weapon = -1; }
    private void reset() { valid = false; fps = presentedFps = frameMs = Float.NaN; flips = presents = -1; micros = 0; first = count = 0; sampleWindowMs = 0; }
    void accept(long[] counters) {
        if (counters == null || counters.length < 4 || counters[0] < 0 || counters[1] < 0 || counters[2] <= 0) { unavailable(); return; }
        bridge = true; weapon = counters[3];
        // New native snapshots include readiness flags. JNI availability alone does
        // not mean guest rendering has started; do not invent a startup zero FPS.
        if (counters.length >= 13 && (counters[4] & 64) == 0) { reset(); weapon = -1; return; }
        long dt = counters[2] - micros, game = delta(counters[0], flips), shown = delta(counters[1], presents);
        if (flips >= 0 && dt > 0 && dt <= 2500000 && game >= 0 && shown >= 0) {
            float rate = game * 1000000f / dt, renderRate = shown * 1000000f / dt;
            if (Float.isFinite(rate) && rate <= 1000 && Float.isFinite(renderRate) && renderRate <= 1000) {
                valid = true; fps = rate; presentedFps = renderRate; frameMs = game > 0 ? dt / (1000f * game) : Float.NaN;
                sampleWindowMs = dt / 1000;
                while (count > 0 && counters[2] - times[first] > 60000000) { first = (first + 1) % CAPACITY; count--; }
                if (count == CAPACITY) { first = (first + 1) % CAPACITY; count--; }
                int index = (first + count++) % CAPACITY; values[index] = fps; times[index] = counters[2];
            } else reset();
        } else reset();
        flips = counters[0]; presents = counters[1]; micros = counters[2];
    }
    private static long delta(long current, long previous) {
        if (previous < 0) return -1;
        if (current >= previous) return current - previous;
        // v97 exposes unsigned 32-bit cumulative counters. Only treat an edge crossing as wrap.
        return previous >= 0xf0000000L && previous <= 0xffffffffL && current <= 0x0fffffffL ? (current - previous) & 0xffffffffL : -1;
    }
    Snapshot snapshot() {
        float[] history = new float[count]; long[] ages = new long[count]; float low = Float.NaN;
        for (int i = 0; i < count; i++) {
            int index = (first + i) % CAPACITY; history[i] = values[index];
            ages[i] = (micros - times[index]) / 1000;
            if (micros - times[index] <= 10000000) low = Float.isNaN(low) ? values[index] : Math.min(low, values[index]);
        }
        return new Snapshot(bridge, valid, fps, presentedFps, frameMs, weapon, sampleWindowMs, low, history, ages);
    }
}
