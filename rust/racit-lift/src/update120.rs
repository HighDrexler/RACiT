//! Fingerprint-bound PPU source hooks for BCUS98124 update 1.20
//! (versions/BCUS98124-1.20-hooks.json). It replaced scripts/update120_hooks.py.
//!
//! Every hooked function's generated source must hash to the recipe's before (or after,
//! when already hooked) checksum, and the edited result to its after checksum. Every
//! function is checked before any file is written, so a mismatch never leaves a
//! half-patched lift.

use crate::{Result, find, find_from, insert_after_include, lifted_sources, sha256_hex, splitlines_keepends};
use regex::bytes::Regex;
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};

struct Hook {
    before: String,
    after: String,
    edits: Vec<(usize, usize, String)>,
    includes: Vec<String>,
}

/// A chunk patched with an earlier recipe carries that recipe's preamble (a hook's
/// declaration may have changed since). Two preambles would redefine RacJbFrame and can
/// conflict, so the old one goes: its marker line and the declarations that follow it,
/// but not a hook row's own include lines.
fn drop_old_preamble(source: &[u8], preamble: &[u8], row_includes: &HashSet<Vec<u8>>) -> Vec<u8> {
    let first = preamble.split(|&b| b == b'\n').next().unwrap_or(b"");
    let marker = [first, b"\n"].concat();
    let Some(start) = find(source, &marker) else { return source.to_vec() };
    let mut end = start + marker.len();
    while let Some(line_end) = find_from(source, b"\n", end) {
        let line = &source[end..line_end];
        if row_includes.contains(line)
            || !(line.starts_with(b"extern \"C\"")
                || line == b"#include \"ps3_setjmp.h\""
                || line.starts_with(b"typedef struct RacJbFrame"))
        {
            break;
        }
        end = line_end + 1;
    }
    [&source[..start], &source[end..]].concat()
}

fn str_of(v: &Value, key: &str) -> Result<String> {
    v.get(key).and_then(Value::as_str).map(str::to_string).ok_or_else(|| format!("update hook recipe: no {key}"))
}

pub fn apply(root: &Path, recomp: &Path, elf: &Path) -> Result<()> {
    let recipe_path = root.join("versions/BCUS98124-1.20-hooks.json");
    let text = fs::read_to_string(&recipe_path).map_err(|e| format!("{}: {e}", recipe_path.display()))?;
    let recipe: Value = serde_json::from_str(&text).map_err(|e| format!("{}: {e}", recipe_path.display()))?;
    if recipe.get("schema").and_then(Value::as_i64) != Some(1)
        || recipe.get("profile").and_then(Value::as_str) != Some("BCUS98124-1.20")
    {
        return Err("Unsupported update hook recipe".into());
    }
    if crate::profile::sha256_file(elf)? != str_of(&recipe, "elf_sha256")? {
        return Err("Update hooks require the exact BCUS98124 1.20 executable".into());
    }

    let rows = recipe.get("hooks").and_then(Value::as_array).ok_or("update hook recipe: no hooks")?;
    let mut hooks: HashMap<String, Hook> = HashMap::new();
    let mut row_includes: HashSet<Vec<u8>> = HashSet::new();
    for row in rows {
        let includes: Vec<String> = row
            .get("includes")
            .and_then(Value::as_array)
            .map(|a| a.iter().filter_map(|i| i.as_str().map(str::to_string)).collect())
            .unwrap_or_default();
        row_includes.extend(includes.iter().map(|i| i.as_bytes().to_vec()));
        let mut edits = Vec::new();
        for e in row.get("edits").and_then(Value::as_array).into_iter().flatten() {
            let e = e.as_array().ok_or("update hook recipe: bad edit")?;
            let (Some(s), Some(c), Some(a)) =
                (e.first().and_then(Value::as_u64), e.get(1).and_then(Value::as_u64), e.get(2).and_then(Value::as_str))
            else {
                return Err("update hook recipe: bad edit".into());
            };
            edits.push((s as usize, c as usize, a.to_string()));
        }
        let hook = Hook { before: str_of(row, "before_sha256")?, after: str_of(row, "after_sha256")?, edits, includes };
        hooks.insert(str_of(row, "function")?, hook);
    }
    if hooks.len() != rows.len() {
        return Err("Duplicate update hook address".into());
    }
    let preamble = str_of(&recipe, "preamble")?.into_bytes();
    let head = Regex::new(r"(?m)^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{").unwrap();

    let mut seen: BTreeSet<String> = BTreeSet::new();
    let mut pending: BTreeMap<PathBuf, Vec<u8>> = BTreeMap::new();
    for path in lifted_sources(recomp) {
        let mut source = crate::read(&path)?;
        let mut replacements: Vec<(usize, usize, Vec<u8>)> = Vec::new();
        let mut includes: BTreeSet<String> = BTreeSet::new();
        for m in head.captures_iter(&source) {
            let address = String::from_utf8_lossy(&m[1]).into_owned();
            let Some(hook) = hooks.get(&address) else { continue };
            if !seen.insert(address.clone()) {
                return Err(format!("Duplicate generated function {address}"));
            }
            let whole = m.get(0).unwrap();
            let end = find_from(&source, b"\n}\n", whole.end())
                .ok_or_else(|| format!("Unterminated generated function {address}"))?
                + 3;
            let before = &source[whole.start()..end];
            includes.extend(hook.includes.iter().cloned());
            let checksum = sha256_hex(before);
            if checksum == hook.after {
                continue;
            }
            if checksum != hook.before {
                return Err(format!("Generated function does not match the supported compiler output: {address}"));
            }
            let mut lines: Vec<Vec<u8>> = splitlines_keepends(before).into_iter().map(<[u8]>::to_vec).collect();
            let mut previous = lines.len() + 1;
            for (start, count, added) in hook.edits.iter().rev() {
                let (start, count) = (*start, *count);
                if start + count > lines.len() || start + count > previous {
                    return Err(format!("Invalid/overlapping update edits for {address}"));
                }
                let new: Vec<Vec<u8>> = splitlines_keepends(added.as_bytes()).into_iter().map(<[u8]>::to_vec).collect();
                lines.splice(start..start + count, new);
                previous = start;
            }
            let after = lines.concat();
            if sha256_hex(&after) != hook.after {
                return Err(format!("Update hook result does not match: {address}"));
            }
            replacements.push((whole.start(), end, after));
        }
        for (start, end, after) in replacements.iter().rev() {
            source = crate::splice(&source, *start, *end, after);
        }
        let missing_includes: Vec<&String> =
            includes.iter().filter(|inc| !crate::contains(&source, inc.as_bytes())).collect();
        for include in &missing_includes {
            source = insert_after_include(&source, &[include.as_bytes(), b"\n"].concat())?;
        }
        if !replacements.is_empty() || !missing_includes.is_empty() {
            if !crate::contains(&source, &preamble) {
                source = drop_old_preamble(&source, &preamble, &row_includes);
                source = insert_after_include(&source, &preamble)?;
            }
            pending.insert(path, source);
        }
    }
    let missing: Vec<&String> = {
        let mut m: Vec<&String> = hooks.keys().filter(|k| !seen.contains(*k)).collect();
        m.sort();
        m
    };
    if !missing.is_empty() {
        return Err(format!(
            "Missing generated update functions: {}",
            missing.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(", ")
        ));
    }
    // Validate every function before modifying any output file.
    for (path, source) in &pending {
        let mut temporary = path.clone().into_os_string();
        temporary.push(".update120.tmp");
        let temporary = PathBuf::from(temporary);
        let mut f = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .map_err(|e| format!("{}: {e}", temporary.display()))?;
        f.write_all(source).map_err(|e| format!("{}: {e}", temporary.display()))?;
        drop(f);
        fs::rename(&temporary, path).map_err(|e| format!("{}: {e}", path.display()))?;
    }
    println!("BCUS98124 1.20 hooks: {} functions checked, {} chunks updated", seen.len(), pending.len());
    Ok(())
}
