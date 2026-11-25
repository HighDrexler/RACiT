//! Lift raw SPU job binaries embedded in the EBOOT as content-matched overlays. It replaced
//! scripts/lift_spu_jobs.py and writes the same files (checked on 1.00 and 1.20); the modules are lifted in parallel
//! (each through tools/ps3recomp's wrap_spu_elf.py and spu_lifter.py, which stay Python).
//!
//!     lift-spu-jobs [EBOOT.ELF] [--only 0xB9B580,0xB8A980,...] [--root RACIT] [--jobs N]
//!     lift-spu-jobs [EBOOT.ELF] --extra | --sub | --policy
//!     lift-spu-jobs --merge-starts FILE
//!
//! Insomniac's igPm SPURS policy module DMAs each job binary into LS 0x100 and branches into
//! it, so dozens of different programs share the same local-store addresses. Each job starts
//! with a 16-byte GUID header (four `ila $r2,...`). This finds those headers, slices each job
//! up to the next header, lifts it at base 0x100 under its own image id, and writes
//! game/spu/spu_jobs.c, which registers the functions and declares each image an overlay
//! (spu_register_overlay_image): the runtime serves a job's lifted code only when the local
//! store holds that job's GUID and code words.
//!
//! --extra: code blocks a policy module copies into LS at run time -> spu_extra.c.
//! --sub: job modules a parent job loads above itself, lifted relocatable -> spu_sub.c.
//! --policy: the raw SPURS policy modules -> policy_modules.c (was lift_policy_modules.py).
//!
//! RACACIT_OUT, RACACIT_SPU_DIR: where the raw blocks and the lifts go (as the Python).
//! RACIT_PYTHON: the Python that runs the ps3recomp tools (default python3).
//!
//! Guest addresses are written as scripts/retarget.py rewrites them for another disc
//! (upper-case 0xXXXXXX at or above the SPU code range).

use racit_lift::spu::{self, UpdateInputs};
use racit_lift::{Result, parallel_map, replace_all, write_if_changed};
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const JOB_BASE: u64 = 0x100;
const IMAGE_ID_BASE: usize = 200;
// Raw job region (ELF images follow). Starts just past igPm's own runtime and policy module
// (0xAD8F80 / 0xAD9500, handled by EXTRA_BLOCKS / the PM lift): Bink's SPU video decoder
// lives at 0xADB700 and the old 0xB00000 start missed it.
const SCAN_LO: u64 = 0xAD9780;
const SCAN_HI: u64 = 0xBE3C80;
const MAX_SIZE: u64 = 0x30000;
// SPU ELF jobs already lifted by build_spu_workloads (image id, text EA, size, LS base): the
// WWS job manager stages them into a code buffer at LS 0x4000, so they too must be found by
// content. Sealed at boot from guest memory.
const ELF_OVERLAYS: &[(u32, u64, u64, u64)] = &[
    (2, 0xBFD480, 0xDC30, 0x4000), // nextsynth (SCREAM synth job)
    (3, 0xC0BB80, 0xB5D0, 0x4000), // lr1 (reverb job)
];

// Code blocks that a policy module copies somewhere in LS at run time, lifted straight from
// the EBOOT (name, EBOOT EA, size, LS base, image id). igPm puts the 0x580 bytes in front of
// its own image at LS 0x3F580 and calls into them (its job-loading/lock-line polling runtime).
const EXTRA_BLOCKS: &[(&str, u64, u64, u64, u32)] = &[("igpm_rt", 0xAD8F80, 0x580, 0x3F580, 150)];
// Lifter hints per block: job-style entry after the GUID header (not found by
// --auto-functions), and where code ends (variables follow).
fn extra_hints(name: &str) -> Vec<String> {
    match name {
        "igpm_rt" => ["--extra-funcs", "0x3F5A0", "--code-end", "0x3FA00"]
            .map(String::from)
            .to_vec(),
        _ => Vec::new(),
    }
}

const SUB_IMAGE_ID_BASE: usize = 400;
const SUB_ANCHOR: u64 = 0x30;
// Modules found at a second base that have their own lift already.
const SUB_SKIP: &[(u64, u64)] = &[(0xC17B00, 0xA00), (0xAD8F80, 0x3F580)];

struct Ctx {
    root: PathBuf,
    out_raw: PathBuf,
    spu_dir: PathBuf,
    python: String,
    jobs: usize,
}

impl Ctx {
    fn tools(&self) -> PathBuf {
        self.root.join("tools/ps3recomp/tools")
    }
}

/// One module to lift: its name, bytes, LS base, wrap entry and lifter arguments after
/// the usual ones.
struct Block {
    name: String,
    raw: Vec<u8>,
    base: u64,
    entry: u64,
    args: Vec<String>,
}

fn hex(x: u64) -> String {
    format!("{x:#x}")
}

/// wrap_spu_elf.py then spu_lifter.py on one block: Ok(false) when the lifter failed (its
/// log says why), Err when the wrapper did.
fn lift(c: &Ctx, b: &Block) -> Result<bool> {
    let bin_p = c.out_raw.join(format!("{}.bin", b.name));
    let elf_p = c.out_raw.join(format!("{}.elf", b.name));
    racit_lift::write(&bin_p, &b.raw)?;
    let wrap = Command::new(&c.python)
        .arg("-I")
        .arg(c.tools().join("wrap_spu_elf.py"))
        .arg(&bin_p)
        .args([
            "--out".as_ref(),
            elf_p.as_os_str(),
            "--base".as_ref(),
            hex(b.base).as_ref(),
        ])
        .args(["--entry", &hex(b.entry)])
        .output()
        .map_err(|e| format!("{}: {e}", c.python))?;
    if !wrap.status.success() {
        return Err(format!(
            "wrap_spu_elf.py failed for {} ({}): {}",
            b.name,
            wrap.status,
            String::from_utf8_lossy(&wrap.stderr)
        ));
    }
    let dst = c.spu_dir.join(&b.name);
    fs::create_dir_all(&dst).map_err(|e| format!("{}: {e}", dst.display()))?;
    let r = Command::new(&c.python)
        .arg("-I")
        .arg(c.tools().join("spu_lifter.py"))
        .arg("--auto-functions")
        .arg(&elf_p)
        .args([
            "--base",
            &hex(b.base),
            "--symbol-prefix",
            &format!("{}_", b.name),
            "--output",
        ])
        .arg(&dst)
        .args(&b.args)
        .output()
        .map_err(|e| format!("{}: {e}", c.python))?;
    racit_lift::write(
        &c.out_raw.join(format!("{}.lift.log", b.name)),
        &[r.stdout, r.stderr].concat(),
    )?;
    if !r.status.success() {
        return Ok(false);
    }
    let src_c = dst.join("spu_recomp.c");
    let mut src = racit_lift::read(&src_c)?;
    for h in ["spu_context.h", "spu_helpers.h"] {
        src = replace_all(
            &src,
            format!("\"../../runtime/spu/{h}\"").as_bytes(),
            format!("\"{h}\"").as_bytes(),
        );
    }
    racit_lift::write(&src_c, &src)?;
    Ok(true)
}

/// Every block lifted on its own thread; results in block order.
fn lift_all(c: &Ctx, blocks: &[Block]) -> Result<Vec<bool>> {
    parallel_map(blocks, c.jobs, |b| lift(c, b))
        .into_iter()
        .collect()
}

fn blob(name: &str, raw: &[u8]) -> String {
    let bytes: Vec<String> = raw.iter().map(|x| x.to_string()).collect();
    format!(
        "static const unsigned char {name}_bytes[{}] = {{{}}};",
        raw.len(),
        bytes.join(",")
    )
}

fn is_header(ws: &[u32]) -> bool {
    ws.iter().all(|w| (w >> 25) == 0x21 && (w & 0x7F) == 2)
}

fn find_headers(d: &[u8], sg: &[(u64, u64, u64)], version: Option<&UpdateInputs>) -> Vec<u64> {
    if let Some(v) = version {
        return v.jobs.iter().map(|j| j.0).collect();
    }
    (SCAN_LO..SCAN_HI)
        .step_by(16)
        .filter(|&a| spu::read(d, sg, a, 16).is_some_and(|b| is_header(&spu::be_words(b))))
        .collect()
}

fn all_headers(d: &[u8], sg: &[(u64, u64, u64)]) -> Vec<u64> {
    let mut hs = Vec::new();
    for &(va, off, fs) in sg {
        let mut a = (va + 15) & !15;
        while a + 16 < va + fs {
            let s = (off + a - va) as usize;
            if is_header(&spu::be_words(&d[s..s + 16])) {
                hs.push(a);
            }
            a += 16;
        }
    }
    hs.sort();
    hs
}

/// Entry points the interpreter was entered at (SPU_INTERP_STARTS=<file>, "<parent guid> <LS
/// addr>" per line), merged into scripts/spu_starts.txt by --merge-starts: indirect-call
/// targets --auto-functions cannot see. {parent guid: {LS addr}}
fn read_starts(root: &Path) -> BTreeMap<String, BTreeSet<u64>> {
    let mut out: BTreeMap<String, BTreeSet<u64>> = BTreeMap::new();
    if let Ok(text) = fs::read_to_string(root.join("scripts/spu_starts.txt")) {
        for line in text.lines() {
            let p: Vec<&str> = line
                .split('#')
                .next()
                .unwrap_or("")
                .split_whitespace()
                .collect();
            if p.len() >= 2
                && let Ok(a) = u64::from_str_radix(p[1], 16)
            {
                out.entry(p[0].to_string()).or_default().insert(a);
            }
        }
    }
    out
}

fn merge_starts(root: &Path, path: &Path) -> Result<()> {
    let mut cur = read_starts(root);
    let n0: usize = cur.values().map(BTreeSet::len).sum();
    let text = fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    for line in text.lines() {
        let p: Vec<&str> = line.split_whitespace().collect();
        if p.len() >= 2 {
            let a = u64::from_str_radix(p[1], 16).map_err(|e| format!("{}: {e}", p[1]))?;
            cur.entry(p[0].to_string()).or_default().insert(a);
        }
    }
    let mut out = String::from(
        "# Addresses the SPU interpreter was entered at: <job GUID at LS 0x100> <LS addr>.\n\
         # From SPU_INTERP_STARTS=<file>, merged by lift-spu-jobs --merge-starts <file>.\n",
    );
    for (g, addrs) in &cur {
        for a in addrs {
            out.push_str(&format!("{g} {a:05X}\n"));
        }
    }
    racit_lift::write(&root.join("scripts/spu_starts.txt"), out.as_bytes())?;
    let n1: usize = cur.values().map(BTreeSet::len).sum();
    println!("{n1} start(s) in scripts/spu_starts.txt ({} new)", n1 - n0);
    Ok(())
}

/// igPm clears local store 16 bytes a turn before each job: a plain loop, not two
/// trampoline hops per turn (a large part of a busy lane's time).
fn patch_igpm_clear(path: &Path) -> Result<()> {
    let old = [
        "void igpm_rt_spu_func_0003F90C(spu_context* SPU_RESTRICT ctx) {",
        "        ctx->gpr[4] = spu_ai(ctx->gpr[4], -1);",
        "        ctx->gpr[66] = spu_ceqi(ctx->gpr[4], -1);",
        "        if (ctx->gpr[66]._u32[0] == 0) { ctx->pc = 0x3F904; g_spu_trampoline_fn = igpm_rt_spu_func_0003F904; return; }",
    ];
    let new = [
        old[0],
        "        /* The job launch clears local store, 16 bytes a turn: as a plain loop here",
        "         * (scripts/lift_spu_jobs.py), not two trampoline hops per turn. */",
        "        for (;;) {",
        "            ctx->gpr[4] = spu_ai(ctx->gpr[4], -1);",
        "            ctx->gpr[66] = spu_ceqi(ctx->gpr[4], -1);",
        "            if (ctx->gpr[66]._u32[0] != 0) break;",
        "            spu_ls_write128(ctx, ctx->gpr[3]._u32[0] + 0x0, ctx->gpr[5]);",
        "            ctx->gpr[3] = spu_ai(ctx->gpr[3], 16);",
        "        }",
    ];
    let text = racit_lift::read(path)?;
    let eol = if text.contains(&b'\r') { "\r\n" } else { "\n" };
    let (a, b) = (old.join(eol) + eol, new.join(eol) + eol);
    if !racit_lift::contains(&text, a.as_bytes()) {
        return Err(
            "igpm_rt: the local store clear loop has changed: review patch_igpm_clear".into(),
        );
    }
    racit_lift::write(
        path,
        &racit_lift::replace_first(&text, a.as_bytes(), b.as_bytes()),
    )
}

fn main_jobs(
    c: &Ctx,
    d: &[u8],
    sg: &[(u64, u64, u64)],
    version: Option<&UpdateInputs>,
    only: Option<&BTreeSet<u64>>,
) -> Result<()> {
    let heads = find_headers(d, sg, version);
    let starts = read_starts(&c.root);
    let env_off = |k: &str| std::env::var(k).is_ok_and(|v| v == "0");
    let mut flags: Vec<String> = Vec::new();
    // One C function per run of blocks between calls: loops keep SPU registers in host
    // registers. Not for --sub modules, which a parent may patch past an entry's validated
    // span (see spu_lookup_fixed).
    if !env_off("RACACIT_SPU_REGIONS") {
        flags.push("--regions".into());
    }
    // SPU registers in C locals where no call sits in a loop: opt-in (RACACIT_SPU_REGCACHE=1).
    if std::env::var("RACACIT_SPU_REGCACHE").is_ok_and(|v| v == "1") {
        flags.push("--reg-cache".into());
    }
    // One float range check per straight-line run of float ops (arm64: SPU_FX_BLOCKS).
    if !env_off("RACACIT_SPU_FXBLOCKS") {
        flags.push("--fx-blocks".into());
    }
    // lqd/shufb/stqd to one address as one fused store
    if !env_off("RACACIT_SPU_STFUSE") {
        flags.push("--st-fuse".into());
    }

    let mut plan: Vec<(usize, u64, u64)> = Vec::new(); // (index, address, size)
    let mut blocks: Vec<Block> = Vec::new();
    for (i, &a) in heads.iter().enumerate() {
        if only.is_some_and(|o| !o.contains(&a)) {
            continue;
        }
        let nxt = heads.get(i + 1).copied().unwrap_or(SCAN_HI);
        let mut size = (nxt - a).min(MAX_SIZE);
        let mut name = format!("job_{a:06X}");
        if let Some(v) = version {
            let j = v
                .jobs
                .iter()
                .find(|j| j.0 == a)
                .ok_or("StopIteration: no update job")?;
            (name, size) = (j.1.clone(), j.2);
        }
        let raw = spu::read(d, sg, a, size)
            .ok_or_else(|| format!("{name}: not in a loaded segment"))?
            .to_vec();
        // igPm enters a job at (header word 6 & 0xFFFF) << 2 -- an indirect call
        // --auto-functions cannot see, so name it, or the job's whole top level runs in the
        // interpreter.
        let entry =
            ((u32::from_be_bytes(raw[0x18..0x1C].try_into().unwrap()) & 0xFFFF) << 2) as u64;
        let mut extra: BTreeSet<u64> = BTreeSet::new();
        if JOB_BASE < entry && entry < JOB_BASE + size {
            extra.insert(entry);
        }
        // ...and so are the places the interpreter was seen entering this job.
        let guid: String = raw[..16].iter().map(|b| format!("{b:02x}")).collect();
        if let Some(s) = starts.get(&guid) {
            extra.extend(
                s.iter()
                    .filter(|&&s| JOB_BASE + 0x20 <= s && s < JOB_BASE + size),
            );
        }
        let mut args = Vec::new();
        if !extra.is_empty() {
            args.push("--extra-funcs".to_string());
            args.push(extra.iter().map(|&x| hex(x)).collect::<Vec<_>>().join(","));
        }
        args.extend(flags.iter().cloned());
        plan.push((i, a, size));
        blocks.push(Block {
            name,
            raw,
            base: JOB_BASE,
            entry: JOB_BASE,
            args,
        });
    }
    let ok = lift_all(c, &blocks)?;
    let (mut decls, mut regs, mut blobs) = (Vec::new(), Vec::new(), Vec::new());
    for ((&(i, a, size), b), ok) in plan.iter().zip(&blocks).zip(ok) {
        let name = &b.name;
        if !ok {
            return Err(format!("{name}: lifter FAILED; registry unchanged. See out/spu_jobs/{name}.lift.log").into());
        }
        let img = IMAGE_ID_BASE + i;
        decls.push(format!("extern void {name}_spu_recomp_register(void);"));
        blobs.push(blob(name, &b.raw));
        regs.push(format!(
            "    spu_begin_image({img}); {name}_spu_recomp_register();\n    spu_register_overlay_image({img}, 0x{JOB_BASE:X}, {name}_bytes, {size});"
        ));
        println!("{name}: 0x{a:X} size 0x{size:X} -> image {img}");
    }
    let mut overlays = String::new();
    for &(i, ea, sz, b) in ELF_OVERLAYS {
        let ea = spu::mapped(version, ea)?;
        overlays.push_str(&format!(
            "    spu_register_overlay_image({i}, 0x{b:X}, vm_base + 0x{ea:X}u, 0x{sz:X});\n"
        ));
    }
    let text = format!(
        "/* Generated by scripts/lift_spu_jobs.py -- igPm job binaries as content-matched overlays. */\n\
         extern void spu_begin_image(int image_id);\n\
         extern void spu_register_overlay_image(int image_id, unsigned base, const unsigned char* bytes, unsigned size);\n\n\
         {}\n\n{}\n\n\
         void racacit_jobs_register_all(void)\n{{\n{}\n    spu_begin_image(0);\n}}\n\n\
         /* Called from main() once the EBOOT is in guest memory. */\n\
         extern unsigned char* vm_base;\n\
         void racacit_register_elf_overlays(void)\n{{\n{overlays}}}\n\n\
         __attribute__((constructor(200))) static void racacit_jobs_register_ctor(void)\n\
         {{\n    racacit_jobs_register_all();\n}}\n",
        decls.join("\n"),
        blobs.join("\n"),
        regs.join("\n")
    );
    write_if_changed(&c.spu_dir.join("spu_jobs.c"), text.as_bytes())
}

fn main_extra(
    c: &Ctx,
    d: &[u8],
    sg: &[(u64, u64, u64)],
    version: Option<&UpdateInputs>,
) -> Result<()> {
    let mut blocks = Vec::new();
    let mut eas = Vec::new();
    for &(name, ea, size, base, _) in EXTRA_BLOCKS {
        let mut ea = ea;
        if let Some(v) = version {
            let (mapped_ea, mapped_size) = v.payload(name)?;
            if mapped_size != size {
                return Err(format!("Extra payload size differs for {name}"));
            }
            ea = mapped_ea;
        }
        let raw = spu::read(d, sg, ea, size)
            .ok_or_else(|| format!("{name}: not in a loaded segment"))?
            .to_vec();
        eas.push(ea);
        blocks.push(Block {
            name: name.into(),
            raw,
            base,
            entry: base,
            args: extra_hints(name),
        });
    }
    let ok = lift_all(c, &blocks)?;
    let (mut decls, mut regs) = (Vec::new(), Vec::new());
    for (((&(name, _, size, base, img), b), ok), ea) in
        EXTRA_BLOCKS.iter().zip(&blocks).zip(ok).zip(eas)
    {
        if !ok {
            return Err(format!("{name}: lifter FAILED; registry unchanged. See out/spu_jobs/{name}.lift.log").into());
        }
        if b.name == "igpm_rt" {
            patch_igpm_clear(&c.spu_dir.join(name).join("spu_recomp.c"))?;
        }
        decls.push(format!("extern void {name}_spu_recomp_register(void);"));
        regs.push(format!(
            "    spu_begin_image({img}); {name}_spu_recomp_register(); spu_begin_image(0);"
        ));
        println!("{name}: 0x{ea:X} size 0x{size:X} at LS 0x{base:X} -> image {img}");
    }
    let mut overlays = String::new();
    for &(_, original_ea, size, base, img) in EXTRA_BLOCKS {
        let ea = spu::mapped(version, original_ea)?;
        overlays.push_str(&format!(
            "    spu_register_overlay_image({img}, 0x{base:X}, vm_base + 0x{ea:X}u, 0x{size:X});\n"
        ));
    }
    let text = format!(
        "/* Generated by scripts/lift_spu_jobs.py --extra -- LS-relocated policy-module code. */\n\
         extern void spu_begin_image(int image_id);\n\
         extern void spu_register_overlay_image(int image_id, unsigned base, const unsigned char* bytes, unsigned size);\n\
         extern unsigned char* vm_base;\n\n{}\n\n\
         __attribute__((constructor(201))) static void racacit_extra_register_ctor(void)\n{{\n{}\n}}\n\n\
         /* Content signatures come from the EBOOT in guest memory (main() calls this). */\n\
         void racacit_register_extra_overlays(void)\n{{\n{overlays}}}\n",
        decls.join("\n"),
        regs.join("\n")
    );
    write_if_changed(&c.spu_dir.join("spu_extra.c"), text.as_bytes())
}

/// Job modules loaded above a parent job (scripts/spu_submodules.txt), each lifted at the LS
/// base it was seen at -> game/spu/spu_sub.c.
fn main_sub(
    c: &Ctx,
    d: &[u8],
    sg: &[(u64, u64, u64)],
    version: Option<&UpdateInputs>,
) -> Result<()> {
    let heads = all_headers(d, sg);
    let starts = read_starts(&c.root);
    // (ea, base) -> {parent guid}, in the file's order
    let mut mods: Vec<((u64, u64), BTreeSet<String>)> = Vec::new();
    if let Ok(text) = fs::read_to_string(c.root.join("scripts/spu_submodules.txt")) {
        for line in text.lines() {
            let p: Vec<&str> = line
                .split('#')
                .next()
                .unwrap_or("")
                .split_whitespace()
                .collect();
            if p.len() >= 3 {
                let key = (
                    u64::from_str_radix(p[0], 16).map_err(|e| e.to_string())?,
                    u64::from_str_radix(p[1], 16).map_err(|e| e.to_string())?,
                );
                match mods.iter_mut().find(|(k, _)| *k == key) {
                    Some((_, s)) => {
                        s.insert(p[2].to_string());
                    }
                    None => mods.push((key, BTreeSet::from([p[2].to_string()]))),
                }
            }
        }
    }
    // Entry points as module offsets (scripts/dev/miss_bases.py --write).
    let mut offsets: HashMap<u64, BTreeSet<u64>> = HashMap::new();
    if let Ok(text) = fs::read_to_string(c.root.join("scripts/spu_submodule_entries.txt")) {
        for line in text.lines() {
            let p: Vec<&str> = line
                .split('#')
                .next()
                .unwrap_or("")
                .split_whitespace()
                .collect();
            if p.len() >= 2 {
                let (ea, o) = (
                    u64::from_str_radix(p[0], 16).map_err(|e| e.to_string())?,
                    u64::from_str_radix(p[1], 16).map_err(|e| e.to_string())?,
                );
                offsets.entry(ea).or_default().insert(o);
            }
        }
    }
    // One relocatable lift per module: a parent may load it at any base, and the lifted code
    // runs wherever the runtime finds it (ls_delta). Lift at the first base recorded from a
    // snapshot (else the lowest seen), and translate every base's interpreter starts into
    // that frame.
    let mut by_ea: BTreeMap<u64, Vec<(u64, &BTreeSet<String>)>> = BTreeMap::new();
    for ((ea, base), parents) in &mods {
        if SUB_SKIP.contains(&(*ea, *base)) || *base == JOB_BASE {
            continue;
        }
        by_ea.entry(*ea).or_default().push((*base, parents));
    }

    enum Step {
        Skip(u64),
        Lift {
            ea: u64,
            size: u64,
            seen: usize,
            hints: usize,
            block: usize,
        },
    }
    let mut steps = Vec::new();
    let mut blocks = Vec::new();
    for (&ea, seen) in &by_ea {
        let mut snap: Vec<u64> = seen
            .iter()
            .filter(|(_, ps)| ps.iter().any(|p| p != "reloc"))
            .map(|(b, _)| *b)
            .collect();
        snap.sort();
        let mut base = snap
            .first()
            .copied()
            .unwrap_or_else(|| seen.iter().map(|(b, _)| *b).min().unwrap());
        let nxt = heads
            .iter()
            .copied()
            .find(|&h| h > ea)
            .unwrap_or(ea + MAX_SIZE);
        let mut size = (nxt - ea).min(MAX_SIZE).min(0x40000 - base);
        let mut name = format!("sub_{ea:06X}_{base:05X}");
        let mut source_ea = ea;
        if let Some(v) = version {
            (source_ea, size) = v.payload(&name)?;
        }
        let Some(raw) = spu::read(d, sg, source_ea, size) else {
            steps.push(Step::Skip(ea));
            continue;
        };
        let raw = raw.to_vec();
        let mut hints: BTreeSet<u64> = BTreeSet::new();
        for (b, parents) in seen {
            for g in parents.iter() {
                if let Some(s) = starts.get(g) {
                    hints.extend(
                        s.iter()
                            .filter(|&&a| b + SUB_ANCHOR <= a && a < b + size)
                            .map(|a| a - b + base),
                    );
                }
            }
        }
        // Recorded entries from 0x20 on: a GUID-headed module is entered at (header word 6 &
        // 0xFFFF) << 2, often 0x20. Never 0..0x1F: that "function" runs the header words to a stop.
        if let Some(o) = offsets.get(&ea) {
            hints.extend(
                o.iter()
                    .filter(|&&o| 0x20 <= o && o < size)
                    .map(|o| o + base),
            );
        }
        let mut args = vec!["--reloc".to_string()];
        if !hints.is_empty() {
            args.push("--extra-funcs".into());
            args.push(hints.iter().map(|&x| hex(x)).collect::<Vec<_>>().join(","));
        }
        let mut entry = base;
        if version.is_some() && ea == 0xDB4180 {
            // The image order is checked when the registry is written (below).
            if source_ea != 0xDB4000
                || spu::read(d, sg, 0xCA45B8, 4) != Some(&(source_ea as u32).to_be_bytes()[..])
            {
                return Err("Changed update module loader pointer differs".into());
            }
            let next_header = spu::read(d, sg, source_ea + size, 16);
            if !next_header.is_some_and(|h| {
                spu::be_words(h)
                    .iter()
                    .all(|w| w & 0xFE00007F == 0x42000002)
            }) {
                return Err("Changed update module following header differs".into());
            }
            base = 0x100;
            name = "update_sub_DB4000_00100".into();
            let (e, code_end) = spu::changed_module(&raw)?;
            entry = e;
            args = [
                "--reloc",
                "--code-end",
                &hex(code_end),
                "--extra-funcs",
                &hex(e),
            ]
            .map(String::from)
            .to_vec();
        }
        steps.push(Step::Lift {
            ea,
            size,
            seen: seen.len(),
            hints: hints.len(),
            block: blocks.len(),
        });
        blocks.push(Block {
            name,
            raw,
            base,
            entry,
            args,
        });
    }
    let ok = lift_all(c, &blocks)?;
    let (mut decls, mut regs, mut blobs) = (Vec::new(), Vec::new(), Vec::new());
    let mut img = SUB_IMAGE_ID_BASE;
    for step in steps {
        match step {
            Step::Skip(ea) => println!("sub {ea:06X}: not in a loaded segment, skipped"),
            Step::Lift {
                ea,
                size,
                seen,
                hints,
                block,
            } => {
                let b = &blocks[block];
                let name = &b.name;
                if version.is_some() && ea == 0xDB4180 && img != 506 {
                    return Err("Changed update module image order differs".into());
                }
                if !ok[block] {
                    return Err(format!("{name}: lifter FAILED; registry unchanged. See out/spu_jobs/{name}.lift.log").into());
                }
                let base = b.base;
                decls.push(format!("extern void {name}_spu_recomp_register(void);"));
                blobs.push(blob(name, &b.raw));
                regs.push(format!(
                    "    spu_begin_image({img}); {name}_spu_recomp_register();\n    spu_register_overlay_image_at({img}, 0x{base:X}, {name}_bytes, {size}, 0x{SUB_ANCHOR:X});\n    spu_mark_overlay_relocatable({img});"
                ));
                println!(
                    "{name}: size 0x{size:X}, {seen} base(s) seen, {hints} entry hint(s) -> image {img}"
                );
                img += 1;
            }
        }
    }
    let text = format!(
        "/* Generated by scripts/lift_spu_jobs.py --sub -- job modules a parent job loads above itself,\n \
         * lifted relocatable (--reloc): served at whatever base the parent picks. */\n\
         extern void spu_begin_image(int image_id);\n\
         extern void spu_register_overlay_image_at(int image_id, unsigned base, const unsigned char* bytes, unsigned size, unsigned anchor);\n\
         extern void spu_mark_overlay_relocatable(int image_id);\n\n{}\n\n{}\n\n\
         __attribute__((constructor(202))) static void racacit_sub_register_ctor(void)\n{{\n{}\n    spu_begin_image(0);\n}}\n",
        decls.join("\n"),
        blobs.join("\n"),
        regs.join("\n")
    );
    write_if_changed(&c.spu_dir.join("spu_sub.c"), text.as_bytes())?;
    println!(
        "{} module(s) -> game/spu/spu_sub.c",
        img - SUB_IMAGE_ID_BASE
    );
    Ok(())
}

// ---- --policy: raw SPURS policy modules -------------------------------------------------
//
// SPURS policy modules are raw SPU code, not ELF files, so extract_spu_images.py misses them.
// cellSpurs loads one at LS 0xA00 and finds its lifted code by an FNV-1a-64 fingerprint over
// its raw bytes (spu_workload_fingerprint over sizePm). Each is sliced out of the EBOOT, lifted
// at 0xA00 with its own prefix, and registered under that fingerprint in policy_modules.c.

const PM_BASE: u64 = 0xA00;
const PM_IMAGE_ID_BASE: usize = 100; // clear of build_spu_workloads' 1..N
// name, guest address, size (from the boot log's AddWorkload lines)
const POLICY_MODULES: &[(&str, u64, u64)] = &[
    ("pm_wwsjob", 0x00C17B00, 11520), // "Wws_Job" SPURS job manager
    ("pm_igpm", 0x00AD9500, 560),     // Insomniac "igPm"
];

/// As runtime/spu/spu_workload.c, whose offset basis is 1469598103934665603, not the
/// standard FNV-1a 14695981039346656037.
fn fnv1a64(data: &[u8]) -> u64 {
    data.iter().fold(1469598103934665603u64, |h, &b| {
        (h ^ b as u64).wrapping_mul(0x100000001B3)
    })
}

fn main_policy(
    c: &Ctx,
    d: &[u8],
    sg: &[(u64, u64, u64)],
    version: Option<&UpdateInputs>,
) -> Result<()> {
    let mut blocks = Vec::new();
    let mut addrs = Vec::new();
    for &(name, addr, size) in POLICY_MODULES {
        let mut addr = addr;
        if let Some(v) = version {
            let (mapped, mapped_size) = v.payload(name)?;
            if mapped_size != size {
                return Err(format!("Policy payload size differs for {name}"));
            }
            addr = mapped;
        }
        let raw = spu::read(d, sg, addr, size)
            .ok_or_else(|| format!("0x{addr:X}+{size} is not inside a loaded segment"))?
            .to_vec();
        addrs.push(addr);
        blocks.push(Block {
            name: name.into(),
            raw,
            base: PM_BASE,
            entry: PM_BASE,
            args: Vec::new(),
        });
    }
    let ok = lift_all(c, &blocks)?;
    let (mut decls, mut regs, mut manifest) = (Vec::new(), Vec::new(), Vec::new());
    for (n, ((b, ok), addr)) in blocks.iter().zip(ok).zip(addrs).enumerate() {
        let name = &b.name;
        if !ok {
            return Err(format!("{name}: lifter failed, see {name}.lift.log"));
        }
        let fp = fnv1a64(&b.raw);
        let img = PM_IMAGE_ID_BASE + n;
        let entry = format!("{name}_spu_func_{PM_BASE:08X}");
        decls.push(format!("extern void {entry}(spu_context*);"));
        decls.push(format!("extern void {name}_spu_recomp_register(void);"));
        regs.push(format!(
            "    spu_begin_image({img}); {name}_spu_recomp_register();\n    spu_workload_register_img(0x{fp:016X}ULL, {entry}, {img}, \"{name}\");"
        ));
        manifest.push(format!(
            "  {{\n    \"name\": \"{name}\",\n    \"addr\": \"{addr:#x}\",\n    \"size\": {},\n    \"fp\": \"0x{fp:016X}\",\n    \"image\": {img}\n  }}",
            b.raw.len()
        ));
        let log = String::from_utf8_lossy(&racit_lift::read(
            &c.out_raw.join(format!("{name}.lift.log")),
        )?)
        .into_owned();
        let last = log.trim().lines().last().unwrap_or("").to_string();
        println!(
            "{name}: 0x{addr:X} size {} fp 0x{fp:016X} -> {}  [{last}]",
            b.raw.len(),
            c.spu_dir.join(name).display()
        );
    }
    let text = format!(
        "/* Generated by scripts/lift_policy_modules.py -- raw SPURS policy modules. */\n\
         #include \"spu_workload.h\"\n\nextern void spu_begin_image(int image_id);\n\n{}\n\n\
         void racacit_pm_register_all(void)\n{{\n{}\n    spu_begin_image(0);\n}}\n\n\
         __attribute__((constructor)) static void racacit_pm_register_ctor(void)\n\
         {{\n    racacit_pm_register_all();\n}}\n",
        decls.join("\n"),
        regs.join("\n")
    );
    write_if_changed(&c.spu_dir.join("policy_modules.c"), text.as_bytes())?;
    racit_lift::write(
        &c.out_raw.join("manifest.json"),
        format!("[\n{}\n]", manifest.join(",\n")).as_bytes(),
    )
}

fn run() -> Result<()> {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut take = |flag: &str| -> Result<Option<String>> {
        match args.iter().position(|a| a == flag) {
            Some(i) => {
                let v = args
                    .get(i + 1)
                    .cloned()
                    .ok_or(format!("{flag} needs a value"))?;
                args.drain(i..i + 2);
                Ok(Some(v))
            }
            None => Ok(None),
        }
    };
    let root = take("--root")?
        .map(PathBuf::from)
        .unwrap_or_else(racit_lift::default_root);
    let jobs: usize = take("--jobs")?
        .map(|j| j.parse().map_err(|_| format!("--jobs {j}")))
        .transpose()?
        .unwrap_or(0);
    if let Some(file) = take("--merge-starts")? {
        return merge_starts(&root, Path::new(&file));
    }
    let only: Option<BTreeSet<u64>> = take("--only")?
        .map(|o| {
            o.split(',')
                .map(|x| {
                    u64::from_str_radix(x.trim_start_matches("0x"), 16).map_err(|e| e.to_string())
                })
                .collect()
        })
        .transpose()?;
    let has = |f: &str| args.iter().any(|a| a == f);
    let (sub, extra, policy) = (has("--sub"), has("--extra"), has("--policy"));
    let positional: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
    let elf = positional
        .first()
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("input/EBOOT.ELF"));

    let out_root = std::env::var_os("RACACIT_OUT")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("out"));
    let spu_dir = std::env::var_os("RACACIT_SPU_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("game/spu"));
    let python = std::env::var("RACIT_PYTHON").unwrap_or_else(|_| "python3".into());
    let c = Ctx {
        out_raw: out_root.join(if policy { "policy_modules" } else { "spu_jobs" }),
        spu_dir,
        python,
        jobs,
        root,
    };
    fs::create_dir_all(&c.out_raw).map_err(|e| format!("{}: {e}", c.out_raw.display()))?;

    let version = spu::update_inputs(&c.root, &elf)?;
    let d = racit_lift::read(&elf)?;
    let sg = spu::segments(&d);
    if policy {
        return main_policy(&c, &d, &sg, version.as_ref());
    }
    if sub {
        return main_sub(&c, &d, &sg, version.as_ref());
    }
    if extra {
        return main_extra(&c, &d, &sg, version.as_ref());
    }
    main_jobs(&c, &d, &sg, version.as_ref(), only.as_ref())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(msg) => {
            eprintln!("{msg}");
            ExitCode::from(1)
        }
    }
}
