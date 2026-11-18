package com.racacit.recomp;

import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** One process-wide worker, one pending task per attached view, no UI waits or stale publication. */
final class BoundedSampler<T> {
    interface Session<T> { T sample(); void close(); }
    interface Factory<T> { Session<T> open(); }
    private static final ScheduledThreadPoolExecutor WORKER = makeWorker();
    private static ScheduledThreadPoolExecutor makeWorker() {
        ScheduledThreadPoolExecutor worker = new ScheduledThreadPoolExecutor(1, runnable -> {
            Thread thread = new Thread(runnable, "companion-telemetry"); thread.setDaemon(true); return thread;
        });
        worker.setRemoveOnCancelPolicy(true);
        return worker;
    }
    private final Factory<T> factory;
    private final T empty;
    private final long delayMs;
    private volatile T latest;
    private Run active;

    BoundedSampler(Factory<T> factory, T empty, long delayMs) {
        if (delayMs < 1) throw new IllegalArgumentException("Positive sample delay required");
        this.factory = factory; this.empty = empty; this.latest = empty; this.delayMs = delayMs;
    }
    synchronized void start() {
        if (active != null) return;
        Run run = new Run(); active = run;
        run.future = WORKER.scheduleWithFixedDelay(run, 0, delayMs, TimeUnit.MILLISECONDS);
    }
    void stop() {
        Run run;
        synchronized (this) {
            run = active; active = null; latest = empty;
            if (run == null) return;
            run.future.cancel(false);
        }
        // Cleanup is ordered after an in-flight sample on the same worker. Never close a
        // stream concurrently with its writer or block the Android lifecycle thread.
        if (run.session != null) WORKER.execute(run::close);
    }
    T snapshot() { return latest; }

    private final class Run implements Runnable {
        ScheduledFuture<?> future;
        volatile Session<T> session;
        @Override public void run() {
            synchronized (BoundedSampler.this) { if (active != this) return; }
            T value;
            try { if (session == null) session = factory.open(); value = session.sample(); }
            catch (RuntimeException unavailable) { value = empty; close(); }
            boolean stopped;
            synchronized (BoundedSampler.this) { stopped = active != this; if (!stopped) latest = value; }
            if (stopped) close();
        }
        void close() {
            Session<T> previous = session; session = null;
            // A diagnostic cleanup failure must not cancel the recurring sampler or
            // leave a stale session installed. All cleanup still runs on the worker.
            if (previous != null) try { previous.close(); } catch (RuntimeException unavailable) { }
        }
    }
}
