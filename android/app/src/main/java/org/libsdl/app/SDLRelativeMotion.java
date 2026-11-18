package org.libsdl.app;

import android.view.InputDevice;
import android.view.MotionEvent;

/** RACiT: consume every relative sample in a coalesced Android event without
 * allocating or making a JNI call for every historical mouse report. */
final class SDLRelativeMotion {
    static boolean isPointer(MotionEvent event) {
        return event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)
                || event.isFromSource(InputDevice.SOURCE_MOUSE)
                || event.isFromSource(InputDevice.SOURCE_TOUCHPAD);
    }

    static float x(MotionEvent event) {
        return sumAxis(event, event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)
                ? MotionEvent.AXIS_X : MotionEvent.AXIS_RELATIVE_X);
    }

    static float y(MotionEvent event) {
        return sumAxis(event, event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)
                ? MotionEvent.AXIS_Y : MotionEvent.AXIS_RELATIVE_Y);
    }

    static float sumAxis(MotionEvent event, int axis) {
        double sum = 0;
        final int history = event.getHistorySize();
        for (int i = 0; i <= history; i++) {
            final float sample = i == history ? event.getAxisValue(axis, 0)
                    : event.getHistoricalAxisValue(axis, 0, i);
            if (Float.isFinite(sample)) sum += sample;
        }
        return (float)Math.max(-32768.0, Math.min(32768.0, sum));
    }
}
