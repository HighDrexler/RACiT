"""The comma key can be bound (launcher settings and the game read it the same way).

A binding is a comma-separated list of SDL key names, and SDL names the comma key
",": saved as itself it split into nothing and the key was lost. The launcher now
saves it as "Comma" and the game reads that; lists saved before ("A,,", ",") still
read as the comma key.

    python -m unittest discover -s tests

Builds the launcher's list reader (launcher/src/app.cpp) and the game's
(cellPad.c, in the runtime patch) with stand-ins for SDL; skipped without a C++
compiler.
"""
import re
import shutil
import sys
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CXX = shutil.which('clang++') or shutil.which('g++')
# Windows runs a program by its .exe name, and clang for MSVC writes -o's name as given.
EXE = '.exe' if sys.platform == 'win32' else ''


def function(text, head):
    """The function starting at `head`, through its closing brace at column 0."""
    start = text.index(head)
    return text[start:text.index('\n}\n', start) + 3]


def runtime_source():
    patch = (ROOT / 'patches' / 'ps3recomp-0001-racacit-runtime-fixes.patch').read_text(encoding='utf-8', errors='replace')
    part = patch[patch.index('+++ b/libs/input/cellPad.c'):]
    return '\n'.join(l[1:] for l in part.splitlines() if l.startswith('+') and not l.startswith('+++'))


HARNESS = r'''
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
enum SDL_Scancode { SDL_SCANCODE_UNKNOWN = 0, SDL_SCANCODE_A = 4, SDL_SCANCODE_B = 5, SDL_SCANCODE_COMMA = 54 };
static SDL_Scancode SDL_GetScancodeFromName(const char* n)
{ return !strcmp(n, "A") ? SDL_SCANCODE_A : !strcmp(n, "B") ? SDL_SCANCODE_B : SDL_SCANCODE_UNKNOWN; }
#ifdef _WIN32
#define SDL_strcasecmp _stricmp
#define SDL_strncasecmp _strnicmp
#else
#include <strings.h>
#define SDL_strcasecmp strcasecmp
#define SDL_strncasecmp strncasecmp
#endif
static std::string trim(const std::string& s)
{ size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
static std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
SPLIT
PARSE
static std::string game(const char* v)
{
    char list[256]; snprintf(list, sizeof list, "%s", v);
    std::string out; int n = 0;
    for (char* tok = list; tok && *tok && n < 4; ) {
        LOOP
    }
    return out;
}
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        std::string l; for (auto& k : split_keys(argv[i])) l += "[" + k + "]";
        printf("%s|%s\n", l.c_str(), game(argv[i]).c_str());
    }
}
'''


@unittest.skipUnless(CXX, 'no C++ compiler')
class CommaBinding(unittest.TestCase):
    def test_comma_reads_back_in_launcher_and_game(self):
        app = (ROOT / 'launcher' / 'src' / 'app.cpp').read_text(encoding='utf-8')
        rt = runtime_source()
        split = function(app, 'static std::vector<std::string> split_keys(')
        parse = function(rt, 'static int kb_parse_one(')
        loop = re.search(r'for \(char\* tok = list; tok && \*tok && n < KB_MAX_KEYS; \) \{\n(.*?)\n        \}\n', rt, re.S).group(1)
        loop = loop.replace('{ s_kb_keys[k][n++] = key; if (k >= KB_QS_1) s_kb_is_qs[key] = 1; }',
                            '{ out += "[" + std::to_string(key) + "]"; n++; }')
        loop = re.sub(r'else if \(\*tok.*?\n', '\n', loop)
        src = HARNESS.replace('SPLIT', split).replace('PARSE', parse).replace('LOOP', loop)
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 't.cpp').write_text(src)
            subprocess.run([CXX, '-std=c++17', '-o', str(Path(d) / ('t' + EXE)), str(Path(d) / 't.cpp')], check=True)
            cases = ['Comma', 'A,Comma', ',', 'A,,', ',,B', 'A,,,B', 'A, B', 'none']
            out = subprocess.run([str(Path(d) / ('t' + EXE))] + cases, check=True, capture_output=True, text=True).stdout.splitlines()
        self.assertEqual(out, [
            '[Comma]|[54]', '[A][Comma]|[4][54]', '[Comma]|[54]', '[A][Comma]|[4][54]',
            '[Comma][B]|[54][5]', '[A][Comma][B]|[4][54][5]', '[A][B]|[4][5]', '|'])

    def test_menu_label_shows_the_comma_key(self):
        menu = (ROOT / 'game' / 'menu_keycaps.h').read_text(encoding='utf-8')
        label = function(menu, 'static void racit_menu_key_label(')
        src = ('#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n'
               '#ifdef _WIN32\nstatic void set(const char* v) { _putenv_s("RACACIT_BIND_X", v); }\n'
               '#else\nstatic void set(const char* v) { setenv("RACACIT_BIND_X", v, 1); }\n#endif\n' + label +
               'int main(int argc, char** argv) { for (int i = 1; i < argc; i++) { set(argv[i]);'
               ' char l[16]; racit_menu_key_label("X", "Q", l); printf("%s\\n", l); } }\n')
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'm.cpp').write_text(src)
            subprocess.run([CXX, '-std=c++17', '-o', str(Path(d) / ('m' + EXE)), str(Path(d) / 'm.cpp')], check=True)
            out = subprocess.run([str(Path(d) / ('m' + EXE)), 'Comma', 'Comma,A', ',', ',,A', 'A,,', 'Space'],
                                 check=True, capture_output=True, text=True).stdout.split()
        self.assertEqual(out, [',', ',', ',', ',', 'A', 'SPC'])

    def test_launcher_saves_comma_by_name(self):
        app = (ROOT / 'launcher' / 'src' / 'app.cpp').read_text(encoding='utf-8')
        self.assertIn('SDL_SCANCODE_COMMA ? std::string("Comma")', app)
        self.assertEqual(app.count('SDL_GetScancodeName('), 1, 'every saved key name goes through bind_name')
        prompts = (ROOT / 'game' / 'desktop_prompts.h').read_text(encoding='utf-8')
        self.assertIn('"Comma"', prompts)


if __name__ == '__main__':
    unittest.main()
