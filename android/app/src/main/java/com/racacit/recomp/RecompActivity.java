package com.racacit.recomp;

import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import java.util.ArrayList;
import java.util.List;
import org.libsdl.app.SDLActivity;

/**
 * The game: loads libSDL2.so and the recompiled game (libmain.so) and runs
 * SDL_main, in its own process (":game") so the game's exit leaves the
 * launcher alone. With the dual-screen companion on, its page goes to the
 * second display (Companion).
 */
public class RecompActivity extends SDLActivity {
    /** Started by the launcher's Prepare shaders: launch.env holds RACACIT_PREPARE=1. */
    static final String EXTRA_PREPARE = "prepare";
    private Companion companion;
    private boolean resumed;
    private android.os.Handler prepTick;

    @Override protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL2", "main" };
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        Store store = new Store(this);
        /* Sustained performance: the clocks the device can hold for an hour,
         * not the peak it reaches for a minute before throttling. */
        if ("1".equals(store.setting("RACACIT_SUSTAINED", "0")) && Build.VERSION.SDK_INT >= 24)
            getWindow().setSustainedPerformanceMode(true);
        if (!"system".equals(store.setting("RACACIT_REFRESH", "max"))) preferFastestRefresh();
        if (getIntent().getBooleanExtra(EXTRA_PREPARE, false)) prepareOverlay();
        else if (store.companionEnabled()) {
            companion = new Companion(this);
        }
        /* Touch play: a swipe in from the side over the stick or the face
         * buttons is a thumb, not Android's back gesture (the system allows
         * 200 dp of each edge to be claimed). */
        if (Build.VERSION.SDK_INT >= 29) {
            final View root = getWindow().getDecorView();
            root.addOnLayoutChangeListener((v, l, t, r, bt, ol, ot, or, ob) -> {
                int h = bt - t, w = r - l, claim = (int) (200 * getResources().getDisplayMetrics().density);
                int edge = (int) (32 * getResources().getDisplayMetrics().density);
                List<Rect> rs = new ArrayList<>();
                rs.add(new Rect(0, Math.max(0, h - claim), edge, h));
                rs.add(new Rect(w - edge, Math.max(0, h - claim), w, h));
                v.setSystemGestureExclusionRects(rs);
            });
        }
    }

    /* The shader prepare's bar over the (black) game screen; the game process exits when it is done. */
    private void prepareOverlay() {
        final float dp = getResources().getDisplayMetrics().density;
        android.widget.LinearLayout box = new android.widget.LinearLayout(this);
        box.setOrientation(android.widget.LinearLayout.VERTICAL);
        box.setGravity(android.view.Gravity.CENTER);
        box.setBackgroundColor(0xFF101418);
        final android.widget.TextView title = new android.widget.TextView(this);
        title.setText("Preparing shaders");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(22);
        title.setGravity(android.view.Gravity.CENTER);
        final android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        final android.widget.TextView line = new android.widget.TextView(this);
        line.setTextColor(0xFFB0B8C0);
        line.setTextSize(16);
        line.setGravity(android.view.Gravity.CENTER);
        line.setText("Starting the renderer...");
        android.widget.LinearLayout.LayoutParams wp = new android.widget.LinearLayout.LayoutParams((int) (420 * dp), (int) (24 * dp));
        wp.topMargin = (int) (16 * dp); wp.bottomMargin = (int) (12 * dp);
        box.addView(title);
        box.addView(bar, wp);
        box.addView(line);
        addContentView(box, new android.view.ViewGroup.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        final long t0 = android.os.SystemClock.elapsedRealtime();
        prepTick = new android.os.Handler(android.os.Looper.getMainLooper());
        prepTick.post(new Runnable() {
            @Override public void run() {
                int[] p = null;
                try { p = NativeBridge.prepareProgress(); } catch (UnsatisfiedLinkError e) { /* library not loaded yet */ }
                if (p != null) {
                    long secs = (android.os.SystemClock.elapsedRealtime() - t0) / 1000;
                    if (p[2] > 0) bar.setProgress((int) (1000L * p[1] / p[2]));
                    switch (p[0]) {
                        case 1: line.setText("Reading the shader cache..."); break;
                        case 5: line.setText("Updating shaders for this version: " + p[1] + " of " + p[2] + "  (" + secs + " s)"); break;
                        case 2: line.setText(p[1] + " of " + p[2] + " pipelines  (" + secs + " s)"); break;
                        case 3:
                            bar.setProgress(1000);
                            line.setText(p[2] == 0 ? "Nothing recorded yet: play a while first."
                                    : "Done: " + (p[1] - p[3] - p[4]) + " of " + p[2] + " built in " + secs + " s"
                                    + (p[3] > 0 ? ", " + p[3] + " failed" : ""));
                            return;
                        case 4: line.setText("Could not prepare: see the game log."); return;
                        default: break;
                    }
                }
                prepTick.postDelayed(this, 200);
            }
        });
    }

    /* The screen's fastest mode at its current size (the Thor: 120 Hz; it idles at 60). The game
     * aims at 60 and paces its frames evenly; at 60 Hz a frame that misses 16.7 ms is shown for
     * 33.3 ms, at 120 Hz after 25 ms, so a dip below 60 reads as a dip instead of a stutter.
     * Android caps the request at the system's peak refresh rate setting. */
    private void preferFastestRefresh() {
        try {
            android.view.Display d = Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
            if (d == null) return;
            android.view.Display.Mode cur = d.getMode(), best = cur;
            for (android.view.Display.Mode m : d.getSupportedModes())
                if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate() + 0.5f) best = m;
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(lp);
            android.util.Log.i("racacit", "screen refresh: asked for " + best.getRefreshRate() + " Hz (mode "
                    + best.getModeId() + ", was " + cur.getRefreshRate() + " Hz)");
        } catch (RuntimeException e) {
            android.util.Log.w("racacit", "screen refresh: " + e);
        }
    }

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        updateCompanion(getWindow().getDecorView().hasWindowFocus());
    }

    @Override protected void onPause() {
        resumed = false;
        // A system dialog or task switch must release synthetic pad input even
        // when the game activity remains visible underneath it.
        if (companion != null) companion.stop();
        super.onPause();
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        // Notification shade and system overlays can take focus without pause.
        // The non-focusable lower window cannot report game focus reliably.
        updateCompanion(focused);
    }

    private void updateCompanion(boolean focused) {
        if (companion == null) return;
        if (resumed && focused) companion.start();
        else companion.stop();
    }

    @Override protected void onStop() {
        if (companion != null) companion.stop();
        super.onStop();
    }

    @Override protected void onDestroy() {
        if (companion != null) { companion.stop(); companion = null; }
        super.onDestroy();
    }
}
