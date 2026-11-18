"""Upgrade only the known lifter vperm pattern; preserve all other lifted code.
The lifter emits this helper directly on future regeneration. Idempotent.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1] / 'game/src/recomp'
pattern = re.compile(
    r'uint8_t tmp\[16\]; for \(int i = 0; i < 16; i\+\+\) \{ '
    r'uint8_t sel = c\[i\] & 0x1F; tmp\[i\] = \(sel < 16\) \? a\[sel\] : b\[sel - 16\]; \} '
    r'memcpy\(d, tmp, 16\);')
changes = []
for path in sorted(root.glob('*.cpp')):
    original = path.read_bytes()
    # The copied lift contains legacy comment bytes; latin1 round-trips them.
    source = original.decode('latin1')
    # Preserve register-pointer setup and prior port fixes around the op.
    updated, count = pattern.subn('ppu_vperm_bytes(d, a, b, c);', source)
    if 'uint8_t sel = c[i] & 0x1F;' in updated:
        raise SystemExit(f'Unrecognized vperm pattern in {path.name}; no files changed.')
    if count:
        newline = '\r\n' if b'\r\n' in original else '\n'
        updated = updated.replace('#include "ppu_recomp.h"', '#include "ppu_recomp.h"' + newline + '#include "ppu_vmx_permute.h"', 1)
        changes.append((path, updated.encode('latin1'), count))
for path, data, count in changes:
    path.write_bytes(data)
    print(f'{path.name}: {count} vperm sites')
print(f'Updated {sum(count for _, _, count in changes)} sites in {len(changes)} files.')
