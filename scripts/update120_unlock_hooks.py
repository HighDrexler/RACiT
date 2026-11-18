"""update120_unlock_hooks.py RECOMP: add the frame-rate unlock hooks
(patch_lift.py steps 23, 24 and 25 for the disc) to the 1.20 hook recipe.

RECOMP is an unhooked 1.20 lift (ppu_recomp_*.cpp straight from the lifter).
Each function below is located by its 1.20 address (matched to the disc's
function with tools/racit_update120_fndiff.py: identical normalized code),
given the same edit the disc build gets, and written to
versions/BCUS98124-1.20-hooks.json as a fingerprinted edit. Counts must match
the disc's, or nothing is written.
"""
import difflib, hashlib, json, os, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECIPE = ROOT / 'versions' / 'BCUS98124-1.20-hooks.json'

CLOCK_TICK = '0066AD24'          # disc 0066A424
FRAME_LIMIT = '005C54A0'         # disc 005C4C00
# disc address -> 1.20 address, then the disc's counts.
F60_LOADS = {'000106FC': 1, '000119A8': 1, '00012F40': 2, '00016B6C': 2, '00016F14': 1,
             '00018E54': 1, '000190C0': 1, '00019160': 3, '0001BFD8': 15, '0001F694': 1,
             '0001FD68': 1, '0001FE0C': 1, '0001FF64': 2, '00020DCC': 2, '00022798': 1,
             '00023DE4': 2}
G60_GATES = {'00023DE4': 2, '00022798': 1, '00012F40': 2, '00019160': 2, '00018E54': 1,
             '00016B6C': 2, '00016F14': 1, '0001BFD8': 1, '000BC060': 1, '00148DA8': 1,
             '0010DDBC': 1, '00169EE0': 1, '002B78C0': 1, '002B9C20': 1, '002BF588': 1,
             '00384B90': 1, '003861B0': 1, '00473930': 1, '0046DED8': 1}
DECLS = ['extern "C" void racacit_clock_tick(ppu_context* ctx);',
         'extern "C" uint32_t racacit_frame60_at(uint32_t base);',
         'extern "C" int racacit_new60(void);',
         'extern "C" int racacit_frame_limit(ppu_context* ctx);']

f60_load = re.compile(r'vm_read32\(ctx->gpr\[(\d+)\] \+ 0xD0\)')
f60_br = re.compile(r'^\s*if \(\(!\(\(ctx->cr >> \d+\) & 2\)\)\) goto loc_[0-9A-F]+;$')


def clock(fn):
    if 'ppu_timebase_now()' not in fn:
        sys.exit('func_%s does not read the timebase: not the game clock' % CLOCK_TICK)
    head = fn.index('{\n') + 2
    return fn[:head] + '        racacit_clock_tick(ctx);   /* racacit: frame-rate unlock */\n' + fn[head:]


def limiter(fn):
    if 'ctx->gpr[2] + -0x5120' not in fn or 'ctx->gpr[30] + -0x7FF8' not in fn or '+ 0x2D8)' not in fn:
        sys.exit('func_%s does not read the display deadline: not the frame limiter' % FRAME_LIMIT)
    head = fn.index('{\n') + 2
    return fn[:head] + '        if (racacit_frame_limit(ctx)) return;   /* racacit: frame-rate unlock */\n' + fn[head:]


def frame60(addr, fn):
    lines = fn.split('\n')
    if addr in G60_GATES:
        gates = 0
        for k, ln in enumerate(lines):
            if not f60_load.search(ln):
                continue
            for j in range(k + 1, min(k + 9, len(lines))):
                if not f60_br.match(lines[j]):
                    continue
                between = '\n'.join(lines[k:j])
                if '/_b' in between or re.search(r'& 0x[0-9A-F]+\b|ppc_rlwinm', between):
                    lines[j] = lines[j].replace(') goto ', ' || !racacit_new60()) goto ', 1)
                    lines[k] = f60_load.sub(lambda m: 'racacit_frame60_at((uint32_t)ctx->gpr[%s])' % m.group(1), lines[k])
                    gates += 1
                break
        if gates != G60_GATES[addr]:
            sys.exit('func_%s: %d frame gates, expected %d' % (addr, gates, G60_GATES[addr]))
    fn = '\n'.join(lines)
    if addr in F60_LOADS:
        n = len(f60_load.findall(fn)) + fn.count('racacit_frame60_at(')
        if n != F60_LOADS[addr]:
            sys.exit('func_%s: %d frame-counter loads, expected %d' % (addr, n, F60_LOADS[addr]))
        fn = f60_load.sub(lambda m: 'racacit_frame60_at((uint32_t)ctx->gpr[%s])' % m.group(1), fn)
    return fn


def main(recomp):
    want = {CLOCK_TICK, FRAME_LIMIT} | set(F60_LOADS) | set(G60_GATES)
    found = {}
    for path in sorted(Path(recomp).glob('ppu_recomp_*.cpp')):
        src = path.read_text(encoding='utf-8')
        for m in re.finditer(r'(?m)^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{', src):
            if m.group(1) in want:
                end = src.index('\n}\n', m.end()) + 3
                found[m.group(1)] = src[m.start():end]
    missing = want - set(found)
    if missing:
        sys.exit('not in this lift: ' + ', '.join(sorted(missing)))
    recipe = json.loads(RECIPE.read_text(encoding='utf-8'))
    have = {h['function'] for h in recipe['hooks']}
    if have & want:
        sys.exit('already in the recipe: ' + ', '.join(sorted(have & want)))
    for addr in sorted(want):
        before = found[addr]
        if 'racacit_' in before:
            sys.exit('func_%s is not a plain lift' % addr)
        after = (clock(before) if addr == CLOCK_TICK else limiter(before) if addr == FRAME_LIMIT
                 else frame60(addr, before))
        a = before.splitlines(keepends=True)
        b = after.splitlines(keepends=True)
        edits = [[i1, i2 - i1, ''.join(b[j1:j2])]
                 for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes()
                 if tag != 'equal']
        recipe['hooks'].append({'function': addr,
                                'before_sha256': hashlib.sha256(before.encode('utf-8')).hexdigest(),
                                'after_sha256': hashlib.sha256(after.encode('utf-8')).hexdigest(),
                                'edits': edits})
    for d in DECLS:
        if d not in recipe['preamble']:
            recipe['preamble'] = recipe['preamble'].rstrip('\n') + '\n' + d + '\n'
    # The recipe's own layout: one top-level key per line, one hook per line.
    c = lambda v: json.dumps(v, separators=(',', ':'), ensure_ascii=False)
    top = ['%s:%s' % (c(k), c(v)) for k, v in recipe.items() if k != 'hooks']
    hooks = ',\n'.join(c(h) for h in recipe['hooks'])
    RECIPE.write_text('{\n' + ',\n'.join(top) + ',\n"hooks":[\n' + hooks + '\n]\n}\n', encoding='utf-8')
    print('added %d hooks' % len(want))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
