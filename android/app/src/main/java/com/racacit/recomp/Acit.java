package com.racacit.recomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BlurMaskFilter;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import java.util.Calendar;
import java.util.Locale;

/**
 * A Crack in Time's look, shared by the launcher and the companion screen,
 * and the same as the Windows launcher's (launcher/src/gfx.h): colours taken
 * from the game's key art (PIC1) -- the Great Clock's teal lightning, the amber
 * sun, deep blue-green space -- panels with thin glowing edges and cut
 * corners, hexagons for bullets and slots, amber for what is focused or
 * pressed, condensed bold capitals for titles (the game's HUD and menus).
 */
final class Acit {
    private Acit() {}

    static final int BG_TOP = 0xFF06141A, BG_BOTTOM = 0xFF0B2228;
    /* 0.1.0: panels nearly opaque (text over the key art read poorly), unfocused edges muted so a grid of
     * tiles is not a wall of bright outlines; the bright teal edge and amber stay for focus and the primary action. */
    static final int PANEL = 0xEE0B1D24, PANEL_DARK = 0xE8081519, PANEL_HOT = 0xF2173C44;
    static final int EDGE_MUTE = 0xFF3C8892;
    static final int EDGE = 0xFF6FE6F0, EDGE_DIM = 0xFF2E6B74, GLOW = 0x806FE6F0;
    static final int ORANGE = 0xFFFFB43C, ORANGE_GLOW = 0xA0FFB43C, ORANGE_SOFT = 0xFFFFCD8C;
    static final int TEXT = 0xFFF4FBFF, TEXT_DIM = 0xFFA7C6CC, TEXT_BAD = 0xFFFF7A5C, TEXT_GOOD = 0xFF8EF0D0;
    static final int TEXT_OFF = 0xFF557A80;
    /* The launcher's secondary text, brighter than the companion's (readable on a phone in daylight). */
    static final int LTEXT_DIM = 0xFFD3E5E9, LTEXT_OFF = 0xFF89A7AD;

    static Typeface titleFace() { return Typeface.create("sans-serif-condensed", Typeface.BOLD); }
    static Typeface bodyFace() { return Typeface.create("sans-serif-condensed", Typeface.NORMAL); }

    static float dp(Context c, float v) { return v * c.getResources().getDisplayMetrics().density; }
    /** v sp in pixels: follows the system font size (non-linearly from Android 14, as TextView does). */
    static float sp(Context c, float v) {
        return android.util.TypedValue.applyDimension(android.util.TypedValue.COMPLEX_UNIT_SP, v, c.getResources().getDisplayMetrics());
    }

    /** A rectangle with its top-left and bottom-right corners cut off. */
    static Path chamfer(RectF r, float cut) {
        Path p = new Path();
        p.moveTo(r.left + cut, r.top);
        p.lineTo(r.right, r.top);
        p.lineTo(r.right, r.bottom - cut);
        p.lineTo(r.right - cut, r.bottom);
        p.lineTo(r.left, r.bottom);
        p.lineTo(r.left, r.top + cut);
        p.close();
        return p;
    }

    /** A pointy-top hexagon centred at (cx, cy) with circumradius r. */
    static Path hexagon(float cx, float cy, float r) {
        Path p = new Path();
        for (int i = 0; i < 6; i++) {
            double a = Math.toRadians(60 * i - 90);
            float x = cx + (float) (r * Math.cos(a)), y = cy + (float) (r * Math.sin(a));
            if (i == 0) p.moveTo(x, y); else p.lineTo(x, y);
        }
        p.close();
        return p;
    }

    static Paint fill(int color) { Paint p = new Paint(Paint.ANTI_ALIAS_FLAG); p.setStyle(Paint.Style.FILL); p.setColor(color); return p; }

    static Paint stroke(int color, float width) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(width); p.setColor(color);
        p.setStrokeJoin(Paint.Join.MITER);
        return p;
    }

    static Paint glow(int color, float width, float blur) {
        Paint p = stroke(color, width); p.setMaskFilter(new BlurMaskFilter(blur, BlurMaskFilter.Blur.NORMAL));
        return p;
    }

    static Paint text(int color, float size, Typeface face) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG); p.setColor(color); p.setTextSize(size); p.setTypeface(face);
        p.setTextAlign(Paint.Align.CENTER);
        return p;
    }

    /** color with its alpha scaled by a (0..1). */
    static int alpha(int color, float a) { return Math.round((color >>> 24) * Math.max(0, Math.min(1, a))) << 24 | color & 0xFFFFFF; }

    /** The baseline that centres one line of p's text vertically on cy. */
    static float mid(Paint p, float cy) { return cy - (p.ascent() + p.descent()) / 2; }

    /** Draws s shortened with an ellipsis to fit max px (p's alignment is kept). */
    static void fitText(Canvas c, String s, float x, float y, float max, Paint p) {
        if (p.measureText(s) > max) {
            int n = p.breakText(s, true, Math.max(0, max - p.measureText("...")), null);
            s = s.substring(0, Math.max(0, n)).trim() + "...";
        }
        c.drawText(s, x, y, p);
    }

    /** The focus pulse, 0.3..1 (the Windows launcher's). */
    static float pulse() { return 0.65f + 0.35f * (float) Math.sin(SystemClock.uptimeMillis() / 260.0); }

    /**
     * The screen background: a blue-green gradient, and over it the game's own
     * key art at full strength on the right, darkened where the text sits.
     * Composed once per size into a bitmap.
     */
    static final class Background extends Drawable {
        private Bitmap art, done;
        private boolean cacheUnavailable;
        Background(Bitmap art) { this.art = art; }
        void setArt(Bitmap a) { if (a != art) { art = a; done = null; invalidateSelf(); } }

        @Override protected void onBoundsChange(Rect b) { done = null; cacheUnavailable = false; }

        @Override public void draw(Canvas c) {
            Rect b = getBounds();
            if (b.width() <= 0 || b.height() <= 0) return;
            if (!cacheUnavailable && (done == null || done.getWidth() != b.width() || done.getHeight() != b.height())) {
                try { done = compose(b.width(), b.height(), art, true); }
                catch (OutOfMemoryError lowMemory) { done = null; cacheUnavailable = true; }
            }
            if (done != null) c.drawBitmap(done, b.left, b.top, null);
            else { int save = c.save(); c.translate(b.left, b.top); paint(c, b.width(), b.height(), art, true); c.restoreToCount(save); }
        }

        /** The background at w x h; side: the art darkened towards the left (the launcher), else evenly (the companion). */
        static Bitmap compose(int w, int h, Bitmap art, boolean side) {
            Bitmap out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(out);
            paint(c, w, h, art, side);
            return out;
        }

        static void paint(Canvas c, int w, int h, Bitmap art, boolean side) {
            Paint g = new Paint();
            g.setShader(new LinearGradient(0, 0, 0, h, BG_TOP, BG_BOTTOM, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, g);
            if (art != null) {
                /* cover: fill the screen, crop the overflow evenly */
                float s = Math.max((float) w / art.getWidth(), (float) h / art.getHeight());
                float aw = art.getWidth() * s, ah = art.getHeight() * s;
                c.drawBitmap(art, null, new RectF((w - aw) / 2, (h - ah) / 2, (w + aw) / 2, (h + ah) / 2), new Paint(Paint.FILTER_BITMAP_FLAG));
                if (side) {
                    /* text can sit anywhere on any screen shape (a narrow one-column layout too): never lighter than ~70% */
                    g.setShader(new LinearGradient(0, 0, w, 0, 0xE806141A, 0xB406141A, Shader.TileMode.CLAMP));
                } else {
                    g.setShader(null); g.setColor(0xB806141A);
                }
                c.drawRect(0, 0, w, h, g);
                float top = h * 0.16f, bottom = h * 0.12f;
                g.setShader(new LinearGradient(0, 0, 0, top, 0xA006141A, 0x0006141A, Shader.TileMode.CLAMP));
                c.drawRect(0, 0, w, top, g);
                g.setShader(new LinearGradient(0, h - bottom, 0, h, 0x0006141A, 0xD006141A, Shader.TileMode.CLAMP));
                c.drawRect(0, h - bottom, w, h, g);
            }
        }

        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter f) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.OPAQUE; }
    }

    /**
     * A chamfered HUD button with a hexagon bullet: label (and a game glyph or
     * a second line) on a panel whose edge turns amber and pulses while it is
     * focused. As an option row (value set), the label sits on the left and the
     * value on the right, in amber, with arrows while focused.
     */
    static class Button extends View {
        String label, sub, value;
        Bitmap glyph;
        boolean accent;            /* the primary action: amber edge */
        boolean tile;              /* a home tile: bigger title at the top, the line under it wraps to two lines */
        boolean enabled = true;    /* false: drawn dim, does nothing */
        Runnable action;
        Runnable longAction;       /* a touch held ~half a second (the launcher: what a setting does) */
        private boolean down, longFired;
        private final Runnable longPress = () -> {
            if (down && longAction != null) { longFired = true; down = false; invalidate(); performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS); longAction.run(); }
        };
        private final Paint panel = fill(PANEL), edge, glowP, title, subP, valP, bullet, bulletEdge;
        private final RectF r = new RectF();
        private final float d;

        Button(Context c, String label, String sub, Runnable action) {
            super(c);
            this.label = label; this.sub = sub; this.action = action;
            d = c.getResources().getDisplayMetrics().density;
            edge = stroke(EDGE, 1.6f * d); glowP = glow(GLOW, 3 * d, 6 * d);
            /* in sp: the text follows Android's font size setting (rows grow with it: LauncherActivity.rowH) */
            title = text(TEXT, sp(c, 20), titleFace()); title.setTextAlign(Paint.Align.LEFT);
            subP = text(LTEXT_DIM, sp(c, 16), bodyFace()); subP.setTextAlign(Paint.Align.LEFT);
            valP = text(ORANGE_SOFT, sp(c, 17), titleFace()); valP.setTextAlign(Paint.Align.RIGHT);
            bullet = fill(ORANGE); bulletEdge = stroke(EDGE, 1.3f * d);
            setFocusable(true); setClickable(true);
        }

        @Override protected void onDraw(Canvas c) {
            float pad = 4 * d, cut = 13 * d;
            r.set(pad, pad, getWidth() - pad, getHeight() - pad);
            Path p = chamfer(r, cut);
            boolean f = isFocused() || down;
            int edgeC = f ? ORANGE : accent && enabled ? ORANGE : enabled ? EDGE_MUTE : EDGE_DIM;
            if (f) { glowP.setColor(alpha(ORANGE_GLOW, pulse())); c.drawPath(p, glowP); }
            else if (accent && enabled) { glowP.setColor(0x60FFB43C); c.drawPath(p, glowP); }
            panel.setColor(down ? 0xF0204A50 : f ? PANEL_HOT : PANEL);
            c.drawPath(p, panel);
            edge.setColor(edgeC); edge.setStrokeWidth((f ? 2.2f : 1.5f) * d);
            c.drawPath(p, edge);

            float cy = getHeight() / 2f, x = r.left + 20 * d;
            if (tile && value == null) {
                /* a home tile: an amber bar when focused, the title large at the top, its line (up to two) under it */
                if (f) c.drawRect(r.left + 2 * d, r.top + cut, r.left + 6 * d, r.bottom - 6 * d, bullet);
                title.setColor(enabled ? TEXT : TEXT_OFF);
                float tx = r.left + 18 * d, right = r.right - 14 * d;
                float tsz = title.getTextSize();
                title.setTextSize(tsz * 1.12f);
                boolean two = sub != null && !sub.isEmpty();
                float block = -title.ascent() + (two ? subP.getTextSize() * 2.5f : 0);
                float top = r.top + Math.max(8 * d, (r.height() - block) / 2);
                float base = top - title.ascent();
                fitText(c, label, tx, base, right - tx, title);
                title.setTextSize(tsz);
                if (two) {
                    subP.setColor(enabled ? LTEXT_DIM : LTEXT_OFF);
                    float y = base + subP.getTextSize() * 1.35f, w = right - tx;
                    int n = subP.breakText(sub, true, w, null);
                    if (n < sub.length()) {
                        int cut2 = sub.lastIndexOf(' ', n);
                        if (cut2 > 0) n = cut2;
                        c.drawText(sub.substring(0, n).trim(), tx, y, subP);
                        fitText(c, sub.substring(n).trim(), tx, y + subP.getTextSize() * 1.2f, w, subP);
                    } else c.drawText(sub, tx, y, subP);
                }
                if (f) postInvalidateDelayed(40);
                return;
            }
            if (glyph != null) {
                float gh = getHeight() * 0.40f, gw = gh * glyph.getWidth() / Math.max(1, glyph.getHeight());
                c.drawBitmap(glyph, null, new RectF(x - 4 * d, cy - gh / 2, x - 4 * d + gw, cy + gh / 2), null);
                x += gw + 6 * d;
            } else {
                Path h = hexagon(x + 3 * d, cy, 6.5f * d);
                if (f) c.drawPath(h, bullet);
                bulletEdge.setColor(f ? ORANGE : edgeC);
                c.drawPath(h, bulletEdge);
                x += 22 * d;
            }
            title.setColor(enabled ? TEXT : TEXT_OFF);
            float right = r.right - 16 * d;
            if (value != null) {
                String v = f ? "◀  " + value + "  ▶" : value;
                valP.setColor(f ? ORANGE : ORANGE_SOFT);
                float vw = Math.min(valP.measureText(v), (right - x) * 0.6f);
                fitText(c, v, right, mid(valP, cy), vw, valP);
                fitText(c, label, x, mid(title, cy), right - vw - x - 12 * d, title);
            } else if (sub == null || sub.isEmpty()) {
                fitText(c, label, x, mid(title, cy), right - x, title);
            } else {
                fitText(c, label, x, cy - 1 * d, right - x, title);
                subP.setColor(enabled ? LTEXT_DIM : LTEXT_OFF);
                fitText(c, sub, x, cy + subP.getTextSize() + 2 * d, right - x, subP);
            }
            if (f) postInvalidateDelayed(40);   /* the pulse */
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    down = true; longFired = false; invalidate();
                    if (longAction != null) postDelayed(longPress, android.view.ViewConfiguration.getLongPressTimeout());
                    return true;
                case MotionEvent.ACTION_MOVE:
                    /* a finger that slides off (a scroll) is not a press */
                    if (down && (e.getX() < -8 * d || e.getY() < -8 * d || e.getX() > getWidth() + 8 * d || e.getY() > getHeight() + 8 * d)) {
                        down = false; removeCallbacks(longPress); invalidate();
                    }
                    return true;
                case MotionEvent.ACTION_UP: {
                    boolean was = down;
                    down = false; removeCallbacks(longPress); invalidate();
                    if (was && !longFired && e.getX() >= 0 && e.getY() >= 0 && e.getX() <= getWidth() && e.getY() <= getHeight()) performClick();
                    return true;
                }
                case MotionEvent.ACTION_CANCEL: down = false; removeCallbacks(longPress); invalidate(); return true;
            }
            return super.onTouchEvent(e);
        }

        @Override public boolean performClick() { super.performClick(); if (action != null && enabled) action.run(); return true; }

        @Override protected void onFocusChanged(boolean f, int dir, android.graphics.Rect prev) { super.onFocusChanged(f, dir, prev); invalidate(); }
    }

    /** A chamfered panel background for a block of content. */
    static final class Panel extends Drawable {
        private final Paint fillP = fill(PANEL_DARK), edgeP;
        private final float cut;
        Panel(Context c) { edgeP = stroke(EDGE_DIM, dp(c, 1.5f)); cut = dp(c, 18); }
        @Override public void draw(Canvas c) {
            RectF r = new RectF(getBounds()); r.inset(2, 2);
            Path p = chamfer(r, cut); c.drawPath(p, fillP); c.drawPath(p, edgeP);
        }
        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter f) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    /** A section heading's faint amber underline, as a TextView's background. */
    static final class Underline extends Drawable {
        private final Paint p = fill(0x60FFB43C);
        @Override public void draw(Canvas c) { Rect b = getBounds(); c.drawRect(b.left + 4, b.bottom - 3, b.right, b.bottom - 2, p); }
        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter f) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    /** A small status hexagon: filled green (ready), red (needs you), or an outline (neither). */
    static final class Mark extends View {
        final int state;   /* 1 ok, -1 bad, 0 neutral */
        private final Paint f = fill(TEXT_GOOD), s;
        Mark(Context c, int state) { super(c); this.state = state; s = stroke(EDGE_DIM, dp(c, 1.4f)); }
        @Override protected void onDraw(Canvas c) {
            float r = Math.min(getWidth(), getHeight()) / 2f - dp(getContext(), 1.5f);
            Path h = hexagon(getWidth() / 2f, getHeight() / 2f, r);
            if (state != 0) { f.setColor(state > 0 ? TEXT_GOOD : TEXT_BAD); c.drawPath(h, f); }
            s.setColor(state > 0 ? TEXT_GOOD : state < 0 ? TEXT_BAD : EDGE_DIM);
            c.drawPath(h, s);
        }
    }

    /** A chamfered progress bar: amber fill, the percentage over it. */
    static final class Progress extends View {
        private float frac = -1;   /* < 0: not started */
        private final Paint back = fill(PANEL_DARK), fillP = fill(ORANGE), edgeP, label;
        Progress(Context c) { super(c); edgeP = stroke(EDGE_DIM, dp(c, 1.5f)); label = text(TEXT, dp(c, 15), titleFace()); }
        void set(float f) { frac = f; invalidate(); }
        @Override protected void onDraw(Canvas c) {
            RectF r = new RectF(2, 2, getWidth() - 2, getHeight() - 2);
            Path p = chamfer(r, r.height() * 0.35f);
            c.drawPath(p, back);
            if (frac > 0) {
                c.save();
                c.clipRect(r.left, r.top, r.left + r.width() * Math.min(1, frac), r.bottom);
                c.drawPath(p, fillP);
                c.restore();
            }
            c.drawPath(p, edgeP);
            if (frac >= 0) {
                label.setColor(frac > 0.55f ? BG_TOP : TEXT);
                c.drawText(Math.round(Math.min(1, frac) * 100) + "%", r.centerX(), mid(label, r.centerY()), label);
            }
        }
    }

    /**
     * The time as the Great Clock keeps it: a brass hexagon face with Zoni-teal
     * marks, hour, minute and a second hand that ticks each second (the
     * Windows launcher's draw_clock). Drawn by GreatClock and the companion.
     */
    static final class ClockFace {
        private final Paint face = fill(0xB0081519), rim, ring1, ring2, tick, hand, sec, hub, digits;
        private final float d;
        ClockFace(Context c) {
            d = c.getResources().getDisplayMetrics().density;
            rim = stroke(ORANGE, 2 * d); ring1 = stroke(0xA0FFB43C, 1.2f * d); ring2 = stroke(0x506FE6F0, 1 * d);
            tick = stroke(ORANGE, 1); tick.setStrokeCap(Paint.Cap.ROUND);
            hand = stroke(TEXT, 3 * d); hand.setStrokeCap(Paint.Cap.ROUND);
            sec = stroke(EDGE, 1.6f * d); sec.setStrokeCap(Paint.Cap.ROUND);
            hub = fill(ORANGE);
            digits = text(TEXT_DIM, 13 * d, titleFace());
        }

        /** The face centred on (cx, cy), radius R; with digitsSize > 0 the HH:MM:SS line under it. */
        void draw(Canvas c, float cx, float cy, float R, float digitsSize) {
            Calendar t = Calendar.getInstance();
            float k = Math.max(1, R / (46 * d));   /* thicker lines on a big face */
            c.drawPath(hexagon(cx, cy, R * 1.08f), face);
            rim.setStrokeWidth(2 * d * k); c.drawPath(hexagon(cx, cy, R * 1.08f), rim);
            c.drawCircle(cx, cy, R * 0.94f, ring1);
            c.drawCircle(cx, cy, R * 0.80f, ring2);
            for (int i = 0; i < 60; i++) {
                double a = i * Math.PI * 2 / 60 - Math.PI / 2;
                boolean big = i % 5 == 0;
                float r0 = R * (big ? 0.76f : 0.85f), r1 = R * 0.92f;
                tick.setColor(big ? ORANGE : 0x906FE6F0); tick.setStrokeWidth((big ? 2.2f : 1f) * d * k);
                c.drawLine(cx + (float) Math.cos(a) * r0, cy + (float) Math.sin(a) * r0,
                           cx + (float) Math.cos(a) * r1, cy + (float) Math.sin(a) * r1, tick);
            }
            float s = t.get(Calendar.SECOND), m = t.get(Calendar.MINUTE) + s / 60f, h = t.get(Calendar.HOUR) + m / 60f;
            hand.setStrokeWidth(4.5f * d * k); line(c, cx, cy, h / 12f, R * 0.48f, R * 0.08f, hand);
            hand.setStrokeWidth(3 * d * k); line(c, cx, cy, m / 60f, R * 0.70f, R * 0.10f, hand);
            sec.setStrokeWidth(1.6f * d * k); line(c, cx, cy, s / 60f, R * 0.84f, R * 0.18f, sec);
            c.drawPath(hexagon(cx, cy, R * 0.07f), hub);
            if (digitsSize > 0) {
                digits.setTextSize(digitsSize);
                c.drawText(String.format(Locale.US, "%02d:%02d:%02d", t.get(Calendar.HOUR_OF_DAY), t.get(Calendar.MINUTE), (int) s),
                           cx, cy + R * 1.12f + digitsSize, digits);
            }
        }

        private static void line(Canvas c, float cx, float cy, float turns, float len, float back, Paint p) {
            double a = turns * Math.PI * 2 - Math.PI / 2;
            float dx = (float) Math.cos(a), dy = (float) Math.sin(a);
            c.drawLine(cx - dx * back, cy - dy * back, cx + dx * len, cy + dy * len, p);
        }

        /** Milliseconds to the next whole second (for the next tick). */
        static long untilNextSecond() { return 1000 - System.currentTimeMillis() % 1000 + 5; }
    }

    /** The Great Clock in the launcher's corner: ticks once a second while it is on screen. */
    static final class GreatClock extends View {
        private final ClockFace face;
        private final Runnable tick = this::invalidate;
        GreatClock(Context c) { super(c); face = new ClockFace(c); }
        @Override protected void onDraw(Canvas c) {
            float digits = dp(getContext(), 13);
            float R = Math.min(getWidth() / 2f / 1.1f, (getHeight() - digits * 1.6f) / 2f / 1.12f);
            face.draw(c, getWidth() / 2f, R * 1.1f, R, digits);
            removeCallbacks(tick);
            postDelayed(tick, ClockFace.untilNextSecond());
        }
        @Override protected void onDetachedFromWindow() { super.onDetachedFromWindow(); removeCallbacks(tick); }
    }
}
