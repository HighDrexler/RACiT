#!/usr/bin/env python3
"""Post-lift source patches for A Crack in Time (re-run after every lift).

1. Guest setjmp/longjmp (Lua error handling: luaD_rawrunprotected at 0x50A6BC
   calls setjmp 0x90B138; luaD_throw at 0x50A83C calls longjmp 0x90B25C).
   A static lift turns longjmp into "restore guest regs, return to caller",
   which falls through with r3=1 and dereferences garbage. Here the protected
   frame does a real host setjmp in its own C frame (registered on a
   per-thread stack keyed by the guest jmp_buf), and the lifted longjmp, after
   restoring guest registers exactly as before, host-longjmps to it. The
   target is always an ancestor frame on the same host stack, as on hardware.

2. GPU-idle wait in func_005CB084 (0x5CB1AC): the title reads ctrl->get ONCE
   and spins until ctrl->put equals it (Insomniac's control struct is not
   volatile, so the compiler hoisted the get load out of "while (put != get)").
   On hardware the RSX has long consumed the last flush by then; our paced
   FIFO drain sometimes has not, and the snapshot never matches again. Re-read
   get inside the loop, which is what the source meant.

3. End-of-frame blit in func_005F3E50: the title moves put past a return JUMP
   that lives in scratch memory and reuses the scratch at once, relying on the
   RSX having taken the JUMP (microseconds on hardware). Tell the runtime at
   both put writes so the FIFO walker catches up first (game/stubs.cpp
   racacit_gcm_put_moved has the full account).

11. Vector instructions whose destination is also a source (vcmpequh. v0,
   v1, v0): the lifted lane loop overwrote the source as it went. Rewritten
   by scripts/fix_vmx_alias.py to read a snapshot -- the cause of the
   striped textures (the texture streamer's vectorised id search failed).

Usage: scripts/patch_lift.py [game/src/recomp]
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "game", "src", "recomp")

DECLS = '''
/* racacit: guest setjmp/longjmp support (scripts/patch_lift.py) */
#include "ps3_setjmp.h"   /* non-unwinding on Windows (tools/ps3recomp/runtime/platform) */
typedef struct RacJbFrame { jmp_buf env; uint32_t guest_buf; struct RacJbFrame* prev; } RacJbFrame;
extern "C" void racacit_jb_push(RacJbFrame* f, uint32_t guest_buf);
extern "C" void racacit_jb_pop(RacJbFrame* f);
extern "C" void racacit_longjmp(uint32_t guest_buf);
'''


# Script (Lua C) functions traced by RACACIT_LUA_CTRACE; addresses from the
# title's registration table of (name, function descriptor) pairs at ~0xC8D000.
LUA_CTRACE = (
    ('func_00279EA8', 'actor_speak'), ('func_00279E18', 'actor_is_speaking'),
    ('func_00279FD4', 'make_actor'), ('func_00279828', 'actor_move'),
    ('func_00290804', 'queue_scene'), ('func_00290A2C', 'queue_movie'),
    ('func_00290998', 'queue_cinematic'), ('func_00290E8C', 'on_next_cine_end'),
    ('func_002918C8', 'on_elapsed'), ('func_00290BD8', 'unset_event'),
    ('func_00288928', 'set_follow_cam'), ('func_002882B0', 'activate_cam'),
    ('func_00281774', 'hero_lockout_control'), ('func_00282DA0', 'hero_give_weapon'),
    ('func_00292E84', 'has_valid_checkpoint'), ('func_00292EC8', 'checkpoint'),
    ('func_0028B398', 'warp'), ('func_00279350', 'scene_show_subtitle'),
    ('func_002792EC', 'scene_hide_subtitle'), ('func_00507EC4', 'print'),
    ('func_0051C048', 'stop_all_vo'), ('func_00521804', 'run_script'),
    ('func_00292B5C', 'add_mission'), ('func_00292D44', 'request_auto_save'),
    ('func_0026DC64', 'qwark_door_enable_break'), ('func_0052194C', 'screen_out'),
    ('func_0007DE04', 'screen_out2'),
    # space: what makes a planet / moon landable, and the ship radio
    ('func_0026C71C', 'space_enable_planet'), ('func_0026C7EC', 'space_enable_moon'),
    ('func_0026C964', 'space_land_ship'), ('func_0026C0A0', 'space_unlock_level'),
    ('func_0026C5D4', 'space_set_planet_tag'), ('func_0026C678', 'space_set_moon_tag'),
    ('func_0026C58C', 'space_enable_takeoff'), ('func_0026C268', 'space_set_objective'),
    ('func_0026B5E4', 'space_show_radio'), ('func_0026BA24', 'space_set_radio_channel'),
    # the radio's chain (global space_radio.lua): each track's end starts the next
    ('func_00285B54', 'play_music'), ('func_00285ADC', 'stop_music'),
    ('func_00291444', 'on_audio_end'), ('func_0051CF70', 'play_explicit_dialog_spec'),
    ('func_0051D078', 'play_explicit_sound_spec'), ('func_0051C4D8', 'stop_vo'),
)


def func_span(src, name):
    m = re.search(r'void %s\(ppu_context\* ctx\) \{\n' % name, src)
    if not m:
        return None
    end = src.index('\n}\n', m.end())
    return m.start(), m.end(), end


def patch_file(path):
    src = open(path).read()
    changed = False

    # Each half is tested on its own function: the lifter may put the setjmp
    # caller and longjmp in different chunks.
    sp = func_span(src, 'func_0050A6BC')
    if sp and 'racacit_jb_push' not in src[sp[0]:sp[2]]:
        a, body, end = sp
        fn = src[a:end]
        call = 'ctx->lr = 0x0050A6F8; func_0090B138(ctx); DRAIN_TRAMPOLINE(ctx);'
        assert call in fn, 'rawrunprotected setjmp call not found'
        fn = fn.replace('void func_0050A6BC(ppu_context* ctx) {\n',
                        'void func_0050A6BC(ppu_context* ctx) {\n        RacJbFrame _jbf;\n', 1)
        fn = fn.replace(call,
            'ctx->lr = 0x0050A6F8;\n'
            '        racacit_jb_push(&_jbf, (uint32_t)ctx->gpr[3]);\n'
            '        if (setjmp(_jbf.env) == 0) { func_0090B138(ctx); DRAIN_TRAMPOLINE(ctx); }\n'
            '        else { g_trampoline_fn = 0; /* longjmp landed: guest regs restored, r3 = val */ }', 1)
        # pop the frame on the (only) normal exit
        tail = fn.rindex('        return;')
        fn = fn[:tail] + '        racacit_jb_pop(&_jbf);\n' + fn[tail:]
        src = src[:a] + fn + src[end:]
        changed = True

    sp = func_span(src, 'func_0090B25C')
    if sp and 'racacit_longjmp' not in src[sp[0]:sp[2]]:
        a, body, end = sp
        fn = src[a:end]
        fn = fn.replace('void func_0090B25C(ppu_context* ctx) {\n',
                        'void func_0090B25C(ppu_context* ctx) {\n        uint32_t _jb = (uint32_t)ctx->gpr[3];\n', 1)
        first_ret = fn.index('        return;')
        fn = fn[:first_ret] + '        racacit_longjmp(_jb);\n' + fn[first_ret:]
        src = src[:a] + fn + src[end:]
        changed = True

    sp = func_span(src, 'func_005CB084')
    if sp and '/* racacit: get re-read */' not in src:
        a, body, end = sp
        fn = src[a:end]
        old = ('        ctx->gpr[29] = vm_read32(ctx->gpr[12] + 0x4);\n'
               'loc_005CB1BC:\n')
        assert old in fn, 'GPU idle wait not found in func_005CB084'
        fn = fn.replace(old, 'loc_005CB1BC:\n'
                             '        ctx->gpr[29] = vm_read32(ctx->gpr[12] + 0x4);   /* racacit: get re-read */\n', 1)
        src = src[:a] + fn + src[end:]
        changed = True

    sp = func_span(src, 'func_0050A83C')
    if sp and 'racacit_lua_throw_hook' not in src:
        a, body, end = sp
        fn = src[a:end]
        fn = fn.replace('void func_0050A83C(ppu_context* ctx) {\n',
                        'void func_0050A83C(ppu_context* ctx) {\n        racacit_lua_throw_hook(ctx);   /* racacit: log Lua errors */\n', 1)
        src = src[:a] + fn + src[end:]
        if 'void racacit_lua_throw_hook' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" void racacit_lua_throw_hook(ppu_context* ctx);\n' + src[i:]
        changed = True

    sp = func_span(src, 'func_0005069C')
    if sp and 'racacit_lua_exec_hook' not in src:
        a, body, end = sp
        fn = src[a:end]
        fn = fn.replace('void func_0005069C(ppu_context* ctx) {\n',
                        'void func_0005069C(ppu_context* ctx) {\n        racacit_lua_exec_hook(ctx);   /* racacit: log Lua execute() strings */\n', 1)
        src = src[:a] + fn + src[end:]
        i = src.index('\n', src.index('#include')) + 1
        src = src[:i] + 'extern "C" void racacit_lua_exec_hook(ppu_context* ctx);\n' + src[i:]
        changed = True

    # 12. Sound starts (game/stubs.cpp racacit_snd_play_hook): func_00577ED8 is
    #     the engine's play-a-sound-spec entry (33 call sites; the script
    #     bindings play_explicit_sound_spec / _dialog_spec reach it through
    #     func_004FA9F8 / func_004FB0D4); func_0057AEC0 is the virtual it ends
    #     in (vtable 0xC53208 + 0x20), which the lower sound-instance layer
    #     (0x94ED9C) calls directly. Their callers and arguments say what is
    #     playing -- the DualSense speaker routing keys on them.
    for fname, tag in (('func_00577ED8', 0), ('func_0057AEC0', 1), ('func_006DDC90', 2), ('func_00979CF0', 3)):
        sp = func_span(src, fname)
        if sp and 'racacit_snd_play_hook' not in src[sp[0]:sp[2]]:
            a, body, end = sp
            fn = src[a:end]
            head = 'void %s(ppu_context* ctx) {\n' % fname
            assert fn.count(head) == 1, fname + ' head not found'
            fn = fn.replace(head, head + '        racacit_snd_play_hook(ctx, %d);   /* racacit: sound starts (DualSense routing) */\n' % tag, 1)
            src = src[:a] + fn + src[end:]
            if 'void racacit_snd_play_hook' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_snd_play_hook(ppu_context* ctx, int tag);\n' + src[i:]
            changed = True

    #     func_006DDC90 starts one Insomniac sound instance (name at
    #     [[inst+0xC]]) through SCREAM's func_00979CF0 (bank r3, sound r4);
    #     a routed sound is handed to the controller's player between the
    #     two, and racacit_snd_start_end clears that state on the way out.
    #     The instance's stops (racacit_snd_stop_hook, r3 = the instance):
    #     func_006DE478 sets its stopping flag and stops its SCREAM sound,
    #     func_006DDC58 stops it at once -- the controller's copy ends too.
    for fname, tag in (('func_006DE478', 0), ('func_006DDC58', 1)):
        sp = func_span(src, fname)
        if sp and 'racacit_snd_stop_hook' not in src[sp[0]:sp[2]]:
            a, body, end = sp
            fn = src[a:end]
            head = 'void %s(ppu_context* ctx) {\n' % fname
            assert fn.count(head) == 1, fname + ' head not found'
            fn = fn.replace(head, head + '        racacit_snd_stop_hook(ctx, %d);   /* racacit: sound stops (DualSense speaker) */\n' % tag, 1)
            src = src[:a] + fn + src[end:]
            if 'void racacit_snd_stop_hook' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_snd_stop_hook(ppu_context* ctx, int tag);\n' + src[i:]
            changed = True
    sp = func_span(src, 'func_006DDC90')
    if sp and 'racacit_snd_start_end' not in src[sp[0]:sp[2]]:
        a, body, end = sp
        fn = src[a:end]
        assert fn.count('        return;') == 1, 'func_006DDC90: expected exactly one return'
        tail = fn.rindex('        return;')
        fn = fn[:tail] + '        racacit_snd_start_end();\n' + fn[tail:]
        src = src[:a] + fn + src[end:]
        if 'void racacit_snd_start_end' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" void racacit_snd_start_end(void);\n' + src[i:]
        changed = True

    # 4. Start skips movies (game/stubs.cpp): the movie-slot update's
    #    "still playing" result and every BinkOpen go through hooks.
    sp = func_span(src, 'func_006511BC')
    if sp and 'racacit_movie_update_hook' not in src[sp[0]:sp[2]]:
        a, body, end = sp
        fn = src[a:end]
        anchor = '        ctx->gpr[28] = ctx->gpr[3] + ctx->gpr[0];\n'
        assert anchor in fn, 'movie slot computation not found in func_006511BC'
        fn = fn.replace('void func_006511BC(ppu_context* ctx) {\n',
                        'void func_006511BC(ppu_context* ctx) {\n        uint32_t _rac_slot = 0;\n', 1)
        fn = fn.replace(anchor, anchor + '        _rac_slot = (uint32_t)ctx->gpr[28];   /* racacit: movie slot */\n', 1)
        tail = fn.rindex('        return;')
        fn = (fn[:tail] + '        ctx->gpr[3] = racacit_movie_update_hook(_rac_slot, (uint32_t)ctx->gpr[3]);\n'
              + fn[tail:])
        src = src[:a] + fn + src[end:]
        if 'uint32_t racacit_movie_update_hook' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + ('extern "C" uint32_t racacit_movie_update_hook(uint32_t slot, uint32_t playing);\n'
                             'extern "C" void racacit_bink_opened(uint32_t bink, uint32_t path_ea);\n') + src[i:]
        changed = True

    sp = func_span(src, 'func_0065282C')
    if sp and 'racacit_bink_opened' not in src[sp[0]:sp[2]]:
        a, body, end = sp
        fn = src[a:end]
        n_calls = 0
        for lr in ('0x0065298C', '0x006529E4'):
            call = f'ctx->lr = {lr}; func_009A8798(ctx); DRAIN_TRAMPOLINE(ctx);'
            assert call in fn, 'BinkOpen call not found in func_0065282C'
            fn = fn.replace(call, '{ uint32_t _rac_path = (uint32_t)ctx->gpr[3]; ' + call +
                            ' racacit_bink_opened((uint32_t)ctx->gpr[3], _rac_path); }', 1)
            n_calls += 1
        src = src[:a] + fn + src[end:]
        if 'void racacit_bink_opened' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + ('extern "C" uint32_t racacit_movie_update_hook(uint32_t slot, uint32_t playing);\n'
                             'extern "C" void racacit_bink_opened(uint32_t bink, uint32_t path_ea);\n') + src[i:]
        changed = True

    # 5. Every cutscene skippable (game/stubs.cpp racacit_force_abort).
    for fname, kind in (('func_0004E7B8', 0), ('func_0004E598', 1), ('func_0004E6F0', 2)):
        sp = func_span(src, fname)
        if sp and 'racacit_force_abort' not in src[sp[0]:sp[2]]:
            a, body, end = sp
            fn = src[a:end]
            head = f'void {fname}(ppu_context* ctx) {{\n'
            fn = fn.replace(head, head + f'        racacit_force_abort((uint32_t)ctx->gpr[5], {kind});   /* racacit */\n', 1)
            src = src[:a] + fn + src[end:]
            if 'void racacit_force_abort' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_force_abort(uint32_t opts, int kind);\n' + src[i:]
            changed = True

    # 6. End-of-frame blit (game/stubs.cpp racacit_gcm_put_moved): the two
    #    ctrl->put writes at the tail of func_005F3E50.
    sp = func_span(src, 'func_005F3E50')
    if sp and 'racacit_gcm_put_moved' not in src[sp[0]:sp[2]]:
        a, body, end = sp
        fn = src[a:end]
        # put = end of the scratch packet; the title then waits for idle itself
        first = ('        vm_write32(ctx->gpr[28] + 0x0, ctx->gpr[7]);\n'
                 '        ctx->lr = 0x005F4324; func_0066117C(ctx); DRAIN_TRAMPOLINE(ctx);\n')
        # put = the other main buffer (the return JUMP's target); no wait follows
        second = ('        vm_write32(ctx->gpr[24] + 0x0, ctx->gpr[25]);\n'
                  '        vm_write32(ctx->gpr[23] + 0x0, ctx->gpr[21]);\n')
        assert fn.count(first) == 1, 'first put write not found in func_005F3E50'
        assert fn.count(second) == 1, 'second put write not found in func_005F3E50'
        l1, l2 = first.split('\n')[0] + '\n', first.split('\n')[1] + '\n'
        fn = fn.replace(first, l1 + '        racacit_gcm_put_moved(0);   /* racacit: wake the FIFO walker */\n' + l2, 1)
        s1, s2 = second.split('\n')[0] + '\n', second.split('\n')[1] + '\n'
        fn = fn.replace(second, s1 + '        racacit_gcm_put_moved(1);   /* racacit: RSX takes the return JUMP now */\n' + s2, 1)
        src = src[:a] + fn + src[end:]
        if 'void racacit_gcm_put_moved' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" void racacit_gcm_put_moved(int wait);\n' + src[i:]
        changed = True

    # 7. Test hook RACACIT_START_LEVEL (game/stubs.cpp racacit_level_change):
    #    the level id argument of the level change func_0029A4F4.
    sp = func_span(src, 'func_0029A4F4')
    if sp and 'racacit_level_change' not in src[sp[0]:sp[2]]:
        a, body, end = sp
        fn = src[a:end]
        head = 'void func_0029A4F4(ppu_context* ctx) {\n'
        fn = fn.replace(head, head + '        ctx->gpr[3] = racacit_level_change((uint32_t)ctx->gpr[3]);   /* racacit */\n', 1)
        src = src[:a] + fn + src[end:]
        if 'uint32_t racacit_level_change' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" uint32_t racacit_level_change(uint32_t level);\n' + src[i:]
        changed = True

    # 8. Diagnostic RACACIT_LUA_CTRACE (game/stubs.cpp racacit_luac_trace): the
    #    entry of the script functions that drive scenes, speech and control.
    for fname, lname in LUA_CTRACE:
        sp = func_span(src, fname)
        if sp and 'racacit_luac_trace' not in src[sp[0]:sp[2]]:
            a, body, end = sp
            fn = src[a:end]
            head = f'void {fname}(ppu_context* ctx) {{\n'
            fn = fn.replace(head, head + f'        racacit_luac_trace(ctx, "{lname}");   /* racacit */\n', 1)
            src = src[:a] + fn + src[end:]
            if 'void racacit_luac_trace' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_luac_trace(ppu_context* ctx, const char* name);\n' + src[i:]
            changed = True

    # 9. Per-object shadow maps (game/stubs.cpp racacit_caster_box): refresh a
    #    caster's stale camera-space box (moby in r17) before func_005EDEE8
    #    fits its shadow map to it, at loc_005EE1E8.
    sp = func_span(src, 'func_005EDEE8')
    if sp and 'racacit_caster_box' not in src[sp[0]:sp[2]]:
        a, body, end = sp
        fn = src[a:end]
        lab = 'loc_005EE1E8:\n'
        assert fn.count(lab) == 1, 'loc_005EE1E8 not found in func_005EDEE8'
        fn = fn.replace(lab, lab + '        racacit_caster_box((uint32_t)ctx->gpr[17]);   /* racacit */\n', 1)
        src = src[:a] + fn + src[end:]
        if 'void racacit_caster_box' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" void racacit_caster_box(uint32_t moby);\n' + src[i:]
        changed = True

    # 10. SPU kick on wait (runtime/spu/spu_coherency.c racacit_spu_kick): the
    #     game posts SPU jobs with plain stores, then waits on them -- the job
    #     handle wait func_00696614 and the batch-counter wait func_00660924
    #     (~9 round trips per view per frame in func_004DFDF0). Waking the
    #     parked SPU lanes as the wait begins gets the job picked up at once
    #     instead of at the end of an idle lane's spu_lr_wait bound.
    #     func_00555EAC is the third wait: a spin on an async object's ready
    #     bit (halfword +0x18 < 0, objects from func_00928F44's 0xA0-byte
    #     pool), 36% of the main thread at Galacton -- timed the same way
    #     (RACACIT_WAIT_STATS; callers 0x0067A220, 0x006869EC, 0x0092A018).
    for fname in ('func_00696614', 'func_00660924', 'func_00555EAC'):
        sp = func_span(src, fname)
        if sp and 'racacit_wait_begin' not in src[sp[0]:sp[2]]:
            a, body, end = sp
            fn = src[a:end]
            head = f'void {fname}(ppu_context* ctx) {{\n'
            assert fn.count(head) == 1, f'{fname} head not found'
            assert fn.count('        return;') == 1, f'{fname}: expected exactly one return'
            fn = fn.replace(head, head + '        racacit_wait_begin(ctx);   /* racacit: wake parked SPUs, time the wait */\n', 1)
            tail = fn.rindex('        return;')
            fn = fn[:tail] + '        racacit_wait_end();\n' + fn[tail:]
            src = src[:a] + fn + src[end:]
            if 'void racacit_wait_begin' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + ('extern "C" void racacit_wait_begin(ppu_context* ctx);\n'
                                 'extern "C" void racacit_wait_end(void);\n') + src[i:]
            changed = True

    if (('racacit_jb_push(' in src or 'racacit_longjmp(' in src)
            and 'racacit: guest setjmp' not in src):
        # declarations after the first include
        i = src.index('\n', src.index('#include')) + 1
        src = src[:i] + DECLS + src[i:]
        changed = True
    if changed:
        open(path, 'w').write(src)
    return changed


n = 0
for p in sorted(glob.glob(os.path.join(RECOMP, 'ppu_recomp_*.cpp'))):
    if patch_file(p):
        print('patched', os.path.basename(p)); n += 1
print(f'{n} file(s) patched')

# 13. v42-v62 hooked the SCREAM synth (nextsynth, EBOOT 0xBFD480: the lifts
#     sub_BFD480_04000 and spu_0001_at_00BED400) to copy the controller's
#     sounds out of its mix. The controller has its own player now
#     (tools/ps3recomp/libs/audio/ds_player.c): an older patched lift has
#     those hook calls removed.
SPU_DIR = os.path.join(ROOT, 'game', 'spu')
for lift in ('sub_BFD480_04000', 'spu_0001_at_00BED400'):
    p = os.path.join(SPU_DIR, lift, 'spu_recomp.c')
    if not os.path.exists(p):
        continue
    src = open(p, encoding='latin-1').read()
    out = [l for l in src.split('\n')
           if not re.search(r'\bracacit_syn_(voice|mix|mix4)_hook\b', l)]
    if len(out) != src.count('\n') + 1:
        open(p, 'w', encoding='latin-1', newline='').write('\n'.join(out))
        print('unpatched', lift)

# 11. Vector instructions whose destination is also a source read that source
#     from a snapshot (scripts/fix_vmx_alias.py has the account: a lifted
#     vcmpequh. kept A Crack in Time's texture streamer from ever switching in
#     high-resolution textures, and VRAM compaction then drew the striped ones).
#     The current ppu_lifter.py does this itself; on its output this is a no-op.
import subprocess
subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'fix_vmx_alias.py'), RECOMP], check=False)
