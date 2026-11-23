"""Which files of a release zip the launcher's updater installs (launcher/src/store.cpp).

    python -m unittest discover -s tests

Builds release_part and build_folder_part from store.cpp with -Wall -Werror and checks
them on typical names: the launcher, README.txt, mods, A Crack in Time's builds\\vNN and
builds\\vNN-u120, and Into the Nexus's nexus\\builds\\vNN. Skipped without a C++ compiler.
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CXX = shutil.which('clang++') or shutil.which('g++')
EXE = '.exe' if sys.platform == 'win32' else ''


def function(text, head):
    """The function starting at `head`, through its closing brace at column 0."""
    start = text.index(head)
    return text[start:text.index('\n}\n', start) + 3]


HARNESS = r'''
#include <cstdio>
#include <string>
#define RACIT_LAUNCHER_FILE L"RaCACiT Launcher.app"
static std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
static std::string narrow(const wchar_t* w) { std::string s; while (*w) s += (char)*w++; return s; }
BUILD_FOLDER_PART
RELEASE_PART
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) std::printf("%d\n", release_part(argv[i]) ? 1 : 0);
    return 0;
}
'''

CASES = {
    'README.txt': True,
    'RaCACiT Launcher.exe': True,
    'notes.txt': False,
    'mods/example/mod.lua': True,
    'mods/loose.txt': False,
    'builds/v125/RaCACiT.exe': True,
    'builds/v125-u120/RaCACiT.exe': True,
    'builds/v125-u121/RaCACiT.exe': False,
    'builds/vx/RaCACiT.exe': False,
    'builds/v/RaCACiT.exe': False,
    'builds/v125': False,
    'nexus/builds/v97/RaCNexus.exe': True,
    'nexus/saves/slot1': False,
    'data/settings.txt': False,
}


@unittest.skipUnless(CXX, 'no C++ compiler')
class ReleaseParts(unittest.TestCase):
    def test_release_parts(self):
        src = (ROOT / 'launcher' / 'src' / 'store.cpp').read_text(encoding='utf-8')
        code = HARNESS.replace('BUILD_FOLDER_PART', function(src, 'static bool build_folder_part(')) \
                      .replace('RELEASE_PART', function(src, 'static bool release_part('))
        with tempfile.TemporaryDirectory() as tmp:
            cpp, exe = Path(tmp) / 'parts.cpp', Path(tmp) / ('parts' + EXE)
            cpp.write_text(code, encoding='utf-8')
            build = subprocess.run([CXX, '-std=c++17', '-Wall', '-Werror', str(cpp), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            names = list(CASES)
            out = subprocess.run([str(exe), *names], capture_output=True, text=True, check=True).stdout.split()
        for name, got in zip(names, out):
            self.assertEqual(got == '1', CASES[name], name)


if __name__ == '__main__':
    unittest.main()
