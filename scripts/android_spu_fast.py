#!/usr/bin/env python3
"""Android speed passes over the lifted SPU modules (game/spu/*/spu_recomp.c). Run once after lifting;
a module already done is left alone. Every pass keeps what the drain loop does at a trampoline step
that can matter (spu_task_launch_check, spu_drain.c): a pending interrupt still goes the long way, and
a branch to LS 0, which ends a job, is left alone. No title code registers a stack-reset entry.

1. A block ending in a jump to another block returned to the drain loop to be called from there: about
   30 instructions a hop, hundreds of thousands a second in the modules lifted as one function per
   block (those not lifted as regions, because a parent may patch them). Now a guaranteed tail call.

2. A computed branch that turns out to be to the function's own start. The sound synthesizer ends its
   resampling loop this way, a branchless "loop or return": `selb` picks the loop's address or the link
   register, then `bi`. Each pass of the loop went through spu_indirect_branch: its checks, a hash
   lookup, a compare of up to 1 KB of code. 15% of the sound thread. That compare exists because a job
   can patch a module it has loaded; a function looping on itself was checked on the way in.

3. --regcache <module:function,...>: the lifter's register cache (keep SPU registers in locals) on the
   named functions, which scripts/spu_pick_regcache.py found to come out shorter that way.
"""
import os, re, sys, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SPU = os.path.join(ROOT, 'game', 'spu')
MARK = '/* android_spu_fast */'
MARK_RC = '/* android_spu_fast: register cache */'
FUNC = re.compile(r'^void (\w+_spu_func_([0-9A-F]{8}))\(spu_context\* SPU_RESTRICT ctx\) \{$')
REGION = re.compile(r'^static void (\w+_spu_region_\w+)\(spu_context\* SPU_RESTRICT ctx, uint32_t entry\) \{$')
HOP = re.compile(r'ctx->pc = ([^;]+); g_spu_trampoline_fn = (\w+); return;')
INDIRECT = re.compile(r'ctx->pc = ctx->gpr\[(\d+)\]\._u32\[0\]; g_spu_trampoline_fn = spu_indirect_branch; return;')
PENDING = '__builtin_expect(ctx->int_enable && (ctx->event_status & ctx->event_mask), 0)'


def lifter():
    p = os.path.join(ROOT, 'tools', 'ps3recomp', 'tools')
    sys.path.insert(0, p)
    spec = importlib.util.spec_from_file_location('spu_lifter', os.path.join(p, 'spu_lifter.py'))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    # A channel operation can leave by longjmp (a halt, a restart of the dispatch), which would lose
    # registers held in locals: only local store access keeps them there.
    m._RC_PURE = re.compile(r'^(spu_ls_\w+|__builtin_expect)$')
    return m


def fast_function(name, addr, body):
    """(new body, hops, loops)"""
    hops = loops = 0
    label = f'loc_{addr}'
    has_label = any(l.strip() == label + ':' for l in body)
    out = []
    for l in body:
        if 'int_enable' in l:          # the path taken for a pending interrupt stays as it is
            out.append(l); continue
        def hop(m):
            nonlocal hops
            target = m[2]
            if target == 'spu_indirect_branch' or target.endswith('_00000000'):
                return m[0]
            hops += 1
            return (f'ctx->pc = {m[1]}; if ({PENDING}) {{ g_spu_trampoline_fn = {target}; return; }} '
                    f'__attribute__((musttail)) return {target}(ctx);')
        l2 = HOP.sub(hop, l)
        if not has_label:
            def self_loop(m):
                nonlocal loops
                loops += 1
                return (f'if ((ctx->gpr[{m[1]}]._u32[0] & SPU_LS_MASK) == ((0x{addr}u + ctx->ls_delta) & SPU_LS_MASK) && '
                        f'!(ctx->int_enable && (ctx->event_status & ctx->event_mask))) {{ ctx->pc = ctx->gpr[{m[1]}]._u32[0]; goto {label}; }} ' + m[0])
                # (the plain form follows: anything else is dispatched as before)
            l2 = INDIRECT.sub(self_loop, l2)
        out.append(l2)
    if loops and not has_label:
        out.insert(0, label + ':')
    return out, hops, loops


def module(path, cache, rc):
    raw = open(path, 'rb').read().decode('utf-8', 'surrogateescape')
    done_fast = MARK in raw; done_rc = MARK_RC in raw
    if done_rc or (done_fast and not cache):
        return 0, 0, 0
    crlf = '\r\n' in raw
    L = raw.replace('\r\n', '\n').split('\n')
    out = []; i = 0; hops = loops = cached = 0
    while i < len(L):
        l = L[i]
        f = FUNC.match(l); r = REGION.match(l)
        if not f and not r:
            out.append(l); i += 1; continue
        j = i + 1
        while L[j] != '}': j += 1
        body = L[i + 1:j]
        head = []
        if f:
            # one-line stubs into a region have nothing to do
            if not done_fast and (len(body) > 1 or 'region' not in (body[0] if body else '')):
                body, h, k = fast_function(f[1], f[2], body)
                hops += h; loops += k
        elif body and body[0].strip().startswith('switch (entry)'):
            k = 0
            while body[k] != '    }': k += 1
            head, body = body[:k + 1], body[k + 1:]
        name = (f or r)[1]
        if name in cache:
            body, pro = rc._reg_cache(body)
            if pro: cached += 1
            out.append(l); out.extend(pro); out.extend(head); out.extend(body); out.append('}')
        else:
            out.append(l); out.extend(head); out.extend(body); out.append('}')
        i = j + 1
    s = '\n'.join(out)
    if hops or loops or cached:
        if not done_fast: s = MARK + '\n' + s
        if cached: s = MARK_RC + '\n' + s
        open(path, 'wb').write((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', 'surrogateescape'))
    return hops, loops, cached


def main():
    args = sys.argv[1:]
    only = None; cache = set()
    while args:
        a = args.pop(0)
        if a == '--regcache':
            cache = set(open(args.pop(0)).read().split())
        elif a == '--only':
            only = args.pop(0).split(',')
    rc = lifter() if cache else None
    T = [0, 0, 0]; n = 0
    for m in sorted(os.listdir(SPU)):
        p = os.path.join(SPU, m, 'spu_recomp.c')
        if not os.path.exists(p) or (only and m not in only): continue
        r = module(p, cache, rc)
        n += 1
        for k in range(3): T[k] += r[k]
    print(f'{n} modules: {T[0]} tail-called hops, {T[1]} self-loop checks, {T[2]} functions with the register cache')


if __name__ == '__main__':
    main()
