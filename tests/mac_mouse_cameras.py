#!/usr/bin/env python3
"""Headless mouse-camera regression for macOS: a scripted mouse turn must turn each camera.

Runs a built game offscreen (no window, no audio), loads a save into gameplay and, for each
camera mode it can reach from there, checks that:
  - a scripted mouse move reaches that camera's hook (RACACIT_MOUSE_TRACE "[mouse] <camera>
    turn" lines, scripts/patch_lift.py MOUSE_CAMERAS),
  - the picture turns: the frame shifts sideways by much more than it does standing still, and
    the same way the right stick turns it (the stick still works alongside the mouse),
and that a mouse move in the pause menu turns nothing, then or after the game resumes.

  tests/mac_mouse_cameras.py --player game/build/RaCACiTRecomp --elf input/EBOOT.ELF \\
      --game /path/to/disc --installed /path/to/hdd0/game/BCUS98124 \\
      --save /path/to/BCUS98124_SAVE_x --work /tmp/mouse-test
  (update 1.20: add --update /path/to/update, the folder with its EBOOT.ELF, PARAM.SFO and
  USRDIR; --elf is then that EBOOT.ELF)

--game is the disc folder holding PS3_GAME, --installed the game's HDD install from a profile
that has played (hdd0/game/BCUS98124), cloned so it is never written. --save is a save folder
(GAME.SAV, PARAM.SFO, ...) whose LOAD entry is gameplay with the default camera, copied too. Exits 0 when
every mode passes; a mode the save cannot reach is reported, not passed. Never run it while
someone is playing: it refuses when a game is already running.
"""
import argparse, os, re, shutil, signal, subprocess, sys, time

START, CROSS = 0x0008, 0x4000
# (mode, key held through it with the test hook, cameras whose hook may answer in that mode).
# Mouse3 (the right button) is strafe, L2, in the default keyboard and mouse controls.
MODES = (('follow', None, ('follow',)), ('strafe', 'Mouse3', ('strafe',)))


def ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    head = data.split(b'\n', 3)
    w, h = map(int, head[1].split())
    return w, h, head[3]


def profile(img, rows=True):
    """Mean brightness of each column over the band above the hero (from a tenth to a third
    of the way down): the scenery the camera turned past, never Ratchet himself, who stays
    put on screen (and fills the middle over the shoulder)."""
    w, h, px = img
    cols = []
    for x in range(0, w, 2):
        s = 0
        for y in range(h // 10, h // 3, 2):
            i = (y * w + x) * 3
            s += px[i] + px[i + 1] + px[i + 2]
        cols.append(s)
    m = sum(cols) / len(cols)
    return [c - m for c in cols]


def shift(a, b, most=150):
    """How far (in columns of 2 pixels) picture b is shifted sideways from a, and how well it
    matches there compared with not shifted at all."""
    pa, pb = profile(a), profile(b)
    n = len(pa)
    def cost(s):
        lo, hi = max(0, s), min(n, n + s)
        return sum(abs(pa[i] - pb[i - s]) for i in range(lo, hi)) / max(1, hi - lo)
    best = min(range(-most, most + 1), key=cost)
    return best, cost(best), cost(0)


def vprofile(img):
    """Mean brightness of each row over the left and right quarters of the picture (Ratchet
    stays in the middle): what a pitch moves up or down."""
    w, h, px = img
    rows = []
    for y in range(0, h, 2):
        s = 0
        for x in list(range(0, w // 4, 2)) + list(range(3 * w // 4, w, 2)):
            i = (y * w + x) * 3
            s += px[i] + px[i + 1] + px[i + 2]
        rows.append(s)
    m = sum(rows) / len(rows)
    return [r - m for r in rows]


def vshift(a, b, most=80):
    """As shift(), up and down: rows (of 2 pixels) picture b moved from a, and the match there vs not moved."""
    pa, pb = vprofile(a), vprofile(b)
    n = len(pa)
    def cost(s):
        lo, hi = max(0, s), min(n, n + s)
        return sum(abs(pa[i] - pb[i - s]) for i in range(lo, hi)) / max(1, hi - lo)
    best = min(range(-most, most + 1), key=cost)
    return best, cost(best), cost(0)


MARKER = '.mac_mouse_cameras-work'   # what this script made, so it may clear it next time


def inside(path, folder):
    path, folder = os.path.realpath(path), os.path.realpath(folder)
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


def check_inputs(o):
    """Everything given exists, before anything is touched, and --work (cleared on every run)
    holds none of it; --work is only cleared when it is empty or this script made it."""
    work = os.path.abspath(o.work)
    need = [(o.player, os.path.isfile, '--player'), (o.elf, os.path.isfile, '--elf'),
            (os.path.join(o.game, 'PS3_GAME'), os.path.isdir, '--game (its PS3_GAME)'), (o.save, os.path.isdir, '--save')]
    if o.update:
        need += [(os.path.join(o.update, 'PARAM.SFO'), os.path.isfile, '--update (its PARAM.SFO)'),
                 (os.path.join(o.update, 'USRDIR'), os.path.isdir, '--update (its USRDIR)')]
    for opt, test in ((o.installed, os.path.isdir), (o.settings, os.path.isfile), (o.vk_icd, os.path.isfile)):
        if opt:
            need.append((opt, test, opt))
    for path, ok, what in need:
        if not ok(path):
            sys.exit('%s is missing or not what it should be: %s' % (what, path))
    for name, path in (('--player', o.player), ('--elf', o.elf), ('--game', o.game), ('--save', o.save),
                       ('--update', o.update), ('--installed', o.installed), ('--settings', o.settings),
                       ('--vk-icd', o.vk_icd), ('the current folder', os.getcwd())):
        if path and inside(path, work):
            sys.exit('--work %s holds %s (%s); it is cleared on every run, so give it a folder of its own' % (work, name, path))
    if os.path.lexists(work):
        if not os.path.isdir(work) or os.path.islink(work):
            sys.exit('--work %s is not a folder' % work)
        if os.listdir(work) and not os.path.isfile(os.path.join(work, MARKER)):
            sys.exit('--work %s is not empty and was not made by this script; give it an empty or new folder' % work)


class Game:
    def __init__(self, o):
        self.o, self.work = o, os.path.abspath(o.work)
        if os.path.exists(self.work):
            shutil.rmtree(self.work)
        os.makedirs(self.work)
        open(os.path.join(self.work, MARKER), 'w').close()
        hdd0 = os.path.join(self.work, 'hdd0')
        save = os.path.join(hdd0, 'home', '00000001', 'savedata', os.path.basename(os.path.normpath(o.save)))
        shutil.copytree(o.save, save)
        os.makedirs(os.path.join(hdd0, 'game'))
        if o.installed:   # the game's own HDD install (made on first boot), cloned
            subprocess.run(['/bin/cp', '-c', '-R', o.installed, os.path.join(hdd0, 'game', 'BCUS98124')], check=True)
        vfs = o.game
        if o.update:   # the disc's files with the update's PARAM.SFO; its USRDIR installed in hdd0
            vfs = os.path.join(self.work, 'vfs')
            os.makedirs(os.path.join(vfs, 'PS3_GAME'))
            for e in os.listdir(o.game):
                if e != 'PS3_GAME':
                    os.symlink(os.path.join(o.game, e), os.path.join(vfs, e))
            for e in os.listdir(os.path.join(o.game, 'PS3_GAME')):
                if e != 'PARAM.SFO':
                    os.symlink(os.path.join(o.game, 'PS3_GAME', e), os.path.join(vfs, 'PS3_GAME', e))
            shutil.copy(os.path.join(o.update, 'PARAM.SFO'), os.path.join(vfs, 'PS3_GAME'))
            install = os.path.join(hdd0, 'game', 'BCUS98124')
            os.makedirs(install, exist_ok=True)
            shutil.copy(os.path.join(o.update, 'PARAM.SFO'), install)
            subprocess.run(['/bin/cp', '-c', '-R', os.path.join(o.update, 'USRDIR'), install], check=True)
        self.pad, self.ui_file = os.path.join(self.work, 'pad.txt'), os.path.join(self.work, 'ui.txt')
        self.gate, self.frame = os.path.join(self.work, 'capture.gate'), os.path.join(self.work, 'frame.ppm')
        self.log = os.path.join(self.work, 'game.log')
        open(self.pad, 'w').close()
        settings = os.path.join(self.work, 'settings.txt')
        shutil.copy(o.settings, settings) if o.settings else open(settings, 'w').close()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('RACACIT_', 'PS3', 'PAD_', 'RSX_', 'SPU_', 'PPU_', 'GCM_', 'SDL_'))}
        env.update({
            'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy', 'SDL_MAC_BACKGROUND_APP': '1',
            'PS3RECOMP_VK_WINDOW': '0', 'PS3RECOMP_VK_FULLSCREEN': '0',
            'PS3RECOMP_VK_DUMP': self.frame, 'PS3RECOMP_VK_DUMP_GATE': self.gate,
            'PS3RECOMP_VK_GUEST_PROGRAMS': '1', 'PS3_VFS_ROOT': vfs, 'PS3_HDD0_ROOT': hdd0,
            'PAD_IGNORE_REAL': '1', 'PAD_SCRIPT_ONLY': '1', 'PAD_FILE': self.pad,
            'RACACIT_UI_FILE': self.ui_file, 'RACACIT_SETTINGS_FILE': settings, 'RACACIT_KBM': '1',
            'RACACIT_MOUSE_TRACE': '1', 'RACACIT_HLE_SPURS': '1', 'RACACIT_AUTOSKIP_MOVIES': '1',
            'RACACIT_SKIP_INTRO': '1', 'RACACIT_SKIP_PRESS_START': '1', 'RACACIT_SAVE_UI': '0',
            'RACACIT_RES_SCALE': '1', 'RACACIT_ASYNC_PIPELINES': '0',
        })
        if o.vk_icd:
            env['VK_DRIVER_FILES'] = o.vk_icd
        self.p = subprocess.Popen([os.path.abspath(o.player), os.path.abspath(o.elf)], cwd=self.work,
                                  env=env, stdin=subprocess.DEVNULL, stdout=open(self.log, 'wb'),
                                  stderr=subprocess.STDOUT, start_new_session=True)

    def text(self):
        with open(self.log, 'rb') as f:
            return f.read().decode('latin-1')

    def wait_for(self, pattern, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if self.p.poll() is not None:
                sys.exit('the game exited (%s); see %s' % (self.p.returncode, self.log))
            if re.search(pattern, self.text()):
                return True
            time.sleep(0.5)
        return False

    def settle(self, least, most):
        """After a level load: at least `least` s, then on while the game is still loading (a
        file read in the last 5 s), up to `most` s. False when it is still loading at `most`."""
        start = time.time()
        while True:
            if self.p.poll() is not None:
                sys.exit('the game exited (%s); see %s' % (self.p.returncode, self.log))
            el = time.time() - start
            before = len(self.text()); time.sleep(5)
            loading = re.search(r'\[fs\] (open|read)|levels/', self.text()[before:])
            if el + 5 >= least and not loading:
                return True
            if el + 5 >= most:
                return False

    def press(self, line):
        """A pad line ("mask polls lx ly rx ry"), once the previous one was taken."""
        end = time.time() + 10
        while os.path.getsize(self.pad) and time.time() < end:
            time.sleep(0.05)
        with open(self.pad + '.new', 'w') as f:
            f.write(line + '\n')
        os.replace(self.pad + '.new', self.pad)

    def ui(self, line):
        """A test hook command (rsx_host_ui.c), once the previous one was taken."""
        end = time.time() + 10
        while os.path.exists(self.ui_file) and os.path.getsize(self.ui_file) and time.time() < end:
            time.sleep(0.05)
        with open(self.ui_file + '.new', 'w') as f:
            f.write(line + '\n')
        os.replace(self.ui_file + '.new', self.ui_file)

    def mouse(self, dx, dy):
        self.ui('mouse %d %d' % (dx, dy))

    def capture(self, label):
        old = os.path.getmtime(self.frame) if os.path.exists(self.frame) else 0
        open(self.gate, 'w').close()
        end = time.time() + 30
        while time.time() < end:
            if os.path.exists(self.frame) and os.path.getmtime(self.frame) > old and os.path.getsize(self.frame) > 100:
                time.sleep(0.1)
                break
            time.sleep(0.05)
        os.remove(self.gate)
        if not os.path.exists(self.frame):
            sys.exit('no frame within 30 s; see ' + self.log)
        time.sleep(0.2)
        shutil.copy(self.frame, os.path.join(self.work, label + '.ppm'))   # kept to look at
        return ppm(self.frame)

    def stop(self):
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGTERM)
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                os.killpg(self.p.pid, signal.SIGKILL)


PITCH_WAY = {}   # mode -> True when mouse down moved the picture down (rows > 0)


def turns(g, mode, cameras, key, results):
    """Stand still, then the stick, then the mouse, with the mode's key held throughout."""
    if key:
        g.ui('hold 8000 ' + key)
    # Small turns (a few degrees), so the picture shifts by less than the search range.
    time.sleep(1.5)
    a = g.capture(mode + '-still-0'); time.sleep(0.5); b = g.capture(mode + '-still-1')
    still = shift(a, b)
    g.press('0 6 128 128 255 128'); time.sleep(0.8); s1 = g.capture(mode + '-stick')
    stick = shift(b, s1)
    # The stick's vertical axis (down), for which way up the mouse's pitch should go. Not with a
    # key held: the scripted stick is ignored then, and the time would run the hold out.
    stick_tilt = (0, 0, 0)
    if not key:
        time.sleep(0.5); v0 = g.capture(mode + '-stick-v0')
        g.press('0 6 128 128 128 255'); time.sleep(0.8); v1 = g.capture(mode + '-stick-v1')
        stick_tilt = vshift(v0, v1)
    time.sleep(0.5)
    lines_at = len(g.text())
    m0 = g.capture(mode + '-mouse-0'); g.mouse(40, 0); time.sleep(0.6); m1 = g.capture(mode + '-mouse-1')
    mouse = shift(m0, m1)
    mark = len(g.text())
    # Pitch: by what the picture does (up or down), not only the hook's log line; a small move,
    # so it stays inside the camera's limits, then back.
    # Down, then back up by as much. The camera can move by itself meanwhile (strafe's still
    # settling over the shoulder):
    # The drift is also measured outright (the same time with no input) and taken off each.
    # The key is pressed again before each part, so it is still held for every swipe (a capture
    # can take a while), and each swipe's pitch has to come from this mode's camera.
    def rehold():
        if key:
            g.ui('hold 3000 ' + key)
    rehold(); d0 = g.capture(mode + '-drift-0'); time.sleep(0.6); p0 = g.capture(mode + '-pitch-0')
    drift = vshift(d0, p0)[0]
    swipes = []
    rehold(); at = len(g.text()); g.mouse(0, 30); time.sleep(0.6); p1 = g.capture(mode + '-pitch-1')
    swipes.append(g.text()[at:])
    rehold(); at = len(g.text()); g.mouse(0, -30); time.sleep(0.6); p2 = g.capture(mode + '-pitch-2')
    swipes.append(g.text()[at:])
    # Per swipe: the cameras that answered with pitch. One outside this mode's (follow, when
    # the key had been let go after all) means the swipe didn't measure this mode.
    pitched = [set(c for c, _, y in re.findall(r'\[mouse\] (\S+) turn (\S+) (\S+)', t) if abs(float(y)) > 0)
               for t in swipes]
    from_mode = all(ps and ps <= set(cameras) for ps in pitched)
    foreign = any(ps - set(cameras) for ps in pitched)
    down, up = vshift(p0, p1), vshift(p1, p2)
    tilt = (down[0] - drift, down[1], down[2])   # mouse down's own part
    turned = re.findall(r'\[mouse\] (\S+) turn (\S+) (\S+)', g.text()[lines_at:])
    hooked = sorted(set(c for c, _, _ in turned))
    yaw = any(c in cameras and abs(float(x)) > 0 for c, x, _ in turned)
    pitch_hook = any(c in cameras and abs(float(y)) > 0 for c, _, y in re.findall(r'\[mouse\] (\S+) turn (\S+) (\S+)', g.text()[mark:]))
    pitch_moved = abs(tilt[0]) >= 3 and tilt[1] < tilt[2]
    # Which way: mouse down has to tilt the picture the way the stick's down does, and where a
    # held key masks the scripted stick, the way follow's mouse did (PITCH_WAY, from follow).
    stick_tilted = not key and abs(stick_tilt[0]) >= 3 and stick_tilt[1] < stick_tilt[2]
    if stick_tilted:
        way_ok = (tilt[0] > 0) == (stick_tilt[0] > 0)
    else:
        way_ok = mode != 'follow' and 'follow' in PITCH_WAY and (tilt[0] > 0) == PITCH_WAY['follow']
    if pitch_moved and mode == 'follow':
        PITCH_WAY['follow'] = tilt[0] > 0
    moved = lambda m: abs(m[0]) >= max(4, 3 * abs(still[0])) and m[1] < m[2]
    # The stick must turn the picture too, the same way as the mouse. With a key held the
    # runtime counts a person at the pad and ignores the scripted stick, so it can't be
    # checked in those modes (strafe).
    stick_ok = True if key else moved(stick) and (stick[0] > 0) == (mouse[0] > 0)
    ok = yaw and pitch_hook and from_mode and pitch_moved and way_ok and moved(mouse) and stick_ok
    if not from_mode and key and foreign:
        ok = None   # the held key ran out: not a measurement of this mode, so neither pass nor fail
    results.append((mode, ok, 'hook %s (yaw %s, pitch %s), swipe pitch from %s, shift: still %d, stick %s, mouse %d, pitch %d rows [down %d, up %d, drift %d] (%s %s)'
                    % ('/'.join(hooked) or 'none', yaw, pitch_hook,
                       ' then '.join('/'.join(sorted(ps)) or 'none' for ps in pitched), still[0],
                       'n/a (key held)' if key else '%d' % stick[0], mouse[0], tilt[0], down[0], up[0], drift,
                       'stick %d rows' % stick_tilt[0] if stick_tilted else 'follow mouse',
                       'same way' if way_ok else 'WRONG WAY')))
    time.sleep(4.0)   # the mode's key is let go by now (3 s after the last press)


def menu(g, results):
    """The pause menu takes no mouse turn, and nothing turns when play resumes."""
    g.press('0x%04X 4' % START); time.sleep(2.5)
    before = len(re.findall(r'\[mouse\] \S+ turn', g.text()))
    g.mouse(300, 0); time.sleep(1.0)
    paused = len(re.findall(r'\[mouse\] \S+ turn', g.text())) - before
    g.press('0x%04X 4' % START); time.sleep(2.5)
    resumed = len(re.findall(r'\[mouse\] \S+ turn', g.text())) - before
    results.append(('pause menu', paused == 0 and resumed == 0,
                    'turns while paused %d, after resuming %d' % (paused, resumed)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--player', required=True); ap.add_argument('--elf', required=True)
    ap.add_argument('--game', required=True); ap.add_argument('--save', required=True)
    ap.add_argument('--work', required=True); ap.add_argument('--update')
    ap.add_argument('--installed', help="the game's HDD install folder (hdd0/game/BCUS98124 of a profile that has played)")
    ap.add_argument('--settings'); ap.add_argument('--vk-icd')
    ap.add_argument('--level-wait', type=float, default=45)
    o = ap.parse_args()
    check_inputs(o)
    for name in ('RaCACiTRecomp', 'racit-player'):
        if subprocess.run(['/usr/bin/pgrep', '-x', name], capture_output=True).returncode == 0:
            sys.exit('a game is already running; not starting another')
    g = Game(o)
    results = []
    try:
        if not g.wait_for(r'PRESS START skipped|main menu opens', 300):
            sys.exit('never reached the main menu; see ' + g.log)
        time.sleep(10)
        g.press('0x%04X' % CROSS)   # LOAD: the save's level
        if not g.wait_for(r'levels/(?!front_end)', 120):
            sys.exit('no level loaded after LOAD; see ' + g.log)
        # The clock starts once the level is in, and runs on while it is still loading.
        if not g.settle(o.level_wait, 4 * o.level_wait):
            print('INCONCLUSIVE: still loading %d s after the level started' % (4 * o.level_wait))
            return 2
        # The pause menu first: a held key (strafe's) counts as a person at the pad, and the
        # runtime then ignores scripted pad input (START, the stick) for 20 s.
        mode, key, cameras = MODES[0]
        turns(g, mode, cameras, key, results)
        menu(g, results)
        for mode, key, cameras in MODES[1:]:
            turns(g, mode, cameras, key, results)
    finally:
        g.stop()
    for mode, ok, note in results:
        print('%-11s %s  %s' % (mode, {True: 'PASS', False: 'FAIL', None: 'INCONCLUSIVE'}[ok], note))
    print('log: ' + g.log)
    if not results or any(ok is False for _, ok, _ in results):
        return 1
    return 2 if any(ok is None for _, ok, _ in results) else 0


if __name__ == '__main__':
    sys.exit(main())
