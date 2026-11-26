"""Run the pipeline with fake tools, without game input, lifting, or building."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run_pipeline(root, *, fail_spu=False, jobs=None, cmake_jobs=None):
    (root / 'scripts').mkdir()
    shutil.copyfile(ROOT / 'scripts' / 'pipeline.sh', root / 'scripts' / 'pipeline.sh')
    (root / 'input').mkdir()
    for name in ('EBOOT.ELF', 'libsre.prx'):
        (root / 'input' / name).write_bytes(b'fixture')
    tool = root / 'tool.py'
    tool.write_text('''import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['CALL_LOG'], 'a') as log:
    log.write(json.dumps([args, os.environ.get('JOBS')]) + '\\n')
if '--identify-profile' in args:
    print('BCUS98124-1.00')
if any(a.endswith('ppu_loader.py') for a in args):
    out = pathlib.Path(args[args.index('--output') + 1])
    (out / 'EBOOT.functions.json').write_text('[{"start": 1}]')
if any(a.endswith('lift_spu_jobs.py') for a in args) and os.environ.get('FAIL_SPU'):
    sys.exit(7)
''', encoding='utf-8')
    # CMake is also a fake: the pipeline can reach its build command safely.
    cmake = root / 'cmake'
    cmake.write_text('#!/bin/sh\nexec "$PIPELINE_TEST_PYTHON" "$PIPELINE_TEST_TOOL" "$@"\n', encoding='utf-8')
    cmake.chmod(0o755)
    env = os.environ.copy()
    for key in ('JOBS', 'CMAKE_BUILD_PARALLEL_LEVEL', 'SKIP_DESKTOP', 'BUILD_DIR'):
        env.pop(key, None)
    env.update(PY=f'{sys.executable} {tool}', LIBSRE=str(root / 'input' / 'libsre.prx'),
               CALL_LOG=str(root / 'calls.jsonl'), PATH=str(root) + os.pathsep + env['PATH'],
               PIPELINE_TEST_PYTHON=sys.executable, PIPELINE_TEST_TOOL=str(tool),
               FAIL_SPU='1' if fail_spu else '')
    if jobs is not None:
        env['JOBS'] = jobs
    if cmake_jobs is not None:
        env['CMAKE_BUILD_PARALLEL_LEVEL'] = cmake_jobs
    result = subprocess.run(['bash', str(root / 'scripts' / 'pipeline.sh')], env=env,
                            capture_output=True, text=True)
    log = root / 'calls.jsonl'
    calls = [json.loads(line) for line in log.read_text(encoding='utf-8').splitlines()] if log.exists() else []
    return result, calls
