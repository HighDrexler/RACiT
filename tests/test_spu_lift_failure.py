"""Requested lift failures preserve registries; explicit exclusions still work."""
from contextlib import ExitStack
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from pipeline_fixture import ROOT, run_pipeline

spec = importlib.util.spec_from_file_location('lift_spu_jobs', ROOT / 'scripts' / 'lift_spu_jobs.py')
lift = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lift)


class LiftFailureTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        self.spu = self.root / 'spu'
        self.spu.mkdir()
        self.elf = self.root / 'EBOOT.ELF'
        self.elf.write_bytes(b'fixture')
        modules = self.root / 'modules.txt'
        modules.write_text('1000 200 reloc\n1100 200 reloc\n', encoding='utf-8')
        for key, value in dict(OUT_ROOT=str(self.root), SPU_ROOT=str(self.spu),
                               SUBMODULES=str(modules), SUB_ENTRIES=str(self.root / 'absent')).items():
            self.stack.enter_context(patch.object(lift, key, value))
        self.stack.enter_context(patch.object(lift, 'read_starts', return_value={}))
        self.stack.enter_context(patch.object(lift, 'update_inputs', return_value=None))
        self.stack.enter_context(patch.object(lift, 'segs', return_value=[]))
        self.stack.enter_context(patch.object(lift, 'find_headers', return_value=[0x1000, 0x1100]))
        self.stack.enter_context(patch.object(lift, 'all_headers', return_value=[0x1000, 0x1100, 0x1200]))
        self.stack.enter_context(patch.object(lift, 'rd', side_effect=lambda d, sg, a, n: bytes(n)))
        self.stack.enter_context(patch.object(lift, 'EXTRA_BLOCKS', [
            ('first', 0x1000, 64, 0x200, 150), ('second', 0x1100, 64, 0x200, 151)]))
        self.lifts = 0
        self.fail_at = 2
        self.stack.enter_context(patch.object(lift.subprocess, 'run', side_effect=self.fake_lifter))

    def fake_lifter(self, command, **kwargs):
        if command[2].endswith('wrap_spu_elf.py'):
            return subprocess.CompletedProcess(command, 0)
        self.lifts += 1
        if self.lifts == self.fail_at:
            return subprocess.CompletedProcess(command, 9, 'partial output', 'lifter failed')
        dst = Path(command[command.index('--output') + 1])
        (dst / 'spu_recomp.c').write_text('/* lifted */\n', encoding='utf-8')
        return subprocess.CompletedProcess(command, 0, '', '')

    def test_all_modes_preserve_existing_registry_after_partial_success(self):
        for mode, filename in (('', 'spu_jobs.c'), ('--extra', 'spu_extra.c'), ('--sub', 'spu_sub.c')):
            with self.subTest(mode=mode):
                self.lifts = 0
                registry = self.spu / filename
                registry.write_bytes(b'previous usable registry\n')
                with patch.object(sys, 'argv', ['lift_spu_jobs.py', str(self.elf)] + ([mode] if mode else [])):
                    with self.assertRaises(SystemExit) as failure:
                        lift.main()
                self.assertNotEqual(failure.exception.code, 0)
                self.assertIn('exit 9', str(failure.exception))
                self.assertEqual(registry.read_bytes(), b'previous usable registry\n')
                self.assertEqual(self.lifts, 2)
                self.assertFalse(registry.with_suffix('.tmp').exists())

    def test_only_does_not_lift_unrequested_job(self):
        with patch.object(sys, 'argv', ['lift_spu_jobs.py', str(self.elf), '--only', '0x1000']):
            lift.main()
        self.assertEqual(self.lifts, 1)
        self.assertIn('job_001000_spu_recomp_register', (self.spu / 'spu_jobs.c').read_text())
        self.assertNotIn('job_001100', (self.spu / 'spu_jobs.c').read_text())

    def test_sub_exclusions_do_not_attempt_lifting(self):
        Path(lift.SUBMODULES).write_text('C17B00 A00 reloc\nAD8F80 3F580 reloc\n1000 100 reloc\n', encoding='utf-8')
        lift.main_sub(b'', [])
        self.assertEqual(self.lifts, 0)
        self.assertTrue((self.spu / 'spu_sub.c').exists())


class PipelineFailureTests(unittest.TestCase):
    def test_spu_failure_stops_before_publication_or_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            registry = root / 'game' / 'spu' / 'spu_jobs.c'
            registry.parent.mkdir(parents=True)
            registry.write_bytes(b'previous registry')
            result, calls = run_pipeline(root, fail_spu=True)
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertEqual(registry.read_bytes(), b'previous registry')
            self.assertTrue(calls[-1][0][2].endswith('lift_spu_jobs.py'))
            self.assertFalse(list((root / 'out').glob('.lift.*')))


if __name__ == '__main__':
    unittest.main()
