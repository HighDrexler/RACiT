//! lift-stamp: lets scripts/pipeline.sh skip the lift (stages 1 to 4) when nothing it
//! reads has changed and its published output is as the last lift left it.
//!
//! `lift-stamp check STAMP --root ROOT --elf ELF --libsre PRX --profile ID OUTPUT_DIR...`
//! exits 0 when STAMP matches, 1 when the lift must run. `lift-stamp write ...` (same
//! arguments) records the current state after a lift.
//!
//! Inputs: the executable and libsre bytes, the profile, PS3RECOMP_CHUNK_LINES, and every
//! file under the lift's code and data (ps3recomp's tools, scripts/, versions/, this crate,
//! analysis/ghidra). Outputs: path, size and modification time of every published file, so
//! an edited or deleted lifted file means a fresh lift. Anything unreadable means a fresh
//! lift too. RACIT_RELIFT=1 always lifts.

use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

type Result<T> = std::result::Result<T, String>;

const INPUT_DIRS: &[&str] = &[
    "tools/ps3recomp/tools",
    "scripts",
    "versions",
    "rust/racit-lift/src",
    "analysis/ghidra",
];
const INPUT_FILES: &[&str] = &["rust/racit-lift/Cargo.toml", "rust/racit-lift/Cargo.lock"];

fn files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for e in entries.flatten() {
        let p = e.path();
        let name = e.file_name();
        if matches!(name.to_str(), Some("__pycache__" | "target" | ".git")) {
            continue;
        }
        match e.file_type() {
            Ok(t) if t.is_dir() => files(&p, out),
            Ok(_) => out.push(p),
            Err(_) => {}
        }
    }
}

fn hash_file(h: &mut Sha256, label: &str, path: &Path) -> Result<()> {
    let data = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    h.update(label.as_bytes());
    h.update([0]);
    h.update((data.len() as u64).to_le_bytes());
    h.update(&data);
    Ok(())
}

fn inputs(root: &Path, elf: &Path, libsre: &Path, profile: &str) -> Result<String> {
    let mut h = Sha256::new();
    h.update(b"lift-stamp 1\0");
    h.update(profile.as_bytes());
    h.update([0]);
    h.update(
        std::env::var("PS3RECOMP_CHUNK_LINES")
            .unwrap_or_default()
            .as_bytes(),
    );
    h.update([0]);
    hash_file(&mut h, "elf", elf)?;
    hash_file(&mut h, "libsre", libsre)?;
    let mut list = Vec::new();
    for d in INPUT_DIRS {
        files(&root.join(d), &mut list);
    }
    list.extend(INPUT_FILES.iter().map(|f| root.join(f)));
    list.sort();
    for p in list {
        let rel = p
            .strip_prefix(root)
            .unwrap_or(&p)
            .to_string_lossy()
            .into_owned();
        hash_file(&mut h, &rel, &p)?;
    }
    Ok(hex(&h.finalize()))
}

fn outputs(dirs: &[PathBuf]) -> Result<String> {
    let mut h = Sha256::new();
    for d in dirs {
        let mut list = Vec::new();
        files(d, &mut list);
        if list.is_empty() {
            return Err(format!("{} is empty", d.display()));
        }
        list.sort();
        h.update(d.to_string_lossy().as_bytes());
        h.update([0]);
        for p in list {
            let m = fs::metadata(&p).map_err(|e| format!("{}: {e}", p.display()))?;
            let mtime = m
                .modified()
                .ok()
                .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                .map(|d| d.as_nanos())
                .unwrap_or(0);
            h.update(p.strip_prefix(d).unwrap_or(&p).to_string_lossy().as_bytes());
            h.update([0]);
            h.update(m.len().to_le_bytes());
            h.update(mtime.to_le_bytes());
        }
    }
    Ok(hex(&h.finalize()))
}

fn run() -> Result<bool> {
    let mut args = std::env::args().skip(1);
    let usage = "usage: lift-stamp check|write STAMP --root ROOT --elf ELF --libsre PRX --profile ID OUTPUT_DIR...";
    let mode = args.next().ok_or(usage)?;
    let stamp = PathBuf::from(args.next().ok_or(usage)?);
    let (mut root, mut elf, mut libsre, mut profile, mut outs) =
        (None, None, None, None, Vec::new());
    while let Some(a) = args.next() {
        let mut val = || args.next().ok_or(format!("{a} needs a value"));
        match a.as_str() {
            "--root" => root = Some(PathBuf::from(val()?)),
            "--elf" => elf = Some(PathBuf::from(val()?)),
            "--libsre" => libsre = Some(PathBuf::from(val()?)),
            "--profile" => profile = Some(val()?),
            _ => outs.push(PathBuf::from(a)),
        }
    }
    let (Some(root), Some(elf), Some(libsre), Some(profile)) = (root, elf, libsre, profile) else {
        return Err(usage.into());
    };
    if outs.is_empty() {
        return Err(usage.into());
    }
    let input = inputs(&root, &elf, &libsre, &profile)?;
    match mode.as_str() {
        "check" => {
            if std::env::var("RACIT_RELIFT").is_ok_and(|v| v == "1") {
                return Ok(false);
            }
            let Ok(saved) = fs::read_to_string(&stamp) else {
                return Ok(false);
            };
            let Ok(output) = outputs(&outs) else {
                return Ok(false);
            };
            Ok(saved == format!("inputs {input}\noutputs {output}\n"))
        }
        "write" => {
            let output = outputs(&outs)?;
            fs::write(&stamp, format!("inputs {input}\noutputs {output}\n"))
                .map_err(|e| format!("{}: {e}", stamp.display()))?;
            Ok(true)
        }
        _ => Err(usage.into()),
    }
}

fn main() {
    match run() {
        Ok(true) => {}
        Ok(false) => std::process::exit(1),
        Err(e) => {
            eprintln!("lift-stamp: {e}");
            std::process::exit(2);
        }
    }
}

fn hex(d: &[u8]) -> String {
    d.iter().map(|b| format!("{b:02x}")).collect()
}
