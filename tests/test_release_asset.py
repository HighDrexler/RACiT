"""Which release file the launcher's updater takes (launcher/src/release_asset.h): on Windows the
launcher .zip, on macOS the packaged RACiT-<version>-macos.zip (mac/package_macos.sh), never the
other platform's. Builds a tiny checker from that header with the C++ compiler here and runs it
on releases with both names, one of them, or neither. No game files.

    python -m unittest discover -s tests
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECKER = r'''
#include "release_asset.h"
#include <cstdio>
#include <string>
// argv[1]: "mac" or "win"; then the release's asset names (lower case): prints the one picked.
int main(int argc, char** argv) {
    const bool mac = std::string(argv[1]) == "mac";
    for (int i = 2; i < argc; i++) if (release_asset_for(argv[i], mac)) { std::puts(argv[i]); return 0; }
    std::puts("-");
}
'''
CXX = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


@unittest.skipUnless(CXX, 'no C++ compiler')
class ReleaseAsset(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / 'check.cpp'
        src.write_text(CHECKER, encoding='utf-8')
        cls.exe = str(Path(cls.tmp.name) / 'check.exe')   # .exe: clang for MSVC writes -o as given
        subprocess.run([CXX, '-std=c++17', '-I', str(ROOT / 'launcher' / 'src'), str(src), '-o', cls.exe], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def pick(self, platform, *names):
        return subprocess.run([self.exe, platform] + list(names), check=True, capture_output=True, text=True).stdout.strip()

    def test_both_platforms_in_one_release(self):
        for order in (('racit-1.0.1-macos.zip', 'racacit-launcher-v1.0.1.zip'),
                      ('racacit-launcher-v1.0.1.zip', 'racit-1.0.1-macos.zip')):
            with self.subTest(order=order):
                self.assertEqual(self.pick('mac', *order), 'racit-1.0.1-macos.zip')
                self.assertEqual(self.pick('win', *order), 'racacit-launcher-v1.0.1.zip')

    def test_only_the_other_platform(self):
        self.assertEqual(self.pick('mac', 'racacit-launcher-v1.0.1.zip'), '-')
        self.assertEqual(self.pick('win', 'racit-1.0.1-macos.zip'), '-')

    def test_other_files_are_never_taken(self):
        others = ('racacit-launcher-v1.0.1.7z', 'racit-1.0.1-macos.tar.gz', 'racacit-launcher-android.zip',
                  'source.zip', 'racit-1.0.1-macos.zip.sha256')
        self.assertEqual(self.pick('mac', *others), '-')
        self.assertEqual(self.pick('win', *others), '-')


if __name__ == '__main__':
    unittest.main()
