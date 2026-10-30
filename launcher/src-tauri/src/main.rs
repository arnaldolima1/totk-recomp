// Launcher: settings (settings.cfg next to totk.exe), the install (`python -m installer` of a release folder, its JSON
// events streamed to the window) and the game, started hidden over the launcher's own window so the hand-over looks
// seamless. Two folders: the release (installer/, python/, tools/ ... as docs/installer.md "Release layout", or a
// repository checkout in development) and the install folder the installer fills (totk.exe, totk-install.json).
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
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
    dev: bool,                       // the release is a repository checkout
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

/// The release folder the install runs from: TOTK_LAUNCHER_RELEASE, the one chosen in the launcher, the launcher's
/// own folder or a folder above it (a release unpacked around it, or the checkout it was built in:
/// launcher/src-tauri/target/<profile>/).
fn release_root(app: &AppHandle) -> Option<PathBuf> {
    let env = std::env::var("TOTK_LAUNCHER_RELEASE").ok().map(PathBuf::from);
    let saved = read_config(app).get("release").and_then(|r| r.as_str()).map(PathBuf::from);
    let exe = std::env::current_exe().ok();
    env.into_iter()
        .chain(saved)
        .chain(exe.iter().flat_map(|e| e.ancestors().skip(1).take(8).map(Path::to_path_buf)))
        .find(|d| is_release(d))
}

/// The install folder: TOTK_LAUNCHER_HOME, the one chosen in the launcher, or the launcher's own folder.
fn home(app: &AppHandle) -> PathBuf {
    if let Ok(h) = std::env::var("TOTK_LAUNCHER_HOME") {
        return PathBuf::from(h);
    }
    if let Some(saved) = read_config(app).get("home").and_then(|h| h.as_str()).map(PathBuf::from) {
        return saved;
    }
    std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)).unwrap_or_default()
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
    let finished = state.as_ref().and_then(|s| s["steps"].get("cleanup")).is_some();
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

/// `--view <name>` on the command line: the tab to open on (screenshots, shortcuts).
#[tauri::command]
fn start_view() -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    args.iter().position(|a| a == "--view").and_then(|i| args.get(i + 1).cloned())
}

/// settings.cfg: "NAME=value" lines the game reads before ultratears.cfg (src/host/boot.c).
#[tauri::command]
fn read_settings(app: AppHandle) -> BTreeMap<String, String> {
    std::fs::read_to_string(home(&app).join("settings.cfg"))
        .unwrap_or_default()
        .lines()
        .filter(|l| !l.starts_with('#'))
        .filter_map(|l| l.split_once('=').map(|(k, v)| (k.trim().to_string(), v.trim().to_string())))
        .collect()
}

#[tauri::command]
fn write_settings(app: AppHandle, settings: BTreeMap<String, String>) -> Result<(), String> {
    let mut text = String::from("# Written by the UltraTears launcher: the player's settings (read before ultratears.cfg).\n");
    for (k, v) in settings.iter().filter(|(_, v)| !v.is_empty()) {
        text += &format!("{k}={v}\n");
    }
    std::fs::write(home(&app).join("settings.cfg"), text).map_err(|e| e.to_string())
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
#[tauri::command]
fn start_build(app: AppHandle, procs: State<Procs>, inputs: Inputs) -> Result<(), String> {
    let mut slot = procs.build.lock().unwrap();
    if slot.is_some() {
        return Err("A instalação já está rodando.".into());
    }
    let h = home(&app);
    let root = release_root(&app).ok_or("Escolha a pasta do instalador (o release, com installer/).")?;
    let python = python(&root).ok_or_else(|| format!("O Python do instalador não foi encontrado em {}", root.join("python").display()))?;
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
    let mut child: Child = cmd.spawn().map_err(|e| e.to_string())?;
    stream(app.clone(), child.stdout.take().unwrap(), false, "build-line");
    stream(app.clone(), child.stderr.take().unwrap(), true, "build-line");
    *slot = Some(child.id());
    drop(slot);
    thread::spawn(move || {
        let code = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
        *app.state::<Procs>().build.lock().unwrap() = None;
        let _ = app.emit("build-done", code);
    });
    Ok(())
}

/// The heap overlay the resolution mod needs for `size` (tools/make_res_overlay.py, about a second).
#[tauri::command]
fn make_res_overlay(app: AppHandle, size: String) -> Result<(), String> {
    if !size.split_once('x').map_or(false, |(w, h)| w.parse::<u32>().is_ok() && h.parse::<u32>().is_ok()) {
        return Err(format!("resolução inválida: {size}"));
    }
    let h = home(&app);
    let script = h.join("tools/make_res_overlay.py");
    let Some(python) = python(&h).filter(|_| script.is_file()) else {
        return Err("tools/make_res_overlay.py ou o Python do build não estão na pasta do jogo.".into());
    };
    let mut cmd = Command::new(python);
    cmd.arg(script).arg(&size).current_dir(&h).env("PYTHONNOUSERSITE", "1");
    let out = no_window(&mut cmd).output().map_err(|e| e.to_string())?;
    if out.status.success() {
        Ok(())
    } else {
        Err(String::from_utf8_lossy(&out.stderr).trim().to_string() + &String::from_utf8_lossy(&out.stdout))
    }
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
    #[cfg(windows)]
    let asked = release_root(&app).and_then(|r| python(&r)).is_some_and(|py| {
        no_window(Command::new(py).args(["-c", BREAK_HELPER, &pid.to_string()])).status().is_ok_and(|s| s.success())
    });
    #[cfg(unix)]
    let asked = Command::new("kill").args(["-TERM", &format!("-{pid}")]).status().is_ok_and(|s| s.success());
    if !asked {
        return kill_tree(pid);
    }
    thread::spawn(move || {
        for _ in 0..600 {
            thread::sleep(Duration::from_millis(100));
            if *app.state::<Procs>().build.lock().unwrap() != Some(pid) {
                return;
            }
        }
        kill_tree(pid);
    });
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

/// The game's start-up status (startup.txt in the game folder, written by host/window_sdl.c while it starts hidden):
/// "starting", "shaders <done> <total>", "ready"; empty before the game writes it.
#[tauri::command]
fn startup_status(app: AppHandle) -> String {
    std::fs::read_to_string(home(&app).join("startup.txt")).unwrap_or_default().trim().to_string()
}

/// macOS / Linux: no window enumeration here; the game says "ready" in startup.txt when its window shows.
#[cfg(not(windows))]
fn has_visible_window(_pid: u32, home: &Path) -> bool {
    std::fs::read_to_string(home.join("startup.txt")).map_or(false, |s| s.starts_with("ready"))
}

/// Starts the game hidden over the rectangle (x, y, w, h in pixels); "game-visible" when its window shows its first
/// frame, "game-exit" with the exit code when it ends.
#[tauri::command]
fn launch_game(app: AppHandle, procs: State<Procs>, rect: (i32, i32, i32, i32), fullscreen: bool, borderless: bool, lang: Option<String>) -> Result<(), String> {
    let mut slot = procs.game.lock().unwrap();
    if slot.is_some() {
        return Err("O jogo já está aberto.".into());
    }
    let h = home(&app);
    let exe = h.join(GAME_EXE);
    if !exe.is_file() {
        return Err(format!("{GAME_EXE} não existe ainda: faça a instalação primeiro."));
    }
    let _ = std::fs::remove_file(h.join("startup.txt"));   // (a previous run's)
    let mut cmd = Command::new(exe);
    cmd.current_dir(&h)
        .env("TOTK_START_HIDDEN", "1")
        .env("TOTK_WINDOW", format!("{},{},{},{}", rect.0, rect.1, rect.2, rect.3));
    if fullscreen {
        cmd.env("TOTK_FULLSCREEN", "1");
    }
    if borderless {
        cmd.env("TOTK_BORDERLESS", "1");
    }
    if let Some(l) = lang.filter(|l| l == "en" || l == "pt") {
        cmd.env("TOTK_LANG", l);   // the game's own notices (overlay) in the launcher's language
    }
    #[cfg(unix)]
    cmd.process_group(0);
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let pid = child.id();
    *slot = Some(pid);
    drop(slot);
    let watcher = app.clone();
    let game_home = h.clone();
    thread::spawn(move || {
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
        .setup(|app| {
            pad_thread(app.handle().clone());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            status, set_home, set_release, installer_text, start_view, make_res_overlay, custom_art, read_settings, write_settings,
            start_build, cancel_build, launch_game, stop_game, log_tail, open_path, startup_status,
        ])
        .run(tauri::generate_context!())
        .expect("launcher");
}
