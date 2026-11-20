"""Headless smoke test for macOS: start the packaged game the way a player does, and check it draws.

Starts a release's builds/vNN/RaCACiT with the environment the launcher gives it (game folder,
HDD, settings file, caches, the bundled Vulkan loader and MoltenVK) and nothing else of ours:
no PS3RECOMP_VK_GUEST_PROGRAMS, PS3_TITLE or other RACACIT_/PS3/RSX_ variables from the
calling shell, so a default the game forgets to set shows up here (#72: without guest
programs it took the vtable draw path, uploaded no textures and showed a black window with
sound). The only additions are the switches that keep a test quiet: offscreen
(PS3RECOMP_VK_WINDOW=0, SDL's dummy video driver, a background app) and no audio. Runs the
game for --seconds, then checks the log for the register-file draw path and for texture
uploads (an [rsx-stats] line with texups > 0). Needs a decrypted disc and EBOOT; exits 0 when
both checks pass.

    python3 tests/mac_user_launch.py --build RELEASE/builds/v101 --elf EBOOT.ELF --game DISC --work DIR
    (1.20: add --update UPDATE_DIR, the update's PARAM.SFO and USRDIR)
"""
import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time


MARKER = '.mac_user_launch-work'   # what this script made, so it may clear it next time


def inside(path, folder):
    path, folder = os.path.realpath(path), os.path.realpath(folder)
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


def check_inputs(o, build, work):
    """Everything given exists and is what it should be, before anything is touched; and the
    work folder (cleared each run) holds none of it."""
    need = [(os.path.join(build, 'RaCACiT'), os.path.isfile, '--build has no RaCACiT program'),
            (o.elf, os.path.isfile, '--elf is not a file'),
            (os.path.join(o.game, 'PS3_GAME'), os.path.isdir, '--game has no PS3_GAME folder')]
    if o.update:
        need += [(os.path.join(o.update, 'PARAM.SFO'), os.path.isfile, '--update has no PARAM.SFO'),
                 (os.path.join(o.update, 'USRDIR'), os.path.isdir, '--update has no USRDIR')]
    if o.installed:
        need.append((o.installed, os.path.isdir, '--installed is not a folder'))
    for path, ok, why in need:
        if not ok(path):
            sys.exit('%s: %s' % (why, path))
    for name, path in (('--build', build), ('--elf', o.elf), ('--game', o.game), ('--update', o.update),
                       ('--installed', o.installed), ('the current folder', os.getcwd())):
        if path and inside(path, work):
            sys.exit('--work %s holds %s (%s); it is cleared on every run, so give it a folder of its own' % (work, name, path))


def clear_work(work):
    """Clears --work only if it is empty or this script made it (its marker file is there)."""
    if os.path.lexists(work):
        if not os.path.isdir(work) or os.path.islink(work):
            sys.exit('--work %s is not a folder' % work)
        if os.listdir(work) and not os.path.isfile(os.path.join(work, MARKER)):
            sys.exit('--work %s is not empty and was not made by this script; give it an empty or new folder' % work)
        shutil.rmtree(work)
    os.makedirs(work)
    open(os.path.join(work, MARKER), 'w').close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', required=True, help="a release's builds/vNN folder (RaCACiT and lib/)")
    ap.add_argument('--elf', required=True); ap.add_argument('--game', required=True)
    ap.add_argument('--work', required=True); ap.add_argument('--update')
    ap.add_argument('--installed', help="the game's HDD install (hdd0/game/BCUS98124 of a profile that has played)")
    ap.add_argument('--seconds', type=float, default=150)
    o = ap.parse_args()
    build, work = os.path.abspath(o.build), os.path.abspath(o.work)
    check_inputs(o, build, work)
    if subprocess.run(['/usr/bin/pgrep', '-x', 'RaCACiT'], capture_output=True).returncode == 0:
        sys.exit('a game is already running; not starting another')
    clear_work(work)
    hdd0 = os.path.join(work, 'hdd0'); os.makedirs(os.path.join(hdd0, 'game'))
    if o.installed:
        subprocess.run(['/bin/cp', '-c', '-R', o.installed, os.path.join(hdd0, 'game', 'BCUS98124')], check=True)
    vfs = os.path.abspath(o.game)
    if o.update:   # the disc with the update's PARAM.SFO, its USRDIR installed in hdd0 (as the game installs it)
        vfs = os.path.join(work, 'vfs'); os.makedirs(os.path.join(vfs, 'PS3_GAME'))
        for e in os.listdir(o.game):
            if e != 'PS3_GAME':
                os.symlink(os.path.join(os.path.abspath(o.game), e), os.path.join(vfs, e))
        for e in os.listdir(os.path.join(o.game, 'PS3_GAME')):
            if e != 'PARAM.SFO':
                os.symlink(os.path.join(os.path.abspath(o.game), 'PS3_GAME', e), os.path.join(vfs, 'PS3_GAME', e))
        shutil.copy(os.path.join(o.update, 'PARAM.SFO'), os.path.join(vfs, 'PS3_GAME'))
        install = os.path.join(hdd0, 'game', 'BCUS98124'); os.makedirs(install, exist_ok=True)
        shutil.copy(os.path.join(o.update, 'PARAM.SFO'), install)
        subprocess.run(['/bin/cp', '-c', '-R', os.path.join(o.update, 'USRDIR'), install], check=True)
    settings = os.path.join(work, 'settings.txt'); open(settings, 'w').close()
    log = os.path.join(work, 'game.log')
    # The calling shell's environment minus anything the game or its libraries read.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('RACACIT_', 'RACIT_', 'PS3', 'PAD_', 'RSX_', 'SPU_', 'PPU_', 'GCM_', 'SDL_', 'MVK_', 'VK_', 'DYLD_'))}
    env.update({   # what Store::launch gives it on macOS
        'PS3_VFS_ROOT': vfs, 'PS3_HDD0_ROOT': hdd0, 'RACACIT_SETTINGS_FILE': settings,
        'RSX_SHADER_CACHE': os.path.join(work, 'shadercache.bin'),
        'RSX_PIPELINE_CACHE': os.path.join(work, 'pipelinecache.bin'),
        'RSX_PIPELINE_LIST': os.path.join(work, 'pipelinelist.bin'),
        'RACACIT_LAUNCHER': '1',
        'PS3RECOMP_VULKAN_LIBRARY': os.path.join(build, 'lib', 'libvulkan.1.dylib'),
        'VK_DRIVER_FILES': os.path.join(build, 'lib', 'MoltenVK_icd.json'),
    })
    env.update({   # test switches only: offscreen, no audio
        'PS3RECOMP_VK_WINDOW': '0', 'SDL_VIDEODRIVER': 'dummy', 'SDL_MAC_BACKGROUND_APP': '1', 'SDL_AUDIODRIVER': 'dummy',
    })
    p = subprocess.Popen([os.path.join(build, 'RaCACiT'), os.path.abspath(o.elf)], cwd=build, env=env,
                         stdin=subprocess.DEVNULL, stdout=open(log, 'wb'), stderr=subprocess.STDOUT, start_new_session=True)
    end = time.time() + o.seconds
    while time.time() < end and p.poll() is None:
        time.sleep(2)
    exited = p.poll()
    if exited is None:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL); p.wait()
    text = open(log, encoding='utf-8', errors='replace').read()
    path = 'register-file draw engine path' in text
    tex = max([int(t) for t in re.findall(r'\[rsx-stats\].*?texups=(\d+)', text)] or [-1])
    print('register-file draw path: %s' % ('yes' if path else 'NO'))
    print('texture uploads (max texups): %s' % (tex if tex >= 0 else 'no [rsx-stats] line'))
    if exited is not None:
        print('game stopped by itself after %.0f s: %s' % (o.seconds - (end - time.time()),
              'signal %d' % -exited if exited < 0 else 'exit %d' % exited))
    print('log: ' + log)
    # A game that stops by itself (an error, a crash) during the run fails, whatever it drew.
    ok = path and tex > 0 and exited is None
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
