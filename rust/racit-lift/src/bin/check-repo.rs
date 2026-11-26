//! check-repo: repository rules a build checks before it starts (scripts/pipeline.sh)
//! and an agent runs before committing.
//!
//! 1. The runtime patch (patches/ps3recomp-0001-racacit-runtime-fixes.patch) adds no
//!    CRLF lines and changes no line only by its line ending. Git on Windows with
//!    core.autocrlf on rewrote whole files that way in the 1.2 commit (about 12,500
//!    lines with no change), and every runtime change after it conflicted.
//! 2. No game data is tracked: executables, firmware modules, keys, disc images and
//!    packages, the game's audio and video, and nothing over 10 MB. Those can't be
//!    redistributed, and once pushed they stay in the history.
//!
//! Usage: check-repo [--root RACIT]. Exit 0 when both pass, 1 with the problems listed.

use std::path::{Path, PathBuf};
use std::process::Command;

const PATCH: &str = "patches/ps3recomp-0001-racacit-runtime-fixes.patch";
const MAX_FILE: u64 = 10 * 1024 * 1024;
/// Game and firmware formats that must never be committed.
const FORBIDDEN_EXT: &[&str] = &[
    "elf", "self", "sprx", "prx", "pkg", "rap", "edat", "sdat", "iso", "dkey", "ird", "psarc",
    "pup", "sfo", "at3", "bik", "vag",
];
/// Files of ours that look like game data by name.
const ALLOWED: &[&str] = &["game/menu_keycaps.actions.bin"];

/// Problems in a unified diff: added lines ending in CR, and lines a hunk removes and
/// adds back with only the line ending changed.
fn patch_problems(text: &[u8]) -> Vec<String> {
    let mut out = Vec::new();
    let (mut file, mut crlf, mut eol_only) = (String::new(), 0usize, 0usize);
    let mut removed: Vec<&[u8]> = Vec::new();
    let flush = |file: &str, crlf: &mut usize, eol_only: &mut usize, out: &mut Vec<String>| {
        if *crlf > 0 {
            out.push(format!("{PATCH}: {file}: {crlf} added lines end in CRLF"));
        }
        if *eol_only > 0 {
            out.push(format!(
                "{PATCH}: {file}: {eol_only} lines changed only by their line ending"
            ));
        }
        *crlf = 0;
        *eol_only = 0;
    };
    for line in text.split(|&b| b == b'\n') {
        if let Some(name) = line.strip_prefix(b"diff --git a/") {
            flush(&file, &mut crlf, &mut eol_only, &mut out);
            let name = String::from_utf8_lossy(name);
            file = name.split(" b/").next().unwrap_or("").to_string();
            removed.clear();
            continue;
        }
        if line.starts_with(b"@@") {
            removed.clear();
            continue;
        }
        if line.starts_with(b"---") || line.starts_with(b"+++") {
            continue;
        }
        if let Some(r) = line.strip_prefix(b"-") {
            removed.push(r);
        } else if let Some(a) = line.strip_prefix(b"+") {
            let a_cr = a.ends_with(b"\r");
            if a_cr {
                crlf += 1;
            }
            let bare = a.strip_suffix(b"\r").unwrap_or(a);
            if let Some(i) = removed.iter().position(|r| {
                r.ends_with(b"\r") != a_cr && r.strip_suffix(b"\r").unwrap_or(r) == bare
            }) {
                removed.swap_remove(i);
                eol_only += 1;
            }
        }
    }
    flush(&file, &mut crlf, &mut eol_only, &mut out);
    out
}

fn tracked_problems(root: &Path) -> Result<Vec<String>, String> {
    // A source download without .git (or no git installed) has nothing tracked to check.
    let out = match Command::new("git")
        .args(["ls-files", "-z"])
        .current_dir(root)
        .output()
    {
        Ok(o) if o.status.success() => o,
        _ => {
            eprintln!("check-repo: not a Git checkout, tracked files not checked");
            return Ok(Vec::new());
        }
    };
    let mut problems = Vec::new();
    for name in out.stdout.split(|&b| b == 0).filter(|n| !n.is_empty()) {
        let name = String::from_utf8_lossy(name).into_owned();
        if ALLOWED.contains(&name.as_str()) {
            continue;
        }
        let lower = name.to_ascii_lowercase();
        let base = lower.rsplit('/').next().unwrap_or(&lower);
        let ext = base.rsplit_once('.').map(|(_, e)| e).unwrap_or("");
        if FORBIDDEN_EXT.contains(&ext) || base == "eboot.bin" {
            problems.push(format!("{name}: game data can't be committed"));
            continue;
        }
        if let Ok(m) = std::fs::metadata(root.join(&name))
            && m.len() > MAX_FILE
        {
            problems.push(format!(
                "{name}: {} MB, over the 10 MB limit for tracked files",
                m.len() / (1024 * 1024)
            ));
        }
    }
    Ok(problems)
}

fn run() -> Result<Vec<String>, String> {
    let mut root = PathBuf::from(".");
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--root" => root = PathBuf::from(args.next().ok_or("--root needs a directory")?),
            "-h" | "--help" => return Err("usage: check-repo [--root RACIT]".into()),
            _ => return Err(format!("check-repo: unknown argument {a}")),
        }
    }
    let patch = root.join(PATCH);
    let text = std::fs::read(&patch).map_err(|e| format!("{}: {e}", patch.display()))?;
    let mut problems = patch_problems(&text);
    problems.extend(tracked_problems(&root)?);
    Ok(problems)
}

fn main() {
    match run() {
        Ok(p) if p.is_empty() => {}
        Ok(p) => {
            for line in &p {
                eprintln!("check-repo: {line}");
            }
            eprintln!(
                "check-repo: {} problem(s). Line endings: regenerate the patch with core.autocrlf=false \
                 (BUILDING.md). Game data: remove it from the index (git rm --cached) and the history.",
                p.len()
            );
            std::process::exit(1);
        }
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(2);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::patch_problems;

    #[test]
    fn clean_patch_passes() {
        let p =
            b"diff --git a/x.c b/x.c\n--- a/x.c\n+++ b/x.c\n@@ -1,2 +1,2 @@\n-int a;\n+int b;\n";
        assert!(patch_problems(p).is_empty());
    }

    #[test]
    fn crlf_rewrite_is_caught() {
        let p = b"diff --git a/x.c b/x.c\n--- a/x.c\n+++ b/x.c\n@@ -1,2 +1,2 @@\n-int a;\n-int b;\n+int a;\r\n+int b;\r\n";
        let out = patch_problems(p);
        assert_eq!(out.len(), 2, "{out:?}");
        assert!(out[0].contains("2 added lines end in CRLF"));
        assert!(out[1].contains("2 lines changed only by their line ending"));
    }

    #[test]
    fn crlf_dropped_is_caught() {
        let p = b"diff --git a/y.c b/y.c\n@@ -1 +1 @@\n-int a;\r\n+int a;\n";
        let out = patch_problems(p);
        assert_eq!(out.len(), 1, "{out:?}");
        assert!(out[0].contains("only by their line ending"));
    }

    #[test]
    fn editing_a_crlf_line_is_fine() {
        // A pinned file with CRLF lines: changing one keeps its CRLF context removed only.
        let p = b"diff --git a/z.c b/z.c\n@@ -1 +1 @@\n-int a;\r\n+int b;\n";
        assert!(patch_problems(p).is_empty());
    }
}
