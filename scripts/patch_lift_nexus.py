#!/usr/bin/env python3
"""Into the Nexus: patches applied to the lifted PPU code after every lift.

Usage: patch_lift_nexus.py <game/src/recomp>

Every inserted line carries the marker NEXUS-PATCH, so the script is safe to run
twice (it skips a function that already has its patch) and a patch can be found
and removed by hand. Keyed by function name: a function the lift no longer
produces is an error, not a silent skip. RACiT's scripts/patch_lift.py is keyed
to A Crack in Time's addresses and must never run on this lift.

1. menu-event (2026-11-10): the title menu never appears; the game keeps its own
   "blank the screen" flag (byte 0x00EF21F5) set for good.
   On the first entry into the frontend the game starts a save auto-load and
   shows the menu when the save manager reports the result: func_00788AFC ->
   func_00321BC4 -> func_00530598(frontend 0x00F213A0, event) -> func_00530420 ->
   func_005302D8 (clears the flag). func_00530598 ignores an event unless the
   frontend screen is loaded (state s16 at +0x24 is 3 or 4), and func_005302D8
   does nothing unless the main-menu screen 0x00F215A8 is loaded too -- and marks
   the request done either way (+0xAC). On a PS3 the save utility takes long
   enough for both screens to load first. Here the result (event 8, no save
   data) is ready at once and is delivered in the very frame the frontend
   starts loading, so it is dropped.
   Fix: func_00530598 holds an event that arrives before both screens are
   loaded; the frontend's per-frame update func_0052E640 hands it over once
   they are. NEXUS_NO_MENU_EVENT_FIX=1 turns it off.

2. menu-fallback (2026-11-10, INTERIM WORKAROUND, not a root-cause fix): with no
   save data the auto-load reports event 8, and by design only events 1, 5 and
   7 call func_00530420 (show the main menu); what shows it on a fresh PS3 was
   not found (suspects: a follow-up save-manager request that never starts
   here, cellSysutil NID 0x248BD1D8 = the save call at 0x797554 being
   unresolved, or an intro movie). Until it is: when the frontend has been
   updating for 60 frames with both menu screens loaded, the blank flag still
   set, no event held and the show-menu-once byte (+0xAC) clear, the patch sets
   +0xAC and calls func_00530420 itself, exactly as events 1/5/7 do.
   NEXUS_NO_MENU_FALLBACK=1 turns it off.

3. fps60 (2026-11-10): the engine has a frame-rate mode, func_0069892C(mode),
   current mode at 0xF91404, wanted mode at 0xEF22C8 (copied from the current
   one before a load, restored after it). Modes: 0 unlimited, 1 = 60 fps (vblank
   interval 1, vsync), 2 = 30 fps (interval 2, the game's choice in play),
   3 = 30 fps vsync, 4 / 5 = 20 / 15 fps. The vblank handler func_00697DC4 opens
   each frame every <interval> vblanks (label 0x440), and the game's own frame
   time (float 0xF940EC) follows the mode: 0.033333 at 30, 0.016667 at 60 --
   gameplay speed stays right. The patch turns a request for mode 2 or 3 into
   mode 1. NEXUS_FPS=30 keeps the game's 30 fps.

4. ds-* (2026-11-11): the DualSense Plus hooks (game/ds_plus.cpp): SCREAM's
   sound starts, stops and stream pauses -- see the comment at DS_DECL.

5. lua-jmp (2026-11-12): guest setjmp/longjmp, RACiT's patch #1, which had not
   been ported (game/stubs.cpp already held its support code). Lua reports an
   error with longjmp back to the protected call's setjmp. A static lift turns
   longjmp into "restore the guest registers, return to my caller", so the
   error path ran on into the code after the throw with the protected frame's
   stack and registers. Here every function that calls setjmp (func_00A084CC:
   Lua's protected call and its two inlined copies, 0x704F90 / 0x705AF0 /
   0x705D40, plus the lifter's extra entries into the first) does a host setjmp
   in its own C frame, registered under the guest jmp_buf, and the lifted
   longjmp (func_00A085F0), after restoring the guest registers exactly as
   before, host-longjmps to it. NEXUS_NO_LUA_JMP=1 keeps the old behaviour.

6. varstep (2026-11-13): the frame end (func_007631F8, the entry the callers
   use, and the lifter's copy func_007631EC) calls nexus_varstep()
   (game/stubs.cpp) first; its limiter reads the nominal step 0xF940EC through
   nexus_varstep_limit(). Diagnostics only by default: NEXUS_STEP_TEST makes
   frames longer for a test walk; NEXUS_VARSTEP_EC=1 makes the nominal step
   follow the frame time -- off, because movement does not use it (runs
   151/152: the same end pixel either way).

7. clock-smooth (2026-11-13): the game's per-frame clock update
   (func_0074EFD8, r3 = a clock; the main one is 0xF940F8) ends with
   nexus_clock_smooth(r3): in the 60 fps mode the measured frame length the
   game moves by is handed over as 1/60, the difference carried as a debt and
   paid back past a 4 ms band -- on a PS3 the frame starts on a vblank, here it
   wobbled 15-19 ms between evenly paced presents. NEXUS_CLOCK_SMOOTH=0 = the
   measured steps.

8. no-legal (2026-11-14): the boot's silent legal cards (legal text pages,
   autosave notice, Dolby/DTS card; legal.gfx) are listed per region by
   func_0033BF2C or func_0033BCFC (the legal screen's setup func_0033C230
   picks one by the word at 0xF34550; a retail US boot takes BCFC); both now
   return at once, so the list stays empty. The game's -nolegal switch
   does not stop them (its byte 0xF355A8 is set to 1 again in func_005736B0).
   LegalCheckThread (trophies, game data check) is not touched.
   NEXUS_LEGAL=1 = the cards as on a PS3.

9. legal-fast (2026-11-14): #8 left ~30 s of black at boot. The cards' timing is
   not the game's: legal.gfx (ui/screens/legal.swf in global_cached.psarc, AS2)
   plays its own list full_intro_cards<region> -- seizure warning, autosave
   notice, Sony, Insomniac, Dolby, copyright -- 1 s fade in + 2-8 s each + 1 s
   fade out + 0.4 s blank, through the manager's :delayFunc; the game's list (#8)
   only supplies the pictures. The legal manager func_0054BFB0, phase 3
   (loc_0054C278), stores the next phase (5, or 0x11) at +0x60/+0x64, parks in
   0x12 and calls ShowPlate("intro") (lr 0x0054C42C); the movie's closing
   ":response" (func_0054D2AC) sets the phase to +0x64. Here that call is
   skipped and the phase set to +0x64 at once, as the response would. LegalCheck
   Thread was started in phase 4 before it and phase 5 waits for it (byte
   0xD94931), so trophies and the game data check still run. NEXUS_LEGAL=1 = the
   intro plays (with #8's pictures back too).
   Run 157: the install prompt ("The game will need to install data to the
   system storage. (X) CONTINUE") ~2 s into the boot instead of ~32 s. It is the
   prompt every boot shows (run 156 too: its PAD_SCRIPT's X at 32 s answered it;
   the silent runs 157/158 have no presses, so it stayed). A 35 s hold of the
   prompt (run 158) only delayed it: not used.

10. install-diag (2026-11-14): why that prompt comes every boot. Phase 5's
   install flag (+0x69) = the storage object's method +0x24 (func_001EAF68) OR
   the install list check (func_001EB2E0: per item, cellFsStat of "%s/%s" on the
   install dir, size at +0xC4 against the list's +0xC0; func_001EB190). Logged
   once each: the two answers, and every item that does not match (path, sizes).

11. gpu-idle-wait (2026-11-16, owner's 0.9.2 freeze ~30 s into a Kragg arena
   challenge): the frame end (func_005F6F78, and the lifter's copy func_005F6F6C)
   reads the GCM control block's `get` once (lwz r27,4(r3)) and spins at
   0x005F7120 until `put` equals it. A PS3's get is the RSX fetch pointer and is
   at put by then; ours is published per walker pass and can be a few commands
   short, and the loop never ends (main stuck, get long since at put). Every
   pass of the loop now re-reads get through nexus_gpu_idle_get (game/stubs.cpp),
   which logs each wait the old loop would not have survived.
   NEXUS_NO_GPU_IDLE_FIX=1 = the game's loop unchanged.
"""
import re
import glob
import io
import os
import sys

MARK = "/* NEXUS-PATCH menu-event */"

DECL = ("extern uint32_t g_nexus_menu_evt, g_nexus_menu_obj; " + MARK)

HOLD = (
    "        { " + MARK + " const uint32_t t_ = (uint32_t)ctx->gpr[3]; "
    "const int a_ = (int)(int16_t)vm_read16(t_ + 0x24), b_ = (int)(int16_t)vm_read16(0x00F215A8u + 0x24); "
    "static int off_ = -1; if (off_ < 0) off_ = getenv(\"NEXUS_NO_MENU_EVENT_FIX\") ? 1 : 0; "
    "if (!off_ && !((a_ == 3 || a_ == 4) && (b_ == 3 || b_ == 4))) { "
    "g_nexus_menu_evt = (uint32_t)ctx->gpr[4]; g_nexus_menu_obj = t_; "
    "fprintf(stderr, \"[nexus] menu event %u held until the menu screens are loaded (states %d, %d)%c\", g_nexus_menu_evt, a_, b_, 10); "
    "return; } }"
)

REPLAY = (
    "        { " + MARK + " if (g_nexus_menu_evt && (uint32_t)ctx->gpr[3] == g_nexus_menu_obj) { "
    "const int a_ = (int)(int16_t)vm_read16(g_nexus_menu_obj + 0x24), b_ = (int)(int16_t)vm_read16(0x00F215A8u + 0x24); "
    "if ((a_ == 3 || a_ == 4) && (b_ == 3 || b_ == 4)) { "
    "uint64_t g_[13]; double f_[14]; const uint64_t lr_ = ctx->lr, ctr_ = ctx->ctr; const uint32_t cr_ = ctx->cr, xer_ = ctx->xer; "
    "for (int i_ = 0; i_ < 13; i_++) g_[i_] = ctx->gpr[i_]; for (int i_ = 0; i_ < 14; i_++) f_[i_] = ctx->fpr[i_]; "
    "fprintf(stderr, \"[nexus] menu event %u handed over%c\", g_nexus_menu_evt, 10); "
    "ctx->gpr[4] = g_nexus_menu_evt; g_nexus_menu_evt = 0; ctx->lr = 0x0052E640; "
    "func_00530598(ctx); DRAIN_TRAMPOLINE(ctx); "
    "for (int i_ = 0; i_ < 13; i_++) if (i_ != 1 && i_ != 2) ctx->gpr[i_] = g_[i_]; "
    "for (int i_ = 0; i_ < 14; i_++) ctx->fpr[i_] = f_[i_]; "
    "ctx->lr = lr_; ctx->ctr = ctr_; ctx->cr = cr_; ctx->xer = xer_; } } }"
)

FALLBACK = (
    "        { /* NEXUS-PATCH menu-fallback */ static unsigned n_; static int off_ = -1; "
    "if (off_ < 0) off_ = getenv(\"NEXUS_NO_MENU_FALLBACK\") ? 1 : 0; "
    "const uint32_t t_ = (uint32_t)ctx->gpr[3]; "
    "const int a_ = (int)(int16_t)vm_read16(t_ + 0x24), b_ = (int)(int16_t)vm_read16(0x00F215A8u + 0x24); "
    "if (!off_ && t_ == 0x00F213A0u && !g_nexus_menu_evt && vm_read8(0x00EF21F5u) && !vm_read8(t_ + 0xAC) && "
    "(a_ == 3 || a_ == 4) && (b_ == 3 || b_ == 4)) { if (++n_ == 60) { "
    "uint64_t g_[13]; double f_[14]; const uint64_t lr_ = ctx->lr, ctr_ = ctx->ctr; const uint32_t cr_ = ctx->cr, xer_ = ctx->xer; "
    "for (int i_ = 0; i_ < 13; i_++) g_[i_] = ctx->gpr[i_]; for (int i_ = 0; i_ < 14; i_++) f_[i_] = ctx->fpr[i_]; "
    "fprintf(stderr, \"[nexus] workaround: nothing showed the main menu after the save check; showing it%c\", 10); "
    "vm_write8(t_ + 0xAC, 1); ctx->lr = 0x0052E640; func_00530420(ctx); DRAIN_TRAMPOLINE(ctx); "
    "for (int i_ = 0; i_ < 13; i_++) if (i_ != 1 && i_ != 2) ctx->gpr[i_] = g_[i_]; "
    "for (int i_ = 0; i_ < 14; i_++) ctx->fpr[i_] = f_[i_]; "
    "ctx->lr = lr_; ctx->ctr = ctr_; ctx->cr = cr_; ctx->xer = xer_; } } else n_ = 0; }"
)

FPS60 = (
    "        { /* NEXUS-PATCH fps60 */ static int fps_ = -1; "
    "if (fps_ < 0) { const char* e_ = getenv(\"NEXUS_FPS\"); fps_ = (e_ && atoi(e_) == 30) ? 30 : 60; } "
    "const uint32_t m_ = (uint32_t)ctx->gpr[3]; "
    "if (fps_ == 60 && (m_ == 2u || m_ == 3u)) { static int said_; "
    "if (!said_) { said_ = 1; fprintf(stderr, \"[nexus] frame-rate mode %u -> 1 (60 fps; NEXUS_FPS=30 keeps the game's 30)%c\", m_, 10); } "
    "ctx->gpr[3] = 1; } }"
)

PATCHES = [("func_00530598", HOLD), ("func_0052E640", REPLAY + chr(10) + FALLBACK),
           ("func_0069892C", FPS60)]

# 4. ds-* (2026-11-11): DualSense Plus, game/ds_plus.cpp. SCREAM's stop
#    (func_00A81C00, A Crack in Time's func_00974DF0) and release (func_00A81988,
#    ACiT func_00973EB0) take the sound's handle in r3; its stream pause callback
#    (func_00A9F5D8, ACiT func_0099B4E0) the stream in r3 and paused in r4. The
#    engine starts every sound at one call of SCREAM's play-a-bank-sound
#    (func_00A81FD0), at 0x006C707C in the sound manager's update -- code the
#    lifter copied into four functions; each copy is wrapped (DS_START): bank r3
#    and sound r4 before the call, the handle in r3 after it, the name in r26.
DS_DECL = ('extern "C" void nexus_ds_snd_started(uint32_t, uint32_t, const uint64_t*); '
           'extern "C" void nexus_ds_snd_stop(uint32_t); extern "C" void nexus_ds_snd_release(uint32_t); extern "C" void nexus_ds_stream_pause(uint32_t, uint32_t); '
           '/* NEXUS-PATCH ds-decl */')
DS_STOP = "        nexus_ds_snd_stop((uint32_t)ctx->gpr[3]); /* NEXUS-PATCH ds-stop */"
# Release (func_00A81988) is "free the handle, let the sound finish" in Into the Nexus: the engine
# releases every one-shot right after starting it (2026-11-17 run: each RYNO shot released within
# microseconds), so it must not stop the controller's copy (that made every gun a 30 ms click).
DS_REL = "        nexus_ds_snd_release((uint32_t)ctx->gpr[3]); /* NEXUS-PATCH ds-release */"
DS_PAUSE = "        nexus_ds_stream_pause((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]); /* NEXUS-PATCH ds-pause */"
DS_CALL = "ctx->lr = 0x006C7080; func_00A81FD0(ctx); DRAIN_TRAMPOLINE(ctx);"
DS_START = ("{ const uint32_t b_ = (uint32_t)ctx->gpr[3], s_ = (uint32_t)ctx->gpr[4]; " + DS_CALL +
            " nexus_ds_snd_started(b_, s_, (const uint64_t*)ctx->gpr); } /* NEXUS-PATCH ds-start */")
DS_PATCHES = [("func_00A81C00", DS_STOP), ("func_00A81988", DS_REL), ("func_00A9F5D8", DS_PAUSE)]


def ds_patch(files):
    """The ds-* hooks: function entries (DS_PATCHES) and every copy of the wrapped call (DS_START)."""
    todo = dict(DS_PATCHES)
    done, wrapped = [], 0
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        nl = "\r\n" if "\r\n" in s else "\n"
        s0 = s
        for fn, body in DS_PATCHES:
            head = "void %s(ppu_context* ctx) {" % fn
            if head not in s:
                continue
            at = s.index(head)
            if s[at + len(head):at + len(head) + 200].find("NEXUS-PATCH ds-") < 0:
                s = s[:at] + head + nl + body + s[at + len(head):]
                done.append(fn)
            else:
                done.append(fn + " (already patched)")
            todo.pop(fn, None)
        s = s.replace(DS_CALL + " nexus_ds", "\0")      # a wrapped call stays as it is
        s = s.replace(DS_CALL, DS_START).replace("\0", DS_CALL + " nexus_ds")
        wrapped += s.count("NEXUS-PATCH ds-start")
        if s != s0:
            if "NEXUS-PATCH ds-decl" not in s:
                i = s.index(nl, s.index('#include "ppu_recomp.h"')) + len(nl)
                s = s[:i] + DS_DECL + nl + s[i:]
            io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    if todo:
        sys.exit("patch_lift_nexus: not found in the lift: " + ", ".join(sorted(todo)))
    if not wrapped:
        sys.exit("patch_lift_nexus: the engine's SCREAM play call (lr 0x006C7080) is not in the lift")
    return done + ["ds-start x%d" % wrapped]


# 5. lua-jmp: see the docstring. A function qualifies when it calls setjmp
#    exactly once, the call is not inside a loop (no later goto to a label that
#    precedes it: a second push of the same frame would link it to itself) and
#    it leaves only through plain `return;` statements, where the frame is popped.
JMP_MARK = "/* NEXUS-PATCH lua-jmp */"
JMP_SETJMP, JMP_LONGJMP = "func_00A084CC", "func_00A085F0"
JMP_DECL = [
    '#include "ps3_setjmp.h" ' + JMP_MARK,
    'typedef struct RacJbFrame { jmp_buf env; uint32_t guest_buf; struct RacJbFrame* prev; } RacJbFrame; ' + JMP_MARK,
    'extern "C" void racacit_jb_push(RacJbFrame* f, uint32_t guest_buf); extern "C" void racacit_jb_pop(RacJbFrame* f); '
    'extern "C" void racacit_longjmp(uint32_t guest_buf); ' + JMP_MARK]
JMP_CALL = re.compile(r"^(\s*)ctx->lr = (0x[0-9A-Fa-f]{8}); " + JMP_SETJMP + r"\(ctx\); DRAIN_TRAMPOLINE\(ctx\);$")
FUNC_HEAD = re.compile(r"^void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\) \{$")


def jmp_patch(files):
    done, longjmp_seen = [], False
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        nl = "\r\n" if "\r\n" in s else "\n"
        if JMP_SETJMP + "(ctx)" not in s and ("void %s(ppu_context" % JMP_LONGJMP) not in s:
            continue
        lines = s.split(nl)
        out, changed, i = [], False, 0
        while i < len(lines):
            m = FUNC_HEAD.match(lines[i])
            if not m:
                out.append(lines[i]); i += 1
                continue
            j = i
            while lines[j] != "}":
                j += 1
            name, body = m.group(1), lines[i:j + 1]
            i = j + 1
            if any("NEXUS-PATCH lua-jmp" in l for l in body):
                done.append(name + " (already patched)")
                longjmp_seen = longjmp_seen or name == JMP_LONGJMP
                out += body
                continue
            if name == JMP_LONGJMP:
                # The body restores the guest registers from the jmp_buf and sets
                # r3; its first `return;` is the guest's blr to the saved lr.
                k = next(n for n, l in enumerate(body) if l.strip() == "return;")
                body = ([body[0], "        const uint32_t jb_ = (uint32_t)ctx->gpr[3]; " + JMP_MARK] + body[1:k] +
                        ["        racacit_longjmp(jb_); " + JMP_MARK] + body[k:])
                longjmp_seen = changed = True
                done.append(name + " (longjmp)")
                out += body
                continue
            calls = [n for n, l in enumerate(body) if JMP_CALL.match(l)]
            if not calls:
                out += body
                continue
            if len(calls) != 1:
                sys.exit("patch_lift_nexus lua-jmp: %s calls setjmp %d times" % (name, len(calls)))
            c = calls[0]
            before = set(l[:-1] for l in body[:c] if l.startswith("loc_") and l.endswith(":"))
            for l in body[c + 1:]:
                g = re.search(r"goto (loc_[0-9A-Fa-f]+);", l)
                if g and g.group(1) in before:
                    sys.exit("patch_lift_nexus lua-jmp: %s calls setjmp inside a loop (%s)" % (name, g.group(1)))
            if any("g_trampoline_fn" in l for l in body):
                sys.exit("patch_lift_nexus lua-jmp: %s leaves through a trampoline" % name)
            if not any(l.strip() == "return;" for l in body):
                sys.exit("patch_lift_nexus lua-jmp: %s has no return" % name)
            mc = JMP_CALL.match(body[c])
            ind, lr = mc.group(1), mc.group(2)
            new = [body[0], ind + "RacJbFrame jbf_; int jb_on_ = 0; " + JMP_MARK]
            for n, l in enumerate(body[1:], 1):
                if n == c:
                    new += [ind + "ctx->lr = %s; racacit_jb_push(&jbf_, (uint32_t)ctx->gpr[3]); jb_on_ = 1; %s" % (lr, JMP_MARK),
                            ind + "if (setjmp(jbf_.env) == 0) { %s(ctx); DRAIN_TRAMPOLINE(ctx); } %s" % (JMP_SETJMP, JMP_MARK),
                            ind + "else { g_trampoline_fn = 0; } /* NEXUS-PATCH lua-jmp: a guest longjmp landed, registers restored, r3 = its value */"]
                elif l.strip() == "return;":
                    new += [ind + "if (jb_on_) racacit_jb_pop(&jbf_); " + JMP_MARK, l]
                else:
                    new.append(l)
            changed = True
            done.append(name)
            out += new
        if changed:
            s = nl.join(out)
            if JMP_DECL[1] not in s:
                k = s.index(nl, s.index('#include "ppu_recomp.h"')) + len(nl)
                s = s[:k] + nl.join(JMP_DECL) + nl + s[k:]
            io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    if not longjmp_seen:
        sys.exit("patch_lift_nexus lua-jmp: %s (longjmp) is not in the lift" % JMP_LONGJMP)
    if not [d for d in done if "longjmp" not in d]:
        sys.exit("patch_lift_nexus lua-jmp: no caller of %s (setjmp) in the lift" % JMP_SETJMP)
    return ["lua-jmp: " + " ".join(done)]


VS_MARK = "/* NEXUS-PATCH varstep */"
VS_FUNCS = ["func_007631F8", "func_007631EC"]
VS_DECL = 'extern "C" void nexus_varstep(void); extern "C" uint32_t nexus_varstep_limit(uint32_t); ' + VS_MARK
VS_READ = "{ uint32_t tmp = vm_read32(ctx->gpr[4] + 0x4); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[2] = ftmp; }"
VS_LIMIT = ("{ uint32_t tmp = nexus_varstep_limit(vm_read32(ctx->gpr[4] + 0x4)); float ftmp; memcpy(&ftmp, &tmp, 4); "
            "ctx->fpr[2] = ftmp; } " + VS_MARK)


# 6. varstep: see the docstring. The limiter's read is the one load of
#    0xF940E8 + 4 (r4) after the addic that forms 0xF940E8 in the body.
def varstep_patch(files):
    done = []
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        if not any(("void %s(ppu_context* ctx) {" % f) in s for f in VS_FUNCS):
            continue
        nl = "\r\n" if "\r\n" in s else "\n"
        lines = s.split(nl)
        out, changed, i = [], False, 0
        while i < len(lines):
            m = FUNC_HEAD.match(lines[i])
            if not m or m.group(1) not in VS_FUNCS:
                out.append(lines[i]); i += 1
                continue
            j = i
            while lines[j] != "}":
                j += 1
            name, body = m.group(1), lines[i:j + 1]
            i = j + 1
            if any(VS_MARK in l for l in body):
                done.append(name + " (already patched)")
                out += body
                continue
            at = [n for n, l in enumerate(body) if "(int64_t)(0x40E8)" in l]
            reads = [n for n, l in enumerate(body) if l.strip() == VS_READ and at and n > at[0]]
            if len(at) != 1 or len(reads) != 1:
                sys.exit("patch_lift_nexus varstep: %s: %d forms of 0xF940E8, %d step reads" % (name, len(at), len(reads)))
            ind = body[reads[0]][:len(body[reads[0]]) - len(body[reads[0]].lstrip())]
            body[reads[0]] = ind + VS_LIMIT
            body = [body[0], "        nexus_varstep(); " + VS_MARK] + body[1:]
            changed = True
            done.append(name)
            out += body
        if changed:
            s = nl.join(out)
            if VS_DECL not in s:
                k = s.index(nl, s.index('#include "ppu_recomp.h"')) + len(nl)
                s = s[:k] + VS_DECL + nl + s[k:]
            io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    if "func_007631F8" not in " ".join(done):
        sys.exit("patch_lift_nexus varstep: func_007631F8 (the frame end) is not in the lift")
    return ["varstep: " + " ".join(done)]


CS_MARK = "/* NEXUS-PATCH clock-smooth */"
CS_FUNC = "func_0074EFD8"
CS_DECL = 'extern "C" void nexus_clock_smooth(uint32_t); ' + CS_MARK


# 7. clock-smooth: see the docstring. Before the clock update's first return;
#    r3 still holds the clock (the body only reads it).
def clock_patch(files):
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        head = "void %s(ppu_context* ctx) {" % CS_FUNC
        if head not in s:
            continue
        nl = "\r\n" if "\r\n" in s else "\n"
        at = s.index(head)
        end = s.index(nl + "}" + nl, at)
        body = s[at:end]
        if CS_MARK in body:
            return [CS_FUNC + " (clock-smooth, already patched)"]
        lines = body.split(nl)
        k = next(n for n, l in enumerate(lines) if l.strip() == "return;")
        if any(("ctx->gpr[3] =" in l) for l in lines[1:k]):
            sys.exit("patch_lift_nexus clock-smooth: %s writes r3 before its return" % CS_FUNC)
        lines.insert(k, "        nexus_clock_smooth((uint32_t)ctx->gpr[3]); " + CS_MARK)
        s = s[:at] + nl.join(lines) + s[end:]
        if CS_DECL not in s:
            i = s.index(nl, s.index('#include "ppu_recomp.h"')) + len(nl)
            s = s[:i] + CS_DECL + nl + s[i:]
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
        return [CS_FUNC + " (clock-smooth)"]
    sys.exit("patch_lift_nexus clock-smooth: %s is not in the lift" % CS_FUNC)


NL_MARK = "/* NEXUS-PATCH no-legal */"
NL_FUNCS = ("func_0033BF2C", "func_0033BCFC")
NL_BODY = (
    "        { " + NL_MARK + " static int on_ = -1; if (on_ < 0) { const char* e_ = getenv(\"NEXUS_LEGAL\"); "
    "on_ = (e_ && e_[0] == '1') ? 1 : 0; "
    "fprintf(stderr, \"[nexus] legal cards: %s%c\", on_ ? \"shown (NEXUS_LEGAL=1)\" : \"skipped\", 10); } "
    "if (!on_) return; }"
)


# 8. no-legal: see the docstring. At the top of the card-list builder, before
#    its stack frame, so returning leaves the caller's state untouched.
def legal_patch(files):
    return [r for fn in NL_FUNCS for r in legal_patch_one(files, fn)]


def legal_patch_one(files, NL_FUNC):
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        head = "void %s(ppu_context* ctx) {" % NL_FUNC
        if head not in s:
            continue
        if s.count(head) != 1:
            sys.exit("patch_lift_nexus no-legal: %s defined %d times" % (NL_FUNC, s.count(head)))
        nl = "\r\n" if "\r\n" in s else "\n"
        at = s.index(head) + len(head)
        if NL_MARK in s[at:at + 600]:
            return [NL_FUNC + " (no-legal, already patched)"]
        s = s[:at] + nl + NL_BODY + s[at:]
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
        return [NL_FUNC + " (no-legal)"]
    sys.exit("patch_lift_nexus no-legal: %s is not in the lift" % NL_FUNC)


LF_MARK = "/* NEXUS-PATCH legal-fast */"
# The intro's ShowPlate: skipped, the phase set as its closing response would.
LF_CALL = "        ctx->lr = 0x0054C42C; func_00519400(ctx); DRAIN_TRAMPOLINE(ctx);"
LF_BODY = (
    "        { " + LF_MARK + " static int on_ = -1; if (on_ < 0) { const char* e_ = getenv(\"NEXUS_LEGAL\"); "
    "on_ = (e_ && e_[0] == '1') ? 0 : 1; "
    "fprintf(stderr, \"[nexus] boot intro (health warning, logos, copyright): %s%c\", on_ ? \"skipped\" : \"played (NEXUS_LEGAL=1)\", 10); } "
    "if (on_) { vm_write32(ctx->gpr[26] + 0x5C, vm_read32(ctx->gpr[26] + 0x64)); goto loc_0054D1E0; } }"
)


# 9. legal-fast: see the docstring. Just before the intro's ShowPlate call (its
#    return address is unique), so the arguments built for it are released at
#    loc_0054D1E0 exactly as after the call.
def legal_fast_patch(files):
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        if LF_CALL not in s:
            continue
        if s.count(LF_CALL) != 1 or "loc_0054D1E0:" not in s:
            sys.exit("patch_lift_nexus legal-fast: the intro ShowPlate call or its tail label is not unique in %s" % path)
        nl = "\r\n" if "\r\n" in s else "\n"
        at = s.index(LF_CALL)
        if LF_MARK in s[max(0, at - 800):at]:
            return ["func_0054BFB0 (legal-fast, already patched)"]
        s = s[:at] + LF_BODY + nl + s[at:]
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
        return ["func_0054BFB0 (legal-fast)"]
    sys.exit("patch_lift_nexus legal-fast: the intro ShowPlate call (lr 0x0054C42C) is not in the lift")


ID_MARK = "/* NEXUS-PATCH install-diag */"
# After each of phase 5's two install queries: their answers, once.
ID_AFTER = (
    ("        ctx->lr = 0x0054C45C; func_001EAF68(ctx); DRAIN_TRAMPOLINE(ctx);", "storage query (+0x24)"),
    ("        ctx->lr = 0x0054C468; func_001EB2E0(ctx); DRAIN_TRAMPOLINE(ctx);", "install list check"),
)
ID_AFTER_BODY = (
    "        { " + ID_MARK + " static int said_; if (!said_) { said_ = 1; "
    "fprintf(stderr, \"[nexus] install check: %%s says %%s (%%u)%%c\", \"%s\", "
    "(uint32_t)ctx->gpr[3] ? \"install needed\" : \"installed\", (uint32_t)ctx->gpr[3], 10); } }"
)
# func_001EB190's "not installed" exit: the item's path and sizes (the first 24).
ID_ITEM = "loc_001EB2C0:"
ID_ITEM_BODY = (
    "        { " + ID_MARK + " static int n_; if (n_ < 24) { n_++; char p_[208]; unsigned i_ = 0; "
    "for (; i_ < sizeof p_ - 1; i_++) { p_[i_] = (char)vm_read8((uint32_t)ctx->gpr[1] + 0xA8 + i_); if (!p_[i_]) break; } p_[i_] = 0; "
    "fprintf(stderr, \"[nexus] install check: %s -- list says %u bytes, the drive has %u%c\", p_, "
    "(unsigned)vm_read32((uint32_t)ctx->gpr[31] + 0xC0), (unsigned)vm_read32((uint32_t)ctx->gpr[31] + 0xC4), 10); } }"
)


# 10. install-diag: see the docstring.
def install_diag_patch(files):
    done = []
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        hits = [c for c, _ in ID_AFTER if c in s] + ([ID_ITEM] if ID_ITEM in s else [])
        if not hits:
            continue
        nl = "\r\n" if "\r\n" in s else "\n"
        changed = False
        for call, what in ID_AFTER:
            if call not in s:
                continue
            if s.count(call) != 1:
                sys.exit("patch_lift_nexus install-diag: %r found %d times" % (call, s.count(call)))
            at = s.index(call) + len(call)
            if ID_MARK in s[at:at + 300]:
                done.append(what + " (install-diag, already patched)"); continue
            s = s[:at] + nl + (ID_AFTER_BODY % what) + s[at:]
            changed = True; done.append(what + " (install-diag)")
        if ID_ITEM in s:
            if s.count(ID_ITEM) != 1:
                sys.exit("patch_lift_nexus install-diag: %s found %d times" % (ID_ITEM, s.count(ID_ITEM)))
            at = s.index(ID_ITEM) + len(ID_ITEM)
            if ID_MARK in s[at:at + 300]:
                done.append("func_001EB190 (install-diag, already patched)")
            else:
                s = s[:at] + nl + ID_ITEM_BODY + s[at:]
                changed = True; done.append("func_001EB190 (install-diag)")
        if changed:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    if len(done) != 3:
        sys.exit("patch_lift_nexus install-diag: patched %d of 3 places: %s" % (len(done), done))
    return done


GI_MARK = "/* NEXUS-PATCH gpu-idle-wait */"
GI_DECL = 'extern "C" uint32_t nexus_gpu_idle_get(uint32_t, uint32_t); ' + GI_MARK
GI_LABEL = "loc_005F7120:"
GI_READ = "        ctx->gpr[4] = vm_read32(ctx->gpr[3] + 0x0);"
GI_BODY = "        ctx->gpr[27] = nexus_gpu_idle_get((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[27]); " + GI_MARK


# 11. gpu-idle-wait: see the docstring. Every copy of the loop (the lifter put
#     one in each function that enters ahead of it): the label, then the put read.
def gpu_idle_patch(files):
    done = []
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        if GI_LABEL not in s:
            continue
        nl = "\r\n" if "\r\n" in s else "\n"
        seq = GI_LABEL + nl + GI_READ
        n_label, n_seq = s.count(GI_LABEL), s.count(seq)
        n_done = s.count(GI_LABEL + nl + GI_BODY + nl + GI_READ)
        if n_done == n_label:
            done.append("%s (gpu-idle-wait x%d, already patched)" % (os.path.basename(path), n_done))
            continue
        if n_seq != n_label or n_done:
            sys.exit("patch_lift_nexus gpu-idle-wait: %s: %d labels, %d with the put read next, %d patched"
                     % (path, n_label, n_seq, n_done))
        s = s.replace(seq, GI_LABEL + nl + GI_BODY + nl + GI_READ)
        if GI_DECL not in s:
            k = s.index(nl, s.index('#include "ppu_recomp.h"')) + len(nl)
            s = s[:k] + GI_DECL + nl + s[k:]
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
        done.append("%s (gpu-idle-wait x%d)" % (os.path.basename(path), n_label))
    if not done:
        sys.exit("patch_lift_nexus gpu-idle-wait: %s is not in the lift" % GI_LABEL)
    return done


def main():
    recomp = sys.argv[1]
    files = sorted(glob.glob(os.path.join(recomp, "ppu_recomp_*.cpp")))
    todo = dict(PATCHES)
    done = []
    for path in files:
        s = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        nl = "\r\n" if "\r\n" in s else "\n"
        changed = False
        for fn, body in PATCHES:
            head = "void %s(ppu_context* ctx) {" % fn
            if head not in s:
                continue
            if s.count(head) != 1:
                sys.exit("%s: %s defined %d times" % (path, fn, s.count(head)))
            at = s.index(head)
            if s[at + len(head):at + len(head) + 400].find("NEXUS-PATCH") >= 0:
                done.append(fn + " (already patched)")
            else:
                s = s[:at] + DECL + nl + head + nl + body + s[at + len(head):]
                changed = True
                done.append(fn)
            todo.pop(fn, None)
        if changed:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
    if todo:
        sys.exit("patch_lift_nexus: not found in the lift: " + ", ".join(sorted(todo)))
    done += ds_patch(files)
    done += jmp_patch(files)
    done += varstep_patch(files)
    done += clock_patch(files)
    done += legal_patch(files)
    done += legal_fast_patch(files)
    done += install_diag_patch(files)
    done += gpu_idle_patch(files)
    print("patch_lift_nexus: " + ", ".join(done))


if __name__ == "__main__":
    main()
