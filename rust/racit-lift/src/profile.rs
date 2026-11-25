//! The game profiles of versions/profiles.json, and which one an executable is (by SHA-256).
//! The same rules and messages as scripts/game_version.py's identify_profile.

use crate::Result;
use serde_json::Value;
use std::fs::File;
use std::io::Read;
use std::path::Path;

pub struct Profile {
    pub id: String,
    pub elf_sha256: String,
    pub app_version: String,
    pub title_id: String,
    pub buildable: bool,
    pub status: String,
}

fn field(p: &Value, key: &str) -> String {
    p.get(key).and_then(Value::as_str).unwrap_or_default().to_string()
}

pub fn load(root: &Path) -> Result<Vec<Profile>> {
    let path = root.join("versions/profiles.json");
    let text = std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let json: Value = serde_json::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    let list = json.get("profiles").and_then(Value::as_array).ok_or(format!("{}: no profiles", path.display()))?;
    Ok(list
        .iter()
        .map(|p| Profile {
            id: field(p, "id"),
            elf_sha256: field(p, "elf_sha256"),
            app_version: field(p, "app_version"),
            title_id: field(p, "title_id"),
            buildable: p.get("buildable").and_then(Value::as_bool).unwrap_or(false),
            status: field(p, "status"),
        })
        .collect())
}

pub fn sha256_file(path: &Path) -> Result<String> {
    use sha2::{Digest, Sha256};
    let mut f = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).map_err(|e| format!("{}: {e}", path.display()))?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

pub fn identify(root: &Path, elf: &Path, selected: &str, require_buildable: bool) -> Result<Profile> {
    let profiles = load(root)?;
    let digest = sha256_file(elf)?;
    let actual = profiles
        .into_iter()
        .find(|p| p.elf_sha256 == digest)
        .ok_or_else(|| format!("Unrecognized executable SHA-256 {digest}"))?;
    if selected != "auto" && selected != actual.id {
        return Err(format!("Selected {selected} but the executable is {}", actual.id));
    }
    if require_buildable && !actual.buildable {
        return Err(format!("Recognized {}, but this source tree cannot build it: {}", actual.id, actual.status));
    }
    Ok(actual)
}
