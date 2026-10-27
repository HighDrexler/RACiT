#!/usr/bin/env python3
"""Naming pass: import stubs (NIDs) + tolua++ Lua bindings -> out/names.json.

Merges on top of out/ghidra_names.json (string-derived names) in the
ghidra_names.py format the lifter's --names flag reads:
    { "0x000CAA08": {"label": ..., "cident": ..., "src": ...}, ... }

Priority: import > tolua_api > tolua > string.

    py -3.12 scripts/name_pass.py
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "ps3recomp", "tools"))
from ppc_scan import Image, scan  # noqa: E402
from nid_database import compute_nid, get_default_db  # noqa: E402

# Names the NID database lacks, recovered by hashing candidate SDK names.
EXTRA_NIDS = """
cellHttpClientSetUserAgent cellHttpClientSetAutoRedirect cellHttpClientSetRecvTimeout
cellHttpClientSetSendTimeout cellHttpClientSetKeepAlive cellHttpClientSetResponseBufferMax
cellHttpClientSetSslCallback cellHttpClientSetHeader cellHttpRequestSetContentLength
cellHttpResponseGetStatusCode cellHttpResponseGetContentLength cellHttpClientSetCookieStatus
cellHttpClientSetProxy cellHttpClientSetVersion cellHttpClientSetPipeline
cellHttpClientSetAutoAuthentication cellHttpClientPollConnections
cellSslCertGetRsaPublicKeyModulus cellSslCertGetRsaPublicKeyExponent cellSslCertGetSubjectName
cellSslCertGetIssuerName cellSslCertGetNameEntryCount cellSslCertGetNameEntryInfo
cellSaveDataUserListAutoLoad cellSaveDataUserListAutoSave
sceNpManagerGetAvatarUrl sceNpManagerRequestTicket
""".split()

# tolua++ runtime entry points (code addresses), identified from their call
# shapes in the generated tolua_*_open functions.
TOLUA_API = {
    0x53E670: "tolua_module",
    0x53DAFC: "tolua_beginmodule",
    0x53DAD4: "tolua_endmodule",
    0x53E454: "tolua_cclass",
    0x53DF74: "tolua_function",
    0x53DD78: "tolua_variable",
    0x53DBC4: "tolua_array",
    0x53E00C: "tolua_constant",
    0x53E87C: "tolua_usertype",
}
# Hand-identified from Ghidra decompiles (analysis/decomp_*.txt).
MANUAL = {
    0x695F60: "job_submit",              # generic SPU job submit, returns a handle (33 callers)
    0x696614: "job_wait",                # wait on one job handle: 1 ms spin, then usleep(30)
    0x660818: "batch_last_submitted",    # obj+0x318 - 1 under an lv2 mutex
    0x660924: "batch_wait",              # poll obj+0x31C >= id, usleep(100) per spin
    0x5C52A8: "batch_drain_all",         # wait for every submitted batch, then func_005C50AC(0x41)
    0x4DFDF0: "render_build_views",      # per-view frame build: ~9 submit/wait round trips
    0x677AD0: "r2o_init",                # igR2O job descriptor (SPU code 0xB04E80, prio 0x4040FFFF)
    0x677C0C: "r2o_kick",                # submit the R2O job when [base]+0x29A is set
    0x677B44: "r2o_complete",            # job_wait on the R2O handle, post-process results
    0x677AA4: "r2o_reset",
    0x677950: "r2o_post",
    0x94F178: "r2o_frame_kick",          # func_00671CB8 then r2o_kick
    # The other submit/wait pairs in render_build_views, named by the "igXxx"
    # job string their module references (pairs share module TOC data).
    0x5F9C48: "tie_submit",              0x5F9900: "tie_wait",
    0x649D7C: "moby_prerender_submit",   0x645994: "moby_prerender_wait",
    0x660090: "particle_submit",         0x65EF80: "particle_wait",
    0x5D113C: "index_trim_submit",       0x5D0D38: "index_trim_wait",
    0x64F31C: "moby_particle_submit",    0x67D7E4: "guppy_prerender_submit",
    0x61E1BC: "loose_att_wait",          0x662CE8: "render_mats_wait",
    0x54FB18: "anim_vert_wait",
}
# Generated package open functions (reference "tolua: embedded Lua code").
TOLUA_OPEN = [0x51CE4, 0x25BA20, 0x2957C4, 0x51AFBC]


def key(a):
    return "0x%08X" % a


def cident(s):
    s = re.sub(r"[^A-Za-z0-9_]", "_", s.replace("::", "__"))
    return s if not s[0].isdigit() else "_" + s


def import_names(out):
    db = get_default_db()
    extra = {compute_nid(n): n for n in EXTRA_NIDS}
    n = 0
    for imp in json.load(open(os.path.join(ROOT, "out", "EBOOT.imports.json"))):
        nid = int(imp["nid"], 16)
        hit = db.lookup_nid(nid)
        name = hit[1] if hit else extra.get(nid)
        if not name:
            name = "%s_nid_%08x" % (imp["library"], nid)
        out[key(int(imp["stub"], 16))] = {"label": "%s::%s (import)" % (imp["library"], name),
                                          "cident": "imp_" + name, "src": "import"}
        n += 1
    return n


def tolua_names(img, sizes, out):
    n = 0

    def put(opd, label, kind):
        nonlocal n
        if not opd:
            return
        code = img.opd_code(opd)
        if not code or code not in sizes:
            return
        k = key(code)
        if k in out and out[k]["src"] in ("import", "tolua_api"):
            return
        out[k] = {"label": "lua %s %s" % (kind, label), "cident": "lua_" + cident(label),
                  "src": "tolua"}
        n += 1

    for open_fn in TOLUA_OPEN:
        stack = []
        pending = None  # name passed to tolua_module, entered by beginmodule
        for _pc, tgt, r in scan(img, open_fn, sizes[open_fn]):
            api = TOLUA_API.get(tgt)
            name = img.cstr(r[4]) if r[4] else None
            scope = "::".join(s for s in stack if s)
            q = lambda s: "%s::%s" % (scope, s) if scope else s  # noqa: E731
            if api == "tolua_beginmodule":
                stack.append(name or "")
            elif api == "tolua_endmodule":
                if stack:
                    stack.pop()
            elif api == "tolua_cclass" and name:
                put(r[7], q(name) + "::__gc", "collector")
            elif api == "tolua_function" and name:
                put(r[5], q(name), "function")
            elif api in ("tolua_variable", "tolua_array") and name:
                put(r[5], q(name) + ".get", "getter")
                put(r[6], q(name) + ".set", "setter")
    return n


def main():
    img = Image()
    funcs = json.load(open(os.path.join(ROOT, "analysis", "ghidra", "functions.json")))
    sizes = {int(f["addr"], 16): f["size"] for f in funcs}

    base = os.path.join(ROOT, "out", "ghidra_names.json")
    out = json.load(open(base)) if os.path.exists(base) else {}
    for k in out:
        out[k].setdefault("src", "string")

    ni = import_names(out)
    for a, nm in TOLUA_API.items():
        out[key(a)] = {"label": nm, "cident": nm, "src": "tolua_api"}
    nt = tolua_names(img, sizes, out)
    for a, nm in MANUAL.items():
        out[key(a)] = {"label": nm, "cident": nm, "src": "manual"}

    # cidents must be unique for optional renaming
    seen = {}
    for k in sorted(out):
        c = out[k]["cident"]
        if c in seen:
            out[k]["cident"] = "%s_%s" % (c, k[2:])
        seen[c] = k

    dst = os.path.join(ROOT, "out", "names.json")
    json.dump(out, open(dst, "w"), indent=1, sort_keys=True)
    from collections import Counter
    print("imports %d, tolua %d, total %d of %d functions -> %s"
          % (ni, nt, len(out), len(sizes), dst))
    print(Counter(v["src"] for v in out.values()))


if __name__ == "__main__":
    main()
