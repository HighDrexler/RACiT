"""The scripts that read and write lifted sources don't depend on the locale's encoding.

On Windows Python's default text encoding is cp1252, and v1.0.0's update 1.20 build
broke there: chunks were written in the locale encoding and read back as UTF-8. Every
text open in these scripts now names its encoding, and the pipeline runs Python in
UTF-8 mode. This checks the opens, and runs the 1.20 source hooks with cp1252 forced as
the default on a chunk holding UTF-8 and a byte that isn't UTF-8: both must come back
unchanged.

    python -m unittest discover -s tests
"""
import ast
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ['patch_lift.py', 'update120_hooks.py', 'update120_unlock_hooks.py', 'check_lift_hooks.py', 'retarget.py', 'fix_vmx_alias.py']

# Run a script with cp1252 as the default text encoding, as on Windows.
SHIM = r'''
import builtins, io, pathlib, runpy, sys
_open = io.open
def cp1252_open(file, mode='r', buffering=-1, encoding=None, *args, **kwargs):
    if 'b' not in mode and encoding is None:
        encoding = 'cp1252'
    return _open(file, mode, buffering, encoding, *args, **kwargs)
builtins.open = io.open = cp1252_open
_read, _write = pathlib.Path.read_text, pathlib.Path.write_text
pathlib.Path.read_text = lambda self, encoding=None, errors=None, *a, **k: _read(self, encoding or 'cp1252', errors, *a, **k)
pathlib.Path.write_text = lambda self, data, encoding=None, errors=None, *a, **k: _write(self, data, encoding or 'cp1252', errors, *a, **k)
sys.path.insert(0, sys.argv[1])
module = __import__('update120_hooks')
module.ROOT = sys.argv[2]
module.apply_update120_hooks(sys.argv[3], sys.argv[4])
'''


class TextOpensNameTheirEncoding(unittest.TestCase):
    def test_helpers_run_in_utf8_mode(self):
        # patch_lift.py runs fix_vmx_alias.py itself: in UTF-8 mode too.
        text = (ROOT / 'scripts' / 'patch_lift.py').read_text(encoding='utf-8')
        calls = [l for l in text.splitlines() if 'subprocess.run([sys.executable' in l]
        self.assertTrue(calls)
        for line in calls:
            self.assertIn("'-X', 'utf8'", line)

    def test_every_text_open_has_an_encoding(self):
        missing = []
        for name in SCRIPTS:
            path = ROOT / 'scripts' / name
            if not path.exists():
                continue
            for node in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
                if not isinstance(node, ast.Call):
                    continue
                f = node.func
                called = f.id if isinstance(f, ast.Name) else f.attr if isinstance(f, ast.Attribute) else ''
                keywords = {k.arg for k in node.keywords}
                if called == 'open':
                    mode = node.args[1] if len(node.args) > 1 else next((k.value for k in node.keywords if k.arg == 'mode'), None)
                    binary = isinstance(mode, ast.Constant) and 'b' in str(mode.value)
                    if not binary and 'encoding' not in keywords and not (len(node.args) > 3):
                        missing.append('%s:%d open' % (name, node.lineno))
                elif called in ('read_text', 'write_text') and 'encoding' not in keywords:
                    missing.append('%s:%d %s' % (name, node.lineno, called))
        self.assertEqual(missing, [])


class Update120HooksUnderCp1252(unittest.TestCase):
    def test_chunk_bytes_survive(self):
        function = 'void func_004C76D0(ppu_context* ctx) {\n    ctx->gpr[3] = 0;\n}\n'
        after = 'void func_004C76D0(ppu_context* ctx) {\n        racit_publish_menu_keyboard(ctx);\n    ctx->gpr[3] = 0;\n}\n'
        head = '#include "ppu_recomp.h"\n// caf\xc3\xa9 \x81\n'.encode('latin-1')   # UTF-8 "é", and 0x81 (not UTF-8, not cp1252)
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'versions').mkdir()
            elf = d / 'EBOOT.ELF'
            elf.write_bytes(b'\x7fELF stub')
            recipe = {'schema': 1, 'profile': 'BCUS98124-1.20', 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                      'sdk_base': 0, 'preamble': '// racit update hooks\n',
                      'hooks': [{'function': '004C76D0', 'before_sha256': hashlib.sha256(function.encode()).hexdigest(),
                                 'after_sha256': hashlib.sha256(after.encode()).hexdigest(), 'includes': [],
                                 'edits': [[1, 0, '        racit_publish_menu_keyboard(ctx);\n']]}]}
            (d / 'versions' / 'BCUS98124-1.20-hooks.json').write_text(json.dumps(recipe), encoding='utf-8')
            recomp = d / 'recomp'
            recomp.mkdir()
            chunk = recomp / 'ppu_recomp_000.cpp'
            chunk.write_bytes(head + function.encode())
            env = {k: v for k, v in os.environ.items() if not k.startswith('PYTHON')}
            r = subprocess.run([sys.executable, '-I', '-c', SHIM, str(ROOT / 'scripts'), str(d), str(recomp), str(elf)],
                               capture_output=True, text=True, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            out = chunk.read_bytes()
            self.assertTrue(out.startswith(b'#include "ppu_recomp.h"\n'), out[:80])
            self.assertIn(b'// caf\xc3\xa9 \x81\n', out)
            self.assertIn(after.encode(), out)


if __name__ == '__main__':
    unittest.main()
