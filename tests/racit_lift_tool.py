"""The Rust lift tools (rust/racit-lift: patch-lift and check-lift-hooks), built once for the tests."""
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CRATE = ROOT / 'rust' / 'racit-lift'
TARGET = Path(os.environ.get('CARGO_TARGET_DIR') or CRATE / 'target')
_built = False


def tool(name):
    """The path of a built tool (cargo builds it the first time)."""
    global _built
    if not _built:
        subprocess.run(['cargo', 'build', '--release', '--quiet', '--manifest-path', str(CRATE / 'Cargo.toml'),
                        '--target-dir', str(TARGET)], check=True)
        _built = True
    return str(TARGET / 'release' / (name + ('.exe' if os.name == 'nt' else '')))


def patch_lift(*args):
    """patch-lift with this checkout as its root."""
    return [tool('patch-lift'), '--root', str(ROOT)] + [str(a) for a in args]
