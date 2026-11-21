"""The keyboard menus can be prepared, by the script and by the game itself (issue #22).

In v1.0.0 the menus kept showing controller buttons with a keyboard:
game/menu_keycaps.actions.bin was left out of the repository (the *.BIN game-data
rule in .gitignore matched it), and only a prepared menu archive, which players
never made, carried the keyboard menu actions. This needs no game files: it builds a
small stand-in for global_cached.psarc.

    python -m unittest discover -s tests

The C++ half (game/menu_archive.cpp, which the game uses on its own) runs where a
C++ compiler and zlib are found (clang++/g++ with -lz); it is skipped otherwise.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_menu_keycaps  # noqa: E402

ACTIONS = ROOT / 'game' / 'menu_keycaps.actions.bin'


def swf(prompt, compressed=False):
    """A minimal movie: a DoAction tag (holding the class name or not), ShowFrame, End."""
    def tag(code, data):
        return struct.pack('<HI', (code << 6) | 63, len(data)) + data
    body = b'\x00' + struct.pack('<HH', 0x1800, 1)          # empty RECT, frame rate, 1 frame
    body += tag(12, (b'ButtonPrompt' if prompt else b'SomethingElse') + b'\x00')
    body += struct.pack('<H', 1 << 6) + struct.pack('<H', 0)  # ShowFrame, End
    movie = b'FWS\x0a' + struct.pack('<I', 8 + len(body)) + body
    if compressed:
        return b'CWS' + movie[3:8] + zlib.compress(movie[8:])
    return movie


def psarc(path, members):
    """A zlib PSARC with a manifest (member 0) and `members` (name, bytes), one block each."""
    block = 65536
    datas = ['\n'.join(name for name, _ in members).encode()] + [data for _, data in members]
    count, entry = len(datas), 30
    sizes, blobs = [], []
    for data in datas:
        chunks = [data[i:i + block] for i in range(0, len(data), block)] or [b'']
        blob_sizes = []
        for chunk in chunks:
            z = zlib.compress(chunk, 9)
            use = z if len(z) < len(chunk) else chunk
            blobs.append(use)
            blob_sizes.append(len(use) % block)
        sizes.append(blob_sizes)
    table = count * entry
    toc_size = 32 + table + 2 * sum(len(s) for s in sizes)
    entries, size_table, offset, first = b'', b'', toc_size, 0
    blob_index = 0
    for data, blob_sizes in zip(datas, sizes):
        span = sum(len(blobs[blob_index + k]) for k in range(len(blob_sizes)))
        entries += b'\x00' * 16 + struct.pack('>I', first) + len(data).to_bytes(5, 'big') + offset.to_bytes(5, 'big')
        size_table += b''.join(s.to_bytes(2, 'big') for s in blob_sizes)
        first += len(blob_sizes)
        offset += span
        blob_index += len(blob_sizes)
    header = b'PSAR' + struct.pack('>HH', 1, 4) + b'zlib' + struct.pack('>IIII', toc_size, entry, count, block) + struct.pack('>I', 2)
    path.write_bytes(header + entries + size_table + b''.join(blobs))


def members_of(path):
    with path.open('rb') as source:
        archive = prepare_menu_keycaps.Archive(source)
        return [archive.read(i) for i in range(archive.count)]


MEMBERS = [('/scaleform/pausemenu.swf', swf(True)),
           ('/scaleform/options.swf', swf(True, compressed=True)),
           ('/scaleform/loading.swf', swf(False)),
           ('/levels/other.dat', os.urandom(70000))]


class MenuActions(unittest.TestCase):
    def test_compiled_actions_are_in_the_repository(self):
        self.assertTrue(ACTIONS.is_file(), 'game/menu_keycaps.actions.bin is missing')
        self.assertEqual(hashlib.sha256(ACTIONS.read_bytes()).hexdigest(), prepare_menu_keycaps.ACTIONS_SHA)
        self.assertEqual(hashlib.sha256((ROOT / 'game' / 'menu_keycaps.as').read_bytes()).hexdigest(),
                         prepare_menu_keycaps.SOURCE_SHA, 'menu_keycaps.as changed: recompile the actions')

    def test_gitignore_keeps_the_actions(self):
        if not shutil.which('git') or not (ROOT / '.git').exists():
            self.skipTest('not a git checkout')
        name = 'game/menu_keycaps.actions.bin'
        result = subprocess.run(['git', '-C', str(ROOT), 'check-ignore', '--no-index', '-q', name])
        self.assertEqual(result.returncode, 1, '.gitignore ignores ' + name)


class PrepareScript(unittest.TestCase):
    def test_button_prompt_movies_get_the_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            original, prepared = Path(directory) / 'global_cached.psarc', Path(directory) / 'out' / 'prepared.psarc'
            psarc(original, MEMBERS)
            prepared.parent.mkdir()
            prepare_menu_keycaps.prepare(original, prepared)
            before, after = members_of(original), members_of(prepared)
            self.assertEqual(before[0], after[0], 'manifest changed')
            actions = ACTIONS.read_bytes()
            pause = after[1]
            self.assertIn(actions, pause)
            self.assertLess(pause.index(actions), pause.index(struct.pack('<H', 1 << 6) + struct.pack('<H', 0)))
            self.assertIn(actions, zlib.decompress(after[2][8:]))
            self.assertEqual(before[3], after[3], 'a movie without ButtonPrompt changed')
            self.assertEqual(before[4], after[4], 'other data changed')


def compiler():
    for name in ('clang++', 'g++'):
        if shutil.which(name):
            return name
    return None


class GamePreparesItself(unittest.TestCase):
    """game/menu_archive.cpp makes the same archive as the script, by default on desktops."""

    @classmethod
    def setUpClass(cls):
        cxx = compiler()
        if not cxx:
            raise unittest.SkipTest('no C++ compiler')
        cls.work = tempfile.mkdtemp()
        work = Path(cls.work)
        (work / 'menu_keycaps_actions.inc').write_text(''.join('0x%02x,' % b for b in ACTIONS.read_bytes()))
        (work / 'driver.cpp').write_text(
            '#include <cstdio>\n#include <cstring>\n#include <cstddef>\n'
            'bool racit_menu_archive_init();\n'
            'extern "C" void racit_menu_archive_path(char*, size_t, int);\n'
            'extern "C" int racit_menu_archive_active(void);\n'
            'int main(int argc, char** argv) {\n'
            '  if (!racit_menu_archive_init()) return 2;\n'
            '  char path[4096]; std::snprintf(path, sizeof path, "%s", argv[1]);\n'
            '  racit_menu_archive_path(path, sizeof path, 1);\n'
            '  std::printf("%s\\n%d\\n", path, racit_menu_archive_active());\n'
            '  return 0;\n}\n')
        cls.binary = work / 'menu-archive-check'
        result = subprocess.run([cxx, '-std=c++20', '-O1', '-I', str(work), str(ROOT / 'game' / 'menu_archive.cpp'),
                                 str(work / 'driver.cpp'), '-o', str(cls.binary), '-lz'], capture_output=True, text=True)
        if result.returncode:
            shutil.rmtree(cls.work)
            raise unittest.SkipTest('could not build game/menu_archive.cpp with zlib: ' + result.stderr[-300:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def run_game(self, directory, keycaps):
        game = Path(directory) / 'PS3_GAME' / 'USRDIR' / 'packed' / 'game'
        game.mkdir(parents=True, exist_ok=True)
        original = game / 'global_cached.psarc'
        if not original.exists():
            psarc(original, MEMBERS)
        env = {k: v for k, v in os.environ.items() if not k.startswith('RACACIT_')}
        env['RACACIT_SETTINGS_FILE'] = str(Path(directory) / 'data' / 'settings.txt')
        if keycaps is not None:
            env['RACACIT_MENU_KEYCAPS'] = keycaps
        result = subprocess.run([str(self.binary), str(original)], capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.strip().splitlines()
        return original, Path(lines[-2]), lines[-1] == '1'

    def test_on_by_default_and_same_as_the_script(self):
        with tempfile.TemporaryDirectory() as directory:
            original, opened, active = self.run_game(directory, None)
            self.assertTrue(active, 'menu keycaps are not on by default')
            self.assertNotEqual(opened, original)
            self.assertEqual(opened.parent, Path(directory) / 'data' / 'menu-keycaps')
            script = Path(directory) / 'script.psarc'
            prepare_menu_keycaps.prepare(original, script)
            self.assertEqual(opened.read_bytes(), script.read_bytes(), 'the game and the script made different archives')
            # A second start reuses the prepared archive.
            _, again, _ = self.run_game(directory, None)
            self.assertEqual(again, opened)

    def test_only_its_own_checked_archive_is_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            _, opened, _ = self.run_game(directory, None)
            marker = Path(str(opened) + '.ok')
            self.assertTrue(marker.exists(), 'no marker after publishing')
            good = opened.read_bytes()
            # A file the game didn't check (no marker): made again over it.
            marker.unlink()
            opened.write_bytes(b'PSAR' + bytes(28) + b'junk' * 100)
            _, again, _ = self.run_game(directory, None)
            self.assertEqual(again, opened)
            self.assertEqual(opened.read_bytes(), good)
            self.assertTrue(marker.exists())
            # Changed since its marker (same name, other bytes): made again too.
            tampered = bytearray(good); tampered[-1] ^= 0xFF
            opened.write_bytes(bytes(tampered))
            self.run_game(directory, None)
            self.assertEqual(opened.read_bytes(), good)
            # No temporaries left behind.
            self.assertEqual([p.name for p in opened.parent.iterdir() if p.name.endswith('.tmp')], [])

    def test_unpublishable_archive_serves_its_session_from_the_temporary(self):
        with tempfile.TemporaryDirectory() as directory:
            original, opened, _ = self.run_game(directory, None)
            good = opened.read_bytes()
            # The cache path can't be replaced (a folder sits there): the game uses its
            # checked temporary, never the thing at the path.
            opened.unlink(); Path(str(opened) + '.ok').unlink()
            opened.mkdir(); (opened / 'not-an-archive').write_bytes(b'x')
            _, first, active = self.run_game(directory, None)
            self.assertTrue(active)
            self.assertNotEqual(first, opened)
            self.assertEqual(first.parent, opened.parent)
            self.assertTrue(first.name.endswith('.tmp'), first.name)
            self.assertEqual(first.read_bytes(), good)
            # The next start (that game has exited) removes it and makes its own.
            _, second, _ = self.run_game(directory, None)
            self.assertNotEqual(second, first)
            self.assertFalse(first.exists(), 'the old temporary was left behind')
            self.assertEqual(second.read_bytes(), good)

    @unittest.skipUnless(sys.platform == 'darwin', 'makes the rename fail with a locked file (chflags uchg, macOS)')
    def test_unchecked_archive_is_not_used_when_publishing_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            _, opened, _ = self.run_game(directory, None)
            good = opened.read_bytes()
            # A plausible archive at the path (right header and size, even the right
            # bytes) without this game's marker, and a rename over it that fails: the
            # game must not fall back to it, only to its own checked temporary.
            Path(str(opened) + '.ok').unlink()
            subprocess.run(['chflags', 'uchg', str(opened)], check=True)
            try:
                _, used, active = self.run_game(directory, None)
            finally:
                subprocess.run(['chflags', 'nouchg', str(opened)], check=True)
            self.assertTrue(active)
            self.assertNotEqual(used, opened, 'used an archive it never checked')
            self.assertTrue(used.name.endswith('.tmp'), used.name)
            self.assertEqual(used.read_bytes(), good)

    def test_off_with_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            original, opened, active = self.run_game(directory, '0')
            self.assertFalse(active)
            self.assertEqual(opened, original)
            self.assertFalse((Path(directory) / 'data' / 'menu-keycaps').exists())


if __name__ == '__main__':
    unittest.main()
