// The install folder's RomFS mods (mods.py does the work with the release's Python): <install>/mods/<name>/ folders,
// the enabled ones in their priority order in <install>/mods/mods.json, merged into <install>/mods/.merged, which
// totk.ini's romfs_mods points at. mods.json also keeps a stamp of what the merge was built from, so the launcher
// rebuilds it before playing when a mod or the game files changed.
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

pub const SCRIPT: &str = include_str!("mods.py");

#[derive(Serialize, Deserialize, Default, Clone)]
pub struct State {
    #[serde(default)]
    pub enabled: Vec<String>,
    #[serde(default)]
    pub stamp: String,
}

pub fn dir(home: &Path) -> PathBuf {
    home.join("mods")
}

pub fn load(home: &Path) -> State {
    std::fs::read_to_string(dir(home).join("mods.json")).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_default()
}

pub fn save(home: &Path, state: &State) -> Result<(), String> {
    std::fs::create_dir_all(dir(home)).map_err(|e| e.to_string())?;
    std::fs::write(dir(home).join("mods.json"), serde_json::to_string_pretty(state).unwrap()).map_err(|e| e.to_string())
}

/// A mod name as a folder of mods/: one path component, not hidden.
pub fn valid_name(home: &Path, name: &str) -> bool {
    !name.is_empty() && !name.starts_with('.') && !name.contains(['/', '\\', ':']) && name != ".." && dir(home).join(name).is_dir()
}

fn walk(p: &Path, acc: &mut (u64, u64, u64)) {
    let Ok(rd) = std::fs::read_dir(p) else { return };
    for e in rd.filter_map(|e| e.ok()) {
        let Ok(m) = e.metadata() else { continue };
        if m.is_dir() {
            walk(&e.path(), acc);
        } else {
            acc.0 += 1;
            acc.1 += m.len();
            acc.2 ^= m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs());
        }
    }
}

/// What a merge of `enabled` depends on: each mod's file count, size and times, and the game's RomFS index.
pub fn stamp(home: &Path, enabled: &[String]) -> String {
    let mut parts = Vec::new();
    for name in enabled {
        let mut acc = (0, 0, 0);
        walk(&dir(home).join(name), &mut acc);
        parts.push(format!("{name}:{}:{}:{}", acc.0, acc.1, acc.2));
    }
    let index = std::fs::metadata(home.join("romfs-index.bin")).ok();
    let t = index.as_ref().and_then(|m| m.modified().ok()).and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs());
    parts.push(format!("index:{}:{t}", index.map_or(0, |m| m.len())));
    parts.join("|")
}

/// Copies a mod folder, or unpacks a mod zip, into mods/<its name>; returns the name.
pub fn add(home: &Path, src: &Path) -> Result<String, String> {
    let stem = src.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let name = if src.is_dir() { src.file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default() } else { stem };
    let name = name.trim().trim_start_matches('.').to_string();
    if name.is_empty() {
        return Err("nome de mod inválido".into());
    }
    let dest = dir(home).join(&name);
    if dest.exists() {
        return Err(format!("Já existe um mod chamado \"{name}\"."));
    }
    std::fs::create_dir_all(&dest).map_err(|e| e.to_string())?;
    let result = if src.is_dir() { copy_dir(src, &dest) } else { unzip(src, &dest) };
    if let Err(e) = result {
        let _ = std::fs::remove_dir_all(&dest);
        return Err(e);
    }
    Ok(name)
}

fn copy_dir(from: &Path, to: &Path) -> Result<(), String> {
    for e in std::fs::read_dir(from).map_err(|e| e.to_string())?.filter_map(|e| e.ok()) {
        let (s, d) = (e.path(), to.join(e.file_name()));
        if s.is_dir() {
            std::fs::create_dir_all(&d).map_err(|e| e.to_string())?;
            copy_dir(&s, &d)?;
        } else {
            std::fs::copy(&s, &d).map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}

fn unzip(zip_path: &Path, to: &Path) -> Result<(), String> {
    let f = std::fs::File::open(zip_path).map_err(|e| e.to_string())?;
    let mut zip = zip::ZipArchive::new(f).map_err(|_| "Só pastas ou arquivos .zip (descompacte .rar e .7z antes).".to_string())?;
    for i in 0..zip.len() {
        let mut entry = zip.by_index(i).map_err(|e| e.to_string())?;
        let Some(rel) = entry.enclosed_name() else { continue };
        let out = to.join(rel);
        if entry.is_dir() {
            std::fs::create_dir_all(&out).map_err(|e| e.to_string())?;
            continue;
        }
        if let Some(p) = out.parent() {
            std::fs::create_dir_all(p).map_err(|e| e.to_string())?;
        }
        let mut w = std::fs::File::create(&out).map_err(|e| e.to_string())?;
        std::io::copy(&mut entry, &mut w).map_err(|e| e.to_string())?;
    }
    Ok(())
}
