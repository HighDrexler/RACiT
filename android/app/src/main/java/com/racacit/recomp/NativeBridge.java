package com.racacit.recomp;

/**
 * The companion screen's link to the running game (game/android_companion.cpp,
 * in libmain.so -- loaded by SDLActivity before the companion appears).
 * Button masks are the pad's: CELL_PAD_CTRL_* as DIGITAL1 | DIGITAL2 << 8.
 */
final class NativeBridge {
    private NativeBridge() {}

    static final int SELECT = 0x0001, L3 = 0x0002, R3 = 0x0004, START = 0x0008, UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040,
            LEFT = 0x0080, L2 = 0x0100, R2 = 0x0200, L1 = 0x0400, R1 = 0x0800, TRIANGLE = 0x1000, CIRCLE = 0x2000,
            CROSS = 0x4000, SQUARE = 0x8000;

    static final int OK = 0, NOT_READY = -1, BUSY = -2, CONTEXT = -3, INVALID = -4, STALLED = -5;
    static final int FLIPS = 0, PRESENTED = 1, NOW_US = 2, WEAPON_ID = 3, FLAGS = 4,
            LEVEL_ID = 5, PHASE = 6, STATUS = 7, GENERATION = 8, SNAPSHOT_AGE_US = 9,
            TRANSACTION_FRAMES = 10, GUEST_WHEEL_PAGE = 11, GUEST_WHEEL_SLOT = 12,
            QUICK_SLOTS = 13, QUICK_SLOT_COUNT = 24,
            /* 1 when the player was found, then the float bits of x, y (up), z, and the facing's x and z */
            HERO = QUICK_SLOTS + QUICK_SLOT_COUNT,
            /* the PPU main thread's time in its SPU job waits (us, running total), then its thread id */
            MAIN_WAIT_US = HERO + 6, MAIN_TID = HERO + 7,
            /* the core swap A/B (RACACIT_AB): on, current mode (1 = swap on), then phases counted and
             * fps x100 with the swap off, then the same with it on */
            AB = HERO + 8;
    static final int FLAG_ACTIVE = 1, FLAG_FRESH = 2, FLAG_HOST_MODAL = 4,
            FLAG_RATCHET_LEVEL = 8, FLAG_PHYSICAL_INPUT = 16, FLAG_MOVIE = 32, FLAG_GUEST_READY = 64;
    static final int PHASE_IDLE = 0, PHASE_PRESS = 1, PHASE_OPENING = 2,
            PHASE_SELECTING = 3, PHASE_COMMITTING = 4, PHASE_RELEASING = 5, PHASE_CANCELLING = 6;
    static final int STATUS_IDLE = 0, STATUS_ACTIVE = 1, STATUS_COMPLETE = 2,
            STATUS_CANCELLED = 3, STATUS_TIMEOUT = 4, STATUS_CONTEXT_LOST = 5, STATUS_PHYSICAL_TAKEOVER = 6;

    /** One atomic input transaction. Frames are guest flips, never pad polls. */
    static native int press(int mask, int frames);

    /** A gameplay button (the d-pad or R2) down or up: held while the finger is, merged with the
     *  controller rather than refused while it is in use. A tap reaches the game for a few flips. */
    static native int hold(int mask, boolean down);

    /** Accessible directional input, clockwise from top; does not claim inventory/equipment knowledge. */
    static native int quickSelect(int slot);
    static native int beginQuickSelect();
    /** Normalized stick direction; also refreshes the gesture watchdog. */
    static native int updateQuickSelect(float x, float y);
    /** Completes minimum opening/selection frames; false centres before releasing. */
    static native int endQuickSelect(boolean commit);
    /** Real L1/R1 pulse during an active quick-select gesture; no invented page number. */
    static native int pageQuickSelect(int direction);
    static native void cancel();
    /** Equips a weapon that is on the quick select, by the game's own hero_equip.
     *  OK means the request was taken; the snapshot's WEAPON_ID shows the result. */
    static native int equip(int weapon);
    /** False discards pending actions. An emitted wheel request may retain only
     * a bounded neutral close while its original game context remains safe. */
    static native void setActive(boolean active);

    /** Open or close the in-game Settings menu (as F1, or L3+R3 on a controller). */
    static native void openMenu();

    /** Coherent pad-thread snapshot. Unknown weapon/level/page/slot are -1, and so
     * is an empty quick select slot (from QUICK_SLOTS: 3 rings of 8 weapon ids).
     * PHASE describes injected input only, never asserted guest wheel state.
     * STATUS_COMPLETE means input finished, not that the game equipped an item. */
    static native long[] stats();

    /** The shader prepare (RACACIT_PREPARE=1): {state 0 starting/1 reading/2 building/3 done/4 failed,
     * handled, total, failed, without programs}. */
    static native int[] prepareProgress();

    static boolean ready() {
        try { stats(); return true; } catch (UnsatisfiedLinkError e) { return false; }
    }
}
