"""mac/macos_dylibs.sh bundles libraries from a prefix whose path has spaces in it.

MACOS_LIB_PREFIX (mac/build_macos_libs.sh's output) can be any folder, like
"~/RACiT libs", and the libraries built there name each other by that absolute path.
otool's output used to be split on spaces, so such a path broke bundling. This builds
two small libraries under a prefix with a space in it, a program that loads one (which
loads the other), bundles them, and checks nothing points outside the bundle and the
program still runs. macOS only (clang, otool, install_name_tool).

    python -m unittest discover -s tests
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang') and shutil.which('otool'), 'macOS only')
class BundleFromPrefixWithSpaces(unittest.TestCase):
    def test_bundle(self):
        with tempfile.TemporaryDirectory() as d:
            prefix = Path(d) / 'my libs' / 'prefix with space'
            lib = prefix / 'lib'
            lib.mkdir(parents=True)
            (Path(d) / 'foo.c').write_text('int foo(void) { return 40; }\n')
            (Path(d) / 'bar.c').write_text('int foo(void);\nint bar(void) { return foo() + 2; }\n')
            (Path(d) / 'main.c').write_text('#include <stdio.h>\nint bar(void);\nint main(void) { printf("%d\\n", bar()); return 0; }\n')
            run = lambda *a: subprocess.run(a, check=True, capture_output=True, text=True)
            run('clang', '-dynamiclib', '-o', str(lib / 'libfoo.dylib'), '-install_name', str(lib / 'libfoo.dylib'), str(Path(d) / 'foo.c'))
            run('clang', '-dynamiclib', '-o', str(lib / 'libbar.dylib'), '-install_name', str(lib / 'libbar.dylib'),
                str(Path(d) / 'bar.c'), str(lib / 'libfoo.dylib'))
            app = Path(d) / 'app dir'
            app.mkdir()
            main = app / 'main'
            run('clang', '-o', str(main), str(Path(d) / 'main.c'), str(lib / 'libbar.dylib'), '-Wl,-headerpad_max_install_names')
            script = ('set -e; . "$1"; export MACOS_LIB_PREFIX="$2"; bundle_dylibs "$3" "$4" "@executable_path/lib"; '
                      'foreign_deps "$3"; for f in "$4"/*.dylib; do foreign_deps "$f"; done')
            r = subprocess.run(['bash', '-c', script, 'bundle', str(ROOT / 'mac' / 'macos_dylibs.sh'), str(prefix), str(main),
                                str(app / 'lib')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(r.stdout.strip(), '', 'still pointing outside the bundle: ' + r.stdout)
            self.assertTrue((app / 'lib' / 'libfoo.dylib').exists() and (app / 'lib' / 'libbar.dylib').exists())
            shutil.rmtree(Path(d) / 'my libs')   # only the bundle is left
            self.assertEqual(subprocess.run([str(main)], capture_output=True, text=True).stdout.strip(), '42')


if __name__ == '__main__':
    unittest.main()
