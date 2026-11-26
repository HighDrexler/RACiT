//! Prepare version-selected host sources (main.cpp, stubs.cpp, game_version.h) from the
//! shared game implementation. The update 1.20 build gets every disc func_XXXXXXXX and
//! bound disc address in real code rewritten to its 1.20 counterpart; strings and comments
//! are left alone.
//!
//!     prepare-game-host [--root DIR] ELF OUTPUT [--profile ID|auto]

use racit_lift::{Result, default_root, profile, tsv_rows, write_if_changed};
use regex::{Captures, Regex};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

const UPDATE: &str = racit_lift::spu::UPDATE;

fn rewrite_host(
    source: &str,
    functions: &BTreeMap<String, String>,
    addresses: &BTreeMap<u64, u64>,
) -> Result<String> {
    let token = Regex::new(
        r#"(?s)("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|//[^\n]*|/\*.*?\*/)|(\bfunc_([0-9A-F]{8})\b)|(0x[0-9A-Fa-f]+[uUlL]*)"#,
    )
    .unwrap();
    let mut err = None;
    let out = token.replace_all(source, |m: &Captures| {
        if m.get(1).is_some() {
            return m[0].to_string();
        }
        if let Some(old) = m.get(3) {
            return match functions.get(old.as_str()) {
                Some(new) => format!("func_{new}"),
                None => {
                    err.get_or_insert_with(|| {
                        format!("Missing host function mapping {}", old.as_str())
                    });
                    m[0].to_string()
                }
            };
        }
        let number = m[4].trim_end_matches(['u', 'U', 'l', 'L']);
        match u128::from_str_radix(&number[2..], 16) {
            Ok(a) if a <= u64::MAX as u128 && addresses.contains_key(&(a as u64)) => {
                format!("racit_addr_{:08X}", a)
            }
            _ => m[0].to_string(),
        }
    });
    match err {
        Some(e) => Err(e),
        None => Ok(out.into_owned()),
    }
}

/// Python's text-mode read: UTF-8 with universal newlines.
fn read_text(path: &Path) -> Result<String> {
    let s = fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(s.replace("\r\n", "\n").replace('\r', "\n"))
}

fn index_from(s: &str, needle: &str, from: usize) -> Result<usize> {
    s[from..]
        .find(needle)
        .map(|i| i + from)
        .ok_or_else(|| "substring not found".to_string())
}

fn host_sources(root: &Path, profile_id: &str) -> Result<Vec<(&'static str, String)>> {
    let update = profile_id == UPDATE;
    let mut functions = BTreeMap::new();
    let mut addresses = BTreeMap::new();
    if update {
        for row in tsv_rows(root, &format!("{UPDATE}-host-functions.tsv"))? {
            functions.insert(
                row["disc_function"].clone(),
                row["update120_function"].clone(),
            );
        }
        for row in tsv_rows(root, &format!("{UPDATE}-host-bindings.tsv"))? {
            let hex = |k: &str| {
                u64::from_str_radix(&row[k], 16).map_err(|e| format!("{k} {:?}: {e}", row[k]))
            };
            let (old, new) = (hex("disc_address")?, hex("update120_address")?);
            if addresses.get(&old).is_some_and(|a| *a != new) {
                return Err("Conflicting host address binding".into());
            }
            addresses.insert(old, new);
        }
    } else if profile_id != "BCUS98124-1.00" {
        return Err(format!("No host source implementation for {profile_id}"));
    }
    let mut header = String::from("#pragma once\n#include <stdint.h>\n");
    header += &format!("#define RACACIT_GAME_PROFILE_ID \"{profile_id}\"\n");
    for (old, new) in &addresses {
        header += &format!("static constexpr uint32_t racit_addr_{old:08X} = 0x{new:08X}u;\n");
    }
    let mut prepared = vec![("game_version.h", header)];
    for name in ["main.cpp", "stubs.cpp"] {
        let mut source = read_text(&root.join("game").join(name))?;
        if update {
            if name == "stubs.cpp" {
                // Disc-only ports of 1.20's own fixes: they name disc functions 1.20 has no
                // host mapping for, and the update build never calls them.
                let (begin, finish) = (
                    "// Disc-only 1.20 fixes begin.",
                    "// Disc-only 1.20 fixes end.",
                );
                let start = index_from(&source, begin, 0)?;
                let end = index_from(&source, finish, start)? + finish.len();
                source.replace_range(start..end, "");
            }
            source = rewrite_host(&source, &functions, &addresses)?;
            if name == "stubs.cpp" {
                let start =
                    index_from(&source, "extern \"C\" void racacit_skin_armor_selected(", 0)?;
                let end = index_from(&source, "/* EquipSkin's value", start)?;
                let skin = read_text(&root.join("versions").join(format!("{UPDATE}-skin.inc")))?;
                source.replace_range(start..end, &skin);
            }
        }
        prepared.push((name, format!("#include \"game_version.h\"\n{source}")));
    }
    Ok(prepared)
}

fn run() -> Result<()> {
    let mut root = None;
    let mut selected = "auto".to_string();
    let mut positional = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--root" => {
                root = Some(PathBuf::from(
                    args.next().ok_or("--root needs a directory")?,
                ))
            }
            "--profile" => selected = args.next().ok_or("--profile needs an id")?,
            "-h" | "--help" => {
                println!("usage: prepare-game-host [--root DIR] ELF OUTPUT [--profile ID|auto]");
                return Ok(());
            }
            _ => positional.push(a),
        }
    }
    let [elf, output] = <[String; 2]>::try_from(positional)
        .map_err(|_| "usage: prepare-game-host [--root DIR] ELF OUTPUT [--profile ID|auto]")?;
    let root = root.unwrap_or_else(default_root);
    let profile = profile::identify(&root, Path::new(&elf), &selected, false)?;
    let prepared = host_sources(&root, &profile.id)?;
    let out = Path::new(&output);
    fs::create_dir_all(out).map_err(|e| format!("{output}: {e}"))?;
    for (name, source) in &prepared {
        write_if_changed(&out.join(name), source.as_bytes())?;
    }
    println!("Prepared {} host sources in {output}", profile.id);
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("prepare-game-host: {e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rewrites_code_not_strings_or_comments() {
        let f = BTreeMap::from([("0001E4F0".to_string(), "0001E500".to_string())]);
        let a = BTreeMap::from([(0xF5D690u64, 0xF5D890u64)]);
        let src = "func_0001E4F0(0xF5D690u); // func_0001E4F0 0xF5D690\n\"func_0001E4F0 0xF5D690\" 0x10 /* 0xF5D690 */";
        assert_eq!(
            rewrite_host(src, &f, &a).unwrap(),
            "func_0001E500(racit_addr_00F5D690); // func_0001E4F0 0xF5D690\n\"func_0001E4F0 0xF5D690\" 0x10 /* 0xF5D690 */"
        );
        assert_eq!(
            rewrite_host("func_00ABCDEF()", &f, &a).unwrap_err(),
            "Missing host function mapping 00ABCDEF"
        );
    }

    // The checkout's own sources (formerly tests/test_update120_host.py). v1.0.0 couldn't
    // build 1.20 at all: stubs.cpp called disc functions (the cheat menu's weapon calls)
    // that versions/BCUS98124-1.20-host-functions.tsv didn't map. Needs no game files.

    fn root() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    fn get<'a>(p: &'a [(&str, String)], name: &str) -> &'a str {
        &p.iter().find(|(n, _)| *n == name).unwrap().1
    }

    #[test]
    fn update_sources_name_only_120_functions() {
        let prepared = host_sources(&root(), UPDATE).unwrap();
        let names: Vec<_> = prepared.iter().map(|(n, _)| *n).collect();
        assert_eq!(names, ["game_version.h", "main.cpp", "stubs.cpp"]);
        let mapped: std::collections::HashSet<String> =
            tsv_rows(&root(), &format!("{UPDATE}-host-functions.tsv"))
                .unwrap()
                .into_iter()
                .map(|r| r["update120_function"].clone())
                .collect();
        let code =
            Regex::new(r#"(?s)("(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/)|\bfunc_([0-9A-F]{8})\b"#)
                .unwrap();
        for name in ["main.cpp", "stubs.cpp"] {
            for m in code.captures_iter(get(&prepared, name)) {
                if let Some(f) = m.get(2) {
                    assert!(
                        mapped.contains(f.as_str()),
                        "{name} calls func_{}, not a 1.20 function",
                        f.as_str()
                    );
                }
            }
        }
    }

    #[test]
    fn disc_only_fixes_are_left_out_of_120() {
        let marker = "Disc-only 1.20 fixes begin.";
        assert!(!get(&host_sources(&root(), UPDATE).unwrap(), "stubs.cpp").contains(marker));
        assert!(
            get(
                &host_sources(&root(), "BCUS98124-1.00").unwrap(),
                "stubs.cpp"
            )
            .contains(marker)
        );
    }

    #[test]
    fn disc_sources_are_unchanged() {
        let disc = host_sources(&root(), "BCUS98124-1.00").unwrap();
        for name in ["main.cpp", "stubs.cpp"] {
            let original = read_text(&root().join("game").join(name)).unwrap();
            assert!(get(&disc, name).ends_with(&original));
        }
    }
}
