// The installer releases (TotK-Setup-<version>-windows.zip, docs/installer.md "Release layout") of a GitHub repository:
// listed from the releases API, downloaded with progress, checked against the SHA-256 GitHub records for the asset and
// unpacked into <local data>/releases/<tag>/. The release holds no game data; the installer builds the game from the
// player's own files.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

pub const DEFAULT_REPO: &str = "valentderah/totk-pc-recomp";
pub const ALT_REPO: &str = "arnaldolima1/totk-recomp";
const AGENT: &str = concat!("totk-recomp-launcher/", env!("CARGO_PKG_VERSION"));

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Release {
    pub tag: String,
    pub name: String,
    pub prerelease: bool,
    pub published: String,
    pub notes: String,
    pub asset: String,
    pub url: String,
    #[serde(default)]
    pub api_url: String,   // the asset through the API: the download of a private repository's asset, with the token
    pub size: u64,
    pub sha256: Option<String>,
}

pub fn valid_repo(repo: &str) -> bool {
    let mut parts = repo.split('/');
    let ok = |p: Option<&str>| p.is_some_and(|p| !p.is_empty() && !p.starts_with('.') && p.chars().all(|c| c.is_ascii_alphanumeric() || "-_.".contains(c)));
    ok(parts.next()) && ok(parts.next()) && parts.next().is_none()
}

/// The repository's releases that carry a Windows setup zip, newest first.
pub fn list(repo: &str, token: Option<&str>) -> Result<Vec<Release>, String> {
    if !valid_repo(repo) {
        return Err(format!("repositório inválido: {repo}"));
    }
    let url = format!("https://api.github.com/repos/{repo}/releases?per_page=20");
    let mut req = ureq::get(&url).set("User-Agent", AGENT).set("Accept", "application/vnd.github+json").timeout(Duration::from_secs(20));
    if let Some(t) = token {
        req = req.set("Authorization", &format!("Bearer {t}"));
    }
    let body: serde_json::Value = req
        .call()
        .map_err(|e| match e {
            ureq::Error::Status(404, _) if token.is_none() => format!("{repo}: repositório não encontrado, ou privado (um token do GitHub em Avançado dá acesso)"),
            ureq::Error::Status(404, _) => format!("{repo}: repositório não encontrado, ou o token não tem acesso a ele"),
            ureq::Error::Status(401, _) => "o token do GitHub foi recusado".to_string(),
            e => e.to_string(),
        })?
        .into_json()
        .map_err(|e| e.to_string())?;
    let s = |v: &serde_json::Value, k: &str| v[k].as_str().unwrap_or_default().to_string();
    let mut out = Vec::new();
    for r in body.as_array().into_iter().flatten().filter(|r| !r["draft"].as_bool().unwrap_or(false)) {
        let Some(a) = r["assets"].as_array().into_iter().flatten().find(|a| {
            let n = a["name"].as_str().unwrap_or_default().to_ascii_lowercase();
            n.starts_with("totk-setup") && n.ends_with("windows.zip")
        }) else { continue };
        out.push(Release {
            tag: s(r, "tag_name"),
            name: s(r, "name"),
            prerelease: r["prerelease"].as_bool().unwrap_or(false),
            published: s(r, "published_at"),
            notes: s(r, "body"),
            asset: s(a, "name"),
            url: s(a, "browser_download_url"),
            api_url: s(a, "url"),
            size: a["size"].as_u64().unwrap_or(0),
            sha256: a["digest"].as_str().and_then(|d| d.strip_prefix("sha256:")).map(str::to_ascii_lowercase),
        });
    }
    Ok(out)
}

/// The folder under `dir` (itself or up to two levels down) that holds the installer.
pub fn find_root(dir: &Path) -> Option<PathBuf> {
    let is = |p: &Path| p.join("installer/__main__.py").is_file();
    if is(dir) {
        return Some(dir.to_path_buf());
    }
    let subdirs = |p: &Path| -> Vec<PathBuf> {
        std::fs::read_dir(p).map(|rd| rd.filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.is_dir()).collect()).unwrap_or_default()
    };
    subdirs(dir).into_iter().find_map(|d| if is(&d) { Some(d) } else { subdirs(&d).into_iter().find(|d| is(d)) })
}

/// Downloads `rel` into `releases/<tag>.zip`, checks it and unpacks it into `releases/<tag>/`; returns the installer
/// root inside. `progress(done, total)` is called about four times a second; `cancel` stops the download.
pub fn download(rel: &Release, releases: &Path, token: Option<&str>, cancel: &AtomicBool, progress: impl Fn(u64, u64, &str)) -> Result<PathBuf, String> {
    let tag: String = rel.tag.chars().filter(|c| c.is_ascii_alphanumeric() || "-_.".contains(*c)).collect();
    let private = token.is_some() && rel.api_url.starts_with("https://api.github.com/");
    if tag.is_empty() || !(rel.url.starts_with("https://github.com/") || private) {
        return Err("release inválido".into());
    }
    std::fs::create_dir_all(releases).map_err(|e| e.to_string())?;
    let dest = releases.join(&tag);
    if let Some(root) = find_root(&dest) {
        return Ok(root);   // (downloaded before)
    }
    let part = releases.join(format!("{tag}.zip.part"));
    let agent = ureq::AgentBuilder::new().timeout_connect(Duration::from_secs(20)).timeout_read(Duration::from_secs(60)).build();
    // A private asset comes through the API with the token; the redirect to the storage host drops the Authorization
    // header (ureq's default).
    let req = if private {
        agent.get(&rel.api_url).set("Accept", "application/octet-stream").set("Authorization", &format!("Bearer {}", token.unwrap()))
    } else {
        agent.get(&rel.url)
    };
    let resp = req.set("User-Agent", AGENT).call().map_err(|e| e.to_string())?;
    let total = resp.header("Content-Length").and_then(|v| v.parse().ok()).unwrap_or(rel.size);
    let mut from = resp.into_reader();
    let mut file = std::fs::File::create(&part).map_err(|e| e.to_string())?;
    let mut hash = Sha256::new();
    let (mut done, mut last) = (0u64, Instant::now());
    let mut buf = vec![0u8; 1 << 16];
    loop {
        if cancel.load(Ordering::Relaxed) {
            drop(file);
            let _ = std::fs::remove_file(&part);
            return Err("cancelado".into());
        }
        let n = from.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n]).map_err(|e| e.to_string())?;
        hash.update(&buf[..n]);
        done += n as u64;
        if last.elapsed() > Duration::from_millis(250) {
            progress(done, total, "download");
            last = Instant::now();
        }
    }
    file.flush().map_err(|e| e.to_string())?;
    drop(file);
    progress(done, total, "download");
    let got: String = hash.finalize().iter().map(|b| format!("{b:02x}")).collect();
    if let Some(want) = &rel.sha256 {
        if &got != want {
            let _ = std::fs::remove_file(&part);
            return Err(format!("o arquivo baixado não confere (SHA-256 {got}, esperado {want}); tente de novo"));
        }
    }
    progress(0, 0, "unpack");
    let unpacking = releases.join(format!("{tag}.unpacking"));
    let _ = std::fs::remove_dir_all(&unpacking);
    unzip(&part, &unpacking).map_err(|e| format!("não foi possível descompactar: {e}"))?;
    let _ = std::fs::remove_dir_all(&dest);
    std::fs::rename(&unpacking, &dest).map_err(|e| e.to_string())?;
    let _ = std::fs::remove_file(&part);
    find_root(&dest).ok_or_else(|| "o release não tem a pasta installer/".into())
}

fn unzip(zip_path: &Path, to: &Path) -> Result<(), String> {
    let mut zip = zip::ZipArchive::new(std::fs::File::open(zip_path).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    for i in 0..zip.len() {
        let mut entry = zip.by_index(i).map_err(|e| e.to_string())?;
        let Some(rel) = entry.enclosed_name() else { continue };   // (no absolute paths or "..")
        let out = to.join(rel);
        if entry.is_dir() {
            std::fs::create_dir_all(&out).map_err(|e| e.to_string())?;
            continue;
        }
        if let Some(p) = out.parent() {
            std::fs::create_dir_all(p).map_err(|e| e.to_string())?;
        }
        let mut f = std::fs::File::create(&out).map_err(|e| e.to_string())?;
        std::io::copy(&mut entry, &mut f).map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// Removes the downloaded releases other than `keep` (their installer temp folders go with them).
pub fn prune(releases: &Path, keep: &Path) {
    let Ok(rd) = std::fs::read_dir(releases) else { return };
    for e in rd.filter_map(|e| e.ok()) {
        let p = e.path();
        if p.is_dir() && !keep.starts_with(&p) {
            let _ = std::fs::remove_dir_all(&p);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn repos() {
        assert!(valid_repo("valentderah/totk-pc-recomp"));
        assert!(!valid_repo("a/b/c") && !valid_repo("../x") && !valid_repo("a/b?c"));
    }
}
