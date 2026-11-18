package org.libsdl.app;

/** RACiT: Android capture is asynchronous. A request is not a granted capture.
 * Kept free of Android dependencies so lifecycle races can be host tested. */
final class SDLPointerCaptureState {
    private long generation;
    private boolean requested;
    private boolean captured;

    synchronized long request(boolean enabled) {
        requested = enabled;
        if (!enabled) captured = false;
        return ++generation;
    }

    synchronized boolean isCurrent(long token) { return generation == token; }
    synchronized boolean isRequested() { return requested; }
    synchronized boolean isCaptured() { return captured; }

    synchronized boolean changed(boolean hasCapture) {
        captured = requested && hasCapture;
        if (!hasCapture) {
            requested = false;
            ++generation; // invalidate queued requests after focus/capture loss
        }
        return captured;
    }
}
