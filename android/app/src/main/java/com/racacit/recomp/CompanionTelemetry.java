package com.racacit.recomp;

import android.content.Context;
import android.os.SystemClock;
import java.io.File;
import java.util.List;

/** A view reads immutable snapshots; all native polling, proc/sys IO and logging stays on one worker. */
final class CompanionTelemetry {
    static final class Snapshot {
        final boolean bridge, hasFrameSample, hasPerfSample, guestReady, snapshotFresh;
        final float fps, frameMs, presentedFps, sampleLowFps10s, processPct, headroom;
        final long weapon, sampledAtMs, perfSampledAtMs, sampleWindowMs;
        final String clocks;
        final int gpuBusy, gpuMhz, thermal;
        final int flags, levelId, inputPhase, inputStatus, wheelPage, wheelSlot;
        final long generation, snapshotAgeUs, transactionFrames;
        final int[] quickSlots = new int[NativeBridge.QUICK_SLOT_COUNT];   /* weapon ids, -1 empty or unknown */
        final List<PerfProbe.Busy> top;
        private final FrameTelemetry.Snapshot frames;
        Snapshot(FrameTelemetry.Snapshot frames, PerfProbe.Snapshot facts, long now, boolean requested, long[] nativeState) {
            this.frames = frames; bridge = frames.bridge; hasFrameSample = frames.hasFrameSample;
            fps = frames.fps; frameMs = frames.frameMs; presentedFps = frames.presentedFps; weapon = frames.weapon;
            sampleWindowMs = frames.sampleWindowMs; sampleLowFps10s = frames.sampleLowFps10s; sampledAtMs = now;
            hasPerfSample = requested && facts.sampledAtMs > 0; perfSampledAtMs = facts.sampledAtMs;
            processPct = facts.processPct; clocks = facts.clocks; gpuBusy = facts.gpuBusy; gpuMhz = facts.gpuMhz;
            thermal = facts.thermal; headroom = facts.headroom; top = facts.top;
            boolean complete = nativeState != null && nativeState.length >= 13;
            flags = complete ? (int) nativeState[4] : 0; levelId = complete ? (int) nativeState[5] : -1;
            inputPhase = complete ? (int) nativeState[6] : 0; inputStatus = complete ? (int) nativeState[7] : 0;
            generation = complete ? nativeState[8] : 0; snapshotAgeUs = complete ? nativeState[9] : -1;
            transactionFrames = complete ? nativeState[10] : 0;
            wheelPage = complete ? (int) nativeState[11] : -1; wheelSlot = complete ? (int) nativeState[12] : -1;
            guestReady = (flags & 64) != 0; snapshotFresh = (flags & 2) != 0;
            boolean slots = nativeState != null && nativeState.length >= NativeBridge.QUICK_SLOTS + quickSlots.length;
            for (int i = 0; i < quickSlots.length; i++) quickSlots[i] = slots ? (int) nativeState[NativeBridge.QUICK_SLOTS + i] : -1;
        }
        int historySize() { return frames.historySize(); }
        float historyAt(int index) { return frames.historyAt(index); }
        long historyAgeMs(int index) { return frames.historyAgeMs(index); }
        String thermalName() { return thermal >= 0 && thermal < PerfProbe.THERMAL.length ? PerfProbe.THERMAL[thermal] : "unavailable"; }
    }
    private static final Snapshot EMPTY = new Snapshot(new FrameTelemetry().snapshot(), PerfProbe.EMPTY, 0, false, null);
    private final BoundedSampler<Snapshot> sampler;
    private volatile boolean statusVisible;

    CompanionTelemetry(Context context, File filesDir, boolean preview, boolean loggingEnabled) {
        // A preview cannot construct a probe, invoke native code, create a worker session or touch logs.
        if (preview) { sampler = null; return; }
        Context application = context.getApplicationContext();
        sampler = new BoundedSampler<>(() -> new BoundedSampler.Session<Snapshot>() {
            final FrameTelemetry frames = new FrameTelemetry();
            PerfProbe probe;
            PerfProbe.Snapshot facts = PerfProbe.EMPTY;
            long lastFactsAt;
            boolean collecting;
            @Override public Snapshot sample() {
                long now = SystemClock.elapsedRealtime();
                long[] nativeState = null;
                try { nativeState = NativeBridge.stats(); frames.accept(nativeState); }
                catch (UnsatisfiedLinkError unavailable) { frames.unavailable(); }
                FrameTelemetry.Snapshot frame = frames.snapshot();
                boolean needed = statusVisible || loggingEnabled;
                if (needed && (!collecting || now - lastFactsAt >= 1000)) {
                    if (probe == null) probe = new PerfProbe(application, filesDir);
                    facts = probe.sample(now, nativeState); lastFactsAt = now;
                    if (loggingEnabled) probe.log(frame, facts, now);
                } else if (!needed && collecting) {
                    if (probe != null) probe.pause();
                    facts = PerfProbe.EMPTY;
                }
                collecting = needed;
                return new Snapshot(frame, facts, now, needed, nativeState);
            }
            @Override public void close() { if (probe != null) probe.close(); }
        }, EMPTY, 500);
    }
    void start() { if (sampler != null) sampler.start(); }
    void stop() { if (sampler != null) sampler.stop(); }
    void setStatusVisible(boolean visible) { statusVisible = visible; }
    Snapshot snapshot() { return sampler == null ? EMPTY : sampler.snapshot(); }
}
