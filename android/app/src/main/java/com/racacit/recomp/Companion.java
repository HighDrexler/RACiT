package com.racacit.recomp;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Rect;
import android.graphics.Shader;
import android.hardware.display.DisplayManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Display;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.accessibility.AccessibilityManager;
import java.io.File;
import java.io.IOException;
import java.text.DateFormat;
import java.util.Arrays;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;
import java.lang.ref.WeakReference;

/**
 * The dual-screen companion: the game on the main screen, this on the
 * second one (the AYN Thor's lower screen), in A Crack in Time's look over the
 * game's own key art, with the game's button icons. Four pages, tabs along
 * the top, and a strip along the bottom on every page (the time, the battery
 * and its heat, the frame rate, how long this session has run):
 *
 *   QUICK SELECT  the game's own ring: your weapons where it puts them, with
 *                 its icons. A tap equips; the side buttons change ring. The
 *                 d-pad in the four corners and the wrench throw (R2) below
 *                 the ring are held while a finger is down, with the controller
 *   ACTIONS       Map, Pause, Confirm, Back, L1, R1, the Settings menu
 *   STATUS        frame rate and its low over 10 s, a 60 s graph, the busiest
 *                 threads, clocks, GPU, heat
 *   CLOCK         the Great Clock, the date, the session time: a calm page
 *
 * It runs in the game's process, so it costs as little as it can: everything
 * that does not change is drawn once into a bitmap (the "chrome", rebuilt on
 * a tab change), and each tick only puts that back and draws the numbers --
 * once a second, twice on the Status page. After a while without a touch it
 * dims (RACACIT_COMPANION_DIM seconds; a touch only wakes it).
 * The window never takes key focus, so the controller always drives the game.
 */
final class Companion implements DisplayManager.DisplayListener {
    private final Activity act;
    private final Store store;
    private final DisplayManager dm;
    private final boolean preview;
    private boolean started;
    private final long sessionStarted = SystemClock.elapsedRealtime();
    private Presentation pres;
    private View view;
    private final Handler ui = new Handler(Looper.getMainLooper());

    Companion(Activity a) { this(a, false); }

    Companion(Activity a, boolean preview) {
        act = a; store = new Store(a);
        this.preview = preview;
        dm = (DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE);
    }

    void start() {
        if (dm == null || started) return;
        started = true;
        dm.registerDisplayListener(this, ui);
        show();
    }

    void stop() {
        if (dm != null && started) dm.unregisterDisplayListener(this);
        started = false;
        hide();
    }

    boolean isShowing() { return pres != null && pres.isShowing(); }

    private void nativeActive(boolean active) {
        if (preview) return;
        try { NativeBridge.setActive(active); if (!active) NativeBridge.cancel(); }
        catch (UnsatisfiedLinkError unavailable) { /* Preview/launcher never loads the game. */ }
    }

    private boolean usable(Display d, int game) {
        return d.isValid() && d.getDisplayId() != Display.DEFAULT_DISPLAY &&
                d.getDisplayId() != game && d.getState() != Display.STATE_OFF;
    }

    /** A second screen that is not the game's: a Presentation is a window above
     *  the activity, so on the game's own screen it would hide the game. None
     *  (no companion) when the game is on the only screen that could take it. */
    private Display pick() {
        final int game = act.getWindowManager().getDefaultDisplay().getDisplayId();
        Display[] p = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        for (Display d : p) if (usable(d, game)) return d;
        for (Display d : dm.getDisplays())
            if (usable(d, game) && (d.getFlags() & Display.FLAG_PRIVATE) == 0) return d;
        return null;
    }

    private void show() {
        if (!started || pres != null || act.isFinishing() || act.isDestroyed()) return;
        Display d = pick();
        if (d == null) return;
        try {
            final Presentation next = new Presentation(act, d, android.R.style.Theme_Material_NoActionBar_Fullscreen) {
                @Override protected void onCreate(Bundle b) {
                    super.onCreate(b);
                    view = new Page(getContext(), store, preview, sessionStarted);
                    setContentView(view);
                }
            };
            next.getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            next.setOnDismissListener(x -> {
                if (pres == next) {
                    nativeActive(false);
                    pres = null; view = null;
                    // Android cancels presentations when display metrics change.
                    // Recreate once with the new display context, while still visible.
                    if (started) ui.post(this::show);
                }
            });
            pres = next;
            next.show();
            nativeActive(next.isShowing());
        } catch (WindowManager.InvalidDisplayException | SecurityException e) {
            nativeActive(false);
            pres = null; view = null;
        }
    }

    private void hide() {
        nativeActive(false);
        Presentation previous = pres;
        pres = null; view = null;
        if (previous != null) previous.dismiss();
    }

    @Override public void onDisplayAdded(int id) { show(); }
    @Override public void onDisplayRemoved(int id) { if (pres != null && pres.getDisplay().getDisplayId() == id) hide(); show(); }
    @Override public void onDisplayChanged(int id) {
        if (pres != null && pres.getDisplay().getDisplayId() == id &&
                !usable(pres.getDisplay(), act.getWindowManager().getDefaultDisplay().getDisplayId())) hide();
        show();
    }

    /** At most one asset worker and one pending recipient for the process.
     * Display recreation replaces a weak recipient rather than queuing more
     * decodes or retaining detached Views. Art/Glyphs own the bitmap caches. */
    private static final class ArtLoader {
        private static final Object lock = new Object();
        private static boolean running;
        private static WeakReference<Page> recipient = new WeakReference<>(null);
        private static final Handler main = new Handler(Looper.getMainLooper());
        static void request(Page page) {
            synchronized (lock) {
                recipient = new WeakReference<>(page);
                if (running) return;
                running = true;
            }
            final Store source = page.store;   // Store holds paths, not an Activity.
            new Thread(() -> {
                Bitmap art = null;
                Bitmap[] icons = new Bitmap[Page.glyphCodes.length];
                try {
                    art = Art.keyArt(source, 1280);
                    File glyphs = source.glyphs();
                    for (int i = 0; i < icons.length; i++) icons[i] = Glyphs.get(glyphs, Page.glyphCodes[i]);
                } catch (RuntimeException | OutOfMemoryError unavailable) { /* Draw the vector/text fallback. */ }
                Bitmap[] loadedWeapons = null;
                try { loadedWeapons = WeaponIcons.load(source.gameRoot()); }
                catch (IOException | RuntimeException | OutOfMemoryError unavailable) { /* The ring shows names instead. */ }
                final Bitmap loadedArt = art;
                final Bitmap[] weapons = loadedWeapons;
                main.post(() -> {
                    Page target;
                    synchronized (lock) { target = recipient.get(); recipient.clear(); running = false; }
                    if (target == null || !target.attached) return;
                    if (!target.store.base.equals(source.base)) { request(target); return; }
                    target.art = loadedArt; target.glyphImages = icons; target.weaponIcons = weapons;
                    target.chrome = null; target.invalidate();
                });
            }, "companion art").start();
        }
        static void cancel(Page page) {
            synchronized (lock) { if (recipient.get() == page) recipient.clear(); }
        }
    }

    /* ---- the page ------------------------------------------------------------ */

    static final class Page extends View {
        private static final String[] TABS = { "SELECT", "MAP", "ACTIONS", "STATUS", "CLOCK" };
        private static final int T_QUICK = 0, T_MAP = 1, T_ACTIONS = 2, T_STATUS = 3, T_CLOCK = 4;
        private static final String[][] ACTIONS = { { "MAP", "The map" }, { "PAUSE", "The pause menu" }, { "CONFIRM", "Cross" },
                                                    { "BACK", "Circle" }, { "PREVIOUS", "L1" }, { "NEXT", "R1" },
                                                    { "GRAPHICS", "The game's Settings menu: resolution, filtering, controls" } };
        /* mask 0: not a pad button -- GRAPHICS opens the in-game Settings menu */
        private static final int[] ACTION_MASK = { NativeBridge.SELECT, NativeBridge.START, NativeBridge.CROSS,
                                                   NativeBridge.CIRCLE, NativeBridge.L1, NativeBridge.R1, 0 };
        private static final int[] ACTION_GLYPH = { GlyphCore.SELECT, GlyphCore.START, GlyphCore.CROSS,
                                                    GlyphCore.CIRCLE, GlyphCore.L1, GlyphCore.R1, 0 };
        private static final int NONE = -1, TAB = 100, ACTION = 200, DIRECTION = 300, PAGE_LEFT = 400, PAGE_RIGHT = 401, MAP_VIEW = 402;
        private static final int RINGS = NativeBridge.QUICK_SLOT_COUNT / 8;
        private static final String[] DIRECTIONS = { "up", "upper right", "right", "lower right", "down", "lower left", "left", "upper left" };
        /* The quick select page's gameplay buttons, held while a finger is down and pressed along
         * with the controller (NativeBridge.hold): the d-pad in the corners, turned an eighth
         * (up top left, right top right, down bottom right, left bottom left), and the wrench
         * throw (the game's R2) below the ring. */
        private static final int GAME = 600, GAME_N = 5, G_WRENCH = 4;
        private static final int[] GAME_MASK = { NativeBridge.UP, NativeBridge.RIGHT, NativeBridge.DOWN, NativeBridge.LEFT, NativeBridge.R2 };
        private static final String[] GAME_LABEL = { "UP", "RIGHT", "DOWN", "LEFT", "WRENCH THROW" };
        private static final String[] GAME_SPOKEN = { "D-pad up", "D-pad right", "D-pad down", "D-pad left", "Wrench throw, R2" };

        private final Store store;
        private final boolean preview;
        private final AccessibilityManager accessibility;
        private boolean attached, cacheUnavailable;
        private int tab;
        private int hotSlot = -1, hotAction = -1; private long hotUntil;
        private int pressed = NONE, pointerId = -1, ringPage, wheelSeen, hoveredNode = NONE, accessibilityFocus = NONE;
        private boolean waking;
        private String inputHint = "";
        private final RectF pageLeft = new RectF(), pageRight = new RectF();
        private final RectF wheelBounds = new RectF(), wheelInner = new RectF();
        private final RectF[] gameRect = new RectF[GAME_N];
        private final int[] gamePointer = { -1, -1, -1, -1, -1 };   /* the finger holding each, or -1 */
        /* The game's ring (pausemenu.swf): quick_hex tiles, a radial navy fill;
         * the chosen one gets an orange glow and an orange icon (iconOrangeTransform). */
        private static final int HEX_INNER = 0x803E5097, HEX_OUTER = 0xFF1B2032, QS_ORANGE = 0xFFE08E16;
        private final Path[] hexes = new Path[8];
        private final Paint[] hexFill = new Paint[8], hexGlow = new Paint[8];
        private final Paint iconPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final Paint radialLine = Acit.stroke(Acit.EDGE_DIM, 1), radialAccent = Acit.stroke(Acit.ORANGE, 2);
        private final Paint bitmapPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final RectF bitmapRect = new RectF();
        private static final int[] glyphCodes = { GlyphCore.SELECT, GlyphCore.START, GlyphCore.CROSS, GlyphCore.CIRCLE, GlyphCore.L1, GlyphCore.R1, GlyphCore.R2 };
        private Bitmap[] glyphImages = new Bitmap[glyphCodes.length];
        private Bitmap[] weaponIcons;             /* by weapon id; null if the game's copy could not be read */

        /* layout, from the size (layout()) */
        private float u;                          /* 1% of the shorter side */
        private final RectF[] tabRect = new RectF[TABS.length];
        private final RectF body = new RectF(), strip = new RectF();
        private final float[][] slot = new float[8][2];
        private float ringR, slotR;
        private final RectF[] actRect = new RectF[ACTIONS.length];
        private final RectF fpsBox = new RectF(), graph = new RectF(), threads = new RectF(), facts = new RectF();

        private Bitmap art, chrome;
        private int chromeTab = -1;
        private final Paint edge, edgeHot, glow, glowHot, panel, panelHot, panelDark, title, label, small, smallL, big, dimP, line, area;
        private final Acit.ClockFace clock;

        /* status */
        private boolean bridge;
        private float fps, frameMs;
        private final float[] hist = new float[120]; private int histN;   /* 2 a second: the last 60 s */
        private int battery = -1; private float batteryC = Float.NaN; private boolean charging;
        private final CompanionTelemetry telemetry;
        private CompanionTelemetry.Snapshot status;
        private int ticks;
        private final long started;

        /* dimming */
        private final long dimAfter;
        private long lastTouch = SystemClock.uptimeMillis();
        private float dim;                         /* 0 awake .. 1 dimmed */

        Page(Context c, Store store, boolean preview, long started) {
            super(c);
            this.preview = preview;
            accessibility = (AccessibilityManager)c.getSystemService(Context.ACCESSIBILITY_SERVICE);
            this.started = started;
            this.store = store;
            telemetry = new CompanionTelemetry(c, store.base, preview, "1".equals(store.setting("RACACIT_PERF_LOG", "0")));
            status = telemetry.snapshot();
            setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
            setFocusable(false); setClickable(true);
            edge = Acit.stroke(Acit.EDGE, 2); edgeHot = Acit.stroke(Acit.ORANGE, 3);
            glow = Acit.glow(Acit.GLOW, 4, 6); glowHot = Acit.glow(Acit.ORANGE_GLOW, 5, 8);
            panel = Acit.fill(Acit.PANEL); panelHot = Acit.fill(Acit.PANEL_HOT); panelDark = Acit.fill(Acit.PANEL_DARK);
            title = Acit.text(Acit.TEXT, 20, Acit.titleFace());
            label = Acit.text(Acit.TEXT, 18, Acit.titleFace());
            small = Acit.text(Acit.TEXT_DIM, 14, Acit.bodyFace());
            smallL = Acit.text(Acit.TEXT_DIM, 14, Acit.bodyFace()); smallL.setTextAlign(Paint.Align.LEFT);
            big = Acit.text(Acit.ORANGE, 64, Acit.titleFace());
            dimP = Acit.fill(0xFF000000);
            line = Acit.stroke(Acit.ORANGE, 3); line.setStrokeJoin(Paint.Join.ROUND);
            area = Acit.fill(0);
            clock = new Acit.ClockFace(c);
            String p = store.setting("RACACIT_COMPANION_PAGE", "quick");
            tab = p.equals("map") ? T_MAP : p.equals("actions") ? T_ACTIONS : p.equals("status") ? T_STATUS : p.equals("clock") ? T_CLOCK : T_QUICK;
            long d;
            try { d = Long.parseLong(store.setting("RACACIT_COMPANION_DIM", "120")); } catch (NumberFormatException e) { d = 120; }
            dimAfter = Math.min(86400, Math.max(0, d)) * 1000;
        }

        private void loadArt() {
            ArtLoader.request(this);
        }

        /* Read an immutable worker snapshot; the UI does no native polling,
         * filesystem profiling, or log writes. Idle pages redraw at 1 Hz. */
        private final Runnable tick = new Runnable() {
            @Override public void run() {
                if (!attached) return;
                status = telemetry.snapshot();
                bridge = status.bridge; fps = status.fps; frameMs = status.frameMs;
                histN = Math.min(hist.length, status.historySize());
                for (int i = 0; i < histN; i++) hist[i] = status.historyAt(status.historySize() - histN + i);
                if (ticks % 20 == 0) battery();
                ticks++;
                updateDim();
                int wheel = Arrays.hashCode(status.quickSlots) * 31 + (int) status.weapon;
                if (tab == T_STATUS || (ticks & 1) == 0 || (tab == T_QUICK && wheel != wheelSeen)) invalidate();
                wheelSeen = wheel;
                postDelayed(this, 500 - System.currentTimeMillis() % 500 + 5);
            }
        };

        private void battery() {
            Intent b = getContext().registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (b == null) return;
            int lv = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), sc = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            battery = lv >= 0 && sc > 0 ? lv * 100 / sc : -1;
            int t = b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
            batteryC = t == Integer.MIN_VALUE ? Float.NaN : t / 10f;
            int st = b.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            charging = st == BatteryManager.BATTERY_STATUS_CHARGING || st == BatteryManager.BATTERY_STATUS_FULL;
        }

        /** Dims after dimAfter ms without a touch; the fade runs at about 20 frames a second for half a second. */
        private void updateDim() {
            boolean want = dimAfter > 0 && SystemClock.uptimeMillis() - lastTouch > dimAfter;
            if (want && dim < 1) fade(1); else if (!want && dim > 0) fade(0);
        }

        private void fade(float to) {
            removeCallbacks(fader);
            fadeTo = to; post(fader);
        }
        private float fadeTo;
        private final Runnable fader = new Runnable() {
            @Override public void run() {
                dim = fadeTo > dim ? Math.min(fadeTo, dim + 0.1f) : Math.max(fadeTo, dim - 0.25f);
                invalidate();
                if (dim != fadeTo) postDelayed(this, 50);
            }
        };

        private Bitmap glyph(int cp) {
            for (int i = 0; i < glyphCodes.length; i++) if (glyphCodes[i] == cp) return glyphImages[i];
            return null;
        }

        /* ---- layout and the chrome ------------------------------------------- */

        @Override protected void onSizeChanged(int w, int h, int ow, int oh) { chrome = null; cacheUnavailable = false; layout(w, h); }

        private void layout(int w, int h) {
            u = Math.min(w, h) / 100f;
            float pad = 2.6f * u, gap = 1.4f * u;
            float tabH = Math.max(8.5f * u, Math.min(Acit.dp(getContext(), 48), h * 0.15f)), stripH = 6.5f * u;
            float tw = (w - pad * 2 - gap * (TABS.length - 1)) / TABS.length;
            for (int i = 0; i < TABS.length; i++) tabRect[i] = new RectF(pad + i * (tw + gap), pad * 0.7f, pad + i * (tw + gap) + tw, pad * 0.7f + tabH);
            strip.set(pad, h - pad * 0.6f - stripH, w - pad, h - pad * 0.6f);
            body.set(pad, pad * 0.7f + tabH + gap * 1.6f, w - pad, strip.top - gap * 1.6f);
            title.setTextSize(3.0f * u); label.setTextSize(2.9f * u); small.setTextSize(2.2f * u); smallL.setTextSize(2.2f * u);
            big.setTextSize(11 * u);
            float sw = 0.28f * u;
            edge.setStrokeWidth(sw); edgeHot.setStrokeWidth(sw * 1.3f); line.setStrokeWidth(sw * 1.3f);
            glow.setStrokeWidth(sw * 2); glow.setMaskFilter(new android.graphics.BlurMaskFilter(0.9f * u, android.graphics.BlurMaskFilter.Blur.NORMAL));
            glowHot.setStrokeWidth(sw * 2.4f); glowHot.setMaskFilter(new android.graphics.BlurMaskFilter(1.2f * u, android.graphics.BlurMaskFilter.Blur.NORMAL));

            /* The game's quick select ring: eight hexes, one ring at a time, as big as the page
             * leaves room for. Around it the gameplay buttons: the d-pad in the four corners and
             * the wrench throw below; ring paging at the sides; the ring number and hints above. */
            float qGap = 1.6f * u;
            float corner = Math.max(8 * u, Math.min(body.width() * 0.19f, body.height() * 0.26f));
            float throwH = Math.max(7.5f * u, Math.min(Acit.dp(getContext(), 56), body.height() * 0.14f));
            float wheelTop = body.top + small.getTextSize() * 1.9f, wheelBottom = body.bottom - throwH - qGap;
            float sideFree = body.width() - 2 * (corner + qGap);
            float cx = body.centerX(), cy = (wheelTop + wheelBottom) / 2;
            /* the hexes reach 0.98 of ringR from the middle */
            ringR = Math.max(u * 5, Math.min(sideFree / 2, (wheelBottom - wheelTop) / 2) / 0.98f); slotR = ringR * 0.28f;
            gameRect[0] = new RectF(body.left, body.top, body.left + corner, body.top + corner);
            gameRect[1] = new RectF(body.right - corner, body.top, body.right, body.top + corner);
            gameRect[2] = new RectF(body.right - corner, body.bottom - corner, body.right, body.bottom);
            gameRect[3] = new RectF(body.left, body.bottom - corner, body.left + corner, body.bottom);
            float throwW = Math.min(sideFree, Math.max(ringR * 1.3f, 30 * u));
            gameRect[G_WRENCH] = new RectF(cx - throwW / 2, body.bottom - throwH, cx + throwW / 2, body.bottom);
            wheelBounds.set(cx - ringR, cy - ringR, cx + ringR, cy + ringR);
            float inner = ringR * 0.43f;
            wheelInner.set(cx - inner, cy - inner, cx + inner, cy + inner);
            for (int i = 0; i < 8; i++) {
                double a = Math.toRadians(-90 + 45 * i);
                float x = cx + (float)(ringR * 0.7f * Math.cos(a)), y = cy + (float)(ringR * 0.7f * Math.sin(a)), r = hexR(i);
                slot[i][0] = x; slot[i][1] = y;
                hexes[i] = Acit.hexagon(x, y, r);
                hexFill[i] = new Paint(Paint.ANTI_ALIAS_FLAG);
                hexFill[i].setShader(new RadialGradient(x, y, r, new int[] { HEX_INNER, HEX_OUTER }, new float[] { 88 / 255f, 1f }, Shader.TileMode.CLAMP));
                hexGlow[i] = new Paint(Paint.ANTI_ALIAS_FLAG);
                hexGlow[i].setShader(new RadialGradient(x, y, r * 1.45f, new int[] { 0xB0E08E16, 0x00D88A16 }, null, Shader.TileMode.CLAMP));
            }
            radialLine.setStrokeWidth(Math.max(1, u * 0.22f)); radialAccent.setStrokeWidth(Math.max(2, u * 0.34f));
            /* Ring paging at the sides, between the corners. */
            float sideTop = body.top + corner + qGap, sideBottom = body.bottom - corner - qGap;
            float pageH = Math.min(sideBottom - sideTop, Math.max(corner * 0.8f, 12 * u)), midY = (sideTop + sideBottom) / 2;
            pageLeft.set(body.left, midY - pageH / 2, body.left + corner, midY + pageH / 2);
            pageRight.set(body.right - corner, midY - pageH / 2, body.right, midY + pageH / 2);
            mapView.set(body.left, body.top + title.getTextSize() * 1.7f, body.right, body.bottom - small.getTextSize() * 2.4f);
            arrowEdge.setStrokeWidth(Math.max(1, u * 0.3f));
            /* actions: two columns, four rows, the last one across both */
            final int rows = 4;
            float cg = body.width() * 0.03f, rg = body.height() * 0.035f;
            float gw = (body.width() - cg) / 2f, gh = (body.height() - rg * (rows - 1) - 5 * u) / rows;
            for (int i = 0; i < ACTIONS.length; i++) {
                int col = i % 2, row = i / 2;
                float l = body.left + col * (gw + cg), t = body.top + row * (gh + rg);
                actRect[i] = i == ACTIONS.length - 1 ? new RectF(body.left, t, body.right, t + gh) : new RectF(l, t, l + gw, t + gh);
            }
            /* status: frame rate box top left, the graph beside it, threads, then the facts */
            float th = body.height();
            fpsBox.set(body.left, body.top, body.left + body.width() * 0.34f, body.top + th * 0.30f);
            graph.set(fpsBox.right + gap * 1.5f, body.top, body.right, fpsBox.bottom);
            threads.set(body.left, fpsBox.bottom + gap * 1.5f, body.right, body.top + th * 0.72f);
            facts.set(body.left, threads.bottom + gap * 1.5f, body.right, body.bottom);
        }

        /** Everything that does not change on this tab, drawn once (software: the glows blur). */
        private Bitmap buildChrome(int w, int h) {
            Bitmap b = Acit.Background.compose(w, h, art, false);
            Canvas c = new Canvas(b);
            drawChrome(c);
            return b;
        }

        private void drawChrome(Canvas c) {
            for (int i = 0; i < TABS.length; i++) {
                RectF r = tabRect[i];
                Path p = Acit.chamfer(r, r.height() * 0.3f);
                boolean on = i == tab;
                c.drawPath(p, on ? glowHot : glow); c.drawPath(p, on ? panelHot : panel); c.drawPath(p, on ? edgeHot : edge);
                title.setColor(on ? Acit.ORANGE : Acit.TEXT_DIM);
                Acit.fitText(c, TABS[i], r.centerX(), Acit.mid(title, r.centerY()), r.width() - 2 * u, title);
            }
            title.setColor(Acit.TEXT);
            Path sp = Acit.chamfer(strip, strip.height() * 0.3f);
            c.drawPath(sp, panelDark); c.drawPath(sp, Acit.stroke(Acit.EDGE_DIM, edge.getStrokeWidth() * 0.7f));
            if (tab == T_QUICK) quickChrome(c);
            else if (tab == T_MAP) mapChrome(c);
            else if (tab == T_ACTIONS) for (int i = 0; i < ACTIONS.length; i++) actionTile(c, i, false);
            else if (tab == T_STATUS) statusChrome(c);
            else clockChrome(c);
            chromeTab = tab;
        }

        @Override protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            if (w <= 0 || h <= 0) return;
            if (chrome == null || chromeTab != tab || chrome.getWidth() != w || chrome.getHeight() != h) {
                if (u == 0) layout(w, h);
                if (!cacheUnavailable) {
                    try { chrome = buildChrome(w, h); }
                    catch (OutOfMemoryError lowMemory) { chrome = null; cacheUnavailable = true; }
                }
            }
            if (chrome != null) c.drawBitmap(chrome, 0, 0, null);
            else { Acit.Background.paint(c, w, h, art, false); drawChrome(c); }
            if (tab == T_QUICK) quickLive(c); else if (tab == T_MAP) mapLive(c); else if (tab == T_ACTIONS) actionsLive(c);
            else if (tab == T_STATUS) statusLive(c); else clockLive(c);
            stripLive(c);
            if (accessibilityFocus != NONE) {
                RectF focus = nodeBounds(accessibilityFocus);
                if (focus != null) c.drawRoundRect(focus, u, u, radialAccent);
            }
            if (dim > 0) { dimP.setAlpha(Math.round(dim * 0xA8)); c.drawRect(0, 0, w, h, dimP); }
        }

        private boolean hot(long now) { return now < hotUntil; }

        /* ---- quick select ----------------------------------------------------- */

        private void quickChrome(Canvas c) {
            for (int i = 0; i < 8; i++) { c.drawPath(hexes[i], hexFill[i]); c.drawPath(hexes[i], radialLine); }
            sideZone(c, pageLeft, GlyphCore.L1, "PREV RING", false);
            sideZone(c, pageRight, GlyphCore.R1, "NEXT RING", false);
            for (int i = 0; i < GAME_N; i++) { Path p = gamePath(i); c.drawPath(p, glow); gameButton(c, i, false); }
        }

        private Path gamePath(int i) { RectF r = gameRect[i]; return Acit.chamfer(r, Math.min(r.width(), r.height()) * 0.16f); }

        /** A gameplay button: a d-pad with its arm lit for the corners, R2 and words for the wrench throw. */
        private void gameButton(Canvas c, int i, boolean active) {
            RectF r = gameRect[i];
            Path p = gamePath(i);
            c.drawPath(p, active ? panelHot : panelDark); c.drawPath(p, active ? edgeHot : edge);
            small.setColor(active ? Acit.ORANGE : Acit.TEXT_DIM);
            if (i < 4) {
                float s = Math.min(r.width(), r.height());
                dpad(c, r.centerX(), r.top + r.height() * 0.43f, s * 0.5f, i, active);
                Acit.fitText(c, GAME_LABEL[i], r.centerX(), r.bottom - s * 0.1f, r.width() - 2 * u, small);
            } else {
                Bitmap icon = glyph(GlyphCore.R2);
                float h = r.height() * 0.46f, w = icon != null ? h * icon.getWidth() / Math.max(1, icon.getHeight()) : 0;
                label.setColor(active ? Acit.ORANGE : Acit.TEXT);
                float tw = Math.min(label.measureText(GAME_LABEL[i]), r.width() - w - 5 * u), x = r.centerX() - (w + 1.5f * u + tw) / 2;
                if (icon != null) {
                    bitmapRect.set(x, r.centerY() - h / 2, x + w, r.centerY() + h / 2);
                    c.drawBitmap(icon, null, bitmapRect, bitmapPaint);
                    x += w + 1.5f * u;
                } else x = r.centerX() - tw / 2;
                label.setTextAlign(Paint.Align.LEFT);
                Acit.fitText(c, GAME_LABEL[i], x, Acit.mid(label, r.centerY()), tw, label);
                label.setTextAlign(Paint.Align.CENTER); label.setColor(Acit.TEXT);
            }
            small.setColor(Acit.TEXT_DIM);
        }

        /** A d-pad at (x, y), s across, with arm `dir` (0 up, 1 right, 2 down, 3 left) lit. */
        private void dpad(Canvas c, float x, float y, float s, int dir, boolean active) {
            float w = s * 0.36f, h = s / 2, rr = w * 0.18f;
            Paint base = Acit.fill(active ? 0xFF24434A : 0xFF14272C), lit = Acit.fill(active ? Acit.ORANGE : QS_ORANGE);
            c.drawRoundRect(x - w / 2, y - h, x + w / 2, y + h, rr, rr, base);
            c.drawRoundRect(x - h, y - w / 2, x + h, y + w / 2, rr, rr, base);
            float dx = dir == 1 ? 1 : dir == 3 ? -1 : 0, dy = dir == 0 ? -1 : dir == 2 ? 1 : 0;
            /* the lit arm: from just off the middle to its end */
            float ax = x + dx * (w / 2 + h) / 2, ay = y + dy * (w / 2 + h) / 2;
            float hw = dx != 0 ? (h - w / 2) / 2 : w / 2, hh = dy != 0 ? (h - w / 2) / 2 : w / 2;
            c.drawRoundRect(ax - hw, ay - hh, ax + hw, ay + hh, rr, rr, lit);
            /* an arrow in it, pointing out */
            float t = w * 0.26f;
            Path a = new Path();
            a.moveTo(ax + dx * t, ay + dy * t);
            a.lineTo(ax - dx * t * 0.6f - dy * t, ay - dy * t * 0.6f + dx * t);
            a.lineTo(ax - dx * t * 0.6f + dy * t, ay - dy * t * 0.6f - dx * t);
            a.close();
            c.drawPath(a, Acit.fill(0xE0081519));
        }

        /** Ring paging at a side: the shoulder button's icon above its words. */
        private void sideZone(Canvas c, RectF r, int code, String text, boolean active) {
            c.drawRoundRect(r, u * 0.7f, u * 0.7f, active ? panelHot : panelDark);
            c.drawLine(r.left, r.bottom, r.right, r.bottom, active ? radialAccent : radialLine);
            Bitmap icon = glyph(code);
            float textY = r.bottom - r.height() * 0.2f;
            if (icon != null) {
                float height = Math.min(r.height() * 0.34f, r.width() * 0.3f), width = height * icon.getWidth() / Math.max(1, icon.getHeight());
                float mid = r.top + r.height() * 0.4f;
                bitmapRect.set(r.centerX() - width / 2, mid - height / 2, r.centerX() + width / 2, mid + height / 2);
                c.drawBitmap(icon, null, bitmapRect, bitmapPaint);
            } else textY = Acit.mid(small, r.centerY());
            small.setColor(active ? Acit.ORANGE : Acit.TEXT_DIM);
            Acit.fitText(c, text, r.centerX(), textY, r.width() - 2 * u, small);
            small.setColor(Acit.TEXT_DIM);
        }

        /** Index into the game's table for a direction, clockwise from the top. Its ring
         *  puts a slot at angle (hexIndex + 3) / 4 pi, hexIndex = 7 - slot: slot 2 is on top. */
        private int ringSlot(int direction) { return ringPage * 8 + ((direction + 2) & 7); }
        private int ringWeapon(int direction) { return status.quickSlots[ringSlot(direction)]; }
        /** A hex's size: the diagonals are the game's odd slots, shown at 75% (oddScale). */
        private float hexR(int direction) { return slotR * ((direction & 1) != 0 ? 0.75f : 1f); }

        private void drawWeapon(Canvas c, int weapon, float x, float y, float r, int color) {
            Bitmap icon = weaponIcons != null && weapon >= 0 && weapon < weaponIcons.length ? weaponIcons[weapon] : null;
            if (icon == null) {
                small.setColor(color);
                Acit.fitText(c, WeaponIcons.name(weapon), x, Acit.mid(small, y), r * 2, small);
                return;
            }
            bitmapRect.set(x - r, y - r, x + r, y + r);
            iconPaint.setColor(color);
            c.drawBitmap(icon, null, bitmapRect, iconPaint);
        }

        private void quickLive(Canvas c) {
            float cx = wheelBounds.centerX(), cy = wheelBounds.centerY();
            int equipped = (int) status.weapon;
            boolean any = false;
            for (int i = 0; i < 8; i++) {
                int weapon = ringWeapon(i);
                boolean active = weapon >= 0 && weapon == equipped || i == hotSlot;
                if (active) {
                    c.drawCircle(slot[i][0], slot[i][1], hexR(i) * 1.45f, hexGlow[i]);
                    c.drawPath(hexes[i], hexFill[i]); c.drawPath(hexes[i], radialAccent);
                }
                if (weapon < 0) continue;
                any = true;
                drawWeapon(c, weapon, slot[i][0], slot[i][1], hexR(i) * 0.62f, active ? QS_ORANGE : 0xFFFFFFFF);
            }
            if (pressed == PAGE_LEFT) sideZone(c, pageLeft, GlyphCore.L1, "PREV RING", true);
            else if (pressed == PAGE_RIGHT) sideZone(c, pageRight, GlyphCore.R1, "NEXT RING", true);
            for (int i = 0; i < GAME_N; i++) if (gamePointer[i] >= 0) gameButton(c, i, true);
            if (equipped >= 0) drawWeapon(c, equipped, cx, cy, ringR * 0.26f, 0xFFFFFFFF);
            /* above the ring, between the top corners: the ring, then what the last touch did */
            float lineW = gameRect[1].left - gameRect[0].right - 2 * u, lineY = body.top + small.getTextSize() * 1.1f;
            String hint = !inputHint.isEmpty() && hot(SystemClock.uptimeMillis()) ? inputHint
                : preview ? "PREVIEW · GAME INPUT DISABLED"
                : any ? "Tap a weapon to equip it" : "Your weapons appear during Ratchet gameplay";
            small.setColor(Acit.ORANGE_SOFT);
            Acit.fitText(c, "RING " + (ringPage + 1) + " OF " + RINGS + "  ·  " + hint, cx, lineY, lineW, small);
            small.setColor(Acit.TEXT_DIM);
        }

        /* ---- map ---------------------------------------------------------------- */

        /* The player, read from the bridge ten times a second while this page shows;
         * the level's walkable ground from LevelMap, loaded when the level changes. */
        private static final float MAP_RADIUS = 45;   /* world units from the player to the view's edge */
        private boolean heroKnown, mapFull, mapLoading;
        private float heroX, heroZ, heroFx, heroFz;
        private int heroLevel = -1, mapWanted = -1;
        private LevelMap levelMap;
        private final RectF mapView = new RectF();
        private final Paint mapPaint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Paint arrowPaint = Acit.fill(QS_ORANGE), arrowEdge = Acit.stroke(0xFF101A30, 2);

        private final Runnable mapTick = new Runnable() {
            @Override public void run() {
                if (!attached || tab != T_MAP) return;
                if (!preview) try {
                    long[] v = NativeBridge.stats();
                    if (v != null && v.length > NativeBridge.HERO + 5) {
                        heroKnown = v[NativeBridge.HERO] == 1; heroLevel = (int) v[NativeBridge.LEVEL_ID];
                        heroX = heroBits(v, 1); heroZ = heroBits(v, 3); heroFx = heroBits(v, 4); heroFz = heroBits(v, 5);
                    }
                } catch (UnsatisfiedLinkError unavailable) { heroKnown = false; }
                if (heroLevel > 0 && heroLevel != mapWanted) loadMap(heroLevel);
                invalidate();
                postDelayed(this, 100);
            }
        };

        private static float heroBits(long[] v, int k) { return Float.intBitsToFloat((int) v[NativeBridge.HERO + k]); }

        private void loadMap(final int level) {
            mapWanted = level;
            if (levelMap != null && levelMap.level == level) return;
            levelMap = null; mapLoading = true;
            final File root = store.gameRoot();
            new Thread(() -> {
                LevelMap m = null;
                try { m = LevelMap.load(root, level); }
                catch (IOException | RuntimeException | OutOfMemoryError unavailable) { /* "No map" */ }
                final LevelMap loaded = m;
                post(() -> { if (mapWanted == level) { levelMap = loaded; mapLoading = false; invalidate(); } });
            }, "companion map").start();
        }

        private void mapChrome(Canvas c) {
            Path p = Acit.chamfer(mapView, u * 1.5f);
            c.drawPath(p, panelDark); c.drawPath(p, radialLine);
        }

        private void mapLive(Canvas c) {
            String name = LevelMap.name(heroLevel);
            title.setColor(Acit.TEXT);
            Acit.fitText(c, name.isEmpty() ? "MAP" : name, body.centerX(), body.top + title.getTextSize(), body.width() - 2 * u, title);
            LevelMap m = levelMap;
            String hint;
            if (m == null) {
                hint = preview || heroLevel <= 0 ? "The map appears during gameplay" : mapLoading ? "Loading the map" : "No map for this place";
            } else {
                float w = m.image.getWidth() / m.scale, h = m.image.getHeight() / m.scale;   /* the picture in world units */
                float s, ox, oz;                                                              /* pixels a unit; the world point at the centre */
                if (mapFull || !heroKnown) { s = Math.min(mapView.width() / w, mapView.height() / h) * 0.96f; ox = m.x0 + w / 2; oz = m.z0 + h / 2; }
                else { s = Math.min(mapView.width(), mapView.height()) / 2 / MAP_RADIUS; ox = heroX; oz = heroZ; }
                float cx = mapView.centerX(), cy = mapView.centerY();
                c.save();
                c.clipRect(mapView.left + u * 0.4f, mapView.top + u * 0.4f, mapView.right - u * 0.4f, mapView.bottom - u * 0.4f);
                bitmapRect.set(cx + (m.x0 - ox) * s, cy + (m.z0 - oz) * s, cx + (m.x0 - ox + w) * s, cy + (m.z0 - oz + h) * s);
                c.drawBitmap(m.image, null, bitmapRect, mapPaint);
                if (heroKnown) heroArrow(c, cx + (heroX - ox) * s, cy + (heroZ - oz) * s);
                c.restore();
                hint = mapFull ? "Tap for the area around you" : "Tap for the whole level";
            }
            small.setColor(Acit.TEXT_DIM);
            Acit.fitText(c, hint, body.centerX(), body.bottom - u, body.width() - 2 * u, small);
        }

        /** An arrowhead at x, y pointing where the player faces (x right, z down, as the picture). */
        private void heroArrow(Canvas c, float x, float y) {
            float len = (float) Math.hypot(heroFx, heroFz), fx = len > 0 ? heroFx / len : 0, fz = len > 0 ? heroFz / len : 1, l = u * 3.2f;
            Path p = new Path();
            p.moveTo(x + fx * l, y + fz * l);
            p.lineTo(x - fx * l * 0.6f - fz * l * 0.55f, y - fz * l * 0.6f + fx * l * 0.55f);
            p.lineTo(x - fx * l * 0.25f, y - fz * l * 0.25f);
            p.lineTo(x - fx * l * 0.6f + fz * l * 0.55f, y - fz * l * 0.6f - fx * l * 0.55f);
            p.close();
            c.drawPath(p, arrowPaint); c.drawPath(p, arrowEdge);
        }

        /* ---- actions ---------------------------------------------------------- */

        private void actionTile(Canvas c, int i, boolean on) {
            RectF r = actRect[i];
            Path p = Acit.chamfer(r, Math.min(r.height(), r.width()) * 0.2f);
            c.drawPath(p, on ? glowHot : glow); c.drawPath(p, on ? panelHot : panel); c.drawPath(p, on ? edgeHot : edge);
            Bitmap g = ACTION_GLYPH[i] != 0 ? glyph(ACTION_GLYPH[i]) : null;
            float x = r.left + r.height() * 0.28f, cy = r.centerY();
            if (g != null) {
                float ih = r.height() * 0.40f, iw = ih * g.getWidth() / Math.max(1, g.getHeight());
                c.drawBitmap(g, null, new RectF(x, cy - ih / 2, x + iw, cy + ih / 2), null);
                x += iw + r.height() * 0.2f;
            } else {
                Path h = Acit.hexagon(x + r.height() * 0.13f, cy, r.height() * 0.15f);
                c.drawPath(h, Acit.stroke(on ? Acit.ORANGE : Acit.EDGE, edge.getStrokeWidth()));
                x += r.height() * 0.46f;
            }
            label.setTextAlign(Paint.Align.LEFT);
            label.setColor(on ? Acit.ORANGE : Acit.TEXT);
            float right = r.right - r.height() * 0.2f;
            Acit.fitText(c, ACTIONS[i][0], x, cy - label.getTextSize() * 0.12f, right - x, label);
            smallL.setColor(on ? Acit.ORANGE_SOFT : Acit.TEXT_DIM);
            Acit.fitText(c, ACTIONS[i][1], x, cy + smallL.getTextSize() * 1.15f, right - x, smallL);
            label.setTextAlign(Paint.Align.CENTER); label.setColor(Acit.TEXT);
            smallL.setColor(Acit.TEXT_DIM);
        }

        private void actionsLive(Canvas c) {
            if (hotAction >= 0 && hot(SystemClock.uptimeMillis())) actionTile(c, hotAction, true);
            small.setColor(Acit.ORANGE_SOFT);
            c.drawText(preview ? "PREVIEW · GAME INPUT DISABLED" : hot(SystemClock.uptimeMillis()) ? inputHint : "Touch and release to send a command",
                body.centerX(), body.bottom - u, small);
            small.setColor(Acit.TEXT_DIM);
        }

        /* ---- status ----------------------------------------------------------- */

        private void box(Canvas c, RectF r, String head) {
            Path p = Acit.chamfer(r, Math.min(r.height(), r.width()) * 0.08f);
            c.drawPath(p, panelDark); c.drawPath(p, Acit.stroke(Acit.EDGE_DIM, edge.getStrokeWidth() * 0.7f));
            if (head != null) {
                Paint hp = Acit.text(Acit.EDGE, small.getTextSize() * 0.9f, Acit.titleFace()); hp.setTextAlign(Paint.Align.LEFT);
                hp.setLetterSpacing(0.08f);
                c.drawText(head, r.left + 2 * u, r.top + 3 * u, hp);
            }
        }

        private void statusChrome(Canvas c) {
            box(c, fpsBox, "FRAME RATE");
            box(c, graph, "THE LAST 60 SECONDS");
            box(c, threads, "BUSIEST THREADS  (% OF ONE CORE)");
            box(c, facts, null);
            RectF g = plot();
            Paint ref = Acit.stroke(Acit.alpha(Acit.EDGE_DIM, 0.8f), edge.getStrokeWidth() * 0.6f);
            float y60 = g.bottom - g.height() * 60f / 70f, y30 = g.bottom - g.height() * 30f / 70f;
            c.drawLine(g.left, y60, g.right, y60, ref); c.drawLine(g.left, y30, g.right, y30, ref);
            smallL.setColor(Acit.TEXT_OFF);
            c.drawText("60", g.left + u * 0.5f, y60 - u * 0.6f, smallL); c.drawText("30", g.left + u * 0.5f, y30 - u * 0.6f, smallL);
            smallL.setColor(Acit.TEXT_DIM);
        }

        private RectF plot() { return new RectF(graph.left + 2 * u, graph.top + 4.5f * u, graph.right - 2 * u, graph.bottom - 2 * u); }

        private int fpsColor(float f) { return f >= 55 ? Acit.TEXT_GOOD : f >= 40 ? Acit.ORANGE : Acit.TEXT_BAD; }

        private void statusLive(Canvas c) {
            boolean live = status.hasFrameSample;
            /* the frame rate, a gauge around it (a full arc is 60) */
            float cx = fpsBox.centerX(), cy = fpsBox.top + fpsBox.height() * 0.56f, R = Math.min(fpsBox.width(), fpsBox.height()) * 0.36f;
            RectF arc = new RectF(cx - R, cy - R, cx + R, cy + R);
            Paint ring = Acit.stroke(Acit.alpha(Acit.EDGE_DIM, 0.6f), u * 1.1f); ring.setStrokeCap(Paint.Cap.ROUND);
            c.drawArc(arc, 135, 270, false, ring);
            if (live) {
                ring.setColor(fpsColor(fps));
                c.drawArc(arc, 135, 270 * Math.min(1, fps / 60f), false, ring);
            }
            big.setTextSize(R * 0.78f); big.setColor(live ? Acit.TEXT : Acit.TEXT_OFF);
            c.drawText(live ? String.format(Locale.US, "%.0f", fps) : "--", cx, Acit.mid(big, cy), big);
            small.setColor(Acit.TEXT_DIM);
            c.drawText(live && frameMs > 0 ? String.format(Locale.US, "%.1f ms avg", frameMs) : preview ? "preview" : "waiting", cx, cy + R * 0.62f, small);
            if (Float.isFinite(status.sampleLowFps10s)) c.drawText(String.format(Locale.US, "sample low %.0f / 10 s", status.sampleLowFps10s), cx, fpsBox.bottom - u * 1.4f, small);

            /* the graph: a filled line */
            RectF g = plot();
            int n = Math.min(histN, hist.length);
            if (n > 1) {
                Path p = new Path(), fillPath = new Path();
                for (int i = 0; i < n; i++) {
                    float v = hist[(histN - n + i) % hist.length];
                    float px = g.right - g.width() * Math.min(1, status.historyAgeMs(status.historySize() - n + i) / 60000f);
                    float py = g.bottom - g.height() * Math.min(v, 70f) / 70f;
                    if (i == 0) { p.moveTo(px, py); fillPath.moveTo(px, g.bottom); }
                    p.lineTo(px, py); fillPath.lineTo(px, py);
                    if (i == n - 1) fillPath.lineTo(px, g.bottom);
                }
                fillPath.close();
                area.setShader(new LinearGradient(0, g.top, 0, g.bottom, 0x70FFB43C, 0x00FFB43C, Shader.TileMode.CLAMP));
                c.drawPath(fillPath, area);
                c.drawPath(p, line);
            }

            /* threads */
            float row = (threads.height() - 5 * u) / 5.4f, ty = threads.top + 5.5f * u + row * 0.4f;
            float barL = threads.left + threads.width() * 0.36f, barR = threads.right - threads.width() * 0.12f;
            Paint barBg = Acit.fill(0x60081519), barFg = Acit.fill(Acit.ORANGE), barCyan = Acit.fill(Acit.EDGE),
                  barWait = Acit.fill(0xB0081519);
            smallL.setColor(Acit.TEXT_DIM);
            if (status.top.isEmpty()) c.drawText(bridge ? "Measuring threads..." : "", threads.left + 2 * u, ty, smallL);
            for (int i = 0; i < Math.min(5, status.top.size()); i++) {
                PerfProbe.Busy t = status.top.get(i);
                float ry = ty + i * row, pct = Math.min(t.pct, 100f), bh = Math.min(row * 0.5f, 2.4f * u);
                Acit.fitText(c, t.name, threads.left + 2 * u, ry, barL - threads.left - 3.5f * u, smallL);
                RectF bar = new RectF(barL, ry - bh, barR, ry);
                c.drawRect(bar, barBg);
                bar.right = barL + (barR - barL) * pct / 100f;
                c.drawRect(bar, pct >= 90f ? barFg : barCyan);
                /* the PPU main thread: the end of its bar that was its SPU job wait, dimmed */
                if (Float.isFinite(t.wait) && t.wait > 0f) {
                    RectF w = new RectF(bar); w.left = Math.max(bar.left, bar.right - (barR - barL) * Math.min(t.wait, pct) / 100f);
                    c.drawRect(w, barWait);
                }
                c.drawText(String.format(Locale.US, "%3.0f%%", t.pct), barR + 1.5f * u, ry, smallL);
            }

            /* the facts: CPU, GPU, heat, resolution */
            float fy = facts.top + facts.height() * 0.38f, fy2 = facts.top + facts.height() * 0.78f, fx = facts.left + 2 * u;
            String gpu = status.gpuBusy >= 0 ? "GPU " + status.gpuBusy + "%" + (status.gpuMhz > 0 ? " at " + status.gpuMhz + " MHz" : "") : "GPU load not readable here";
            smallL.setColor(Acit.TEXT);
            if (preview) {
                Acit.fitText(c, "Game telemetry appears when playing", fx, fy, facts.width() - 4 * u, smallL);
                smallL.setColor(Acit.TEXT_DIM);
                Acit.fitText(c, "Configured resolution " + resolution() + " · No game input in preview", fx, fy2, facts.width() - 4 * u, smallL);
                return;
            }
            Acit.fitText(c, Float.isFinite(status.processPct) ? String.format(Locale.US, "Game %.0f%% of a core   ·   CPU %s", status.processPct, status.clocks)
                : status.hasPerfSample ? "CPU activity unavailable" : "Measuring CPU activity...", fx, fy, facts.width() - 4 * u, smallL);
            String heat = "Heat: " + status.thermalName() + (Float.isNaN(status.headroom) ? "" : String.format(Locale.US, " (headroom %.2f)", status.headroom));
            smallL.setColor(status.thermal >= 3 ? Acit.TEXT_BAD : status.thermal >= 2 ? Acit.ORANGE : Acit.TEXT_DIM);
            String ab = abLine();
            if (ab != null) { smallL.setColor(Acit.ORANGE); Acit.fitText(c, ab + "   ·   " + heat, fx, fy2, facts.width() - 4 * u, smallL); }
            else Acit.fitText(c, heat + "   ·   " + gpu + "   ·   Resolution " + resolution(), fx, fy2, facts.width() - 4 * u, smallL);
            smallL.setColor(Acit.TEXT_DIM);
        }

        /** The core swap A/B test's running totals (RACACIT_AB, main.cpp), or null when it is off. */
        private String abLine() {
            long[] v;
            try { v = NativeBridge.stats(); } catch (UnsatisfiedLinkError unavailable) { return null; }
            if (v == null || v.length < NativeBridge.AB + 6 || v[NativeBridge.AB] == 0) return null;
            int k = NativeBridge.AB;
            return String.format(Locale.US, "A/B now %s · swap off %.1f fps (%d) · on %.1f fps (%d)",
                    v[k + 1] != 0 ? "ON" : "OFF", v[k + 3] / 100f, v[k + 2], v[k + 5] / 100f, v[k + 4]);
        }

        private String resolution() {
            String r = store.setting("RACACIT_RES_SCALE", "1");
            try { return Math.round(Float.parseFloat(r) * 100) + "%"; } catch (NumberFormatException e) { return r; }
        }

        /* ---- clock ------------------------------------------------------------- */

        private float clockR() { return Math.min(body.width() / 3.0f, (body.height() - title.getTextSize() * 5.5f) / 2.85f); }

        private void clockChrome(Canvas c) {
            float R = clockR(), cx = body.centerX(), cy = body.top + R * 1.38f;
            c.drawCircle(cx, cy, R * 1.35f, Acit.glow(Acit.alpha(Acit.EDGE, 0.35f), u * 0.6f, u * 3));
        }

        private void clockLive(Canvas c) {
            float R = clockR(), cx = body.centerX(), cy = body.top + R * 1.38f;
            clock.draw(c, cx, cy, R, 0);
            Calendar t = Calendar.getInstance();
            float y = cy + R * 1.42f + title.getTextSize() * 0.6f;
            big.setTextSize(title.getTextSize() * 2.1f); big.setColor(Acit.TEXT);
            c.drawText(String.format(Locale.US, "%02d:%02d", t.get(Calendar.HOUR_OF_DAY), t.get(Calendar.MINUTE)), cx, y + big.getTextSize() * 0.35f, big);
            small.setColor(Acit.TEXT_DIM);
            float y2 = y + big.getTextSize() * 0.35f + small.getTextSize() * 1.8f;
            c.drawText(DateFormat.getDateInstance(DateFormat.FULL).format(new Date()), cx, y2, small);
            small.setColor(Acit.ORANGE_SOFT);
            c.drawText((preview ? "Preview · " : "Playing for ") + session(), cx, y2 + small.getTextSize() * 1.5f, small);
            small.setColor(Acit.TEXT_DIM);
        }

        private String session() {
            long m = (SystemClock.elapsedRealtime() - started) / 60000;
            return m < 1 ? "less than a minute" : m < 60 ? m + " min" : (m / 60) + " h " + (m % 60) + " min";
        }

        /* ---- the strip --------------------------------------------------------- */

        private void stripLive(Canvas c) {
            float cy = strip.centerY(), x = strip.left + 2.2f * u;
            Calendar t = Calendar.getInstance();
            smallL.setColor(Acit.TEXT);
            String time = String.format(Locale.US, "%02d:%02d", t.get(Calendar.HOUR_OF_DAY), t.get(Calendar.MINUTE));
            c.drawText(time, x, Acit.mid(smallL, cy), smallL);
            x += smallL.measureText(time) + 3 * u;
            if (battery >= 0) {   /* a battery: its level filled, amber when low, a bolt when charging */
                float bw = 4.2f * u, bh = 2.1f * u;
                RectF b = new RectF(x, cy - bh / 2, x + bw, cy + bh / 2);
                Paint o = Acit.stroke(Acit.TEXT_DIM, u * 0.25f);
                c.drawRect(b, o); c.drawRect(b.right, cy - bh * 0.22f, b.right + u * 0.45f, cy + bh * 0.22f, Acit.fill(Acit.TEXT_DIM));
                RectF lv = new RectF(b.left + u * 0.4f, b.top + u * 0.4f, b.left + u * 0.4f + (bw - u * 0.8f) * battery / 100f, b.bottom - u * 0.4f);
                c.drawRect(lv, Acit.fill(battery <= 15 && !charging ? Acit.TEXT_BAD : charging ? Acit.TEXT_GOOD : Acit.TEXT_DIM));
                x = b.right + 1.6f * u;
                String bt = battery + "%" + (charging ? " charging" : "") + (Float.isNaN(batteryC) ? "" : String.format(Locale.US, "  %.0f°C", batteryC));
                smallL.setColor(!Float.isNaN(batteryC) && batteryC >= 45 ? Acit.ORANGE : Acit.TEXT_DIM);
                c.drawText(bt, x, Acit.mid(smallL, cy), smallL);
            }
            Paint r = Acit.text(Acit.TEXT_DIM, smallL.getTextSize(), Acit.bodyFace()); r.setTextAlign(Paint.Align.RIGHT);
            float rx = strip.right - 2.2f * u;
            String ses = preview ? "COMPANION PREVIEW" : "Playing " + session();
            c.drawText(ses, rx, Acit.mid(r, cy), r);
            rx -= r.measureText(ses) + 3 * u;
            boolean live = status.hasFrameSample;
            r.setTypeface(Acit.titleFace()); r.setColor(live ? fpsColor(fps) : Acit.TEXT_OFF);
            c.drawText(live ? String.format(Locale.US, "%.0f FPS", fps) : "-- FPS", rx, Acit.mid(r, cy), r);
            smallL.setColor(Acit.TEXT_DIM);
        }

        /* ---- touch ------------------------------------------------------------- */

        @Override public boolean onTouchEvent(MotionEvent e) {
            int action = e.getActionMasked();
            if (action == MotionEvent.ACTION_CANCEL) { releaseGame(); cancelPress(); return true; }
            if (action == MotionEvent.ACTION_DOWN) releaseGame();   /* no finger was down: nothing can be held */
            /* The gameplay buttons: each finger on its own, held until it lifts, even on a dimmed page. */
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                int index = e.getActionIndex(), g = gameAt(e.getX(index), e.getY(index));
                if (g >= 0) { gameDown(g, e.getPointerId(index)); return true; }
                /* a second finger beside a press cancels it, as before */
                if (action == MotionEvent.ACTION_POINTER_DOWN && pointerId >= 0) { cancelPress(); return true; }
                cancelPress();
                pointerId = e.getPointerId(index); lastTouch = SystemClock.uptimeMillis();
                waking = dim > 0.05f;
                if (waking) { fade(0); return true; }
                pressed = hit(e.getX(index), e.getY(index));
                if (pressed >= ACTION && pressed < ACTION + ACTIONS.length) { hotAction = pressed - ACTION; hotUntil = Long.MAX_VALUE; }
                if (pressed >= DIRECTION && pressed < DIRECTION + 8) hotSlot = pressed - DIRECTION;
                invalidate(); return true;
            }
            if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) && gameUp(e.getPointerId(e.getActionIndex()))) return true;
            if (pointerId < 0) return true;
            int pointer = e.findPointerIndex(pointerId);
            if (pointer < 0) { cancelPress(); return true; }
            /* the press's own finger lifting, also while another holds a gameplay button */
            boolean lifted = action == MotionEvent.ACTION_UP ||
                    action == MotionEvent.ACTION_POINTER_UP && e.getPointerId(e.getActionIndex()) == pointerId;
            if (action == MotionEvent.ACTION_POINTER_UP && !lifted) return true;
            if (waking) { if (lifted) cancelPress(); return true; }
            lastTouch = SystemClock.uptimeMillis();
            boolean over = pressed != NONE && pressed == hit(e.getX(pointer), e.getY(pointer));
            if (action == MotionEvent.ACTION_MOVE) {
                if (pressed >= ACTION && pressed < ACTION + ACTIONS.length) hotAction = over ? pressed - ACTION : -1;
                if (pressed >= DIRECTION && pressed < DIRECTION + 8) hotSlot = over ? pressed - DIRECTION : -1;
            }
            if (lifted) {
                if (over) activate(pressed);
                else hotAction = -1;
                hotSlot = -1; pointerId = -1; pressed = NONE; performClick();
            }
            invalidate();
            return true;
        }

        private int gameAt(float x, float y) {
            if (tab != T_QUICK) return -1;
            for (int i = 0; i < GAME_N; i++) if (gameRect[i] != null && gameRect[i].contains(x, y)) return i;
            return -1;
        }

        private void gameHold(int g, boolean down) {
            if (preview) return;
            try {
                int r = NativeBridge.hold(GAME_MASK[g], down);
                /* A hold is refused for the host Settings or a movie only, never for the controller. */
                if (down && r == NativeBridge.CONTEXT) {
                    inputHint = (status.flags & NativeBridge.FLAG_HOST_MODAL) != 0 ? "Close Settings, then try again" : "Available after the movie";
                    hotUntil = SystemClock.uptimeMillis() + 2400; announceForAccessibility(inputHint);
                } else if (down && r != NativeBridge.OK) feedback(r, "");
            } catch (UnsatisfiedLinkError unavailable) { if (down) feedback(NativeBridge.NOT_READY, ""); }
        }

        private void gameDown(int g, int pid) {
            lastTouch = SystemClock.uptimeMillis();
            if (dim > 0) fade(0);
            if (gamePointer[g] >= 0) return;   /* another finger has it */
            gamePointer[g] = pid;
            gameHold(g, true);
            performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
            invalidate();
        }

        /** The gameplay button this finger held, let go; false if it held none. */
        private boolean gameUp(int pid) {
            for (int g = 0; g < GAME_N; g++) if (gamePointer[g] == pid) {
                gamePointer[g] = -1; gameHold(g, false); lastTouch = SystemClock.uptimeMillis(); invalidate();
                return true;
            }
            return false;
        }

        private void releaseGame() {
            for (int g = 0; g < GAME_N; g++) if (gamePointer[g] >= 0) { gamePointer[g] = -1; gameHold(g, false); invalidate(); }
        }

        @Override public boolean performClick() { super.performClick(); return true; }

        private int hit(float x, float y) {
            for (int i = 0; i < TABS.length; i++) if (tabRect[i] != null && tabRect[i].contains(x, y)) return TAB + i;
            if (tab == T_ACTIONS) {
                for (int i = 0; i < ACTIONS.length; i++) if (actRect[i] != null && actRect[i].contains(x, y)) return ACTION + i;
            } else if (tab == T_MAP) {
                if (mapView.contains(x, y)) return MAP_VIEW;
            } else if (tab == T_QUICK) {
                int g = gameAt(x, y);
                if (g >= 0) return GAME + g;
                if (pageLeft.contains(x, y)) return PAGE_LEFT;
                if (pageRight.contains(x, y)) return PAGE_RIGHT;
                int sector = CompanionRadial.direction(x - wheelBounds.centerX(), y - wheelBounds.centerY(), ringR * 0.43f, ringR * 1.06f);
                if (sector >= 0) return DIRECTION + sector;
            }
            return NONE;
        }

        private void cancelPress() {
            waking = false; pointerId = -1; pressed = NONE; hotSlot = hotAction = -1;
            invalidate();
        }

        private void feedback(int result, String accepted) {
            inputHint = result == NativeBridge.OK ? accepted : result == NativeBridge.BUSY ? "Finish the current input first"
                : result == NativeBridge.CONTEXT ? (status.flags & NativeBridge.FLAG_PHYSICAL_INPUT) != 0 ? "Release the controller, then try again"
                    : (status.flags & NativeBridge.FLAG_HOST_MODAL) != 0 ? "Close Settings, then try again"
                    : (status.flags & NativeBridge.FLAG_MOVIE) != 0 ? "Available after the movie"
                    : tab == T_QUICK ? "Available during Ratchet gameplay" : "Finish the game menu, then try again"
                : result == NativeBridge.STALLED ? "Waiting for the game to respond" : "Waiting for gameplay";
            hotUntil = SystemClock.uptimeMillis() + 2400;
            if (!inputHint.isEmpty()) announceForAccessibility(inputHint);
            invalidate();
        }

        private void activate(int target) {
            lastTouch = SystemClock.uptimeMillis();
            if (dim > 0.05f) { fade(0); return; }
            if (target >= TAB && target < TAB + TABS.length) {
                if (accessibilityFocus >= ACTION) { virtualEvent(accessibilityFocus, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED); accessibilityFocus = NONE; }
                hoveredNode = NONE;
                releaseGame(); cancelPress(); tab = target - TAB; telemetry.setStatusVisible(tab == T_STATUS); chrome = null; invalidate();
                removeCallbacks(mapTick); if (tab == T_MAP) post(mapTick);
                sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED); return;
            }
            if (target >= ACTION && target < ACTION + ACTIONS.length) {
                int i = target - ACTION; hotAction = i; hotUntil = SystemClock.uptimeMillis() + 300;
                if (!preview) try {
                    if (ACTION_MASK[i] != 0) feedback(NativeBridge.press(ACTION_MASK[i], 6), "Input sent");
                    else { NativeBridge.openMenu(); feedback(NativeBridge.OK, "Settings requested"); }
                } catch (UnsatisfiedLinkError unavailable) { feedback(NativeBridge.NOT_READY, ""); }
            } else if (target >= DIRECTION && target < DIRECTION + 8) {
                int weapon = ringWeapon(target - DIRECTION);
                if (weapon < 0) feedback(NativeBridge.OK, "This slot is empty");
                else if (!preview) try { feedback(NativeBridge.equip(weapon), WeaponIcons.name(weapon)); }
                    catch (UnsatisfiedLinkError unavailable) { feedback(NativeBridge.NOT_READY, ""); }
            } else if (target >= GAME && target < GAME + GAME_N) {
                /* from accessibility: a tap (the native side keeps it down for a few flips) */
                int g = target - GAME;
                if (gamePointer[g] < 0) { gameHold(g, true); gameHold(g, false); }
            } else if (target == MAP_VIEW) {
                mapFull = !mapFull;
            } else if (target == PAGE_LEFT || target == PAGE_RIGHT) {
                ringPage = (ringPage + (target == PAGE_LEFT ? RINGS - 1 : 1)) % RINGS;
            }
            performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY); invalidate();
        }

        private RectF nodeBounds(int id) {
            if (id >= TAB && id < TAB + TABS.length) return tabRect[id - TAB];
            if (tab == T_ACTIONS && id >= ACTION && id < ACTION + ACTIONS.length) return actRect[id - ACTION];
            if (tab == T_MAP && id == MAP_VIEW) return mapView;
            if (tab == T_QUICK) {
                if (id >= GAME && id < GAME + GAME_N) return gameRect[id - GAME];
                if (id == PAGE_LEFT) return pageLeft;
                if (id == PAGE_RIGHT) return pageRight;
                if (id >= DIRECTION && id < DIRECTION + 8) {
                    int i = id - DIRECTION;
                    float r = Math.max(slotR, Math.min(Acit.dp(getContext(), 24), ringR * 0.26f));
                    return new RectF(slot[i][0] - r, slot[i][1] - r, slot[i][0] + r, slot[i][1] + r);
                }
            }
            return id == 500 && tab >= T_STATUS ? body : null;
        }

        private String nodeLabel(int id) {
            if (id >= TAB && id < TAB + TABS.length) return TABS[id - TAB] + " page";
            if (id >= ACTION && id < ACTION + ACTIONS.length) return ACTIONS[id - ACTION][0] + ". " + ACTIONS[id - ACTION][1];
            if (id >= DIRECTION && id < DIRECTION + 8) { int weapon = ringWeapon(id - DIRECTION); return weapon >= 0 ? "Equip " + WeaponIcons.name(weapon) : "Empty slot, " + DIRECTIONS[id - DIRECTION]; }
            if (id >= GAME && id < GAME + GAME_N) return GAME_SPOKEN[id - GAME];
            if (id == PAGE_LEFT) return "Previous quick select ring";
            if (id == PAGE_RIGHT) return "Next quick select ring";
            if (id == MAP_VIEW) return mapFull ? "Whole level map. Activate for the area around you." : "Map around you. Activate for the whole level.";
            if (id == 500 && tab == T_STATUS) return preview ? "Performance preview. Live readings appear during gameplay."
                : status.hasFrameSample ? String.format(Locale.US, "Sampled game frame rate %.0f FPS. %s", fps,
                    Float.isFinite(status.processPct) ? String.format(Locale.US, "Process CPU %.0f percent of one core. Thermal state %s.", status.processPct, status.thermalName()) : "CPU readings unavailable.")
                    : "Waiting for gameplay performance readings.";
            if (id == 500) return DateFormat.getDateTimeInstance().format(new Date()) + ". Session " + session();
            return "";
        }

        private void virtualEvent(int id, int type) {
            if (id == NONE || getParent() == null) return;
            AccessibilityEvent event = AccessibilityEvent.obtain(type);
            event.setSource(this, id); event.setClassName("android.widget.Button");
            event.setPackageName(getContext().getPackageName()); event.setContentDescription(nodeLabel(id));
            getParent().requestSendAccessibilityEvent(this, event);
        }

        private final AccessibilityNodeProvider nodes = new AccessibilityNodeProvider() {
            @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
                if (id == View.NO_ID) {
                    AccessibilityNodeInfo host = AccessibilityNodeInfo.obtain(Page.this);
                    Page.this.onInitializeAccessibilityNodeInfo(host);
                    host.setClassName("android.view.ViewGroup"); host.setContentDescription(preview ? "Companion preview. No game input." : "Dual-screen companion");
                    for (int i = 0; i < TABS.length; i++) host.addChild(Page.this, TAB + i);
                    if (tab == T_QUICK) {
                        for (int i = 0; i < 8; i++) host.addChild(Page.this, DIRECTION + i);
                        host.addChild(Page.this, PAGE_LEFT); host.addChild(Page.this, PAGE_RIGHT);
                        for (int i = 0; i < GAME_N; i++) host.addChild(Page.this, GAME + i);
                    } else if (tab == T_MAP) host.addChild(Page.this, MAP_VIEW);
                    else if (tab == T_ACTIONS) for (int i = 0; i < ACTIONS.length; i++) host.addChild(Page.this, ACTION + i);
                    else host.addChild(Page.this, 500);
                    return host;
                }
                RectF bounds = nodeBounds(id);
                if (bounds == null) return null;
                AccessibilityNodeInfo node = AccessibilityNodeInfo.obtain();
                node.setSource(Page.this, id); node.setParent(Page.this);
                node.setClassName(id == 500 ? "android.widget.TextView" : "android.widget.Button");
                node.setPackageName(getContext().getPackageName()); node.setContentDescription(nodeLabel(id));
                node.setEnabled(true); node.setVisibleToUser(isShown()); node.setFocusable(true);
                node.setSelected(id >= TAB && id < TAB + TABS.length && id - TAB == tab);
                node.setAccessibilityFocused(accessibilityFocus == id);
                node.addAction(accessibilityFocus == id ? AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS : AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
                if (id != 500) { node.setClickable(true); node.addAction(AccessibilityNodeInfo.ACTION_CLICK); }
                Rect rect = new Rect(); bounds.roundOut(rect); node.setBoundsInParent(rect);
                int[] location = new int[2]; getLocationOnScreen(location); rect.offset(location[0], location[1]); node.setBoundsInScreen(rect);
                return node;
            }
            @Override public AccessibilityNodeInfo findFocus(int focus) {
                return accessibilityFocus == NONE ? null : createAccessibilityNodeInfo(accessibilityFocus);
            }
            @Override public boolean performAction(int id, int action, Bundle arguments) {
                if (nodeBounds(id) == null) return false;
                if (action == AccessibilityNodeInfo.ACTION_CLICK && id != 500) {
                    activate(id); virtualEvent(id, AccessibilityEvent.TYPE_VIEW_CLICKED); return true;
                }
                if (action == AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
                    if (accessibilityFocus != NONE) virtualEvent(accessibilityFocus, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
                    accessibilityFocus = id; virtualEvent(id, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED); invalidate(); return true;
                }
                if (action == AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS && accessibilityFocus == id) {
                    accessibilityFocus = NONE; virtualEvent(id, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED); invalidate(); return true;
                }
                return false;
            }
        };

        @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return nodes; }
        @Override public boolean onHoverEvent(MotionEvent event) {
            if (accessibility == null || !accessibility.isTouchExplorationEnabled()) return super.onHoverEvent(event);
            int target = event.getActionMasked() == MotionEvent.ACTION_HOVER_EXIT ? NONE : hit(event.getX(), event.getY());
            if (target != hoveredNode) {
                virtualEvent(hoveredNode, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT); hoveredNode = target;
                virtualEvent(hoveredNode, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER);
            }
            return target != NONE || super.onHoverEvent(event);
        }

        @Override protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            attached = true;
            loadArt(); telemetry.setStatusVisible(tab == T_STATUS); telemetry.start();
            if (tab == T_MAP) post(mapTick);
            removeCallbacks(tick); post(tick);
        }

        @Override protected void onDetachedFromWindow() {
            releaseGame();
            attached = false;
            ArtLoader.cancel(this);
            telemetry.stop();
            super.onDetachedFromWindow();
            removeCallbacks(tick); removeCallbacks(fader); removeCallbacks(mapTick);
            chrome = null;
        }

        @Override public void onWindowFocusChanged(boolean focused) {
            super.onWindowFocusChanged(focused);
            if (!focused) cancelPress();
        }
    }
}
