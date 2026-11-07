#!/usr/bin/env python3
"""retarget.py <TITLE_ID>: a copy of the project for another disc of the game.

The sources in this repository are written for BCUS98124 and stay that way.
Another disc has the same game at other addresses, so this writes a copy of what
the build uses into build/<TITLE_ID>/, with every guest address moved through
versions/<TITLE_ID>/addr_map.tsv, and the build runs there. tools/ is shared.

Addresses not in the map take the offset of the nearest lower one, with a warning.
The copy's patch_lift.py stops the build if a hook did not land: most of its
patches skip in silence when their target is not found.
"""
import bisect, filecmp, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files that hold guest addresses.
ADDRESS_FILES = ['game/stubs.cpp', 'game/main.cpp', 'game/hot_ppu.h', 'scripts/patch_lift.py',
                 'scripts/fix_vmx_alias.py', 'scripts/name_pass.py', 'scripts/lift_spu_jobs.py',
                 'scripts/lift_policy_modules.py', 'scripts/find_submodules.py',
                 'game/hot_spu_modules.cmake', 'scripts/spu_submodules.txt']
# What the build needs from the repository; generated folders are left out.
COPY_DIRS = ['game', 'scripts', 'windows']
SKIP_DIRS = {'src', 'spu', 'prx', '__pycache__'}

# Where each kind of address sits in BCES00511, measured on its ELF.
VERSIONS = {
    'BCES00511': {
        'code_end': 0xA50000,
        'ranges': [(0xAD0000, 0xC20000, 0x800),    # SPU code, end of the first segment
                   (0xDB0000, 0xFD0000, 0x100)],   # end of the data, bss
        'pool': {0xD2FAA4: 0xC0, 0xD2F988: 0xC0},  # pointer pool
    },
}

REQUIRED_HOOKS = r"""

# Added by scripts/retarget.py: on another disc a wrong address must stop the build.
REQUIRED = '''racacit_jb_push racacit_longjmp racacit_lua_throw_hook racacit_lua_exec_hook
racacit_snd_play_hook racacit_snd_stop_hook racacit_snd_start_end racacit_movie_update_hook
racacit_bink_opened racacit_force_abort racacit_gcm_put_moved racacit_level_change
racacit_stream_pause_hook racacit_luac_trace racacit_caster_box racacit_wait_begin racacit_wait_end
racacit_armor_look racacit_skin_armor_rows racacit_skin_armor_selected racacit_equip_skin
racacit_follow_fov racacit_follow_fov_unscale racacit_follow_dist racacit_follow_dist_unscale'''.split()
REQUIRED += ['racacit: get re-read', 'racacit: on the disc']
_blob = ''.join(open(p, encoding='latin-1').read() for p in glob.glob(os.path.join(RECOMP, 'ppu_recomp_*.cpp')))
_missing = [r for r in REQUIRED if r not in _blob]
if _missing:
    sys.exit('patch_lift: hooks not applied: ' + ', '.join(_missing))
print('patch_lift: all %d hooks applied' % len(REQUIRED))
"""


class Retarget:
    def __init__(self, title):
        self.v = VERSIONS[title]
        self.map = {}
        with open(os.path.join(ROOT, 'versions', title, 'addr_map.tsv')) as f:
            for line in f:
                if line.startswith('#'):
                    continue
                usa, eu = line.split()
                self.map[int(usa, 16)] = int(eu, 16)
        self.code = sorted(a for a in self.map if a < self.v['code_end'])
        self.guessed = set()

    def code_address(self, a):
        i = bisect.bisect_right(self.code, a) - 1
        if i < 0:
            return a
        self.guessed.add(a)
        return a + self.map[self.code[i]] - self.code[i]

    def convert(self, a):
        if a in self.map:
            return self.map[a]
        if a in self.v['pool']:
            return a + self.v['pool'][a]
        if 0x10000 <= a < self.v['code_end']:
            return self.code_address(a)
        for lo, hi, delta in self.v['ranges']:
            if lo <= a < hi:
                return a + delta
        return None

    def token(self, m):
        pre, digits, post = m.group(1), m.group(2), m.group(3) or ''
        n = self.convert(int(digits, 16))
        if n is None:
            return m.group(0)
        return pre + ('%0' + str(len(digits)) + 'X') % n + post

    def literal(self, m):
        a = int(m.group(2), 16)
        if a < 0x10000 or a & 0xFFF in (0, 0xFFF):   # sizes and masks
            return m.group(0)
        return self.token(m)

    def text(self, name, t):
        if name == 'scripts/spu_submodules.txt':
            return re.sub(r'(?m)^()([0-9A-F]{6})( )', self.token, t)
        n = re.sub(r'\b(func_|loc_)([0-9A-Fa-f]{8})\b()', self.token, t)
        n = re.sub(r'(\\nloc_)([0-9A-Fa-f]{8})\b()', self.token, n)          # inside a Python string
        n = re.sub(r'\b(job_|sub_)([0-9A-F]{6})(_|\b)', self.token, n)
        n = re.sub(r'\b(spu_\d{4}_at_00)([0-9A-F]{6})\b()', self.token, n)
        n = re.sub(r'\b(0x00)([0-9A-Fa-f]{6})(u?)\b', self.literal, n)
        short = r'(?<![0-9A-Za-z_])(0x)([0-9A-F]{5,6})(u?)\b'
        if name == 'scripts/name_pass.py':
            n = re.sub(short, self.literal, n)
        elif name in ('scripts/lift_spu_jobs.py', 'scripts/lift_policy_modules.py', 'scripts/find_submodules.py'):
            low = self.v['ranges'][0][0]
            n = re.sub(short, lambda m: self.literal(m) if int(m.group(2), 16) >= low else m.group(0), n)
        elif name in ('game/stubs.cpp', 'game/main.cpp', 'scripts/patch_lift.py'):
            n = re.sub(r'(?<![0-9A-Za-z_])(0x)([0-9A-F]{6})(u?)\b', self.literal, n)
        return n


def write_if_changed(path, text):
    """Unchanged files keep their date, so a rebuild stays incremental."""
    if os.path.exists(path) and open(path, encoding='utf8', newline='').read() == text:
        return
    with open(path, 'w', encoding='utf8', newline='') as f:
        f.write(text)


def link_folder(link, target):
    if os.path.lexists(link):
        return
    if os.name == 'nt':
        subprocess.run(['cmd', '/c', 'mklink', '/J', link, target], check=True, stdout=subprocess.DEVNULL)
    else:
        os.symlink(target, link)


def main():
    title = sys.argv[1] if len(sys.argv) > 1 else ''
    if title not in VERSIONS:
        sys.exit('usage: retarget.py <%s>' % '|'.join(VERSIONS))
    work = os.path.join(ROOT, 'build', title)
    input_dir = os.path.join(ROOT, 'input', title)
    for name in ('EBOOT.ELF', 'libsre.prx'):
        if not os.path.isfile(os.path.join(input_dir, name)):
            sys.exit('No %s in input/%s/' % (name, title))

    for top in COPY_DIRS:
        for folder, dirs, files in os.walk(os.path.join(ROOT, top)):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith('build')]
            to = os.path.join(work, os.path.relpath(folder, ROOT))
            os.makedirs(to, exist_ok=True)
            for f in files:
                src, dst = os.path.join(folder, f), os.path.join(to, f)
                if not os.path.exists(dst) or not filecmp.cmp(src, dst, shallow=False):
                    shutil.copy2(src, dst)

    r = Retarget(title)
    for name in ADDRESS_FILES:
        text = open(os.path.join(ROOT, name), encoding='utf8', newline='').read()
        out = r.text(name, text)
        if name == 'scripts/patch_lift.py':
            out = out.rstrip('\n') + '\n' + REQUIRED_HOOKS
        write_if_changed(os.path.join(work, name), out)

    # The runtime picks the few addresses it holds with this define.
    cmake = os.path.join(work, 'windows', 'CMakeLists.txt')
    text = open(cmake, encoding='utf8', newline='').read()
    line = 'project(racacit_windows C CXX)'
    assert line in text
    write_if_changed(cmake, text.replace(line, line + '\nadd_compile_definitions(RACACIT_%s)' % title, 1))

    os.makedirs(os.path.join(work, 'input'), exist_ok=True)
    for name in ('EBOOT.ELF', 'libsre.prx'):
        src, dst = os.path.join(input_dir, name), os.path.join(work, 'input', name)
        if not os.path.exists(dst) or not filecmp.cmp(src, dst, shallow=False):
            shutil.copy2(src, dst)
    link_folder(os.path.join(work, 'tools'), os.path.join(ROOT, 'tools'))

    for a in sorted(r.guessed):
        print('warning: 0x%X is not in the map, moved with its neighbour' % a)
    print('build/%s ready, %d addresses in the map' % (title, len(r.map)))


if __name__ == '__main__':
    main()
