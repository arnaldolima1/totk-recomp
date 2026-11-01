// Launcher: settings (totk.ini next to totk.exe, ini.rs), the installer release (downloaded from GitHub, release.rs), the install (`python -m installer` of a release folder, its JSON
// events streamed to the window) and the game, started under the launcher's transition screen, which hides once the
// game's window shows. Two folders: the release (installer/, python/, tools/ ... as docs/installer.md "Release layout", or a
// repository checkout in development) and the install folder the installer fills (totk.exe, totk-install.json).
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod ini;
mod mods;
mod release;
mod room;

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::io::{BufRead, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::thread;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager, State};

#[cfg(windows)]
use std::os::windows::process::CommandExt;
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
#[cfg(windows)]
const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;

#[derive(Default)]
struct Procs {
    build: Mutex<Option<u32>>,
    game: Mutex<Option<u32>>,
    downloading: AtomicBool,
    cancel_download: AtomicBool,
}

#[derive(Serialize)]
struct Status {
    home: String,
    installed: bool,                 // totk.exe and a finished install (totk-install.json)
    has_state: bool,                 // totk-install.json: the installer resumes or updates, the game files are optional
    release: Option<String>,         // the release folder the install runs from
    release_version: Option<String>, // its VERSION
    installed_release: Option<String>,
    game_version: Option<String>,    // the update the folder was built from (1.2.1, 1.4.3)
    lang: Option<String>,            // the game language recorded by the installer
    base_name: Option<String>,       // file names of the recorded inputs
    update_name: Option<String>,
    free_gb: Option<f64>,            // free space where the install folder is
    building: bool,
    running: bool,
    downloading: bool,
    dev: bool,                       // the release is a repository checkout
    repo: String,                    // the GitHub repository the releases come from
    multiplayer: bool,               // the release has the room server (tools/mp_server.py): the Multiplayer tab shows
}

#[derive(Serialize, Clone)]
struct Line {
    text: String,
    err: bool,
}

#[cfg(unix)]
use std::os::unix::process::CommandExt;

const GAME_EXE: &str = if cfg!(windows) { "totk.exe" } else { "totk" };
const STATE_FILE: &str = "totk-install.json";

/// No console window (Windows); its own process group, so kill_tree ends what it started too (elsewhere).
fn no_window(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW);
    #[cfg(unix)]
    cmd.process_group(0);
    cmd
}

/// The Python that runs the installer: the release's own (python/), in a checkout the embeddable one the release ships
/// (installer/layout.py, dev mode) or the extraction environment, else (macOS, Linux) the system's.
fn python(root: &Path) -> Option<PathBuf> {
    let own: &[&str] = if cfg!(windows) {
        &["python/python.exe", "build/py314-embed/py/python.exe", ".venv-py314/Scripts/python.exe"]
    } else {
        &[".venv-py314/bin/python3", "/opt/homebrew/bin/python3", "/usr/local/bin/python3", "/usr/bin/python3"]
    };
    own.iter().map(|p| root.join(p)).find(|p| p.is_file())
}

fn is_release(p: &Path) -> bool {
    p.join("installer/__main__.py").is_file() && p.join("installer/text/en.json").is_file()
}

fn config_file(app: &AppHandle) -> Option<PathBuf> {
    app.path().app_config_dir().ok().map(|d| d.join("launcher.json"))
}

fn read_config(app: &AppHandle) -> serde_json::Map<String, serde_json::Value> {
    config_file(app)
        .and_then(|f| std::fs::read_to_string(f).ok())
        .and_then(|s| serde_json::from_str::<serde_json::Value>(&s).ok())
        .and_then(|v| v.as_object().cloned())
        .unwrap_or_default()
}

fn write_config(app: &AppHandle, key: &str, value: &str) -> Result<(), String> {
    let f = config_file(app).ok_or("sem pasta de configuração")?;
    std::fs::create_dir_all(f.parent().unwrap()).map_err(|e| e.to_string())?;
    let mut cfg = read_config(app);
    cfg.insert(key.into(), value.into());
    std::fs::write(f, serde_json::Value::Object(cfg).to_string()).map_err(|e| e.to_string())
}

/// totk-install.json of the install folder, when it has one.
fn install_state(h: &Path) -> Option<serde_json::Value> {
    serde_json::from_str(&std::fs::read_to_string(h.join(STATE_FILE)).ok()?).ok()
}

/// A folder the installer accepts as the install folder: new, empty, or holding an earlier install
/// (installer check.dest_not_empty refuses anything else).
fn usable_home(p: &Path) -> bool {
    !p.exists() || p.join(STATE_FILE).is_file() || std::fs::read_dir(p).map_or(false, |mut d| d.next().is_none())
}

/// Free bytes on the disk of `p` (or of its nearest existing parent).
fn free_space(p: &Path) -> Option<u64> {
    let p = p.ancestors().find(|a| a.exists())?;
    #[cfg(unix)]
    {
        let c = std::ffi::CString::new(p.as_os_str().as_encoded_bytes()).ok()?;
        let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
        if unsafe { libc::statvfs(c.as_ptr(), &mut st) } != 0 {
            return None;
        }
        Some(st.f_bavail as u64 * st.f_frsize as u64)
    }
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        let w: Vec<u16> = p.as_os_str().encode_wide().chain(Some(0)).collect();
        let mut free = 0u64;
        let ok = unsafe { windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW(w.as_ptr(), &mut free, std::ptr::null_mut(), std::ptr::null_mut()) };
        (ok != 0).then_some(free)
    }
}

/// The release folder the install runs from: TOTK_LAUNCHER_RELEASE, the one chosen or downloaded in the launcher, else
/// a release unpacked around the launcher (its own folder or one above it, with the release's python/). A repository
/// checkout is used only when chosen: its dev mode needs the build toolchain (docs/building.md), which players lack.
fn release_root(app: &AppHandle) -> Option<PathBuf> {
    let env = std::env::var("TOTK_LAUNCHER_RELEASE").ok().map(PathBuf::from);
    let saved = read_config(app).get("release").and_then(|r| r.as_str()).map(PathBuf::from);
    let exe = std::env::current_exe().ok();
    let around = exe.iter().flat_map(|e| e.ancestors().skip(1).take(8).map(Path::to_path_buf))
        .filter(|d| d.join(if cfg!(windows) { "python/python.exe" } else { "python/bin/python3" }).is_file());
    env.into_iter().chain(saved).filter(|d| is_release(d)).chain(around.filter(|d| is_release(d))).next()
}

/// The install folder: TOTK_LAUNCHER_HOME, the one chosen in the launcher, the launcher's own folder when the game is
/// installed there (a portable copy), else Games\TotK Recomp in the user's home.
fn home(app: &AppHandle) -> PathBuf {
    if let Ok(h) = std::env::var("TOTK_LAUNCHER_HOME") {
        return PathBuf::from(h);
    }
    if let Some(saved) = read_config(app).get("home").and_then(|h| h.as_str()).map(PathBuf::from) {
        return saved;
    }
    let own = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf));
    if let Some(d) = own.filter(|d| d.join(GAME_EXE).is_file() || d.join(STATE_FILE).is_file()) {
        return d;
    }
    app.path().home_dir().map(|h| h.join("Games").join("TotK Recomp")).unwrap_or_default()
}

#[tauri::command]
fn set_home(app: AppHandle, path: String) -> Result<(), String> {
    if !usable_home(Path::new(&path)) {
        return Err("A pasta de instalação precisa estar vazia ou ter uma instalação anterior (totk-install.json).".into());
    }
    write_config(&app, "home", &path)
}

#[tauri::command]
fn set_release(app: AppHandle, path: String) -> Result<(), String> {
    if !is_release(Path::new(&path)) {
        return Err("Essa pasta não tem o instalador (installer/).".into());
    }
    write_config(&app, "release", &path)
}

#[tauri::command]
fn status(app: AppHandle, procs: State<Procs>) -> Status {
    let h = home(&app);
    let release = release_root(&app);
    let state = install_state(&h);
    let field = |path: &[&str]| -> Option<String> {
        path.iter().try_fold(state.as_ref()?, |v, k| v.get(k)).and_then(|v| v.as_str()).map(String::from)
    };
    // The installer records the steps that build something; check and cleanup run every time and are never recorded
    // (installer/runner.py `always`), so a finished install is one whose last recorded step, prepare, is there.
    let finished = state.as_ref().and_then(|s| s["steps"].get("prepare")).is_some();
    Status {
        installed: h.join(GAME_EXE).is_file() && finished,
        has_state: state.is_some(),
        release_version: release.as_ref()
            .and_then(|r| std::fs::read_to_string(r.join("VERSION")).ok())
            .map(|v| v.trim().to_string()),
        dev: release.as_ref().is_some_and(|r| r.join(".git").exists()),
        release: release.map(|r| r.to_string_lossy().into_owned()),
        installed_release: field(&["release"]),
        game_version: field(&["version"]),
        lang: field(&["lang"]),
        base_name: field(&["inputs", "base", "name"]),
        update_name: field(&["inputs", "update", "name"]),
        free_gb: free_space(&h).map(|b| b as f64 / 1e9),
        building: procs.build.lock().unwrap().is_some(),
        downloading: procs.downloading.load(Ordering::Relaxed),
        repo: source(&app).repo,
        multiplayer: release_root(&app).is_some_and(|r| r.join("tools/mp_server.py").is_file()),
        running: procs.game.lock().unwrap().is_some(),
        home: h.to_string_lossy().into_owned(),
    }
}

/// The installer's English message catalog (installer/text/en.json): ids of steps, phases, errors and warnings
/// with their text and {param} placeholders.
#[tauri::command]
fn installer_text(app: AppHandle) -> serde_json::Value {
    release_root(&app)
        .and_then(|r| std::fs::read_to_string(r.join("installer/text/en.json")).ok())
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

#[derive(Serialize, Default)]
struct Art {
    backgrounds: Vec<String>,
    logo: Option<String>,
    loading: Option<String>,
    music: Option<String>,
}

/// The player's own artwork, from launcher-art\ in the game folder (data: URLs): bg*.jpg/png/webp, logo.png,
/// loading.jpg/png, music.mp3/ogg. The launcher ships with neutral art of its own and uses these instead when present.
#[tauri::command]
fn custom_art(app: AppHandle) -> Art {
    use base64::Engine;
    let dir = home(&app).join("launcher-art");
    let mut art = Art::default();
    let Ok(rd) = std::fs::read_dir(&dir) else { return art };
    let mut names: Vec<String> = rd.filter_map(|e| e.ok()).filter_map(|e| e.file_name().into_string().ok()).collect();
    names.sort();
    let url = |name: &str| -> Option<String> {
        let ext = name.rsplit('.').next()?.to_ascii_lowercase();
        let mime = match ext.as_str() {
            "jpg" | "jpeg" => "image/jpeg",
            "png" => "image/png",
            "webp" => "image/webp",
            "mp3" => "audio/mpeg",
            "ogg" => "audio/ogg",
            _ => return None,
        };
        let bytes = std::fs::read(dir.join(name)).ok()?;
        if bytes.len() > 32 << 20 {
            return None;
        }
        Some(format!("data:{mime};base64,{}", base64::engine::general_purpose::STANDARD.encode(bytes)))
    };
    for n in &names {
        let lower = n.to_ascii_lowercase();
        let stem = lower.rsplit_once('.').map_or(lower.as_str(), |(s, _)| s);
        if stem.starts_with("bg") {
            if let Some(u) = url(n) { art.backgrounds.push(u); }
        } else if stem == "logo" && art.logo.is_none() {
            art.logo = url(n);
        } else if stem == "loading" && art.loading.is_none() {
            art.loading = url(n);
        } else if stem == "music" && art.music.is_none() {
            art.music = url(n);
        }
    }
    art
}

/// The room this launcher hosts (room.rs): its state, the addresses to give friends and who is in it.
#[tauri::command]
fn room_info(room: State<room::Room>) -> room::RoomInfo {
    room.info()
}

#[tauri::command]
async fn room_start(app: AppHandle, name: String, port: u16, password: String) -> Result<room::RoomInfo, String> {
    let release = release_root(&app).ok_or("Escolha o instalador em Instalação primeiro.")?;
    let py = python(&release).ok_or("O instalador não tem o Python dele (python/).")?;
    let room = app.state::<room::Room>();
    room.start(&app, &py, &release, &name, if port == 0 { room::DEFAULT_PORT } else { port }, &password)
}

#[tauri::command]
fn room_stop(room: State<room::Room>) {
    room.stop();
}

/// Whether a room answers at an address (room.rs test): the milliseconds it took, or why not.
#[tauri::command]
async fn mp_test(server: String) -> Result<u64, String> {
    tauri::async_runtime::spawn_blocking(move || room::test(&server)).await.map_err(|e| e.to_string())?
}

/// `--view <name>` on the command line: the tab to open on (screenshots, shortcuts).
#[tauri::command]
fn start_view() -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    args.iter().position(|a| a == "--view").and_then(|i| args.get(i + 1).cloned())
}

fn ini_path(app: &AppHandle) -> PathBuf {
    home(app).join("totk.ini")
}

/// The keys set in totk.ini (lower-case, without the TOTK_ prefix); unset keys are at the game's defaults.
#[tauri::command]
fn read_ini(app: AppHandle) -> BTreeMap<String, String> {
    std::fs::read_to_string(ini_path(&app)).map(|t| ini::read(&t)).unwrap_or_default()
}

/// Sets keys in totk.ini (an empty value puts a key back to its default) and keeps every other line. A folder without
/// totk.ini yet starts from the release's totk.ini.example, as the installer does.
#[tauri::command]
fn write_ini(app: AppHandle, changes: BTreeMap<String, String>) -> Result<(), String> {
    let path = ini_path(&app);
    let mut text = match std::fs::read_to_string(&path) {
        Ok(t) => t,
        Err(_) => release_root(&app).and_then(|r| std::fs::read_to_string(r.join("totk.ini.example")).ok()).unwrap_or_default(),
    };
    for (k, v) in &changes {
        let k = k.trim().to_ascii_lowercase();
        if k.is_empty() || !k.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') || v.contains(['\n', '\r']) {
            return Err(format!("chave inválida: {k}"));
        }
        text = ini::set(&text, &k, v.trim());
    }
    std::fs::create_dir_all(home(&app)).map_err(|e| e.to_string())?;
    let tmp = path.with_extension("ini.tmp");
    std::fs::write(&tmp, text).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())
}

/// Where the installer releases come from: "official" (valentderah/totk-pc-recomp) or "alt" (a repository of the
/// player's choice, by default this launcher's own). A GitHub token, when set (in the launcher, else the
/// TOTK_GITHUB_TOKEN environment variable), is sent to GitHub only, for a private repository.
#[derive(Serialize)]
struct Source {
    source: String,
    official: String,
    alt: String,
    repo: String,
    has_token: bool,
}

fn source(app: &AppHandle) -> Source {
    let cfg = read_config(app);
    let s = |k: &str| cfg.get(k).and_then(|v| v.as_str()).map(str::trim).filter(|v| !v.is_empty()).map(String::from);
    let which = s("source").filter(|v| v == "alt").unwrap_or_else(|| "official".into());
    let alt = s("repo_alt").unwrap_or_else(|| release::ALT_REPO.into());
    Source {
        repo: if which == "alt" { alt.clone() } else { release::DEFAULT_REPO.into() },
        source: which,
        official: release::DEFAULT_REPO.into(),
        alt,
        has_token: token(app).is_some(),
    }
}

fn token(app: &AppHandle) -> Option<String> {
    let saved = read_config(app).get("token").and_then(|v| v.as_str()).map(str::trim).filter(|v| !v.is_empty()).map(String::from);
    saved.or_else(|| std::env::var("TOTK_GITHUB_TOKEN").ok().map(|t| t.trim().to_string()).filter(|t| !t.is_empty()))
}

#[tauri::command]
fn get_source(app: AppHandle) -> Source {
    source(&app)
}

/// Sets the source, the alternative repository and the token (None keeps the saved token, "" removes it).
#[tauri::command]
fn set_source(app: AppHandle, source: String, alt: String, token: Option<String>) -> Result<(), String> {
    let alt = alt.trim();
    if !alt.is_empty() && !release::valid_repo(alt) {
        return Err(format!("repositório inválido: {alt}"));
    }
    write_config(&app, "source", if source == "alt" { "alt" } else { "official" })?;
    write_config(&app, "repo_alt", alt)?;
    if let Some(t) = token {
        write_config(&app, "token", t.trim())?;
    }
    Ok(())
}

/// The source's installer releases, newest first (release.rs list).
#[tauri::command]
async fn releases(app: AppHandle) -> Result<Vec<release::Release>, String> {
    let (repo, token) = (source(&app).repo, token(&app));
    tauri::async_runtime::spawn_blocking(move || release::list(&repo, token.as_deref())).await.map_err(|e| e.to_string())?
}

// ---------------------------------------------------------------- mods (mods.rs, mods.py)

/// Runs mods.py with the release's Python in the install folder: its stdout lines, each a JSON object, go to `line`.
fn run_mods_py(app: &AppHandle, args: &[String], line: impl FnMut(serde_json::Value)) -> Result<(), String> {
    run_py(app, "mods.py", mods::SCRIPT, args, line)
}

/// Runs one of the launcher's scripts (mods.py, adapt.py) with the release's Python in the install folder.
fn run_py(app: &AppHandle, name: &str, source: &str, args: &[String], mut line: impl FnMut(serde_json::Value)) -> Result<(), String> {
    let h = home(app);
    if !h.join("romfs-index.bin").is_file() {
        return Err("Instale o jogo antes de usar mods.".into());
    }
    let root = release_root(app).ok_or("Os mods precisam do instalador (baixe-o em Instalação).")?;
    let py = python(&root).ok_or("O Python do instalador não foi encontrado.")?;
    let dir = app.path().app_local_data_dir().map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let script = dir.join(name);
    std::fs::write(&script, source).map_err(|e| e.to_string())?;
    let mut cmd = Command::new(py);
    cmd.arg(&script).args(args).arg("--home").arg(&h).current_dir(&h)
        .env("PYTHONNOUSERSITE", "1").env("PYTHONIOENCODING", "utf-8").env("PYTHONUTF8", "1")
        .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = no_window(&mut cmd).spawn().map_err(|e| e.to_string())?;
    let mut err_text = String::new();
    let mut stderr = child.stderr.take().unwrap();
    let err_thread = thread::spawn(move || { let mut s = String::new(); let _ = stderr.read_to_string(&mut s); s });
    let mut last_error = None;
    for l in std::io::BufReader::new(child.stdout.take().unwrap()).lines().map_while(Result::ok) {
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&l) {
            if v["type"] == "error" {
                last_error = v["message"].as_str().map(String::from);
            }
            line(v);
        }
    }
    let ok = child.wait().map(|s| s.success()).unwrap_or(false);
    err_text += &err_thread.join().unwrap_or_default();
    match (ok, last_error) {
        (true, _) => Ok(()),
        (false, Some(e)) => Err(e),
        (false, None) => Err(err_text.lines().last().map(String::from).unwrap_or_else(|| format!("{name} falhou"))),
    }
}

/// The mods of mods/ (mods.py scan) with the enabled ones in priority order.
#[tauri::command]
async fn mods_list(app: AppHandle) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let mut out = serde_json::Value::Null;
        run_mods_py(&app, &["scan".into()], |v| out = v)?;
        let state = mods::load(&home(&app));
        out["enabled"] = serde_json::json!(state.enabled);
        out["stale"] = serde_json::json!(!state.enabled.is_empty() && state.stamp != mods::stamp(&home(&app), &state.enabled));
        Ok(out)
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Enables `enabled` (in priority order): merges them (mods.py build, "mods-progress" events) and points totk.ini at
/// the merge; an empty list turns mods off. Returns mods.py's result.
#[tauri::command]
async fn mods_apply(app: AppHandle, enabled: Vec<String>) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let h = home(&app);
        if let Some(bad) = enabled.iter().find(|n| !mods::valid_name(&h, n)) {
            return Err(format!("mod não encontrado: {bad}"));
        }
        let mut changes = BTreeMap::new();
        let mut result = serde_json::json!({ "files": 0 });
        if enabled.is_empty() {
            changes.insert("romfs_mods".to_string(), String::new());
            changes.insert("romfs_patches".to_string(), String::new());
            let _ = std::fs::remove_dir_all(mods::dir(&h).join(".merged"));
        } else {
            let mut args = vec!["build".to_string(), "--mods".to_string()];
            args.extend(enabled.iter().cloned());
            run_mods_py(&app, &args, |v| {
                if v["type"] == "progress" {
                    let _ = app.emit("mods-progress", v.clone());
                } else if v["type"] == "result" {
                    result = v;
                }
            })?;
            changes.insert("romfs_mods".to_string(), mods::dir(&h).join(".merged").to_string_lossy().into_owned());
            changes.insert("romfs_patches".to_string(), if result["patches_merged"] == true { "0".into() } else { String::new() });
        }
        write_ini(app.clone(), changes)?;
        let stamp = mods::stamp(&h, &enabled);
        mods::save(&h, &mods::State { enabled, stamp })?;
        Ok(result)
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Copies a mod folder or unpacks a mod zip into mods/.
#[tauri::command]
async fn mods_add(app: AppHandle, path: String) -> Result<String, String> {
    let h = home(&app);
    tauri::async_runtime::spawn_blocking(move || mods::add(&h, Path::new(&path))).await.map_err(|e| e.to_string())?
}

/// Adapts a mod made for another game version to the installed one (adapt.py): a mod folder of mods/ or a folder or
/// zip elsewhere becomes the new mod "<name> (<installed version>)" in mods/; "adapt-progress" events while it runs.
/// Returns adapt.py's result (what was copied, merged and left out).
#[tauri::command]
async fn mods_adapt(app: AppHandle, src: String) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let h = home(&app);
        let src_path = if mods::valid_name(&h, &src) { mods::dir(&h).join(&src) } else { PathBuf::from(&src) };
        if !src_path.exists() {
            return Err(format!("mod não encontrado: {src}"));
        }
        let version = install_state(&h).and_then(|s| s["version"].as_str().map(String::from)).unwrap_or_else(|| "adaptado".into());
        let stem = src_path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "mod".into());
        let stem = stem.trim_start_matches('.').trim().to_string();
        let mut name = format!("{stem} ({version})");
        let mut n = 2;
        while mods::dir(&h).join(&name).exists() {
            name = format!("{stem} ({version}) {n}");
            n += 1;
        }
        let out = mods::dir(&h).join(&name);
        let args = vec!["--src".to_string(), src_path.to_string_lossy().into_owned(), "--out".to_string(), out.to_string_lossy().into_owned()];
        let mut result = serde_json::Value::Null;
        let run = run_py(&app, "adapt.py", mods::ADAPT_SCRIPT, &args, |v| {
            if v["type"] == "progress" {
                let _ = app.emit("adapt-progress", v.clone());
            } else if v["type"] == "result" {
                result = v;
            }
        });
        if let Err(e) = run {
            let _ = std::fs::remove_dir_all(&out);
            return Err(e);
        }
        result["name"] = serde_json::json!(name);
        Ok(result)
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Deletes a mod folder (the launcher asks first).
#[tauri::command]
fn mods_remove(app: AppHandle, name: String) -> Result<(), String> {
    let h = home(&app);
    if !mods::valid_name(&h, &name) {
        return Err(format!("mod não encontrado: {name}"));
    }
    std::fs::remove_dir_all(mods::dir(&h).join(&name)).map_err(|e| e.to_string())
}

#[derive(Serialize, Clone)]
struct DlProgress {
    done: u64,
    total: u64,
    phase: &'static str,
}

/// Downloads and unpacks a release into the launcher's data folder and makes it the installer release: "dl-progress"
/// while it runs, "dl-done" with null or the error when it ends. Older downloaded releases are removed after.
#[tauri::command]
fn download_release(app: AppHandle, procs: State<Procs>, rel: release::Release) -> Result<(), String> {
    if procs.build.lock().unwrap().is_some() {
        return Err("Espere a instalação terminar.".into());
    }
    if procs.downloading.swap(true, Ordering::SeqCst) {
        return Err("O download já está rodando.".into());
    }
    procs.cancel_download.store(false, Ordering::SeqCst);
    let tok = token(&app);
    let dir = match app.path().app_local_data_dir() {
        Ok(d) => d.join("releases"),
        Err(e) => {
            procs.downloading.store(false, Ordering::SeqCst);
            return Err(e.to_string());
        }
    };
    thread::spawn(move || {
        let procs = app.state::<Procs>();
        let emit = |done, total, phase: &str| {
            let phase = if phase == "unpack" { "unpack" } else { "download" };
            let _ = app.emit("dl-progress", DlProgress { done, total, phase });
        };
        let result = release::download(&rel, &dir, tok.as_deref(), &procs.cancel_download, emit).and_then(|root| {
            write_config(&app, "release", &root.to_string_lossy())?;
            release::prune(&dir, &root);
            Ok(())
        });
        procs.downloading.store(false, Ordering::SeqCst);
        let _ = app.emit("dl-done", result.err());
    });
    Ok(())
}

#[derive(Serialize, Default)]
struct Found {
    base: Option<String>,
    update: Option<String>,
    keys: Option<String>,
}

/// Game files under `dir` (three levels down) by name: the base game and the update by their title ids
/// (0100F2C0115B6000, 0100F2C0115B6800) or names, the newest update by its [v...] number; prod.keys there or in the
/// usual emulator key folders. Only paths: the installer identifies the files by hash and reads the keys itself.
#[tauri::command]
async fn find_game_files(app: AppHandle, dir: String) -> Found {
    let appdata = app.path().config_dir().ok();
    tauri::async_runtime::spawn_blocking(move || {
        let mut files = Vec::new();
        let mut stack = vec![(PathBuf::from(&dir), 0)];
        while let Some((d, depth)) = stack.pop() {
            let Ok(rd) = std::fs::read_dir(&d) else { continue };
            for e in rd.filter_map(|e| e.ok()) {
                let p = e.path();
                if p.is_dir() {
                    if depth < 3 { stack.push((p, depth + 1)); }
                } else {
                    files.push(p);
                }
            }
        }
        let name = |p: &PathBuf| p.file_name().map(|n| n.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
        let game = |n: &str| n.ends_with(".nsp") || n.ends_with(".xci");
        let is_update = |n: &str| n.contains("0100f2c0115b6800") || n.contains("update") || n.contains("[upd]");
        let is_totk = |n: &str| n.contains("0100f2c0115b6") || n.contains("totk") || n.contains("tears of the kingdom");
        let update_version = |n: &str| -> u64 {
            n.split("[v").nth(1).and_then(|s| s.split(']').next()).and_then(|v| v.parse().ok()).unwrap_or(0)
        };
        let mut found = Found::default();
        found.base = files.iter().filter(|p| { let n = name(p); game(&n) && is_totk(&n) && !is_update(&n) })
            .max_by_key(|p| p.metadata().map(|m| m.len()).unwrap_or(0)).map(|p| p.to_string_lossy().into_owned());
        found.update = files.iter().filter(|p| { let n = name(p); n.ends_with(".nsp") && is_totk(&n) && is_update(&n) })
            .max_by_key(|p| update_version(&name(p))).map(|p| p.to_string_lossy().into_owned());
        let key_dirs = appdata.iter().flat_map(|a| ["yuzu/keys", "suyu/keys", "eden/keys", "citron/keys", "sudachi/keys", "Ryujinx/system"]
            .map(|k| a.join(k).join("prod.keys")));
        found.keys = files.iter().find(|p| name(p) == "prod.keys").cloned().into_iter().chain(key_dirs)
            .find(|p| p.is_file()).map(|p| p.to_string_lossy().into_owned());
        found
    })
    .await
    .unwrap_or_default()
}

#[tauri::command]
fn cancel_download(procs: State<Procs>) {
    procs.cancel_download.store(true, Ordering::SeqCst);
}

/// Splits a child's output on \n and \r (progress lines rewrite themselves with \r) and sends each line to the window.
fn stream(app: AppHandle, mut from: impl Read + Send + 'static, err: bool, event: &'static str) {
    thread::spawn(move || {
        let mut buf = [0u8; 4096];
        let mut line = Vec::new();
        loop {
            let n = match from.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => n,
            };
            for &b in &buf[..n] {
                if b == b'\n' || b == b'\r' {
                    if !line.is_empty() {
                        let text = String::from_utf8_lossy(&line).into_owned();
                        let _ = app.emit(event, Line { text, err });
                        line.clear();
                    }
                } else {
                    line.push(b);
                }
            }
        }
        if !line.is_empty() {
            let _ = app.emit(event, Line { text: String::from_utf8_lossy(&line).into_owned(), err });
        }
    });
}

/// The game files of a first install (or of an install whose extraction must run again). The installer takes them
/// only as paths: the keys file is read by its tools, never by the launcher.
#[derive(Deserialize)]
struct Inputs {
    base: Option<String>,
    update: Option<String>,
    keys: Option<String>,
    lang: Option<String>,
}

/// `python -m installer --dest <install folder> [--base --update --keys --lang] --root <release>`: one JSON event per
/// stdout line ("build-line"; installer/events.py), the exit code in "build-done" (0 done, 1 failed, 2 bad command
/// line, 3 cancelled). A folder with totk-install.json needs only --dest.
/// What the launcher itself keeps in the install folder: its settings, the mods and the artwork.
const LAUNCHER_OWNED: [&str; 3] = ["totk.ini", "mods", "launcher-art"];

/// The installer takes only an empty folder for a first install (check.dest_not_empty). The launcher's own files there
/// (settings changed or mods added before installing) move to "<folder>.launcher-hold" beside it, on the same disk,
/// for the install; anything else stops it with the names.
fn hold_launcher_files(h: &Path) -> Result<Option<PathBuf>, String> {
    if h.join(STATE_FILE).is_file() || !h.is_dir() {
        return Ok(None);
    }
    let names: Vec<String> = std::fs::read_dir(h).map_err(|e| e.to_string())?
        .filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
    if names.is_empty() {
        return Ok(None);
    }
    let others: Vec<&String> = names.iter().filter(|n| !LAUNCHER_OWNED.contains(&n.to_ascii_lowercase().as_str())).collect();
    if !others.is_empty() {
        let list = others.iter().take(5).map(|n| n.as_str()).collect::<Vec<_>>().join(", ");
        return Err(format!("A pasta de instalação {} não está vazia ({list}). Escolha uma pasta vazia ou esvazie essa.", h.display()));
    }
    let hold = PathBuf::from(format!("{}.launcher-hold", h.display()));
    if hold.exists() {
        return Err(format!("{} já existe (de uma instalação anterior interrompida): mova o conteúdo dela de volta ou apague-a.", hold.display()));
    }
    std::fs::create_dir_all(&hold).map_err(|e| e.to_string())?;
    for n in &names {
        std::fs::rename(h.join(n), hold.join(n)).map_err(|e| format!("não foi possível mover {n}: {e}"))?;
    }
    Ok(Some(hold))
}

/// Puts the held files back after the install: folders as they were, and the held totk.ini's keys over the one the
/// installer wrote (except the language, which the install chose).
fn restore_launcher_files(h: &Path, hold: &Path) {
    let Ok(rd) = std::fs::read_dir(hold) else { return };
    for e in rd.filter_map(|e| e.ok()) {
        let name = e.file_name();
        let (from, to) = (e.path(), h.join(&name));
        if name.to_string_lossy().eq_ignore_ascii_case("totk.ini") {
            let held = std::fs::read_to_string(&from).unwrap_or_default();
            let mut text = std::fs::read_to_string(&to).unwrap_or_default();
            for (k, v) in ini::read(&held) {
                if k != "lang" {
                    text = ini::set(&text, &k, &v);
                }
            }
            if std::fs::create_dir_all(h).is_ok() && std::fs::write(&to, text).is_ok() {
                let _ = std::fs::remove_file(&from);
            }
        } else if !to.exists() && std::fs::rename(&from, &to).is_err() {
            continue;
        }
    }
    let _ = std::fs::remove_dir(hold);   // (only when everything went back)
}

#[tauri::command]
fn start_build(app: AppHandle, procs: State<Procs>, inputs: Inputs) -> Result<(), String> {
    let mut slot = procs.build.lock().unwrap();
    if slot.is_some() {
        return Err("A instalação já está rodando.".into());
    }
    let h = home(&app);
    let root = release_root(&app).ok_or("Escolha a pasta do instalador (o release, com installer/).")?;
    let python = python(&root).ok_or_else(|| format!("O Python do instalador não foi encontrado em {}", root.join("python").display()))?;
    let hold = hold_launcher_files(&h)?;
    let mut cmd = Command::new(python);
    cmd.args(["-m", "installer", "--dest"]).arg(&h).arg("--root").arg(&root).current_dir(&root);
    for (flag, value) in [("--base", inputs.base), ("--update", inputs.update), ("--keys", inputs.keys), ("--lang", inputs.lang)] {
        if let Some(v) = value.map(|v| v.trim().to_string()).filter(|v| !v.is_empty()) {
            cmd.arg(flag).arg(v);
        }
    }
    cmd.env("PYTHONUNBUFFERED", "1").env("PYTHONNOUSERSITE", "1").env("PYTHONIOENCODING", "utf-8").env("PYTHONUTF8", "1");
    cmd.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    // Its own process group: the cancel's Ctrl+Break (cancel_build) reaches the installer and the tools it runs.
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP);
    #[cfg(unix)]
    cmd.process_group(0);
    let mut child: Child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            if let Some(hold) = &hold { restore_launcher_files(&h, hold); }
            return Err(e.to_string());
        }
    };
    stream(app.clone(), child.stdout.take().unwrap(), false, "build-line");
    stream(app.clone(), child.stderr.take().unwrap(), true, "build-line");
    *slot = Some(child.id());
    drop(slot);
    thread::spawn(move || {
        let code = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
        if let Some(hold) = &hold {
            restore_launcher_files(&h, hold);
        }
        *app.state::<Procs>().build.lock().unwrap() = None;
        let _ = app.emit("build-done", code);
    });
    Ok(())
}

fn kill_tree(pid: u32) {
    #[cfg(windows)]
    let _ = no_window(Command::new("taskkill").args(["/PID", &pid.to_string(), "/T", "/F"])).status();
    #[cfg(unix)]   // the whole group (no_window gave each child its own)
    let _ = Command::new("kill").args(["-KILL", &format!("-{pid}")]).status();
}

/// Ctrl+Break to the console of `pid`, sent by a helper Python (the launcher has no console to share with it):
/// installer/guilogic.py send_break does the same for the installer's own window.
#[cfg(windows)]
const BREAK_HELPER: &str = "import ctypes, sys, time\n\
k = ctypes.windll.kernel32\n\
pid = int(sys.argv[1])\n\
k.FreeConsole()\n\
if k.AttachConsole(pid):\n    k.SetConsoleCtrlHandler(None, True)\n    k.GenerateConsoleCtrlEvent(1, pid)\n    time.sleep(1)\n    k.FreeConsole()\n\
else:\n    sys.exit(1)\n";

/// Asks the installer to stop (Ctrl+Break on Windows, SIGTERM elsewhere), so it records the steps it finished and
/// exits with 3; a killed installer would leave its step unrecorded. Killed after a minute if it is still running.
#[tauri::command]
fn cancel_build(app: AppHandle, procs: State<Procs>) {
    let Some(pid) = *procs.build.lock().unwrap() else { return };
    if !ask_stop(&app, pid) {
        return kill_tree(pid);
    }
    thread::spawn(move || {
        if !wait_build_gone(&app, pid, 600) {
            kill_tree(pid);
        }
    });
}

fn ask_stop(app: &AppHandle, pid: u32) -> bool {
    #[cfg(windows)]
    return release_root(app).and_then(|r| python(&r)).is_some_and(|py| {
        no_window(Command::new(py).args(["-c", BREAK_HELPER, &pid.to_string()])).status().is_ok_and(|s| s.success())
    });
    #[cfg(unix)]
    {
        let _ = app;
        Command::new("kill").args(["-TERM", &format!("-{pid}")]).status().is_ok_and(|s| s.success())
    }
}

/// Waits up to `tenths` tenths of a second for the install `pid` to end (start_build's waiting thread clears the slot
/// once the installer exited and the launcher's files are back). True when it ended.
fn wait_build_gone(app: &AppHandle, pid: u32, tenths: u32) -> bool {
    for _ in 0..tenths {
        if *app.state::<Procs>().build.lock().unwrap() != Some(pid) {
            return true;
        }
        thread::sleep(Duration::from_millis(100));
    }
    false
}

/// The launcher is closing during an install: the installer and the tool it runs (totk.exe --prepare-shaders, clang)
/// would go on unseen and fight the next install over the same files, so they stop with it. Asked first, so the
/// installer records the steps it finished; ended after 10 s.
fn stop_build_on_exit(app: &AppHandle) {
    let Some(pid) = *app.state::<Procs>().build.lock().unwrap() else { return };
    if !ask_stop(app, pid) || !wait_build_gone(app, pid, 100) {
        kill_tree(pid);
        wait_build_gone(app, pid, 30);
    }
}

#[cfg(windows)]
fn has_visible_window(pid: u32, _home: &Path) -> bool {
    use windows_sys::Win32::Foundation::{BOOL, HWND, LPARAM};
    use windows_sys::Win32::UI::WindowsAndMessaging::{EnumWindows, GetWindowThreadProcessId, IsWindowVisible};
    unsafe extern "system" fn each(hwnd: HWND, lp: LPARAM) -> BOOL {
        let found = &mut *(lp as *mut (u32, bool));
        let mut owner = 0u32;
        GetWindowThreadProcessId(hwnd, &mut owner);
        if owner == found.0 && IsWindowVisible(hwnd) != 0 {
            found.1 = true;
            return 0;
        }
        1
    }
    let mut found = (pid, false);
    unsafe { EnumWindows(Some(each), &mut found as *mut _ as LPARAM) };
    found.1
}

/// macOS / Linux: no window enumeration here; the game counts as shown once it has run for a few seconds.
#[cfg(not(windows))]
fn has_visible_window(_pid: u32, _home: &Path) -> bool {
    true
}

/// Starts totk.exe in the install folder (it reads totk.ini there); "game-visible" when its window shows, "game-exit"
/// with the exit code when it ends.
#[tauri::command]
fn launch_game(app: AppHandle, procs: State<Procs>) -> Result<(), String> {
    let mut slot = procs.game.lock().unwrap();
    if slot.is_some() {
        return Err("O jogo já está aberto.".into());
    }
    let h = home(&app);
    let exe = h.join(GAME_EXE);
    if !exe.is_file() {
        return Err(format!("{GAME_EXE} não existe ainda: faça a instalação primeiro."));
    }
    let mut cmd = Command::new(exe);
    cmd.current_dir(&h);
    // totk.exe is a console program: without this its console window opens behind the game. Its log goes to
    // logs/totk.log either way.
    no_window(&mut cmd);
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let pid = child.id();
    *slot = Some(pid);
    drop(slot);
    let watcher = app.clone();
    let game_home = h.clone();
    thread::spawn(move || {
        #[cfg(not(windows))]
        thread::sleep(Duration::from_secs(4));
        for _ in 0..(10 * 60 * 10) {   // up to 10 minutes for the first frame
            if has_visible_window(pid, &game_home) {
                #[cfg(target_os = "macos")]   // one Dock icon: the game's now (it enters the Dock with its window)
                let _ = watcher.set_dock_visibility(false);
                let _ = watcher.emit("game-visible", ());
                return;
            }
            if watcher.state::<Procs>().game.lock().unwrap().is_none() {
                return;
            }
            thread::sleep(Duration::from_millis(100));
        }
    });
    thread::spawn(move || {
        let code = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
        *app.state::<Procs>().game.lock().unwrap() = None;
        #[cfg(target_os = "macos")]
        let _ = app.set_dock_visibility(true);
        let _ = app.emit("game-exit", code);
    });
    Ok(())
}

#[tauri::command]
fn stop_game(procs: State<Procs>) {
    if let Some(pid) = *procs.game.lock().unwrap() {
        kill_tree(pid);
    }
}

/// The end of the game's log (logs/totk.log of the last run), for the error shown when it stops.
#[tauri::command]
fn log_tail(app: AppHandle, lines: usize) -> String {
    let text = std::fs::read(home(&app).join("logs/totk.log")).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
    let all: Vec<&str> = text.lines().collect();
    all[all.len().saturating_sub(lines)..].join("\n")
}

const OPENER: &str = if cfg!(windows) { "explorer" } else if cfg!(target_os = "macos") { "open" } else { "xdg-open" };

#[tauri::command]
fn open_path(app: AppHandle, path: String) -> Result<(), String> {
    if path.starts_with("https://") {   // the default browser
        return Command::new(OPENER).arg(&path).spawn().map(|_| ()).map_err(|e| e.to_string());
    }
    let p = if Path::new(&path).is_absolute() { PathBuf::from(&path) } else { home(&app).join(&path) };
    if path == "launcher-art" {
        let _ = std::fs::create_dir_all(&p);
    }
    Command::new(OPENER).arg(p).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Game controllers drive the launcher too: "pad" events with "up" / "down" / "left" / "right" (d-pad or left stick,
/// repeating while held), "ok" (the bottom face button), "back" (the right one) and "start". Nothing while the game
/// runs (it reads the controller then).
fn pad_thread(app: AppHandle) {
    use gilrs::{Axis, Button, EventType};
    thread::spawn(move || {
        let Ok(mut gil) = gilrs::Gilrs::new() else { return };
        let mut held: Option<(&'static str, std::time::Instant)> = None;   // the direction held and when it repeats
        loop {
            let mut out: Vec<&'static str> = Vec::new();
            while let Some(gilrs::Event { event, .. }) = gil.next_event() {
                if let EventType::ButtonPressed(b, _) = event {
                    match b {
                        Button::South => out.push("ok"),
                        Button::East => out.push("back"),
                        Button::Start => out.push("start"),
                        _ => {}
                    }
                }
            }
            let mut dir = None;
            for (_, g) in gil.gamepads() {
                let (x, y) = (g.value(Axis::LeftStickX), g.value(Axis::LeftStickY));
                dir = dir
                    .or(g.is_pressed(Button::DPadUp).then_some("up"))
                    .or(g.is_pressed(Button::DPadDown).then_some("down"))
                    .or(g.is_pressed(Button::DPadLeft).then_some("left"))
                    .or(g.is_pressed(Button::DPadRight).then_some("right"))
                    .or((y > 0.6).then_some("up"))
                    .or((y < -0.6).then_some("down"))
                    .or((x < -0.6).then_some("left"))
                    .or((x > 0.6).then_some("right"));
            }
            let now = std::time::Instant::now();
            held = match (dir, held) {
                (Some(d), Some((h, at))) if d == h => {
                    if now < at { Some((h, at)) } else { out.push(d); Some((d, now + Duration::from_millis(110))) }
                }
                (Some(d), _) => { out.push(d); Some((d, now + Duration::from_millis(380))) }
                (None, _) => None,
            };
            if !out.is_empty() && app.state::<Procs>().game.lock().unwrap().is_none() {
                for e in out {
                    let _ = app.emit("pad", e);
                }
            }
            thread::sleep(Duration::from_millis(16));
        }
    });
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .plugin(tauri_plugin_process::init())
        .manage(Procs::default())
        .manage(room::Room::default())
        .setup(|app| {
            pad_thread(app.handle().clone());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            status, set_home, set_release, get_source, set_source, installer_text, start_view, custom_art, read_ini, write_ini, releases,
            download_release, cancel_download, find_game_files, mods_list, mods_apply, mods_add, mods_adapt, mods_remove, start_build, cancel_build, launch_game, stop_game, log_tail, open_path,
            room_info, room_start, room_stop, mp_test,
        ])
        .build(tauri::generate_context!())
        .expect("launcher")
        .run(|app, event| {
            if let tauri::RunEvent::Exit = event {
                app.state::<room::Room>().stop();   // the room closes with the launcher that opened it
                stop_build_on_exit(app);
            }
        });
}
