#ifndef RACIT_ANDROID_COMPANION_INPUT_H
#define RACIT_ANDROID_COMPANION_INPUT_H

#include <stdint.h>

// Controller transactions only. No guest-memory writes or equipment APIs.
struct CompanionContext {
    uint32_t frame, level_epoch;
    uint64_t now_us;
    int level;
    bool ready, modal, physical, movie;
};
struct CompanionOutput { unsigned mask; int sticks; unsigned char ax[4]; };

struct CompanionInput {
    enum { OK=0, NOT_READY=-1, BUSY=-2, CONTEXT=-3, INVALID=-4, STALLED=-5 };
    enum { IDLE=0, PRESS=1, OPENING=2, SELECTING=3, COMMITTING=4, RELEASING=5, CANCELLING=6 };
    enum { NONE=0, ACTIVE=1, COMPLETE=2, CANCELLED=3, TIMEOUT=4, CONTEXT_LOST=5, PHYSICAL_TAKEOVER=6 };
    CompanionContext context = {};
    bool enabled=false, sampled=false, emitted=false, wheel=false, commit=false, automatic=false;
    bool selection_emitted=false, cleanup=false, cleanup_pending=false, page_released=false;
    int phase=IDLE, status=NONE, direction=-1, pending_page=0, page=0;
    unsigned mask=0, hold_frames=0, generation=0, start_frame=0, phase_frame=0, page_frame=0;
    unsigned selection_frame=0, page_released_frame=0;
    uint64_t frame_at=0, started_at=0, phase_at=0, gesture_at=0;

    /* Gameplay buttons (the second screen's d-pad corners and wrench throw): held while a
     * finger is down, alongside the transaction above and merged with the controller, since
     * they are pressed while playing with it. Only the d-pad and R2. A tap still reaches the
     * game for HOLD_MIN_FLIPS flips; a finger left down lets go after HOLD_MAX_US. */
    enum { HOLDABLE=0x02F0u, HOLD_MIN_FLIPS=4u };
    static constexpr uint64_t HOLD_MAX_US=10000000;
    unsigned held=0, held_out=0;            // fingers down; bits being sent
    uint32_t held_frame[16] = {};
    uint64_t held_at[16] = {};

    static bool ratchet_level(int level) {
        // Evidenced table in discord_rpc.cpp, guest table 0xDE1820.
        return level==2 || level==4 || level==7 || level==8 || level==11 || level==12 ||
            level==13 || level==16 || level==17 || level==19 || level==21;
    }
    bool fresh(uint64_t now) const {
        return sampled && now >= context.now_us && now-context.now_us <= 500000 &&
            now >= frame_at && now-frame_at <= 750000;
    }
    int gate(uint64_t now, bool needs_ratchet) const {
        if (!enabled || !sampled || !context.ready) return NOT_READY;
        if (!fresh(now)) return STALLED;
        if (context.modal || context.physical || context.movie ||
            (needs_ratchet && !ratchet_level(context.level))) return CONTEXT;
        return OK;
    }
    void finish(int result) {
        phase=IDLE; status=result; emitted=false; wheel=false; commit=false; automatic=false;
        selection_emitted=false; cleanup=false; cleanup_pending=false; page_released=false;
        mask=0; direction=-1; pending_page=page=0;
    }
    void drop_holds() { held=held_out=0; }
    /* A gameplay button down or up. Up is always taken; down needs live gameplay frames and no
     * host or movie screen, but not a still controller. */
    int hold(unsigned buttons, bool down, uint64_t now) {
        if (!buttons || (buttons & ~(unsigned)HOLDABLE)) return INVALID;
        if (!down) { held&=~buttons; return OK; }
        if (!enabled || !sampled || !context.ready) return NOT_READY;
        if (!fresh(now)) return STALLED;
        if (context.modal || context.movie) return CONTEXT;
        for (unsigned b=0;b<16;b++) if (buttons>>b&1u) {
            if (!(held_out>>b&1u)) held_frame[b]=context.frame;   // a new press; a re-press while
            held_at[b]=now;                                     // its tap is still going continues it
        }
        held|=buttons; held_out|=buttons; return OK;
    }
    unsigned tick_holds(const CompanionContext& c, bool changed) {
        if (!held_out) return 0;
        if (!enabled || !c.ready || c.modal || c.movie || changed || !fresh(c.now_us)) { drop_holds(); return 0; }
        for (unsigned b=0;b<16;b++) {
            const unsigned bit=1u<<b;
            if (!(held_out&bit)) continue;
            if (held&bit) { if (c.now_us-held_at[b]>HOLD_MAX_US) { held&=~bit; held_out&=~bit; } }
            else if ((uint32_t)(c.frame-held_frame[b])>=HOLD_MIN_FLIPS) held_out&=~bit;
        }
        return held_out;
    }
    void set_active(bool on) {
        enabled=on;
        if (!on) drop_holds();
        if (!on && !cleanup) {
            if (wheel && emitted && phase!=RELEASING && !context.modal && !context.physical) {
                // Discard the requested selection. Only a neutral close may
                // survive suspension; it cannot replay a direction/page.
                commit=false; automatic=true; direction=-1; pending_page=page=0;
                phase=CANCELLING; cleanup=cleanup_pending=true; selection_emitted=false;
            } else finish(CANCELLED);
        }
    }
    void start(int next_phase, uint64_t now, bool is_wheel) {
        phase=next_phase; status=ACTIVE; wheel=is_wheel; emitted=false; commit=false; automatic=false;
        selection_emitted=false; cleanup=false; cleanup_pending=false; page_released=false;
        generation++; started_at=gesture_at=phase_at=now;
        start_frame=phase_frame=context.frame; direction=-1; pending_page=page=0;
    }
    int press(unsigned buttons, unsigned frames, uint64_t now) {
        if (!buttons || (buttons & ~65535u) || frames<1 || frames>30) return INVALID;
        const int allowed=gate(now,false); if (allowed) return allowed;
        if (phase!=IDLE) return BUSY;
        start(PRESS,now,false); mask=buttons; hold_frames=frames; return OK;
    }
    int begin(uint64_t now) {
        const int allowed=gate(now,true); if (allowed) return allowed;
        if (phase!=IDLE) return BUSY;
        start(OPENING,now,true); return OK;
    }
    int update_direction(int sector, uint64_t now) {
        if (sector < -1 || sector > 7) return INVALID;
        if (!wheel || (phase!=OPENING && phase!=SELECTING)) return CONTEXT;
        if (direction!=sector) selection_emitted=false;
        direction=sector; gesture_at=now; return OK;
    }
    int end(bool accept, uint64_t now) {
        if (!wheel || (phase!=OPENING && phase!=SELECTING)) return CONTEXT;
        gesture_at=now; automatic=true;
        if (accept && direction>=0) commit=true;
        else if (!emitted) finish(CANCELLED);
        else { commit=false; phase=CANCELLING; phase_frame=context.frame; phase_at=now; pending_page=page=0; }
        return OK;
    }
    void cancel(uint64_t now) {
        if (wheel && (phase==CANCELLING || phase==RELEASING)) { commit=false; return; }
        if (wheel && (phase==OPENING || phase==SELECTING) && enabled && fresh(now) && !context.modal && !context.physical) end(false,now);
        else finish(CANCELLED);
    }
    int change_page(int step, uint64_t now) {
        if (step!=-1 && step!=1) return INVALID;
        if (!wheel || commit || (phase!=OPENING && phase!=SELECTING)) return CONTEXT;
        if (pending_page || page || (page_released && (uint32_t)(context.frame-page_released_frame)<2u)) return BUSY;
        pending_page=step; selection_emitted=false; gesture_at=now; return OK;
    }
    int select(int slot, uint64_t now) {
        if (slot<0 || slot>7) return INVALID;
        const int result=begin(now); if (result) return result;
        direction=slot; commit=automatic=true; return OK;
    }
    void transition(int next, const CompanionContext& c) {
        phase=next; phase_frame=c.frame; phase_at=c.now_us;
    }
    CompanionOutput tick(const CompanionContext& c) {
        const bool changed=sampled && (c.level!=context.level || c.level_epoch!=context.level_epoch);
        CompanionOutput out=tick_transaction(c);
        out.mask|=tick_holds(c,changed);
        return out;
    }
    CompanionOutput tick_transaction(const CompanionContext& c) {
        CompanionOutput out = {0,0,{128,128,128,128}};
        const bool changed=sampled && (c.level!=context.level || c.level_epoch!=context.level_epoch);
        if (!sampled || c.frame!=context.frame) frame_at=c.now_us;
        context=c; sampled=true;
        if (phase==IDLE) return out;
        if ((!enabled && !cleanup) || !c.ready || c.modal || c.movie || changed || (wheel && !ratchet_level(c.level))) {
            finish(CONTEXT_LOST); return out;
        }
        if (c.physical) { finish(PHYSICAL_TAKEOVER); return out; }
        if (cleanup_pending) { phase_frame=c.frame; phase_at=c.now_us; cleanup_pending=false; }
        if (cleanup && c.now_us-phase_at>750000) { finish(CANCELLED); return out; }
        if (!cleanup && (!fresh(c.now_us) || c.now_us-started_at>15000000 ||
            (!automatic && wheel && c.now_us-gesture_at>750000))) { finish(TIMEOUT); return out; }
        if (!emitted) { emitted=true; start_frame=phase_frame=c.frame; phase_at=c.now_us; }
        if (!cleanup && (uint32_t)(c.frame-start_frame)>600u) { finish(TIMEOUT); return out; }
        const uint32_t elapsed=c.frame-phase_frame;
        if (phase==PRESS && elapsed>=hold_frames) transition(RELEASING,c);
        // v97's controller adapter recognizes a Triangle hold after 150 ms.
        // Keep neutral until 12 guest flips AND that minimum; this is an input
        // guard, not a claim that a guest wheel-open flag has been observed.
        else if (phase==OPENING && elapsed>=12u && c.now_us-phase_at>=150000) transition(SELECTING,c);
        else if (phase==SELECTING && commit && direction>=0 && elapsed>=4u && !page && !pending_page &&
                 selection_emitted && (uint32_t)(c.frame-selection_frame)>=2u)
            transition(RELEASING,c);
        else if (phase==CANCELLING && elapsed>=2u) transition(RELEASING,c);
        else if (phase==RELEASING && elapsed>=2u) {
            finish(commit || !wheel ? COMPLETE : CANCELLED); return out;
        }
        if (phase==PRESS) out.mask=mask;
        if (wheel) {
            out.sticks=1; // left only; never seize the camera/right stick
            if (phase!=RELEASING) out.mask=0x1000u;
            if (phase==SELECTING) {
                if (pending_page) { page=pending_page; pending_page=0; page_frame=c.frame; }
                if (page && (uint32_t)(c.frame-page_frame)>=2u) {
                    page=0; page_released=true; page_released_frame=c.frame;
                }
                if (page) out.mask|=page<0 ? 0x0400u : 0x0800u;
                static const unsigned char axes[8][2]={{128,0},{219,37},{255,128},{219,219},
                    {128,255},{37,219},{0,128},{37,37}};
                if (direction>=0) { out.ax[0]=axes[direction][0]; out.ax[1]=axes[direction][1]; }
                if (direction>=0 && !page && !pending_page && !selection_emitted) {
                    selection_frame=c.frame; selection_emitted=true;
                }
            }
        }
        return out;
    }
};

#endif
