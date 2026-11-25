//! Bring a lift's hooks up to date, and check that every expected hook is there.
//!
//!     check-lift-hooks RECOMP --profile PROFILE [--elf EBOOT.ELF] [--patch] [--stamp FILE]
//!                     [--root RACIT] [--python PYTHON]
//!
//! It replaced scripts/check_lift_hooks.py, with the same rules and messages.
//! Every hook listed in versions/<profile>-hook-symbols.txt must be called in the
//! lifted sources at least as often as listed. Calls are counted in code only:
//! comments, declarations and definitions don't count, so a commented-out hook is
//! missing. The sources are read in parallel, one file per thread at a time.
//!
//! --patch (configure time): when patch_lift.py, one of its inputs or the lift itself
//! is newer than the stamp (or there is no stamp), re-run patch_lift.py on the lift
//! first. Without --patch it only checks, and never writes a source. The stamp is
//! written only when the check passes.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::SystemTime;
use std::{env, fs, thread};

/// What patch_lift.py reads: a change to any of them re-patches the lift.
const PATCHER_INPUTS: &[&str] = &[
    "scripts/patch_lift.py",
    "scripts/update120_hooks.py",
    "scripts/fix_vmx_alias.py",
    "scripts/game_version.py",
    "versions/profiles.json",
];

/// A name after one of these is a call ("return racit_x(...)"); after any other word, a
/// type ("int racit_x(...)"), so a declaration or a definition.
const CALL_KEYWORDS: &[&[u8]] = &[b"return", b"else", b"do", b"case", b"throw", b"co_return"];

struct Options {
    recomp: PathBuf,
    profile: String,
    elf: String,
    stamp: String,
    patch: bool,
    root: PathBuf,
    python: String,
}

fn fail(msg: String) -> ExitCode {
    eprintln!("{msg}");
    ExitCode::from(1)
}

fn usage() -> String {
    "usage: check-lift-hooks RECOMP --profile PROFILE [--elf EBOOT.ELF] [--patch] [--stamp FILE] \
     [--root RACIT] [--python PYTHON]"
        .to_string()
}

/// The RACiT checkout: the first ancestor of this executable that has versions/profiles.json,
/// else the one this crate was built in.
fn default_root() -> PathBuf {
    if let Ok(exe) = env::current_exe() {
        for dir in exe.ancestors().skip(1) {
            if dir.join("versions/profiles.json").is_file() {
                return dir.to_path_buf();
            }
        }
    }
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn parse_args() -> Result<Options, String> {
    let mut args = env::args().skip(1);
    let (mut recomp, mut profile, mut root) = (None, None, None);
    let (mut elf, mut stamp, mut patch, mut python) = (String::new(), String::new(), false, "python3".to_string());
    while let Some(a) = args.next() {
        let mut value = |name: &str| args.next().ok_or_else(|| format!("check-lift-hooks: {name} needs a value"));
        match a.as_str() {
            "--profile" => profile = Some(value("--profile")?),
            "--elf" => elf = value("--elf")?,
            "--stamp" => stamp = value("--stamp")?,
            "--root" => root = Some(PathBuf::from(value("--root")?)),
            "--python" => python = value("--python")?,
            "--patch" => patch = true,
            "-h" | "--help" => return Err(usage()),
            _ if a.starts_with("--") => return Err(format!("check-lift-hooks: unknown option {a}\n{}", usage())),
            _ if recomp.is_none() => recomp = Some(PathBuf::from(a)),
            _ => return Err(usage()),
        }
    }
    Ok(Options {
        recomp: recomp.ok_or_else(usage)?,
        profile: profile.ok_or_else(usage)?,
        elf,
        stamp,
        patch,
        root: root.unwrap_or_else(default_root),
        python,
    })
}

struct HookList {
    path: PathBuf,
    hooks: Vec<(String, u64)>,
    functions: Vec<String>,
}

fn expected_hooks(root: &Path, profile: &str) -> Result<HookList, String> {
    let path = root.join("versions").join(format!("{profile}-hook-symbols.txt"));
    let text = fs::read_to_string(&path)
        .map_err(|_| format!("check_lift_hooks: no expected-hook list for {profile} ({})", path.display()))?;
    let (mut hooks, mut functions) = (Vec::new(), Vec::new());
    for line in text.lines() {
        let line = line.split('#').next().unwrap_or("").trim();
        if let Some(rest) = line.strip_prefix("@function ") {
            functions.push(rest.split_whitespace().next().unwrap_or("").to_string());
        } else if !line.is_empty() {
            let mut parts = line.split_whitespace();
            let (Some(name), Some(count), None) = (parts.next(), parts.next(), parts.next()) else {
                return Err(format!("check_lift_hooks: bad line in {}: {line}", path.display()));
            };
            let count = count.parse().map_err(|_| format!("check_lift_hooks: bad count in {}: {line}", path.display()))?;
            match hooks.iter_mut().find(|(n, _)| n == name) {
                Some(entry) => entry.1 = count,
                None => hooks.push((name.to_string(), count)),
            }
        }
    }
    Ok(HookList { path, hooks, functions })
}

fn check_identity(sources: &[PathBuf], profile: &str, functions: &[String]) -> Result<(), String> {
    // Patching another version's lift with this profile's hooks would corrupt it.
    let mut wanted: HashSet<Vec<u8>> =
        functions.iter().map(|f| format!("void func_{f}(ppu_context* ctx) {{").into_bytes()).collect();
    for path in sources {
        if wanted.is_empty() {
            return Ok(());
        }
        let data = fs::read(path).map_err(|e| format!("check_lift_hooks: {}: {e}", path.display()))?;
        for line in data.split(|&b| b == b'\n') {
            if line.starts_with(b"void func_") {
                wanted.remove(line);
            }
        }
    }
    if wanted.is_empty() {
        return Ok(());
    }
    let mut names: Vec<String> = wanted
        .iter()
        .map(|w| {
            let w = String::from_utf8_lossy(w);
            w[5..w.find('(').unwrap_or(w.len())].to_string()
        })
        .collect();
    names.sort();
    Err(format!(
        "check_lift_hooks: {} is not a {profile} lift (it has no {}). Check RECOMP_DIR, \
         RACACIT_GAME_PROFILE and HOST_DIR; nothing was patched.",
        sources[0].parent().unwrap_or(Path::new("")).display(),
        names.join(", ")
    ))
}

/// The text with its comments and string and character literals blanked out (newlines
/// kept in comments). Byte for byte what check_lift_hooks.py's code_only does to the
/// lines, including an unterminated literal swallowing its newline.
fn code_only(text: &[u8]) -> Vec<u8> {
    let n = text.len();
    let mut out = Vec::with_capacity(n);
    let mut i = 0;
    while i < n {
        let c = text[i];
        if c == b'/' && i + 1 < n && text[i + 1] == b'*' {
            let j = find(&text[i + 2..], b"*/").map_or(n, |k| i + 2 + k + 2);
            out.extend(text[i..j].iter().map(|&b| if b == b'\n' { b'\n' } else { b' ' }));
            i = j;
        } else if c == b'/' && i + 1 < n && text[i + 1] == b'/' {
            let j = text[i..].iter().position(|&b| b == b'\n').map_or(n, |k| i + k);
            out.resize(out.len() + (j - i), b' ');
            i = j;
        } else if c == b'"' || c == b'\'' {
            let mut j = i + 1;
            while j < n && text[j] != c && text[j] != b'\n' {
                j += if text[j] == b'\\' { 2 } else { 1 };
            }
            out.push(c);
            out.resize(out.len() + (j.min(n) - i - 1), b' ');
            if j < n {
                out.push(c);
            }
            i = j + 1;
        } else {
            out.push(c);
            i += 1;
        }
    }
    out
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

fn is_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c)
}

fn is_word(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_' || b >= 0x80
}

fn trim(s: &[u8]) -> &[u8] {
    let start = s.iter().position(|&b| !is_space(b)).unwrap_or(s.len());
    let end = s.iter().rposition(|&b| !is_space(b)).map_or(start, |e| e + 1);
    &s[start..end]
}

/// "void func_XXXXXXXX(ppu_context* ctx) {" exactly (eight upper-case hex digits).
fn is_lifted_function(line: &[u8]) -> bool {
    const HEAD: &[u8] = b"void func_";
    const TAIL: &[u8] = b"(ppu_context* ctx) {";
    line.len() == HEAD.len() + 8 + TAIL.len()
        && line.starts_with(HEAD)
        && line.ends_with(TAIL)
        && line[HEAD.len()..HEAD.len() + 8].iter().all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(b))
}

/// Calls each line inside a lifted function body (its "{" line to the "}" that closes it at
/// the start of a line), without preprocessor lines and what a constant-false #if / #elif
/// leaves out. A condition that isn't a plain 0 counts as true.
fn for_each_body_line(code: &[u8], mut f: impl FnMut(&[u8])) {
    // One entry per open #if: [code here is live, a branch was taken, the enclosing was live]
    let mut stack: Vec<[bool; 3]> = Vec::new();
    let (mut live, mut inside) = (true, false);
    let value = |rest: &[u8]| trim(rest) != b"0";
    for line in code.split(|&b| b == b'\n') {
        let stripped = trim(line);
        if let Some(word) = stripped.strip_prefix(b"#") {
            let word = trim(word);
            let len = word.iter().take_while(|b| b.is_ascii_lowercase()).count();
            let (name, rest) = word.split_at(len);
            match name {
                b"if" | b"ifdef" | b"ifndef" => {
                    let cond = name != b"if" || value(rest);
                    stack.push([live && cond, cond, live]);
                }
                b"elif" if !stack.is_empty() => {
                    let top = stack.last_mut().unwrap();
                    let cond = !top[1] && value(rest);
                    top[0] = top[2] && cond;
                    top[1] |= cond;
                }
                b"else" if !stack.is_empty() => {
                    let top = stack.last_mut().unwrap();
                    top[0] = top[2] && !top[1];
                    top[1] = true;
                }
                b"endif" => {
                    stack.pop();
                }
                _ => {}
            }
            live = stack.last().map_or(true, |t| t[0]);
            continue;
        }
        if !live {
            continue;
        }
        if !inside {
            inside = is_lifted_function(line);
            continue;
        }
        if line.starts_with(b"}") {
            inside = false;
            continue;
        }
        f(line);
    }
}

/// The word before a call, as check_lift_hooks.py's `([A-Za-z_][A-Za-z0-9_]*|[*&])\s*$`
/// finds it: None when the call follows anything else.
fn prior_word(before: &[u8]) -> Option<&[u8]> {
    let end = before.iter().rposition(|&b| !is_space(b))? + 1;
    let last = before[end - 1];
    if last == b'*' || last == b'&' {
        return Some(&before[end - 1..end]);
    }
    if !(last.is_ascii_alphanumeric() || last == b'_') {
        return None;
    }
    let start = before[..end].iter().rposition(|&b| !(b.is_ascii_alphanumeric() || b == b'_')).map_or(0, |s| s + 1);
    let first = before[start..end].iter().position(|&b| b.is_ascii_alphabetic() || b == b'_')?;
    Some(&before[start + first..end])
}

/// How often each hook is called in one source: real calls only.
fn count_calls(data: &[u8], hooks: &HashMap<&[u8], usize>, counts: &mut [u64]) {
    let code = code_only(data);
    for_each_body_line(&code, |line| {
        let mut i = 0;
        while i < line.len() {
            if !is_word(line[i]) {
                i += 1;
                continue;
            }
            let start = i;
            while i < line.len() && is_word(line[i]) {
                i += 1;
            }
            let Some(&slot) = hooks.get(&line[start..i]) else { continue };
            let mut k = i;
            while k < line.len() && is_space(line[k]) {
                k += 1;
            }
            if k >= line.len() || line[k] != b'(' {
                continue;
            }
            if let Some(word) = prior_word(&line[..start]) {
                if !CALL_KEYWORDS.contains(&word) {
                    continue; // "int name(" / "void* name(": a declaration or a definition
                }
            }
            counts[slot] += 1;
        }
    });
}

fn count_all(sources: &[PathBuf], names: &[(String, u64)]) -> Result<Vec<u64>, String> {
    let hooks: HashMap<&[u8], usize> = names.iter().enumerate().map(|(i, (n, _))| (n.as_bytes(), i)).collect();
    let next = AtomicUsize::new(0);
    let workers = thread::available_parallelism().map_or(4, |n| n.get()).min(sources.len().max(1));
    thread::scope(|s| {
        let handles: Vec<_> = (0..workers)
            .map(|_| {
                s.spawn(|| -> Result<Vec<u64>, String> {
                    let mut counts = vec![0u64; names.len()];
                    loop {
                        let i = next.fetch_add(1, Ordering::Relaxed);
                        let Some(path) = sources.get(i) else { return Ok(counts) };
                        let data = fs::read(path).map_err(|e| format!("check_lift_hooks: {}: {e}", path.display()))?;
                        count_calls(&data, &hooks, &mut counts);
                    }
                })
            })
            .collect();
        let mut total = vec![0u64; names.len()];
        for h in handles {
            for (t, c) in total.iter_mut().zip(h.join().expect("worker panicked")?) {
                *t += c;
            }
        }
        Ok(total)
    })
}

fn mtime(p: &Path) -> Option<SystemTime> {
    fs::metadata(p).and_then(|m| m.modified()).ok()
}

fn lifted_sources(recomp: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = fs::read_dir(recomp)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.starts_with("ppu_recomp_") && n.ends_with(".cpp"))
        })
        .collect();
    v.sort();
    v
}

fn run(o: &Options) -> Result<(), String> {
    let sources = lifted_sources(&o.recomp);
    if sources.is_empty() {
        return Err(format!("check_lift_hooks: no lifted sources in {}", o.recomp.display()));
    }
    let list = expected_hooks(&o.root, &o.profile)?;

    let mut inputs: Vec<PathBuf> = PATCHER_INPUTS.iter().map(|p| o.root.join(p)).collect();
    inputs.push(list.path.clone());
    inputs.push(o.root.join("versions").join(format!("{}-hooks.json", o.profile)));
    // A regenerated lift (sources newer than the stamp) is patched again too.
    let newest = inputs.iter().chain(&sources).filter_map(|p| mtime(p)).max();
    // Every run, patching or not: another version's lift must not pass as this one.
    check_identity(&sources, &o.profile, &list.functions)?;
    let stale = o.patch
        && (o.stamp.is_empty() || match (mtime(Path::new(&o.stamp)), newest) {
            (None, _) => true,
            (Some(s), Some(n)) => s < n,
            (Some(_), None) => false,
        });
    if stale {
        let mut cmd = Command::new(&o.python);
        cmd.args(["-I", "-X", "utf8"]).arg(o.root.join("scripts/patch_lift.py")).arg(&o.recomp);
        if !o.elf.is_empty() {
            cmd.args(["--elf", &o.elf, "--profile", &o.profile]);
        } else if o.profile != "BCUS98124-1.00" {
            return Err(format!(
                "check_lift_hooks: re-patching the {} lift needs its executable: \
                 configure with -DRACACIT_ELF=<its decrypted EBOOT.ELF>",
                o.profile
            ));
        }
        println!("check_lift_hooks: re-running patch_lift.py on {}", o.recomp.display());
        let status = cmd.status().map_err(|e| format!("check_lift_hooks: {}: {e}", o.python))?;
        if !status.success() {
            return Err(format!("check_lift_hooks: patch_lift.py failed ({status})"));
        }
    }

    let counts = count_all(&sources, &list.hooks)?;
    let missing: Vec<String> = list
        .hooks
        .iter()
        .zip(&counts)
        .filter(|((_, want), got)| *got < want)
        .map(|((name, want), got)| format!("{name} ({got} of {want})"))
        .collect();
    if !missing.is_empty() {
        let rel = list.path.strip_prefix(&o.root).unwrap_or(&list.path);
        return Err(format!(
            "check_lift_hooks: the {} lift in {} lacks hooks that {} expects:\n  {}\n\
             Re-run scripts/patch_lift.py on it (or regenerate it) and rebuild; \
             a build without them must not be staged.",
            o.profile,
            o.recomp.display(),
            rel.display(),
            missing.join("\n  ")
        ));
    }
    println!(
        "check_lift_hooks: {}: all {} expected hooks present in {}",
        o.profile,
        list.hooks.len(),
        o.recomp.display()
    );
    if !o.stamp.is_empty() {
        fs::write(&o.stamp, "ok\n").map_err(|e| format!("check_lift_hooks: {}: {e}", o.stamp))?;
    }
    Ok(())
}

fn main() -> ExitCode {
    match parse_args().and_then(|o| run(&o)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(msg) => fail(msg),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn count(src: &str, hook: &str) -> u64 {
        let hooks: HashMap<&[u8], usize> = [(hook.as_bytes(), 0)].into_iter().collect();
        let mut c = [0u64];
        count_calls(src.as_bytes(), &hooks, &mut c);
        c[0]
    }

    const F: &str = "void func_00012345(ppu_context* ctx) {\n";

    #[test]
    fn calls_in_bodies_only() {
        assert_eq!(count(&format!("racit_x(1);\n{F}  racit_x(ctx);\n  return racit_x (ctx);\n}}\nracit_x();\n"), "racit_x"), 2);
    }

    #[test]
    fn declarations_comments_and_strings_do_not_count() {
        let body = "  int racit_x(int);\n  void* racit_x(void);\n  // racit_x(ctx);\n  /* racit_x(ctx); */\n  \"racit_x(\";\n  x = racit_xy(1);\n";
        assert_eq!(count(&format!("{F}{body}}}\n"), "racit_x"), 0);
    }

    #[test]
    fn if_zero_is_skipped() {
        let body = "#if 0\n  racit_x(ctx);\n#elif 1\n  racit_x(ctx);\n#else\n  racit_x(ctx);\n#endif\n";
        assert_eq!(count(&format!("{F}{body}}}\n"), "racit_x"), 1);
    }

    #[test]
    fn prior_word_matches_the_python_regex() {
        assert_eq!(prior_word(b"x = 0abc "), Some(&b"abc"[..]));
        assert_eq!(prior_word(b"x = 123 "), None);
        assert_eq!(prior_word(b"  *"), Some(&b"*"[..]));
        assert_eq!(prior_word(b"(  "), None);
        assert_eq!(prior_word(b""), None);
    }

    #[test]
    fn unterminated_literal_swallows_its_newline() {
        assert_eq!(code_only(b"a 'b\nc"), b"a ' 'c".to_vec());
        assert_eq!(code_only(b"/* x\ny */z"), b"    \n    z".to_vec());
    }
}
