//! Shared pieces of the RACiT lift tools: the checkout root, the lifted sources, byte-string
//! editing that behaves like the Python str methods the tools were first written with, and
//! the game profiles (versions/profiles.json).

pub mod profile;
pub mod spu;
pub mod update120;
pub mod vmx;

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

pub type Result<T> = std::result::Result<T, String>;

/// The RACiT checkout: the first ancestor of this executable that has versions/profiles.json,
/// else the one this crate was built in.
pub fn default_root() -> PathBuf {
    if let Ok(exe) = env::current_exe() {
        for dir in exe.ancestors().skip(1) {
            if dir.join("versions/profiles.json").is_file() {
                return dir.to_path_buf();
            }
        }
    }
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

/// RECOMP/ppu_recomp_*.cpp, sorted by name.
pub fn lifted_sources(recomp: &Path) -> Vec<PathBuf> {
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

pub fn read(path: &Path) -> Result<Vec<u8>> {
    fs::read(path).map_err(|e| format!("{}: {e}", path.display()))
}

pub fn write(path: &Path, data: &[u8]) -> Result<()> {
    fs::write(path, data).map_err(|e| format!("{}: {e}", path.display()))
}

/// The file name, for messages.
pub fn base(path: &Path) -> String {
    path.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

// ---- byte strings, as Python's str methods ----------------------------------------------

pub fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    memchr::memmem::find(hay, needle)
}

pub fn find_from(hay: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if from > hay.len() {
        return None;
    }
    memchr::memmem::find(&hay[from..], needle).map(|i| i + from)
}

pub fn rfind(hay: &[u8], needle: &[u8]) -> Option<usize> {
    memchr::memmem::rfind(hay, needle)
}

pub fn contains(hay: &[u8], needle: &[u8]) -> bool {
    memchr::memmem::find(hay, needle).is_some()
}

/// Non-overlapping occurrences, as str.count.
pub fn count(hay: &[u8], needle: &[u8]) -> usize {
    memchr::memmem::find_iter(hay, needle).count()
}

/// s.replace(old, new, 1)
pub fn replace_first(s: &[u8], old: &[u8], new: &[u8]) -> Vec<u8> {
    match find(s, old) {
        Some(i) => [&s[..i], new, &s[i + old.len()..]].concat(),
        None => s.to_vec(),
    }
}

/// s.replace(old, new)
pub fn replace_all(s: &[u8], old: &[u8], new: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(s.len());
    let mut last = 0;
    for i in memchr::memmem::find_iter(s, old) {
        out.extend_from_slice(&s[last..i]);
        out.extend_from_slice(new);
        last = i + old.len();
    }
    out.extend_from_slice(&s[last..]);
    out
}

/// s[:a] + mid + s[b:]
pub fn splice(s: &[u8], a: usize, b: usize, mid: &[u8]) -> Vec<u8> {
    [&s[..a], mid, &s[b..]].concat()
}

/// Just past the line of the first #include: where the tools put their declarations.
pub fn after_first_include(src: &[u8]) -> Result<usize> {
    let inc = find(src, b"#include").ok_or("substring not found: no #include in the source")?;
    let nl = find_from(src, b"\n", inc).ok_or("substring not found: no line end after #include")?;
    Ok(nl + 1)
}

/// src with text inserted after the first #include line.
pub fn insert_after_include(src: &[u8], text: &[u8]) -> Result<Vec<u8>> {
    let i = after_first_include(src)?;
    Ok(splice(src, i, i, text))
}

/// str.splitlines(keepends=True): every line boundary Python knows, the UTF-8 ones included.
pub fn splitlines_keepends(s: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let (mut start, mut i) = (0, 0);
    while i < s.len() {
        let len = match s[i] {
            b'\r' if s.get(i + 1) == Some(&b'\n') => 2,
            b'\n' | b'\r' | 0x0b | 0x0c | 0x1c | 0x1d | 0x1e => 1,
            0xc2 if s.get(i + 1) == Some(&0x85) => 2,
            0xe2 if s.get(i + 1) == Some(&0x80) && matches!(s.get(i + 2), Some(0xa8 | 0xa9)) => 3,
            _ => 0,
        };
        if len > 0 {
            out.push(&s[start..i + len]);
            i += len;
            start = i;
        } else {
            i += 1;
        }
    }
    if start < s.len() {
        out.push(&s[start..]);
    }
    out
}

/// f over items on up to `workers` threads (0: one per core), results in item order.
pub fn parallel_map<T: Sync, R: Send>(
    items: &[T],
    workers: usize,
    f: impl Fn(&T) -> R + Sync,
) -> Vec<R> {
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicUsize, Ordering};
    let next = AtomicUsize::new(0);
    let out: Mutex<Vec<Option<R>>> = Mutex::new((0..items.len()).map(|_| None).collect());
    let cores = std::thread::available_parallelism().map_or(4, |n| n.get());
    let workers = if workers == 0 { cores } else { workers }.min(items.len().max(1));
    std::thread::scope(|s| {
        for _ in 0..workers {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(item) = items.get(i) else { break };
                    let r = f(item);
                    out.lock().unwrap()[i] = Some(r);
                }
            });
        }
    });
    out.into_inner()
        .unwrap()
        .into_iter()
        .map(|r| r.unwrap())
        .collect()
}

/// Write `contents` unless the file holds them already (through a .tmp beside it, as the
/// Python scripts' write_registry did).
pub fn write_if_changed(path: &Path, contents: &[u8]) -> Result<()> {
    if fs::read(path).is_ok_and(|old| old == contents) {
        return Ok(());
    }
    let tmp = path.with_extension("tmp");
    write(&tmp, contents)?;
    fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
}

/// versions/NAME as csv.DictReader(delimiter='\t') reads it: one map per row, keyed by the
/// header line's columns.
pub fn tsv_rows(root: &Path, name: &str) -> Result<Vec<std::collections::HashMap<String, String>>> {
    let path = root.join("versions").join(name);
    let text = fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut lines = text.lines();
    let header: Vec<String> = lines
        .next()
        .unwrap_or("")
        .split('\t')
        .map(str::to_string)
        .collect();
    Ok(lines
        .filter(|l| !l.is_empty())
        .map(|l| {
            let mut cells = l.split('\t');
            header
                .iter()
                .map(|h| (h.clone(), cells.next().unwrap_or("").to_string()))
                .collect()
        })
        .collect())
}

pub fn sha256_hex(data: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    let d = Sha256::digest(data);
    d.iter().map(|b| format!("{b:02x}")).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn str_methods() {
        assert_eq!(count(b"aaaa", b"aa"), 2);
        assert_eq!(replace_first(b"abab", b"b", b"X"), b"aXab");
        assert_eq!(replace_all(b"abab", b"b", b""), b"aa");
        assert_eq!(
            splitlines_keepends(b"a\nb\r\nc\rd"),
            vec![&b"a\n"[..], b"b\r\n", b"c\r", b"d"]
        );
        assert_eq!(splitlines_keepends(b"a\n"), vec![&b"a\n"[..]]);
        assert_eq!(
            insert_after_include(b"x\n#include \"a\"\ny\n", b"Z\n").unwrap(),
            b"x\n#include \"a\"\nZ\ny\n"
        );
    }
}
