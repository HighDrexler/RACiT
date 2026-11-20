#!/usr/bin/env python3
"""retarget.py <TITLE_ID>: a copy of the project for another disc of the game.

The sources in this repository are written for BCUS98124 and stay that way.
Another disc has the same game at other addresses, so this writes a copy of what
the build uses into build/<TITLE_ID>/, with every guest address moved through
versions/<TITLE_ID>/addr_map.tsv, and the build runs there. tools/ is shared.
(Contributed by Norby99 as public PR #26.)

Addresses not in the map take the offset of the nearest lower one.

With the USA executable in input/EBOOT.ELF as well, every converted code address
is CHECKED: the instructions at the USA address and at the converted address must
match (branch targets and immediates masked). A mismatch stops here, before
anything is built. Without it the conversion is not checked, and says so.

The copy's patch_lift.py stops the build if a hook did not land: most of its
patches skip in silence when their target is not found.
"""
import bisect, filecmp, hashlib, json, os, re, shutil, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files that hold guest addresses.
ADDRESS_FILES = ['game/stubs.cpp', 'game/main.cpp', 'game/hot_ppu.h', 'game/discord_rpc.cpp',
                 'game/mod_hookable.h', 'scripts/patch_lift.py',
                 'scripts/fix_vmx_alias.py', 'scripts/name_pass.py', 'scripts/lift_spu_jobs.py',
                 'scripts/lift_policy_modules.py', 'scripts/find_submodules.py',
                 'game/hot_spu_modules.cmake', 'scripts/spu_submodules.txt']
# What the build needs from the repository; generated folders are left out.
COPY_DIRS = ['game', 'scripts', 'windows', 'versions']
SKIP_DIRS = {'src', 'spu', 'prx', '__pycache__'}

# Where each kind of address sits in BCES00511, measured on its ELF.
VERSIONS = {
    'BCES00511': {
        'code_end': 0xA50000,
        'ranges': [(0xAD0000, 0xC20000, 0x800),    # SPU code, end of the first segment
                   (0xDB0000, 0xFD0000, 0x100)],   # end of the data, bss
        'pool': {0xD2FAA4: 0xC0, 0xD2F988: 0xC0},  # pointer pool
        # Functions whose code is not the same on this disc (checked by hand, the
        # address is right): the check skips them.
        'changed': {0x0004E7B8: 'movie queue: a language lookup is added at its start',
                    0x00051CE4: 'a Lua binding table: larger stack frame',
                    0x004C4C68: 'hero part masks (func_004C46FC) compiled differently; patch_lift #24 is USA-only'},
    },
}

# Every hook patch_lift.py puts into the USA lift. Add a new hook's name here.
REQUIRED_HOOKS = r"""

# Added by scripts/retarget.py: on another disc a wrong address must stop the build.
REQUIRED = '''racacit_armor_look racacit_bink_opened racacit_bonus_grant racacit_bonus_owned
racacit_caster_box racacit_equip_skin racacit_follow_dist racacit_follow_dist_unscale racacit_follow_fov
racacit_follow_fov_unscale racacit_force_abort racacit_front_end_cards racacit_gcm_put_moved racacit_jb_pop
racacit_jb_push racacit_level_change racacit_longjmp racacit_lua_exec_hook racacit_lua_throw_hook
racacit_luac_trace racacit_movie_update_hook racacit_skillpoint_blocked racacit_skin_armor_rows
racacit_skin_armor_selected racacit_snd_play_hook racacit_snd_start_end racacit_snd_stop_hook
racacit_stream_pause_hook racacit_wait_begin racacit_wait_end racit_camera_take
racit_publish_menu_keyboard'''.split()
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
        with open(os.path.join(ROOT, 'versions', title, 'addr_map.tsv'), encoding='utf-8') as f:
            for line in f:
                if line.startswith('#') or not line.strip():
                    continue
                usa, eu = line.split()
                self.map[int(usa, 16)] = int(eu, 16)
        self.code = sorted(a for a in self.map if a < self.v['code_end'])
        self.guessed = set()
        self.converted = {}

    def code_address(self, a):
        i = bisect.bisect_right(self.code, a) - 1
        if i < 0:
            return a
        self.guessed.add(a)
        return a + self.map[self.code[i]] - self.code[i]

    def convert(self, a):
        n = self._convert(a)
        if n is not None:
            self.converted[a] = n
        return n

    def _convert(self, a):
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
        if name == 'game/mod_hookable.h':
            return re.sub(r'(RACIT_HOOKABLE\()([0-9A-F]{8})(\))', self.token, t)
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
        elif name in ('game/stubs.cpp', 'game/main.cpp', 'game/discord_rpc.cpp', 'scripts/patch_lift.py'):
            n = re.sub(r'(?<![0-9A-Za-z_])(0x)([0-9A-F]{6})(u?)\b', self.literal, n)
        return n


# ---- the check against both executables -----------------------------------

class Elf:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        phoff, = struct.unpack_from('>Q', self.b, 0x20)
        phentsize, phnum = struct.unpack_from('>HH', self.b, 0x36)
        self.segs = []
        for i in range(phnum):
            t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('>IIQQQQQQ', self.b, phoff + i * phentsize)
            if t == 1 and msz:
                self.segs.append((va, fsz, msz, off, fl))

    def words(self, va, n):
        for sva, fsz, msz, off, fl in self.segs:
            if (fl & 1) and sva <= va and va + 4 * n <= sva + fsz:
                return struct.unpack_from('>%dI' % n, self.b, off + va - sva)
        return None


# Immediates that hold addresses or TOC offsets, and branch targets, move between discs.
_DFORM = {14, 15, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
          48, 49, 50, 51, 52, 53, 54, 55, 58, 62}


def _mask(w):
    op = w >> 26
    if op == 18:
        return w & 0xFC000003
    if op in _DFORM:
        return w & 0xFFFF0000
    return w


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def check(r, title, target_elf):
    """Every converted code address against both executables; None if it cannot run."""
    profiles = {p['id'].split('-')[0]: p for p in json.load(open(os.path.join(ROOT, 'versions', 'profiles.json'), encoding='utf-8'))['profiles']
                if p['app_version'] == '1.00'}
    if sha256(target_elf) != profiles[title]['elf_sha256']:
        sys.exit('input/%s/EBOOT.ELF is not the %s disc 1.00 executable (SHA-256 differs).' % (title, title))
    usa_elf = os.path.join(ROOT, 'input', 'EBOOT.ELF')
    if not os.path.isfile(usa_elf) or sha256(usa_elf) != profiles['BCUS98124']['elf_sha256']:
        return None
    usa, tgt = Elf(usa_elf), Elf(target_elf)
    bad, checked = [], 0
    for a, b in sorted(r.converted.items()):
        if a >= r.v['code_end'] or a in r.v['changed']:
            continue
        x, y = usa.words(a, 16), tgt.words(b, 16)
        if x is None:
            continue                    # not code in the USA image (a size or an offset)
        checked += 1
        if y is None or sum(_mask(p) == _mask(q) for p, q in zip(x, y)) < 14:
            bad.append((a, b))
    # The bss range rule: the data segment grew by exactly that much.
    d_usa, d_tgt = usa.segs[1], tgt.segs[1]
    delta = r.v['ranges'][-1][2]
    if d_tgt[1] - d_usa[1] != delta or d_tgt[2] - d_usa[2] != delta:
        sys.exit('The %s data segment did not grow by 0x%X: the data/bss range rule in retarget.py is wrong.' % (title, delta))
    if bad:
        sys.exit('retarget: %d converted address(es) do not land on the same code in %s:\n%s\n'
                 'Add the right ones to versions/%s/addr_map.tsv.'
                 % (len(bad), title, '\n'.join('  0x%08X -> 0x%08X' % ab for ab in bad), title))
    return checked


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

    r = Retarget(title)
    out = {}
    for name in ADDRESS_FILES:
        text = open(os.path.join(ROOT, name), encoding='utf8', newline='').read()
        out[name] = r.text(name, text)
    checked = check(r, title, os.path.join(input_dir, 'EBOOT.ELF'))

    for top in COPY_DIRS:
        for folder, dirs, files in os.walk(os.path.join(ROOT, top)):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith('build')]
            to = os.path.join(work, os.path.relpath(folder, ROOT))
            os.makedirs(to, exist_ok=True)
            for f in files:
                src, dst = os.path.join(folder, f), os.path.join(to, f)
                if not os.path.exists(dst) or not filecmp.cmp(src, dst, shallow=False):
                    shutil.copy2(src, dst)

    for name, text in out.items():
        if name == 'scripts/patch_lift.py':
            text = text.rstrip('\n') + '\n' + REQUIRED_HOOKS
        write_if_changed(os.path.join(work, name), text)

    # The copy builds this disc and only this disc (scripts/pipeline.sh checks the profile).
    prof = json.load(open(os.path.join(ROOT, 'versions', 'profiles.json'), encoding='utf-8'))
    for p in prof['profiles']:
        mine = p['title_id'] == title and p['app_version'] == '1.00'
        p['buildable'] = mine
        p['status'] = 'Converted copy (scripts/retarget.py)' if mine else 'Not this copy: build it from the repository root'
    write_if_changed(os.path.join(work, 'versions', 'profiles.json'), json.dumps(prof, indent=2) + '\n')

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

    if checked is None:
        for a in sorted(r.guessed):
            print('warning: 0x%X is not in the map, moved with its neighbour' % a)
        print('warning: the conversion is NOT checked (no USA input/EBOOT.ELF to compare with)')
    else:
        print('checked: %d converted code addresses land on the same code (%d of them not in the map)'
              % (checked, len(r.guessed)))
    print('build/%s ready, %d addresses in the map' % (title, len(r.map)))


if __name__ == '__main__':
    main()
