"""The tools that read and write lifted sources don't depend on the locale's encoding.

On Windows Python's default text encoding is cp1252, and v1.0.0's update 1.20 build
broke there: chunks were written in the locale encoding and read back as UTF-8. Every
text open in the Python scripts that still touch lifted sources names its encoding,
and the pipeline runs Python in UTF-8 mode. patch-lift (rust/racit-lift) works on bytes.
This checks the opens, and runs the 1.20 source hooks on a chunk holding UTF-8 and a
byte that isn't UTF-8: both must come back unchanged.

    python -m unittest discover -s tests
"""
import ast
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from racit_lift_tool import ROOT, tool

SCRIPTS = ['update120_unlock_hooks.py', 'retarget.py']


class TextOpensNameTheirEncoding(unittest.TestCase):
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


class Update120HooksKeepBytes(unittest.TestCase):
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
            r = subprocess.run([tool('patch-lift'), '--root', str(d), '--apply-update-hooks', str(recomp), str(elf)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            out = chunk.read_bytes()
            self.assertTrue(out.startswith(b'#include "ppu_recomp.h"\n'), out[:80])
            self.assertIn(b'// caf\xc3\xa9 \x81\n', out)
            self.assertIn(after.encode(), out)


if __name__ == '__main__':
    unittest.main()
