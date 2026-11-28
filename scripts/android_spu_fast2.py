#!/usr/bin/env python3
"""Second set of Android speed passes over the lifted SPU modules (game/spu/*/spu_recomp.c), run after
scripts/android_spu_fast.py. A module already done (MARK below) is left alone. 2026-11-26.

1. shufb(x, x, c) -> spu_shufb_aa(x, c): both halves of the table are the same quadword, so the NEON
   form needs one table register (spu_helpers.h), where the two-register table made the compiler copy x
   into the register after it first. A third of the lifted shufbs.

2. Loops with side entries. A region (one C function for several SPU blocks, entered through a switch on
   the entry address) has a case for every block start the function finder saw, and many of them lie
   inside a loop. A switch that can jump into the middle of a loop makes the loop irreducible, and LLVM
   then does no loop work on it at all: nothing is hoisted (the shufb control preparation, the local
   store base), and the SPU registers stay in memory, loaded and stored around every instruction. In
   CollNarrow's transform loop (job_B8A980 0x120, which the main thread runs every frame) that was 512
   ARM instructions a pass; reducible, 360.
   Those side entries are used: an interrupt pending at an inner loop's back edge resumes there, a job
   call made inside a loop returns there (spu_drain_call's return pc), and a computed branch may land
   there. So each region with side entries is split in two: the region keeps the entries that leave its
   loops reducible (checked on a line-level control-flow graph: every retreating edge of a depth-first
   walk goes to a line that dominates its source), and a copy of the whole region as it was,
   `<region>_cold`, takes the others -- the stubs for those addresses call it. The copy runs only until
   it reaches a label the region takes (a goto to it, or falling into it): there it stores its register
   locals back and goes on in the region from that label. Same code from that label either way, so the
   same result. (The first version, 0.1.0.1, compiled the copies for size and stayed in them: in the
   Fongoid fight CollBroad's loop resumed in its copy after every interrupt and job call.) The first entry
   into a copy is logged once per region (`[spu-cold]`).

--check: report what would change, write nothing.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SPU = os.path.join(ROOT, 'game', 'spu')
MARK = '/* android_spu_fast2: shufb_aa, loop entries */'
REGION = re.compile(r'^static void (\w+_spu_region_\w+)\(spu_context\* SPU_RESTRICT ctx, uint32_t entry\) \{$')
CASE = re.compile(r'^\s*case 0x([0-9A-F]+)u: goto (loc_[0-9A-F]{8});$')
LABEL = re.compile(r'^(loc_[0-9A-F]{8}):$')
GOTO = re.compile(r'\bgoto (loc_[0-9A-F]{8});')
AA = re.compile(r'\bspu_shufb\((ctx->gpr\[\d+\]|_r\d+), \1, ')


def top_level(line):
    """The line with comments and every {...} block taken out."""
    s = re.sub(r'/\*.*?\*/', '', line)
    prev = None
    while prev != s:
        prev = s
        s = re.sub(r'\{[^{}]*\}', '', s)
    return s


def ends_flow(line):
    """True if control never falls through to the next line: its last top-level statement is a goto or a
    return (a line wholly in braces is looked at inside them; an if's braced body counts as one statement
    that may not run). Taking a line as falling through when it does not only adds an edge, which can
    only make a graph look less reducible: the safe way to be wrong."""
    s = re.sub(r'/\*.*?\*/', '', line).strip()
    while s.startswith('{') and s.endswith('}'):
        depth = 0; whole = True
        for n, ch in enumerate(s):
            depth += ch == '{'; depth -= ch == '}'
            if depth == 0 and n < len(s) - 1: whole = False; break
        if not whole: break
        s = s[1:-1].strip()
    prev = None
    while prev != s:
        prev = s
        s = re.sub(r'\{[^{}]*\}', ';', s)
    stmts = [x.strip() for x in s.split(';') if x.strip()]
    if not stmts: return False
    last = stmts[-1]
    if last.startswith('if') or last.startswith('else'): return False
    return bool(last.startswith('goto loc_') or last.startswith('SPU_RET') or re.search(r'\breturn\b', last))


def cfg(body):
    """Basic blocks of a region body: (starts, succ, label_block)."""
    n = len(body)
    label_line = {}
    for i, l in enumerate(body):
        m = LABEL.match(l.strip())
        if m: label_line[m[1]] = i
    leaders = {0}
    for i, l in enumerate(body):
        if LABEL.match(l.strip()): leaders.add(i)
        if GOTO.search(l) or ends_flow(l):
            if i + 1 < n: leaders.add(i + 1)
    starts = sorted(leaders)
    block_of = {}
    for b, s in enumerate(starts):
        e = starts[b + 1] if b + 1 < len(starts) else n
        for i in range(s, e): block_of[i] = b
    succ = [set() for _ in starts]
    for b, s in enumerate(starts):
        e = starts[b + 1] if b + 1 < len(starts) else n
        for i in range(s, e):
            for lab in GOTO.findall(body[i]):
                if lab in label_line: succ[b].add(block_of[label_line[lab]])
        last = body[e - 1]
        if not ends_flow(last) and e < n:
            succ[b].add(block_of[e])
    label_block = {lab: block_of[i] for lab, i in label_line.items()}
    return starts, succ, label_block


def reducible(succ, entries):
    """Virtual root -> entries. Reducible iff every retreating edge of a DFS targets a dominator of its
    source."""
    N = len(succ); R = N
    adj = [sorted(s) for s in succ] + [sorted(set(entries))]
    # iterative DFS: preorder ancestors on the stack, postorder for dominators
    state = [0] * (N + 1); order = []; parent = [-1] * (N + 1); onstack = [False] * (N + 1)
    retreat = []
    stack = [(R, 0)]; state[R] = 1; onstack[R] = True
    while stack:
        v, k = stack[-1]
        if k < len(adj[v]):
            stack[-1] = (v, k + 1)
            w = adj[v][k]
            if state[w] == 0:
                state[w] = 1; onstack[w] = True; parent[w] = v; stack.append((w, 0))
            elif onstack[w]:
                retreat.append((v, w))
        else:
            stack.pop(); onstack[v] = False; order.append(v)
    rpo = order[::-1]
    idx = {v: i for i, v in enumerate(rpo)}
    preds = [[] for _ in range(N + 1)]
    for v in rpo:
        for w in adj[v]: preds[w].append(v)
    idom = [-1] * (N + 1); idom[R] = R
    def inter(a, b):
        while a != b:
            while idx[a] > idx[b]: a = idom[a]
            while idx[b] > idx[a]: b = idom[b]
        return a
    changed = True
    while changed:
        changed = False
        for v in rpo[1:]:
            ps = [p for p in preds[v] if idom[p] != -1]
            if not ps: continue
            d = ps[0]
            for p in ps[1:]: d = inter(p, d)
            if idom[v] != d: idom[v] = d; changed = True
    def dominates(a, b):
        while True:
            if a == b: return True
            if b == R: return False
            b = idom[b]
    return all(dominates(w, v) for v, w in retreat)


def split_points(body, cases):
    """cases: [(addr, label)] -> set of addrs to move to the cold copy."""
    starts, succ, label_block = cfg(body)
    entries = [0] + [label_block[lab] for _, lab in cases if lab in label_block]
    if reducible(succ, entries) or not reducible(succ, [0]):   # nothing to gain, or loops tangled without them
        return set(), []
    keep = [0]; cold = set()
    for addr, lab in cases:
        if lab not in label_block: continue
        if reducible(succ, keep + [label_block[lab]]): keep.append(label_block[lab])
        else: cold.add(addr)
    return cold, loop_heads(body, label_block, succ, keep, {lab for _, lab in cases})


def loop_heads(body, label_block, succ, keep, have):
    """Labels a backward goto targets (loop heads) that the region can also take as entries and stay
    reducible: a cold copy goes on in the region at the next of them it reaches."""
    pos = {}
    for i, l in enumerate(body):
        m = LABEL.match(l.strip())
        if m: pos[m[1]] = i
    heads = []
    for i, l in enumerate(body):
        for lab in GOTO.findall(l):
            if lab in pos and pos[lab] <= i and lab not in have and lab not in heads: heads.append(lab)
    extra = []
    for lab in sorted(heads, key=lambda x: pos[x]):
        if reducible(succ, keep + [label_block[lab]]):
            keep.append(label_block[lab]); extra.append(lab)
    return extra


def module(path, check):
    raw = open(path, 'rb').read().decode('utf-8', 'surrogateescape')
    if MARK in raw:
        return None
    crlf = '\r\n' in raw
    L = raw.replace('\r\n', '\n').split('\n')
    # pass 1: regions and their cold entries
    cold_of = {}
    i = 0
    while i < len(L):
        m = REGION.match(L[i])
        if not m: i += 1; continue
        j = i + 1
        while L[j] != '}': j += 1
        body = L[i + 1:j]
        k = 0
        while k < len(body) and not body[k].strip().startswith('switch (entry)'): k += 1
        if k < len(body):
            e = k
            while body[e] != '    }': e += 1
            cases = [(int(c[1], 16), c[2]) for c in (CASE.match(x) for x in body[k:e]) if c]
            cold, extra = split_points(body[e + 1:], cases)
            if cold: cold_of[m[1]] = (cold, i, j, k, e, extra)
        i = j + 1
    n_aa = sum(len(AA.findall(l)) for l in L)
    stats = (len(cold_of), sum(len(v[0]) for v in cold_of.values()),
             sum(v[2] - v[1] for v in cold_of.values()), n_aa)
    if check:
        return stats
    out = []; i = 0
    while i < len(L):
        l = L[i]
        m = REGION.match(l)
        if m and m[1] in cold_of:
            cold, a, b, k, e, extra = cold_of[m[1]]
            body = L[a + 1:b]
            body = [AA.sub(r'spu_shufb_aa(\1, ', x) for x in body]
            sw = [x for x in body[k + 1:e] if not (CASE.match(x) and int(CASE.match(x)[1], 16) in cold)]
            d = next(n for n, x in enumerate(sw) if x.strip() == 'default: break;')
            sw = sw[:d] + [f'    case 0x{int(lab[4:], 16):X}u: goto {lab};   /* a loop head (android_spu_fast2) */'
                           for lab in extra] + sw[d:]
            hot = body[:k + 1] + sw + body[e:]
            out.append(l); out.extend(hot); out.append('}')
            name = m[1]
            out.append('/* android_spu_fast2: the entries inside its loops (an interrupt resumed at an inner loop\'s back edge,')
            out.append(' * a job call returning into a loop, a computed branch): runs until a label the region takes, then goes on there */')
            out.append(f'__attribute__((noinline)) static void {name}_cold(spu_context* SPU_RESTRICT ctx, uint32_t entry) {{')
            log = (f'    {{ static int s_seen; if (!s_seen) {{ s_seen = 1; extern void spu_cold_entry_note(const char*, uint32_t); '
                   f'spu_cold_entry_note("{name}", entry); }} }}')
            pro = body[:k]
            # the region's own entries: its start and the cases it kept
            start = int(name.rsplit('_', 1)[1], 16)
            kept = ({start} | {int(c[1], 16) for c in (CASE.match(x) for x in body[k:e]) if c}
                    | {int(lab[4:], 16) for lab in extra}) - set(cold)
            regs = re.findall(r'\b_r(\d+) = ctx->gpr\[\1\]', ' '.join(pro))
            flush = ' '.join(f'ctx->gpr[{r}] = _r{r};' for r in regs)
            def go(addr):
                return f'{{ {flush}{" " if flush else ""}{name}(ctx, 0x{addr:X}u); return; }}'
            cbody = []
            for x in body[e + 1:]:
                lm = LABEL.match(x.strip())
                if lm and int(lm[1][4:], 16) in kept and int(lm[1][4:], 16) != start:
                    cbody.append('        ' + go(int(lm[1][4:], 16)))   # falling through into it
                x = GOTO.sub(lambda g: go(int(g[1][4:], 16)) if int(g[1][4:], 16) in kept else g[0], x)
                cbody.append(x)
            out.extend(pro); out.append(log); out.extend(body[k:e + 1]); out.extend(cbody); out.append('}')
            i = b + 1
            continue
        mm = re.match(r'^    (\w+_spu_region_\w+)\(ctx, 0x([0-9A-F]+)u\);$', l)
        if mm and mm[1] in cold_of and int(mm[2], 16) in cold_of[mm[1]][0]:
            out.append(f'    {mm[1]}_cold(ctx, 0x{mm[2]}u);')
            i += 1; continue
        out.append(AA.sub(r'spu_shufb_aa(\1, ', l))
        i += 1
    s = MARK + '\n' + '\n'.join(out)
    open(path, 'wb').write((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', 'surrogateescape'))
    return stats


def main():
    check = '--check' in sys.argv
    only = None
    if '--only' in sys.argv: only = sys.argv[sys.argv.index('--only') + 1].split(',')
    T = [0, 0, 0, 0]; n = 0; per = []
    for m in sorted(os.listdir(SPU)):
        p = os.path.join(SPU, m, 'spu_recomp.c')
        if not os.path.exists(p) or (only and m not in only): continue
        r = module(p, check)
        if r is None: continue
        n += 1
        for k in range(4): T[k] += r[k]
        if r[0]: per.append((r[2], m, r[0], r[1]))
    for lines, m, nr, ne in sorted(per, reverse=True)[:25]:
        print(f'  {m:26} {nr:3} regions split, {ne:4} entries to the cold copy, {lines:6} lines copied')
    print(f'{n} modules: {T[0]} regions split ({T[1]} entries moved, {T[2]} lines copied), {T[3]} shufb(x, x, c)'
          + (' [check only]' if check else ''))


if __name__ == '__main__':
    main()
