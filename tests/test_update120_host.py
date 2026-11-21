"""The update 1.20 build's host sources can be made from the shared game sources.

v1.0.0 couldn't build 1.20 at all: stubs.cpp called disc functions (the cheat
menu's weapon calls) that versions/BCUS98124-1.20-host-functions.tsv didn't map,
and scripts/prepare_game_host.py stopped. This needs no game files.

    python -m unittest discover -s tests
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_game_host  # noqa: E402


class Update120HostSources(unittest.TestCase):
    def test_every_disc_function_has_a_120_mapping(self):
        # Raises ValueError('Missing host function mapping ...') if one is unmapped.
        prepared = prepare_game_host.host_sources('BCUS98124-1.20')
        self.assertEqual(set(prepared), {'game_version.h', 'main.cpp', 'stubs.cpp'})

    def test_120_sources_name_only_120_functions(self):
        prepared = prepare_game_host.host_sources('BCUS98124-1.20')
        mapped = {row['update120_function'] for row in prepare_game_host.rows('BCUS98124-1.20-host-functions.tsv')}
        code = re.compile(r'("(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/)|\bfunc_([0-9A-F]{8})\b', re.S)
        for name in ('main.cpp', 'stubs.cpp'):
            for match in code.finditer(prepared[name]):
                if match.group(2):
                    self.assertIn(match.group(2), mapped, '%s calls func_%s, not a 1.20 function' % (name, match.group(2)))

    def test_disc_only_fixes_are_left_out_of_120(self):
        prepared = prepare_game_host.host_sources('BCUS98124-1.20')
        self.assertNotIn('Disc-only 1.20 fixes begin.', prepared['stubs.cpp'])
        disc = prepare_game_host.host_sources('BCUS98124-1.00')
        self.assertIn('Disc-only 1.20 fixes begin.', disc['stubs.cpp'])

    def test_disc_sources_are_unchanged(self):
        disc = prepare_game_host.host_sources('BCUS98124-1.00')
        for name in ('main.cpp', 'stubs.cpp'):
            self.assertTrue(disc[name].endswith((ROOT / 'game' / name).read_text(encoding='utf-8')
                                                .replace('"../tools/ps3recomp/runtime/ppu/ppu_context.h"', '"ppu_context.h"')))


if __name__ == '__main__':
    unittest.main()
