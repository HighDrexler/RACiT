"""The launcher's update trusts a release's game build and launcher only if they start like a
program: "MZ" on Windows, a Mach-O header on macOS (launcher/src/program_magic.h). Builds a
tiny checker from that header with the C++ compiler here and runs it on made-up headers, and
on macOS on this machine's own programs (a thin and a universal one). No game files.

    python -m unittest discover -s tests
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECKER = r'''
#include "program_magic.h"
#include <cstdio>
int main(int argc, char** argv) {
    unsigned char h[4] = {0}; size_t n = 0;
    if (FILE* f = std::fopen(argv[2], "rb")) { n = std::fread(h, 1, 4, f); std::fclose(f); }
    const char k = argv[1][0];
    const bool ok = k == 'w' ? looks_like_windows_program(h, n) : k == 'm' ? looks_like_macos_program(h, n) : looks_like_program_head(h, n);
    std::puts(ok ? "yes" : "no");
}
'''
CXX = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')


@unittest.skipUnless(CXX, 'no C++ compiler')
class ProgramMagic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / 'check.cpp'
        src.write_text(CHECKER, encoding='utf-8')
        cls.exe = str(Path(cls.tmp.name) / 'check.exe')   # .exe: clang for MSVC writes -o's name as given
        subprocess.run([CXX, '-std=c++17', '-I', str(ROOT / 'launcher' / 'src'), str(src), '-o', cls.exe], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check(self, kind, data):
        path = Path(self.tmp.name) / 'head'
        path.write_bytes(data)
        return subprocess.run([self.exe, kind, str(path)], check=True, capture_output=True, text=True).stdout.strip() == 'yes'

    def test_headers(self):
        cases = {
            b'MZ\x90\x00': (True, False),
            b'\xcf\xfa\xed\xfe': (False, True),   # arm64 / x86_64, as on disk
            b'\xfe\xed\xfa\xcf': (False, True),
            b'\xca\xfe\xba\xbe': (False, True),   # universal
            b'\xca\xfe\xba\xbf': (False, True),
            b'PK\x03\x04': (False, False),
            b'#!/bin/sh\n': (False, False),
            b'\xcf\xfa': (False, False),          # cut short
            b'': (False, False),
        }
        for data, (windows, macos) in cases.items():
            with self.subTest(data=data):
                self.assertEqual(self.check('w', data), windows)
                self.assertEqual(self.check('m', data), macos)
                self.assertEqual(self.check('p', data), macos if sys.platform == 'darwin' else windows)

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS programs')
    def test_real_macos_programs(self):
        for prog in (sys.executable, '/bin/ls', '/usr/bin/true'):
            if os.path.exists(prog):
                with self.subTest(prog=prog):
                    self.assertTrue(self.check('p', Path(os.path.realpath(prog)).read_bytes()[:4]))


if __name__ == '__main__':
    unittest.main()
