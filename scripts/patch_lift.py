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
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from game_version import identify_profile


def func_span(src, name):
    m = re.search(r'void %s\(ppu_context\* ctx\) \{\n' % name, src)
    if not m:
        return None
    end = src.index('\n}\n', m.end())
    return m.start(), m.end(), end


# 22. The mouse turns every camera the right stick turns, directly (game/desktop_mouse.cpp;
#     public PRs #34 and #66, petralithic). Each camera's update takes the mouse motion since
#     the last camera update (radians, used once) and adds it to that camera's own yaw and
#     pitch step, after the stick's: a controller keeps working with the mouse captured. The
#     pad runtime no longer turns the mouse into a right-stick push.
#
#     MOUSE_CAMERAS is the recipe per camera, with places given relative to the function's
#     start so one recipe serves every executable; MOUSE_CAMERA_FUNCS says where each camera's
#     update is in each profile (scripts/retarget.py converts the disc names for BCES00511).
#     An edit is ('before', offset, code): before the call returning to start+offset;
#     ('after', offset, code): after it; ('text', line, code): before that line. Signs follow
#     "the mouse is the right stick with the game's invert settings off" (mouse right and down
#     positive). The build stops if any camera of the profile is missing a hook.
MOUSE_CAMERAS = {
    # Follow camera (the default third-person one). f1 is the yaw step into the rotation;
    # f31 the pitch target, kept within the game's own limits ([r30-0x7F24], [r30-0x7F1C]).
    'follow': (('before', 0x398, 'ctx->fpr[1] -= mouse_angles[0];'),
               ('text', 'ctx->fpr[3] = ppu_fp_single(ppu_fsub(ctx->fpr[31], ctx->fpr[1]));',
                '{ uint32_t lb = vm_read32(ctx->gpr[30] - 0x7F24), hb = vm_read32(ctx->gpr[30] - 0x7F1C);\n'
                '          float lo, hi; memcpy(&lo, &lb, 4); memcpy(&hi, &hb, 4);\n'
                '          const double target = ctx->fpr[31] + mouse_angles[1] - ctx->fpr[1];\n'
                '          ctx->fpr[31] = fmin(fmax(target, lo), hi) + ctx->fpr[1]; }')),
    # Two free-look cameras: yaw then pitch step, each into its own rotation.
    'look': (('before', 0x264, 'ctx->fpr[1] -= mouse_angles[0];'),
             ('before', 0x280, 'ctx->fpr[1] += mouse_angles[1];')),
    'look2': (('before', 0x270, 'ctx->fpr[1] -= mouse_angles[0];'),
              ('before', 0x294, 'ctx->fpr[1] += mouse_angles[1];')),
    # An orbit camera: the step returned for the yaw and the pitch angle, which the game then
    # wraps or clamps to its own limits.
    'orbit': (('after', 0x270, 'ctx->fpr[1] -= mouse_angles[0];'),
              ('after', 0x2D4, 'ctx->fpr[1] -= mouse_angles[1];')),

    # A camera that reads the stick with its axes swapped: pitch first, then yaw.
    'free': (('before', 0x3D8, 'ctx->fpr[1] -= mouse_angles[1];'),
             ('before', 0x3F8, 'ctx->fpr[1] += mouse_angles[0];')),
    # Strafe (L2, over the shoulder): yaw and pitch steps, which the game then limits.
    'strafe': (('after', 0x300, 'ctx->fpr[1] -= mouse_angles[0];'),
               ('after', 0x3A4, 'ctx->fpr[1] += mouse_angles[1];')),   # the same way up as strafe's stick and follow's mouse
    # The lock camera (class 0xC239BC): the yaw step, and its tilt. The tilt isn't an angle the
    # player holds: it follows the stick's vertical axis and springs back to level when the
    # stick does. The mouse's pitch goes into that input (f13: the stick's vertical value times
    # its magnitude, the other way up from the raw axis, so the mouse is taken off) right before
    # the game's own gate picks it, so it tilts with the stick centred too: about full deflection
    # for 0.05 rad in one update, kept within the stick's range.
    'lock': (('text', 'ctx->fpr[12] = (ctx->fpr[1] >= 0.0) ? ctx->fpr[13] : ctx->fpr[0];',
              'ctx->fpr[13] = (float)fmin(fmax(ctx->fpr[13] - mouse_angles[1] * 20.0, -1.0), 1.0);'),
             ('before', 0x4C8, 'ctx->fpr[1] -= mouse_angles[0];')),
    # First-person look (hero state): the yaw step, and the pitch before the game clamps it.
    'hero_look': (('before', 0x228, 'ctx->fpr[1] -= mouse_angles[0];'),
                  ('text', 'ctx->fpr[6] = ppu_fp_single(ppu_fadd(ctx->fpr[31], ctx->fpr[8]));',
                   'ctx->fpr[31] += mouse_angles[1];')),
    # Turrets: yaw and pitch steps into the turret's own limited turns.
    'turret': (('before', 0x110, 'ctx->fpr[1] -= mouse_angles[0];'),
               ('before', 0x11C, 'ctx->fpr[1] += mouse_angles[1];')),
    # The camera that hands back to the default one when the stick moves: a mouse move
    # counts as that push. It only looks; the camera it hands to takes the motion.
    'handback': (('after', 0x9C, 'peek'),),
}
_DISC_CAMERAS = {
    'follow': 'func_000459D4', 'look': 'func_0003DD98', 'look2': 'func_00044050',
    'orbit': 'func_00047104', 'strafe': 'func_0004833C', 'free': 'func_00048B38',
    'lock': 'func_00041E34', 'hero_look': 'func_001AFF7C', 'turret': 'func_0037F18C',
    'handback': 'func_00049450',
}
MOUSE_CAMERA_FUNCS = {
    'BCUS98124-1.00': _DISC_CAMERAS,
    'BCES00511-1.00': _DISC_CAMERAS,
    # Update 1.20 (tools/racit_update120_fndiff.py): written as numbers so retarget leaves them.
    'BCUS98124-1.20': {name: 'func_%08X' % a for name, a in (
        ('follow', 0x459d4), ('look', 0x3dd98), ('look2', 0x44050), ('orbit', 0x47104),
        ('strafe', 0x4833c), ('free', 0x48b38), ('lock', 0x41e34), ('hero_look', 0x1aff80),
        ('turret', 0x37f4c4), ('handback', 0x49450))},
}
MOUSE_DECLS = ('extern "C" int racit_camera_turn(const char*, double*, unsigned long long);\n'
               'extern "C" int racit_camera_peek(unsigned long long);\n'
               'extern "C" unsigned long long SDL_GetPerformanceCounter(void);\n'
               'extern "C" unsigned long long SDL_GetPerformanceFrequency(void);\n'
               'static inline unsigned long long racit_mouse_now(void) { return (unsigned long long)'
               '((double)SDL_GetPerformanceCounter() * 1000000000.0 / (double)SDL_GetPerformanceFrequency()); }\n')


# What v1.0.0 put into the follow camera (func_000459D4) before this recipe: patch_lift
# step 22 on the disc, the update 1.20 recipe on 1.20. A lift patched then still has it,
# and the recipe below would declare mouse_angles a second time, so it comes out first.
OLD_FOLLOW_MOUSE = (
    # The disc's step 22.
    '        double mouse_angles[2] = {};   /* racacit */\n'
    '        const int mouse_direct = racit_camera_take(mouse_angles, (uint64_t)((double)SDL_GetPerformanceCounter() * 1000000000.0 / (double)SDL_GetPerformanceFrequency()));\n',
    '        if (mouse_direct) ctx->fpr[1] -= mouse_angles[0];   /* racacit: added to the stick turn (PR #66) */\n',
    '        if (mouse_direct) {   /* racacit */\n'
    '            uint32_t lb = vm_read32(ctx->gpr[30] - 0x7F24), hb = vm_read32(ctx->gpr[30] - 0x7F1C);\n'
    '            float lo, hi; memcpy(&lo, &lb, 4); memcpy(&hi, &hb, 4);\n'
    '            const double target = ctx->fpr[31] + mouse_angles[1] - ctx->fpr[1];\n'
    '            ctx->fpr[31] = fmin(fmax(target, lo), hi) + ctx->fpr[1];\n'
    '        }\n',
    # Update 1.20's recipe (versions/BCUS98124-1.20-hooks.json in v1.0.0).
    '        double mouse_angles[2] = {};\n'
    '        const int mouse_direct = racit_camera_take(mouse_angles,(uint64_t)((double)SDL_GetPerformanceCounter()*1000000000.0/(double)SDL_GetPerformanceFrequency()));\n',
    '        if (mouse_direct) {\n'
    '            uint32_t lb=vm_read32(ctx->gpr[30]-0x7F24), hb=vm_read32(ctx->gpr[30]-0x7F1C);\n'
    '            float lo,hi; memcpy(&lo,&lb,4); memcpy(&hi,&hb,4);\n'
    '            /* Clamp before the original angle wrapping so a fast vertical\n'
    '             * swipe cannot jump across the pole or wrap to the other limit. */\n'
    '            const double target=mouse_angles[1]-ctx->fpr[1];\n'
    '            ctx->fpr[31]=fmin(fmax(target,lo),hi)+ctx->fpr[1];\n'
    '        }\n',
    '        if (mouse_direct) ctx->fpr[1] = -mouse_angles[0];\n',
    # 494cfa6 and 3efe71f (before 1.0.0): what the shipped v1.0.0 USA Windows exes were built from.
    '        double mouse_angles[2] = {};\n'
    '        const int mouse_direct = racit_camera_take(mouse_angles, SDL_GetPerformanceCounter() * (1000000000.0 / SDL_GetPerformanceFrequency()));\n',
    '        if (mouse_direct) {\n'
    '            uint32_t lb=vm_read32(ctx->gpr[30]-0x7F24), hb=vm_read32(ctx->gpr[30]-0x7F1C);\n'
    '            float lo,hi; memcpy(&lo,&lb,4); memcpy(&hi,&hb,4);\n'
    '            const double target=mouse_angles[1]-ctx->fpr[1];\n'
    '            ctx->fpr[31]=fmin(fmax(target,lo),hi)+ctx->fpr[1];\n'
    '        }\n',
)
OLD_MOUSE_DECL = 'extern "C" int racit_camera_take(double*, unsigned long long);\n'
# 494cfa6 also declared SDL's counter beside it.
OLD_SDL_DECL = ('extern "C" unsigned long long SDL_GetPerformanceCounter(void);\n'
                'extern "C" unsigned long long SDL_GetPerformanceFrequency(void);\n')


def strip_old_follow_mouse(fn, func):
    """fn without v1.0.0's follow camera mouse patch (fn itself when it has none)."""
    if 'racit_camera_take' not in fn:
        return fn
    for block in OLD_FOLLOW_MOUSE:
        fn = fn.replace(block, '', 1)
    if 'racit_camera_take' in fn or 'mouse_direct' in fn or 'mouse_angles' in fn:
        sys.exit('patch_lift #22: %s holds a mouse patch this script does not know; '
                 'regenerate the lift (scripts/pipeline.sh)' % func)
    print('patch_lift #22: %s: the v1.0.0 follow camera patch taken out' % func)
    return fn


def mouse_camera_edit(fn, start, name, edit):
    """fn with one edit of camera name applied, or None when its place is not there once."""
    kind, where, code = edit
    if code == 'peek':
        code = 'if (racit_camera_peek(racit_mouse_now())) ctx->fpr[1] = 1.0;   /* racacit mouse: %s */\n' % name
    else:
        code = ('if (!mouse_taken) { mouse_taken = 1; mouse_direct = racit_camera_turn("%s", mouse_angles, racit_mouse_now()); }\n'
                '        if (mouse_direct) %s   /* racacit mouse: %s */\n' % (name, code, name))
    if kind == 'text':
        lines = [m.start() for m in re.finditer(r'(?m)^ *' + re.escape(where) + r'\n', fn)]
    else:
        pat = r'(?m)^ *ctx->lr = 0x%08X; func_[0-9A-F]{8}\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n' % (start + where)
        lines = [m.start() if kind == 'before' else m.end() for m in re.finditer(pat, fn)]
    if len(lines) != 1:
        return None
    return fn[:lines[0]] + '        ' + code + fn[lines[0]:]


MOUSE_HEAD = '        double mouse_angles[2] = {0, 0}; int mouse_taken = 0, mouse_direct = 0;   /* racacit */\n'


def strip_camera_marks(fn, name):
    """fn without this script's own edits for camera name (and its mouse_angles line)."""
    mark = re.escape('/* racacit mouse: %s */' % name)
    fn = re.sub(r'(?ms)^        if \(!mouse_taken\) \{[^\n]*\n        if \(mouse_direct\) .*?' + mark + r'\n', '', fn)
    fn = re.sub(r'(?m)^        if \(racit_camera_peek\([^\n]*' + mark + r'\n', '', fn)
    if 'racacit mouse:' not in fn:
        fn = fn.replace(MOUSE_HEAD, '', 1)
    return fn


def add_camera_edits(fn, func, name):
    """fn with camera name's recipe put in, or None when a place for it is missing."""
    start = int(func[5:], 16)
    for edit in MOUSE_CAMERAS[name]:
        fn = mouse_camera_edit(fn, start, name, edit)
        if fn is None:
            return None
    if any(e[2] != 'peek' for e in MOUSE_CAMERAS[name]) and 'double mouse_angles[2]' not in fn:
        head = 'void %s(ppu_context* ctx) {\n' % func
        fn = fn.replace(head, head + MOUSE_HEAD, 1)
    return fn


def apply_mouse_cameras(recomp, profile_id):
    """Puts every camera's mouse hook into the lift; stops the build if one is missing."""
    funcs = MOUSE_CAMERA_FUNCS.get(profile_id)
    if funcs is None or set(funcs) != set(MOUSE_CAMERAS):
        sys.exit('patch_lift #22: no complete mouse camera table for ' + profile_id)
    done, failed = set(), []
    for path in sorted(glob.glob(os.path.join(recomp, 'ppu_recomp_*.cpp'))):
        with open(path, encoding='utf-8', errors='surrogateescape') as f:   # UTF-8 whatever the locale
            src = f.read()
        changed = False
        for name, func in sorted(funcs.items()):
            sp = func_span(src, func)
            if not sp:
                continue
            fn = src[sp[0]:sp[2]]
            mark = '/* racacit mouse: %s */' % name
            if mark in fn:
                # Patched before: up to date when taking our edits out and putting this recipe's
                # in gives the same function; otherwise an earlier recipe (the lock camera's yaw
                # only, strafe's pitch the other way up) is replaced.
                bare = strip_camera_marks(fn, name)
                if mark in bare:
                    failed.append('%s (%s: an earlier patch this script cannot take out)' % (name, func)); continue
                fresh = add_camera_edits(bare, func, name)
                if fresh == fn:
                    done.add(name); continue
                if fresh is None:
                    failed.append('%s (%s)' % (name, func)); continue
                src = src[:sp[0]] + fresh + src[sp[2]:]
                print('patch_lift #22: %s: the earlier %s camera patch replaced' % (func, name))
                done.add(name); changed = True; continue
            stripped = strip_old_follow_mouse(fn, func)
            if stripped is not fn:
                src = src[:sp[0]] + stripped + src[sp[2]:]
                changed = True
                sp = func_span(src, func)
                fn = src[sp[0]:sp[2]]
            fn = add_camera_edits(fn, func, name)
            if fn is None:
                failed.append('%s (%s)' % (name, func)); continue
            src = src[:sp[0]] + fn + src[sp[2]:]
            done.add(name); changed = True
        if changed:
            src = src.replace(OLD_MOUSE_DECL + OLD_SDL_DECL, '').replace(OLD_MOUSE_DECL, '')
            if 'racit_mouse_now(void)' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + MOUSE_DECLS + src[i:]
            with open(path, 'w', encoding='utf-8', errors='surrogateescape') as f:
                f.write(src)
    missing = sorted(set(MOUSE_CAMERAS) - done)
    if failed or missing:
        sys.exit('patch_lift #22: mouse camera hooks missing for %s: %s'
                 % (profile_id, ', '.join(failed + ['%s (%s not found)' % (m, funcs[m]) for m in missing if not any(f.startswith(m + ' ') for f in failed)])))
    print('patch_lift #22: mouse hooks in all %d cameras (%s)' % (len(done), profile_id))



def check_mouse_camera_tables():
    """Every known profile (versions/profiles.json) has a place for every mouse camera."""
    with open(os.path.join(ROOT, 'versions', 'profiles.json'), encoding='utf-8') as f:
        ids = [p['id'] for p in json.load(f)['profiles']]
    bad = [i for i in ids if set(MOUSE_CAMERA_FUNCS.get(i, ())) != set(MOUSE_CAMERAS)]
    if bad:
        sys.exit('patch_lift #22: mouse camera table incomplete for ' + ', '.join(bad))


# An update of the same title can have a different code and data layout.
# Check the decrypted executable before any generated source is touched.
def check_game_profile(elf, selected='auto', require_buildable=True, quiet=False):
    try:
        actual = identify_profile(elf, selected, require_buildable=require_buildable)
    except ValueError as error:
        sys.exit(str(error) + '. Generation stopped.')
    if not quiet:
        print('Game profile: ' + actual['id'] + ' (SHA-256 ' + actual['elf_sha256'] + ')')
    return actual

if len(sys.argv) > 1 and sys.argv[1] == '--check-profile':
    if len(sys.argv) not in (3, 4):
        sys.exit('usage: patch_lift.py --check-profile EBOOT.ELF [PROFILE_ID|auto]')
    check_game_profile(sys.argv[2], sys.argv[3] if len(sys.argv) == 4 else 'auto')
    sys.exit(0)

if len(sys.argv) > 1 and sys.argv[1] == '--identify-profile':
    if len(sys.argv) not in (3, 4):
        sys.exit('usage: patch_lift.py --identify-profile EBOOT.ELF [PROFILE_ID|auto]')
    profile = check_game_profile(sys.argv[2], sys.argv[3] if len(sys.argv) == 4 else 'auto',
                                 require_buildable=False, quiet=True)
    print(profile['id'])
    sys.exit(0)

# Existing callers may still patch the disc lift by directory alone. The
# pipeline supplies the executable so updates select their own complete recipe.
if len(sys.argv) > 1 and sys.argv[1] == '--apply-update-hooks':
    if len(sys.argv) != 4:
        sys.exit('usage: patch_lift.py --apply-update-hooks RECOMP EBOOT.ELF')
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    from update120_hooks import apply_update120_hooks
    apply_update120_hooks(sys.argv[2], sys.argv[3])
    sys.exit(0)

import argparse
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('recomp', nargs='?', default=os.path.join(ROOT, 'game', 'src', 'recomp'))
parser.add_argument('--elf', help='decrypted executable used for this lift')
parser.add_argument('--profile', default=os.environ.get('RACACIT_GAME_PROFILE', 'auto'))
options = parser.parse_args()
RECOMP = options.recomp
if options.elf:
    profile = check_game_profile(options.elf, options.profile, require_buildable=False)
    if profile['id'] == 'BCUS98124-1.20':
        sys.path.insert(0, os.path.join(ROOT, 'scripts'))
        from update120_hooks import apply_update120_hooks
        apply_update120_hooks(RECOMP, options.elf)
        check_mouse_camera_tables()
        apply_mouse_cameras(RECOMP, profile['id'])
        import subprocess
        subprocess.run([sys.executable, '-X', 'utf8', os.path.join(ROOT, 'scripts', 'fix_vmx_alias.py'), RECOMP],
                       check=True)
        sys.exit(0)
    # A disc 1.00: the USA one here, the European one in scripts/retarget.py's copy.
    if profile['app_version'] != '1.00' or not profile.get('buildable'):
        sys.exit('No PPU hook implementation for ' + profile['id'])
elif options.profile != 'auto':
    parser.error('--profile requires --elf to verify the selected executable')

# The disc this tree builds: scripts/retarget.py marks its copy's profile buildable.
import json
with open(os.path.join(ROOT, 'versions', 'profiles.json'), encoding='utf-8') as _f:
    BUILD_TITLE = next((p['title_id'] for p in json.load(_f)['profiles'] if p.get('buildable')), 'BCUS98124')

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


def patch_file(path):
    # Lifted sources are UTF-8 whatever the locale (Windows defaults to cp1252),
    # and bytes that aren't UTF-8 are written back unchanged.
    with open(path, encoding='utf-8', errors='surrogateescape') as f:
        src = f.read()
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

    # 13. The game's GPU watchdog (func_005C50AC, its wait for a kick's end label): past
    #     2.5 s it prints "GPU sit and spin timeout", then, while the get pointer "is still
    #     moving", a line every 50 ms for 5 s, sleeping, on the main thread. A retail PS3's RSX
    #     never gets there; one 2.9 s hitch here cost a 12 s freeze and the boss music (owner,
    #     0.9.4). 20 s, as a real hang still has to get out: 0x5F2EDC00 ticks at 79.8 MHz.
    sp = func_span(src, 'func_005C50AC')
    if sp and '0xBE4 << 16' in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        fn = fn.replace('(uint32_t)0xBE4 << 16', '(uint32_t)0x5F2E << 16', 1).replace('ctx->gpr[8] | 0x20E0', 'ctx->gpr[8] | 0xDC00', 1)
        src = src[:a_] + fn + src[end_:]
        changed = True

    # 14. SCREAM's stream pause callback (game/stubs.cpp racacit_stream_pause_hook): the
    #     controller's dialogue lines hold with the game's streams.
    sp = func_span(src, 'func_0099B4E0')
    if sp and 'racacit_stream_pause_hook' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        head = 'void func_0099B4E0(ppu_context* ctx) {\n'
        fn = fn.replace(head, head + '        racacit_stream_pause_hook(ctx);   /* racacit */\n', 1)
        src = src[:a_] + fn + src[end_:]
        if 'void racacit_stream_pause_hook' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" void racacit_stream_pause_hook(ppu_context* ctx);\n' + src[i:]
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

    # 15. The game's per-title language list (func_0066C518): a BCUS disc allows only
    #     English, French and Spanish and falls back to English, though it holds the
    #     full German, Italian and Dutch text and speech (RACACIT_LANGUAGE, game/stubs.cpp).
    sp = func_span(src, 'func_0066C518')
    if sp and 'racacit: on the disc' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        read = 'ctx->gpr[10] = vm_read8(ctx->gpr[9] + 0x70);\n'
        fn = fn.replace(read, read + '        if ((uint32_t)ctx->gpr[11] - 4 <= 2) ctx->gpr[10] = 1;   /* racacit: on the disc */\n', 1)
        src = src[:a_] + fn + src[end_:]
        changed = True

    # 16. Ratchet's armor look (game/stubs.cpp racacit_armor_look): func_001709B4 builds
    #     the hero's part mask from the equipped skin, or with none from the armor level
    #     (player record +0x5BC; 1 Holoflux .. 4 Hyperflux). Its one read of the level
    #     goes through the setting; the damage taken stays the real armor's (hero
    #     +0x17F8, set with the armor by func_0016AECC).
    sp = func_span(src, 'func_001709B4')
    if sp and 'racacit_armor_look' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        read = 'ctx->gpr[8] = vm_read32(ctx->gpr[12] + 0x5BC);\n'
        if fn.count(read) == 1:
            fn = fn.replace(read, 'ctx->gpr[8] = racacit_armor_look(vm_read32(ctx->gpr[12] + 0x5BC));   /* racacit */\n', 1)
            src = src[:a_] + fn + src[end_:]
            if 'uint32_t racacit_armor_look' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" uint32_t racacit_armor_look(uint32_t armor);\n' + src[i:]
            changed = True

    # 17. The armor looks in the game's Extras > Skins menu (game/stubs.cpp): the skin
    #     list builder func_004A3DB4 marks the look as equipped (its skinSelected value,
    #     sp+0x80, set after the list's loop at loc_004A4DB4) and appends four entries
    #     after its own six arrays (after the last, skinValues); the EquipSkin command
    #     (func_004A27F8, the value parsed by func_00906828) turns an armor entry into
    #     the look and equips "None".
    sp = func_span(src, 'func_004A3DB4')
    if sp and 'racacit_skin_armor_rows' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        lab = '\nloc_004A4DB4:\n'
        last = '        ctx->lr = 0x004A4EE8; func_0095D30C(ctx); DRAIN_TRAMPOLINE(ctx);\n'
        if fn.count(lab) == 1 and fn.count(last) == 1:
            fn = fn.replace(lab, lab + '        racacit_skin_armor_selected(ctx);   /* racacit */\n', 1)
            fn = fn.replace(last, last + '        racacit_skin_armor_rows(ctx);   /* racacit */\n', 1)
            src = src[:a_] + fn + src[end_:]
            if 'void racacit_skin_armor_rows' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + ('extern "C" void racacit_skin_armor_selected(ppu_context* ctx);\n'
                                 'extern "C" void racacit_skin_armor_rows(ppu_context* ctx);\n') + src[i:]
            changed = True
    sp = func_span(src, 'func_004A27F8')
    if sp and 'racacit_equip_skin' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        parse = '        ctx->lr = 0x004A2B54; func_00906828(ctx); DRAIN_TRAMPOLINE(ctx);\n'
        if fn.count(parse) == 1:
            fn = fn.replace(parse, parse + '        ctx->gpr[3] = racacit_equip_skin((uint32_t)ctx->gpr[3]);   /* racacit */\n', 1)
            src = src[:a_] + fn + src[end_:]
            if 'uint32_t racacit_equip_skin' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" uint32_t racacit_equip_skin(uint32_t skin);\n' + src[i:]
            changed = True

    # 18. Field of view and camera distance (Settings > General > Camera; game/stubs.cpp),
    #     both on the follow camera (Ratchet's gameplay camera), so cutscenes and other
    #     cameras keep the game's own. Its target angle (+0x1D4) where its angle (+0x90)
    #     eases toward it (func_0003E874), and get_follow_cam_fov (func_0022CDC0, after its
    #     call to func_0003E874 at 0x0022CE60) handing the angle back to the scripts. (The
    #     view setups that copy the current camera's +0x90 into a view -- func_00201A34,
    #     func_003309B0, func_004AB328, func_004E5498 -- run once or never in play.)
    #     The follow camera's target distance
    #     (+0x1D0) where its distance (+0x1CC) eases toward it (func_00042558 inline,
    #     before func_00624DBC at 0x000426D4, and func_0003E9F0); and get_follow_cam_dist
    #     (func_0022D058) reading +0x1CC back for the scripts.
    hooks18 = (
        ('func_0003E874', 'racacit_follow_fov',
         '        { uint32_t tmp = vm_read32(ctx->gpr[3] + 0x1D4); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[13] = ftmp; }\n',
         '        ctx->fpr[13] = racacit_follow_fov(ctx->fpr[13]);   /* racacit */\n'),
        ('func_0022CDC0', 'racacit_follow_fov_unscale',
         '        ctx->lr = 0x0022CE60; func_0003E874(ctx); DRAIN_TRAMPOLINE(ctx);\n',
         '        ctx->fpr[1] = racacit_follow_fov_unscale(ctx->fpr[1]);   /* racacit */\n'),
        ('func_00042558', 'racacit_follow_dist',
         '        { uint32_t tmp = vm_read32(ctx->gpr[31] + 0x1D0); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[2] = ftmp; }\n'
         '        { uint32_t tmp = vm_read32(ctx->gpr[31] + 0x1CC); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[1] = ftmp; }\n',
         '        ctx->fpr[2] = racacit_follow_dist(ctx->fpr[2]);   /* racacit */\n'),
        ('func_0003E9F0', 'racacit_follow_dist',
         '        { uint32_t tmp = vm_read32(ctx->gpr[3] + 0x1D0); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[2] = ftmp; }\n',
         '        ctx->fpr[2] = racacit_follow_dist(ctx->fpr[2]);   /* racacit */\n'),
        ('func_0022D058', 'racacit_follow_dist_unscale',
         '        { uint32_t tmp = vm_read32(ctx->gpr[3] + 0x1CC); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[1] = ftmp; }\n',
         '        ctx->fpr[1] = racacit_follow_dist_unscale(ctx->fpr[1]);   /* racacit */\n'),
    )
    for name, hook, anchor, add in hooks18:
        sp = func_span(src, name)
        if not sp or hook + '(' in src[sp[0]:sp[2]]:
            continue
        a_, body_, end_ = sp
        fn = src[a_:end_]
        if fn.count(anchor) != 1:
            print('patch_lift #18: %s: anchor not found once (%d)' % (name, fn.count(anchor)))
            continue
        fn = fn.replace(anchor, anchor + add, 1)
        src = src[:a_] + fn + src[end_:]
        if 'double ' + hook + '(' not in src:
            i = src.index('\n', src.index('#include')) + 1
            decl = 'extern "C" double %s(double d);\n' % hook
            src = src[:i] + decl + src[i:]
        changed = True

    # 19. The game's owner bonuses (game/stubs.cpp racacit_bonus_owned / racacit_bonus_grant).
    #     func_0029AFCC (the "NP DRM" thread) looks for the other games installed on the PS3
    #     (/dev_hdd0/game/<id>/PARAM.SFO) and the pre-order licences, and keeps what it found
    #     as bytes at 0x00E20D98: +0..+3 the pre-order packs, +4 Tools of Destruction, +5
    #     Quest for Booty, +6 "checked". The first hook runs right after it sets +6, before
    #     func_0029B1D8 grants the bonuses to a save; the second at the end of that grant
    #     (r30 still its pool: [r30-0x7FC4] = the player record, [r30-0x7FBC] the table
    #     whose +4 is the Power pack's health bonus).
    sp = func_span(src, 'func_0029AFCC')
    if sp and 'racacit_bonus_owned(' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        anchor = '        vm_write8(ctx->gpr[11] + 0x6, ctx->gpr[0]);\n'
        if fn.count(anchor) != 1:
            print('patch_lift #19: func_0029AFCC: anchor not found once (%d)' % fn.count(anchor))
        else:
            fn = fn.replace(anchor, anchor + '        racacit_bonus_owned((uint32_t)ctx->gpr[11]);   /* racacit */\n', 1)
            src = src[:a_] + fn + src[end_:]
            if 'void racacit_bonus_owned(' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_bonus_owned(uint32_t flags);\n' + src[i:]
            changed = True
    sp = func_span(src, 'func_0029B1D8')
    if sp and 'racacit_bonus_grant(' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        anchor = '        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xA0);\n'
        if fn.count(anchor) != 1:
            print('patch_lift #19: func_0029B1D8: anchor not found once (%d)' % fn.count(anchor))
        else:
            fn = fn.replace(anchor, '        racacit_bonus_grant(vm_read32(ctx->gpr[30] + -0x7FC4), (uint32_t)ctx->gpr[30]);   /* racacit */\n' + anchor, 1)
            src = src[:a_] + fn + src[end_:]
            if 'void racacit_bonus_grant(' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_bonus_grant(uint32_t player, uint32_t pool);\n' + src[i:]
            changed = True

    # 20. The cheat menu's lockout (game/stubs.cpp racacit_skillpoint_blocked): every skill
    #     point goes through func_0021EF30 (its only caller is the scripts' set_skillpoint_done,
    #     func_0028597C); at its entry, while cheats are on or the loaded save was cheated in,
    #     it returns before awarding anything.
    sp = func_span(src, 'func_0021EF30')
    if sp and 'racacit_skillpoint_blocked(' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        head = 'void func_0021EF30(ppu_context* ctx) {\n'
        fn = fn.replace(head, head + '        if (racacit_skillpoint_blocked((uint32_t)ctx->gpr[3])) return;   /* racacit */\n', 1)
        src = src[:a_] + fn + src[end_:]
        if 'int racacit_skillpoint_blocked(' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" int racacit_skillpoint_blocked(uint32_t id);\n' + src[i:]
        changed = True

    # 21. Boot straight to the main menu (game/stubs.cpp racacit_front_end_cards and
    #     racacit_front_end_menu): right after func_004C8898 hands the Scaleform main
    #     menu its startMode (the movie's SetVariable, name from r30 - 0x7F28), the
    #     intro cards' list is emptied; right after it invokes the movie's Init (the
    #     function's last call), PRESS START is skipped.
    sp = func_span(src, 'func_004C8898')
    if sp and 'racacit_front_end_cards' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        # The call's TOC restore holds the disc's own TOC (BCUS98124 0x00D349C8,
        # BCES00511 0x00D34A88), so the anchor matches any value there.
        anchor = re.compile(r'        ctx->ctr = \(uint32_t\)ctx->lr; ps3_indirect_call\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n'
                            r'        ctx->gpr\[2\] = 0x[0-9A-F]+ULL; /\*TOCFIX ld r2,N\(r1\)\*/\n'
                            r'(?=        ctx->gpr\[6\] = vm_read32\(ctx->gpr\[30\] \+ -0x7F24\);\n)')
        name_load = '        ctx->gpr[4] = vm_read32(ctx->gpr[30] + -0x7F28);\n'
        hits = list(anchor.finditer(fn))
        if len(hits) == 1 and name_load in fn[:hits[0].start()]:
            add = '        racacit_front_end_cards(ctx);   /* racacit */\n'
            cut = hits[0].end()
            fn = fn[:cut] + add + fn[cut:]
            src = src[:a_] + fn + src[end_:]
            if 'void racacit_front_end_cards' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_front_end_cards(ppu_context* ctx);\n' + src[i:]
            changed = True
        else:
            print('patch_lift #21: func_004C8898: startMode SetVariable not found once (%d)' % len(hits))
    sp = func_span(src, 'func_004C8898')
    if sp and 'racacit_front_end_menu' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        # The Invoke of "Init" is the last call before the epilogue.
        init = re.compile(r'        ctx->lr = 0x[0-9A-F]+; func_[0-9A-F]{8}\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n'
                          r'        /\* nop \*/;\n'
                          r'(?=loc_[0-9A-F]{8}:\n        ctx->gpr\[3\] = vm_read64\(ctx->gpr\[1\] \+ 0x140\);\n)')
        hits = list(init.finditer(fn))
        if len(hits) == 1:
            cut = hits[0].end()
            fn = fn[:cut] + '        racacit_front_end_menu(ctx);   /* racacit */\n' + fn[cut:]
            src = src[:a_] + fn + src[end_:]
            if 'void racacit_front_end_menu' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_front_end_menu(ppu_context* ctx);\n' + src[i:]
            changed = True
        else:
            print('patch_lift #21: func_004C8898: Init call not found once (%d)' % len(hits))

    # 23. Keyboard keycaps in the menus (game/menu_keycaps.h, game/menu_archive.cpp; public
    #     PR #46, petralithic): the menu movies have two frame paths in BCUS98124 disc 1.00,
    #     func_004C6F6C and func_004ADB3C. Before each frame the bound key labels and the
    #     keyboard/controller mode go to the movie through its own string setter, outside
    #     ActionScript, the guest context kept. Does nothing unless RACACIT_MENU_ARCHIVE names
    #     an archive made by scripts/prepare_menu_keycaps.py.
    for entry, offsets in (('func_004C6F6C', (4,)),
                           ('func_004ADB3C', (8, 4))):
        sp = func_span(src, entry)
        if sp and 'racit_publish_menu_keyboard(' not in src[sp[0]:sp[2]]:
            a_, body_, end_ = sp
            fn = src[a_:end_]
            marker = 'void %s(ppu_context* ctx) {\n' % entry
            if fn.count(marker) != 1:
                print('patch_lift #23: %s: function start not found once' % entry)
                continue
            calls = ''.join('        racit_publish_menu_keyboard(ctx, %d);   /* racacit */\n' % offset
                            for offset in offsets)
            fn = fn.replace(marker, marker + calls, 1)
            src = src[:a_] + fn + src[end_:]
            if '#include "menu_keycaps.h"' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + '#include "menu_keycaps.h"\n' + src[i:]
            changed = True

    # 24. Update 1.20's hero part masks in the disc build (game/stubs.cpp
    #     racacit_u120_part_masks). func_004C3C44 unpacks the hero's appearance
    #     (armor style, skin, group and its three row choices) and func_004C46FC
    #     turns it into part masks. Update 1.20 changed three things here:
    #     func_004C3C44's default for an unset row 3 of group 1 (0 -> 3, as
    #     groups 0 and 2 already had), func_004C46FC's bound on row 3 (3 -> 4)
    #     and the masks it builds (the stubs.cpp hook, called where its mask
    #     paths meet). Every anchor is the disc's own code, so another
    #     executable's lift is left alone. Public PR #43 (petralithic); on the European
    #     disc retarget.py converts the anchors and REQUIRED checks that the hook landed.
    #     USA disc only for now: BCES00511 compiles func_004C46FC differently (other
    #     registers and lengths), so its join, its row bound and the hook's register reads
    #     are unchecked there.
    sp = func_span(src, 'func_004C3C44') if BUILD_TITLE == 'BCUS98124' else None
    if sp and 'racacit: 1.20 default' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        li = '        ctx->gpr[5] = (int64_t)(int32_t)(0);\n'
        st = '        vm_write32(ctx->gpr[1] + 0x78, ctx->gpr[5]);\n'
        if fn.count(li) == 1 and fn.count(st) == 1 and fn.count('vm_write32(ctx->gpr[1] + 0x78, ') == 3 \
                and fn.index(li) < fn.index(st) \
                and 'ctx->gpr[5] =' not in fn[fn.index(li) + len(li):fn.index(st)]:
            fn = fn.replace(li, '        ctx->gpr[5] = (int64_t)(int32_t)(3);   /* racacit: 1.20 default (was 0) */\n', 1)
            src = src[:a_] + fn + src[end_:]
            changed = True
        else:
            sys.exit('patch_lift #24: func_004C3C44: group 1 row 3 default not found once. The function is in this lift but not the code update 1.20\'s port expects (another disc?): stopping rather than building without the fix.')
    sp = func_span(src, 'func_004C46FC') if BUILD_TITLE == 'BCUS98124' else None
    if sp and 'racacit_u120_part_masks' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        bound = ('        ctx->gpr[28] = vm_read32(ctx->gpr[9] + 0x0);\n'
                 '        { uint64_t a = (uint32_t)ctx->gpr[28]; uint64_t b = (uint64_t)0x3;')
        join = '\nloc_004C4C68:\n'
        if fn.count(bound) == 1 and fn.count(join) == 1 and fn.index(bound) < fn.index(join):
            fn = fn.replace(bound, bound.replace('(uint64_t)0x3;', '(uint64_t)0x4; /* racacit: 1.20 bound (was 3) */'), 1)
            fn = fn.replace(join, join + '        racacit_u120_part_masks(ctx);   /* racacit */\n', 1)
            src = src[:a_] + fn + src[end_:]
            if 'void racacit_u120_part_masks' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_u120_part_masks(ppu_context* ctx);\n' + src[i:]
            changed = True
        else:
            sys.exit('patch_lift #24: func_004C46FC: row bound or mask join not found once. The function is in this lift but not the code update 1.20\'s port expects (another disc?): stopping rather than building without the fix.')

    # 25. Update 1.20's level heap budget in the disc build (game/stubs.cpp
    #     racacit_u120_level_budget). func_0032C7A4, the level loader, extends the
    #     level pool so a level gets at least 0x740000 past its image; update 1.20
    #     (func_0032C800) raised that to 0x820000, and to 0x8A0000 on the five
    #     space sectors. The two loads of the constant are the anchor, so another
    #     executable's lift is left alone.
    #     Public PR #53 (petralithic). USA disc only for now, like #24: the TOC offsets
    #     and (for #26) the function addresses are unchecked on BCES00511.
    sp = func_span(src, 'func_0032C7A4') if BUILD_TITLE == 'BCUS98124' else None
    if sp and 'racacit_u120_level_budget' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        two = ('        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)0x74 << 16);\n'
               '        ctx->gpr[19] = (int64_t)(int32_t)((uint32_t)0x74 << 16);\n')
        if fn.count(two) == 1 and fn.count('0x74 << 16') == 2:
            fn = fn.replace(two,
                '        { const uint32_t b_ = racacit_u120_level_budget(ctx);   /* racacit: 1.20 level budget (was 0x740000) */\n'
                '          ctx->gpr[0] = (int64_t)(int32_t)b_; ctx->gpr[19] = (int64_t)(int32_t)b_; }\n', 1)
            src = src[:a_] + fn + src[end_:]
            if 'uint32_t racacit_u120_level_budget' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" uint32_t racacit_u120_level_budget(ppu_context* ctx);\n' + src[i:]
            changed = True
        else:
            sys.exit('patch_lift #25: func_0032C7A4: level budget loads not found once. The function is in this lift but not the code update 1.20\'s port expects (another disc?): stopping rather than building without the fix.')

    # 26. Update 1.20's space-mission bits resync in the disc build (game/stubs.cpp
    #     racacit_u120_space_bits): update 1.20's func_0047B7E8, an init method,
    #     re-derives two of the five mission bits at G+0x13D94 from saved mission
    #     flags; the disc only sets them from a level script, so a save that missed
    #     one could never complete the mask (entry 0x1E). Called at the disc
    #     function's only return, anchored on its own epilogue.
    #     Public PR #54 (petralithic). USA disc only for now, like #24: the TOC offsets
    #     and (for #26) the function addresses are unchecked on BCES00511.
    sp = func_span(src, 'func_0047B7E8') if BUILD_TITLE == 'BCUS98124' else None
    if sp and 'racacit_u120_space_bits' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[a_:end_]
        tail = ('        ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x90);\n'
                '        return;')
        if fn.count(tail) == 1 and fn.count('return;') == 1 and 'vm_write8(ctx->gpr[29] + 0x1B8, ' in fn:
            fn = fn.replace(tail, '        ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x90);\n'
                                  '        racacit_u120_space_bits(ctx);   /* racacit: 1.20 mission bits resync */\n'
                                  '        return;', 1)
            src = src[:a_] + fn + src[end_:]
            if 'void racacit_u120_space_bits' not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + 'extern "C" void racacit_u120_space_bits(ppu_context* ctx);\n' + src[i:]
            changed = True
        else:
            sys.exit('patch_lift #26: func_0047B7E8: epilogue not found once. The function is in this lift but not the code update 1.20\'s port expects (another disc?): stopping rather than building without the fix.')

    # 27. Frame-rate unlock, the game clock (game/stubs.cpp racacit_clock_tick;
    #     docs in its comment): at the head of func_0066A424 (Clock_Tick, r3 =
    #     the clock), which every level load leaves with a 1/60 s minimum step.
    #     Does nothing unless RACACIT_FPS_CAP is above 60. Public PR #57 (petralithic); USA disc only.
    sp = func_span(src, 'func_0066A424') if BUILD_TITLE == 'BCUS98124' else None
    if sp and 'racacit_clock_tick' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        if 'ppu_timebase_now()' not in src[body_:end_]:
            sys.exit('patch_lift #27: func_0066A424 does not read the timebase: not the game clock on this disc. '
                     'Stopping rather than hooking the wrong function.')
        src = src[:body_] + '        racacit_clock_tick(ctx);   /* racacit: frame-rate unlock */\n' + src[body_:]
        if 'void racacit_clock_tick' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" void racacit_clock_tick(ppu_context* ctx);\n' + src[i:]
        changed = True

    # 28. Frame-rate unlock: code that counts frames as time (game/stubs.cpp
    #     racacit_frame60_at / racacit_new60; docs: the stubs comment). In the
    #     AI/behaviour family below every clock+0xD0 load reads a counter at
    #     60 per second of unpaused time, and the "every N frames" gates fire
    #     at most once per 1/60 s. Both are the original behaviour unless
    #     RACACIT_FPS_CAP is above 60. Counts are the disc's; another lift
    #     whose functions differ stops the build. Public PR #59 (petralithic); USA disc only.
    F60_LOADS = {'func_000106FC': 1, 'func_000119A8': 1, 'func_00012F40': 2, 'func_00016B6C': 2,
                 'func_00016F14': 1, 'func_00018E54': 1, 'func_000190C0': 1, 'func_00019160': 3,
                 'func_0001BFD8': 15, 'func_0001F694': 1, 'func_0001FD68': 1, 'func_0001FE0C': 1,
                 'func_0001FF64': 2, 'func_00020DCC': 2, 'func_00022798': 1, 'func_00023DE4': 2}
    G60_GATES = {'func_00023DE4': 2, 'func_00022798': 1, 'func_00012F40': 2, 'func_00019160': 2,
                 'func_00018E54': 1, 'func_00016B6C': 2, 'func_00016F14': 1, 'func_0001BFD8': 1,
                 'func_000BC018': 1, 'func_00148DA4': 1, 'func_0010DDB8': 1, 'func_00169EDC': 1,
                 'func_002B7828': 1, 'func_002B9B88': 1, 'func_002BF4F0': 1, 'func_00384858': 1,
                 'func_00385E78': 1, 'func_004735E8': 1, 'func_0046DB90': 1}
    f60_load = re.compile(r'vm_read32\(ctx->gpr\[(\d+)\] \+ 0xD0\)')
    f60_any = re.compile(r'(?:vm_read32\(ctx->gpr\[\d+\] \+ 0xD0\)|racacit_frame60_at\(\(uint32_t\)ctx->gpr\[\d+\]\))')
    f60_br = re.compile(r'^\s*if \(\(!\(\(ctx->cr >> \d+\) & 2\)\)\) goto loc_[0-9A-F]+;$')
    for fname in (sorted(set(F60_LOADS) | set(G60_GATES)) if BUILD_TITLE == 'BCUS98124' else ()):
        sp = func_span(src, fname)
        if not sp or 'racacit_new60' in src[sp[0]:sp[2]] or 'racacit_frame60_at' in src[sp[0]:sp[2]]:
            continue
        a_, body_, end_ = sp
        lines = src[a_:end_].split('\n')
        gates = 0
        if fname in G60_GATES:
            for k, ln in enumerate(lines):
                if not f60_any.search(ln):
                    continue
                for j in range(k + 1, min(k + 9, len(lines))):
                    if not f60_br.match(lines[j]):
                        continue
                    between = '\n'.join(lines[k:j])
                    if '/_b' in between or re.search(r'& 0x[0-9A-F]+\b|ppc_rlwinm', between):
                        lines[j] = lines[j].replace(') goto ', ' || !racacit_new60()) goto ', 1)
                        lines[k] = f60_load.sub(lambda m: 'racacit_frame60_at((uint32_t)ctx->gpr[%s])' % m.group(1), lines[k])
                        gates += 1
                    break
            if gates != G60_GATES[fname]:
                sys.exit('patch_lift #28: %s: %d frame gates, expected %d -- not the disc\'s function' % (fname, gates, G60_GATES[fname]))
        fn = '\n'.join(lines)
        if fname in F60_LOADS:
            n = len(f60_load.findall(fn)) + fn.count('racacit_frame60_at(')
            if n != F60_LOADS[fname]:
                sys.exit('patch_lift #28: %s: %d frame-counter loads, expected %d -- not the disc\'s function' % (fname, n, F60_LOADS[fname]))
            fn = f60_load.sub(lambda m: 'racacit_frame60_at((uint32_t)ctx->gpr[%s])' % m.group(1), fn)
        src = src[:a_] + fn + src[end_:]
        for decl in ('extern "C" uint32_t racacit_frame60_at(uint32_t base);', 'extern "C" int racacit_new60(void);'):
            if decl not in src:
                i = src.index('\n', src.index('#include')) + 1
                src = src[:i] + decl + '\n' + src[i:]
        changed = True

    # 29. Frame-rate unlock, the CPU frame limiter (game/stubs.cpp
    #     racacit_frame_limit): at the head of func_005C4C00, which waits for a
    #     1/59.94 s deadline in the states that use it. Above a 60 FPS cap the
    #     hook runs the same deadline logic at 1/cap and returns; otherwise it
    #     returns 0 and the original runs.
    sp = func_span(src, 'func_005C4C00') if BUILD_TITLE == 'BCUS98124' else None
    if sp and 'racacit_frame_limit' not in src[sp[0]:sp[2]]:
        a_, body_, end_ = sp
        fn = src[body_:end_]
        if 'ctx->gpr[2] + -0x5120' not in fn or 'ctx->gpr[30] + -0x7FF8' not in fn or '+ 0x2D8)' not in fn:
            sys.exit('patch_lift #29: func_005C4C00 does not read the display deadline: not the frame limiter '
                     'on this disc. Stopping rather than hooking the wrong function.')
        src = src[:body_] + '        if (racacit_frame_limit(ctx)) return;   /* racacit: frame-rate unlock */\n' + src[body_:]
        if 'int racacit_frame_limit' not in src:
            i = src.index('\n', src.index('#include')) + 1
            src = src[:i] + 'extern "C" int racacit_frame_limit(ppu_context* ctx);\n' + src[i:]
        changed = True

    if (('racacit_jb_push(' in src or 'racacit_longjmp(' in src)
            and 'racacit: guest setjmp' not in src):
        # declarations after the first include
        i = src.index('\n', src.index('#include')) + 1
        src = src[:i] + DECLS + src[i:]
        changed = True
    if changed:
        with open(path, 'w', encoding='utf-8', errors='surrogateescape') as f:
            f.write(src)
    return changed


n = 0
for p in sorted(glob.glob(os.path.join(RECOMP, 'ppu_recomp_*.cpp'))):
    if patch_file(p):
        print('patched', os.path.basename(p)); n += 1
print(f'{n} file(s) patched')

# 22. The mouse cameras (MOUSE_CAMERAS above), checked for every profile.
check_mouse_camera_tables()
apply_mouse_cameras(RECOMP, profile['id'] if options.elf else BUILD_TITLE + '-1.00')

# 13. v42-v62 hooked the SCREAM synth (nextsynth, EBOOT 0xBFD480: the lifts
#     sub_BFD480_04000 and spu_0001_at_00BED400) to copy the controller's
#     sounds out of its mix. The controller has its own player now
#     (tools/ps3recomp/libs/audio/ds_player.c): an older patched lift has
#     those hook calls removed.
SPU_DIR = os.environ.get('RACACIT_SPU_DIR', os.path.join(ROOT, 'game', 'spu'))
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
subprocess.run([sys.executable, '-X', 'utf8', os.path.join(ROOT, 'scripts', 'fix_vmx_alias.py'), RECOMP], check=False)
