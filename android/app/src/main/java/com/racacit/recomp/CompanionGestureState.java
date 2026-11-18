package com.racacit.recomp;

/** UI transaction lifetime only; the native controller separately validates gameplay. */
final class CompanionGestureState {
    static final int CANCEL = 0, COMMIT = 1, KEEP_OPEN = 2;
    boolean choosing, latched;
    int pointerId = -1;
    private long deadline;

    boolean canContinue(boolean radialTarget) { return choosing && latched && radialTarget; }
    void begin(long now) { choosing = true; latched = false; deadline = now + 10000; }
    void pointer(int id) { pointerId = id; latched = false; }
    int release(int id, boolean pageZone, boolean validDirection) {
        if (!choosing || pointerId != id) { reset(); return CANCEL; }
        pointerId = -1;
        if (pageZone) { latched = true; return KEEP_OPEN; }
        reset(); return validDirection ? COMMIT : CANCEL;
    }
    void latch() { if (choosing) { latched = true; pointerId = -1; } }
    boolean expired(long now) { return choosing && now >= deadline; }
    void reset() { choosing = latched = false; pointerId = -1; deadline = 0; }
}
