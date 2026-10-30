"""make_icon.py OUT.ico: the launcher's icon -- the RACiT emblem (assets/emblem_<size>.png,
16, 24, 32, 48, 64 and 256 px, rendered from assets/emblem.png) packed as a PNG-in-ICO.
Pure Python."""
import os, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
sizes = [16, 24, 32, 48, 64, 256]
pngs = [open(os.path.join(here, 'assets', 'emblem_%d.png' % z), 'rb').read() for z in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for z, p in zip(sizes, pngs):
    out += struct.pack('<BBBBHHII', z % 256, z % 256, 0, 0, 1, 32, len(p), off)
    off += len(p)
open(sys.argv[1], 'wb').write(out + b''.join(pngs))
print('wrote', sys.argv[1])
