//! SPU inputs of an executable: its loaded segments, and for update 1.20 where each
//! disc SPU payload sits (versions/BCUS98124-1.20-spu.tsv). The same rules and messages
//! as scripts/game_version.py's update_inputs, mapped_address and changed_module.

use crate::Result;
use std::collections::BTreeMap;
use std::path::Path;

pub const UPDATE: &str = "BCUS98124-1.20";

/// The loaded segments of a big-endian ELF64: (vaddr, file offset, file size).
pub fn segments(d: &[u8]) -> Vec<(u64, u64, u64)> {
    let be16 = |o: usize| u16::from_be_bytes([d[o], d[o + 1]]) as usize;
    let be32 = |o: usize| u32::from_be_bytes(d[o..o + 4].try_into().unwrap());
    let be64 = |o: usize| u64::from_be_bytes(d[o..o + 8].try_into().unwrap());
    let phoff = be64(0x20) as usize;
    let (phentsize, phnum) = (be16(0x36), be16(0x38));
    let mut out = Vec::new();
    for i in 0..phnum {
        let p = phoff + i * phentsize;
        let (ptype, off, vaddr, filesz) = (be32(p), be64(p + 8), be64(p + 16), be64(p + 32));
        if ptype == 1 && filesz != 0 {
            out.push((vaddr, off, filesz));
        }
    }
    out
}

/// n bytes at guest address a, if one segment holds them all.
pub fn read<'a>(d: &'a [u8], sg: &[(u64, u64, u64)], a: u64, n: u64) -> Option<&'a [u8]> {
    for &(va, off, fs) in sg {
        if va <= a && a + n <= va + fs {
            let s = (off + a - va) as usize;
            return d.get(s..s + n as usize);
        }
    }
    None
}

pub fn be_words(b: &[u8]) -> Vec<u32> {
    b.as_chunks::<4>()
        .0
        .iter()
        .map(|w| u32::from_be_bytes(*w))
        .collect()
}

/// Update 1.20's SPU layout: disc address -> update address, each module's payload
/// (address, size), and its jobs (address, module, size) sorted.
pub struct UpdateInputs {
    pub addresses: BTreeMap<u64, u64>,
    pub payloads: BTreeMap<String, (u64, u64)>,
    pub jobs: Vec<(u64, String, u64)>,
}

impl UpdateInputs {
    pub fn mapped(&self, address: u64) -> Result<u64> {
        self.addresses
            .get(&address)
            .copied()
            .ok_or_else(|| format!("KeyError: {address}"))
    }

    pub fn payload(&self, name: &str) -> Result<(u64, u64)> {
        self.payloads
            .get(name)
            .copied()
            .ok_or_else(|| format!("KeyError: '{name}'"))
    }
}

/// mapped_address: the update's address of a disc address, or the address itself on a disc.
pub fn mapped(version: Option<&UpdateInputs>, address: u64) -> Result<u64> {
    match version {
        Some(v) => v.mapped(address),
        None => Ok(address),
    }
}

/// None for a disc build (its tree is at its own addresses); the update's table for 1.20.
pub fn update_inputs(root: &Path, elf: &Path) -> Result<Option<UpdateInputs>> {
    let selected = std::env::var("RACACIT_GAME_PROFILE").unwrap_or_else(|_| "auto".into());
    let profile = crate::profile::identify(root, elf, &selected, false)?;
    if profile.id == "BCUS98124-1.00" || profile.id == "BCES00511-1.00" {
        return Ok(None);
    }
    if profile.id != UPDATE {
        return Err(format!("No SPU input implementation for {}", profile.id));
    }
    let path = root.join("versions").join(format!("{UPDATE}-spu.tsv"));
    let text = std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut lines = text.lines();
    let header: Vec<&str> = lines.next().unwrap_or("").split('\t').collect();
    let col = |name: &str| {
        header
            .iter()
            .position(|h| *h == name)
            .ok_or_else(|| format!("{}: no {name} column", path.display()))
    };
    let (c_module, c_disc, c_bytes, c_new, c_status) = (
        col("module")?,
        col("disc_ea")?,
        col("bytes")?,
        col("update120_ea")?,
        col("status")?,
    );
    let hex =
        |s: &str| u64::from_str_radix(s, 16).map_err(|e| format!("{}: {s}: {e}", path.display()));
    let mut v = UpdateInputs {
        addresses: BTreeMap::new(),
        payloads: BTreeMap::new(),
        jobs: Vec::new(),
    };
    for line in lines {
        if line.is_empty() {
            continue;
        }
        let row: Vec<&str> = line.split('\t').collect();
        let get = |i: usize| row.get(i).copied().unwrap_or("");
        let (module, status) = (get(c_module), get(c_status));
        if status == "CHANGED_HEADER_PORT" {
            if module != "sub_DB4180_10680" {
                return Err(format!("Unsupported changed SPU module {module}"));
            }
        } else if status != "UNIQUE_FULL_PAYLOAD_MATCH" {
            return Err(format!("Unresolved SPU module {module}"));
        }
        let (old, new) = (hex(get(c_disc))?, hex(get(c_new))?);
        let bytes: u64 = get(c_bytes)
            .parse()
            .map_err(|e| format!("{}: {e}", path.display()))?;
        if v.addresses.get(&old).is_some_and(|&a| a != new) {
            return Err("Conflicting SPU address mapping".into());
        }
        v.addresses.insert(old, new);
        v.payloads.insert(module.to_string(), (new, bytes));
        if module.starts_with("job_") {
            v.jobs.push((new, module.to_string(), bytes));
        }
    }
    v.jobs.sort();
    Ok(Some(v))
}

/// The relocatable entry and proven code end of update 1.20's changed module (its bytes).
pub fn changed_module(raw: &[u8]) -> Result<(u64, u64)> {
    if raw.len() != 0x5700 {
        return Err("Changed update module envelope differs".into());
    }
    let w = be_words(&raw[..48]);
    if !w[..4].iter().all(|&x| x & 0xFE00007F == 0x42000002) {
        return Err("Changed update module GUID header differs".into());
    }
    let mut entry = ((w[6] & 0xFFFF) * 4) as usize;
    if entry == 0 {
        if w[5..] != [0, 0, 0x10, 0xFFFFFFEF, 0, 0xDEADDEAD, 0xDEADDEAD] {
            return Err("Unsupported SPU entry header".into());
        }
        entry = 32 + w[4] as usize;
    }
    if entry != 0x20 {
        return Err("Changed update module entry differs".into());
    }
    let literal = b"ASSERT FAILED";
    let code_end = crate::find(raw, literal);
    let again = code_end.and_then(|c| crate::find_from(raw, literal, c + 1));
    match code_end {
        Some(c) if c > entry + 4 && c % 16 == 0 && again.is_none() => {
            for word in be_words(&raw[entry..c]) {
                if matches!(word >> 23, 0x60 | 0x62) {
                    return Err("Absolute SPU branch requires a separate relocation port".into());
                }
            }
            Ok((0x100 + entry as u64, 0x100 + c as u64))
        }
        _ => Err("Changed update module literal boundary differs".into()),
    }
}
