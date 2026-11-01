// The Multiplayer tab: a room hosted from the launcher (the release's tools/mp_server.py under its own Python) and a
// reachability test for a room's address. The tab shows only when the release in use has that server (status
// `multiplayer`): releases without it have no multiplayer in the game either.
//
// The server prints the addresses friends join (one line per local IPv4 address: the network, and Radmin, Tailscale
// or ZeroTier when installed) and a line per player who comes and goes; the launcher keeps them and passes each line
// on to the window as a "room-line" event.
use serde::Serialize;
use std::io::BufRead;
use std::net::{TcpStream, ToSocketAddrs};
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter};

pub const DEFAULT_PORT: u16 = 5577;
const MAX_LINES: usize = 200;

#[derive(Default)]
pub struct Room {
    child: Mutex<Option<Child>>,
    info: Arc<Mutex<RoomInfo>>,
}

#[derive(Serialize, Clone, Default)]
pub struct RoomInfo {
    pub running: bool,
    pub port: u16,
    pub name: String,
    pub addresses: Vec<Address>,   // what friends type in the game: ip:port, with the network it is on
    pub players: Vec<Player>,      // in the room now, in the order they came
    pub lines: Vec<String>,        // the server's output, newest last
}

#[derive(Serialize, Clone)]
pub struct Player {
    pub name: String,
    pub slot: i32,   // 0..3: the colour (green, red, blue, purple); -1 from a server that does not say
}

#[derive(Serialize, Clone)]
pub struct Address {
    pub address: String,
    pub kind: String,   // "lan", "radmin", "tailscale", "zerotier" or "other"
}

/// The network an address is most likely on: Radmin VPN gives 26.x.x.x, Tailscale 100.64.0.0/10, ZeroTier usually
/// 10.147.x.x (or a network of its own: "other"); 192.168.x.x and 172.16-31.x.x are a home network.
fn kind_of(ip: &str) -> &'static str {
    let o: Vec<u8> = ip.split('.').filter_map(|p| p.parse().ok()).collect();
    match o.as_slice() {
        [26, ..] => "radmin",
        [100, b, ..] if (64..128).contains(b) => "tailscale",
        [10, 147, ..] => "zerotier",
        [192, 168, ..] => "lan",
        [172, b, ..] if (16..32).contains(b) => "lan",
        [10, ..] => "lan",
        _ => "other",
    }
}

/// One line of the server's output into the room's state: its address lines ("...: 192.168.0.5:5577") and the
/// "<name> entrou (ip) na vaga <n>" / "<name> saiu" lines of players coming and going.
fn take_line(info: &mut RoomInfo, line: &str) {
    if let Some(addr) = line.strip_prefix("  ").and_then(|l| l.rsplit(": ").next()) {
        if addr.contains(':') && addr.split(':').next().is_some_and(|ip| ip.parse::<std::net::Ipv4Addr>().is_ok()) {
            let ip = addr.split(':').next().unwrap_or_default();
            info.addresses.push(Address { address: addr.trim().to_string(), kind: kind_of(ip).into() });
        }
    }
    // "HH:MM:SS <name> entrou (ip) na vaga <n>" and "HH:MM:SS <name> saiu"
    if let Some((_, rest)) = line.split_once(' ') {
        if let Some(name) = rest.strip_suffix(" saiu") {
            info.players.retain(|p| p.name != name);
        } else if let Some((name, after)) = rest.rsplit_once(" entrou (") {
            let slot = after.rsplit_once(" na vaga ").and_then(|(_, n)| n.trim().parse::<i32>().ok()).map_or(-1, |n| n - 1);
            info.players.retain(|p| p.name != name);
            info.players.push(Player { name: name.to_string(), slot });
        }
    }
    info.lines.push(line.to_string());
    if info.lines.len() > MAX_LINES {
        info.lines.remove(0);
    }
}

impl Room {
    pub fn info(&self) -> RoomInfo {
        let mut child = self.child.lock().unwrap();
        let alive = child.as_mut().is_some_and(|c| matches!(c.try_wait(), Ok(None)));
        if !alive {
            *child = None;
        }
        let mut info = self.info.lock().unwrap().clone();
        info.running = alive;
        info
    }

    /// Starts the release's server: python -u tools/mp_server.py. Its state (the group's challenge progress) stays at
    /// the server's default, %APPDATA%/TotK Recomp/mp_room.json, which the setup's shortcut uses too.
    pub fn start(&self, app: &AppHandle, python: &Path, release: &Path, name: &str, port: u16,
                 password: &str) -> Result<RoomInfo, String> {
        self.stop();
        let server = release.join("tools/mp_server.py");
        if !server.is_file() {
            return Err("Esta versão do jogo não tem multijogador.".into());
        }
        let mut cmd = Command::new(python);
        cmd.arg("-u").arg(&server).arg("--port").arg(port.to_string());
        if !name.trim().is_empty() {
            cmd.arg("--name").arg(name.trim());
        }
        if !password.is_empty() {
            cmd.arg("--password").arg(password);
        }
        cmd.current_dir(release).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped())
            .env("PYTHONIOENCODING", "utf-8");
        crate::no_window(&mut cmd);
        let mut child = cmd.spawn().map_err(|e| format!("não foi possível abrir a sala: {e}"))?;
        *self.info.lock().unwrap() = RoomInfo { running: true, port, name: name.trim().to_string(), ..Default::default() };
        for pipe in [child.stdout.take().map(|s| Box::new(s) as Box<dyn std::io::Read + Send>),
                     child.stderr.take().map(|s| Box::new(s) as Box<dyn std::io::Read + Send>)].into_iter().flatten() {
            let info = self.info.clone();
            let app = app.clone();
            thread::spawn(move || {
                for line in std::io::BufReader::new(pipe).lines().map_while(Result::ok) {
                    take_line(&mut info.lock().unwrap(), &line);
                    let _ = app.emit("room-line", line);
                }
            });
        }
        *self.child.lock().unwrap() = Some(child);
        // the address lines come at once; a server that stops at start (the port taken) says why on stderr
        let until = Instant::now() + Duration::from_millis(1500);
        while Instant::now() < until && self.info.lock().unwrap().addresses.is_empty() {
            if !self.info().running {
                let why = self.info.lock().unwrap().lines.join("\n");
                return Err(if why.contains("10048") || why.to_lowercase().contains("address already in use") {
                    format!("A porta {port} já está em uso: feche a outra sala ou escolha outra porta.")
                } else {
                    format!("A sala fechou ao abrir.\n{why}")
                });
            }
            thread::sleep(Duration::from_millis(50));
        }
        Ok(self.info())
    }

    pub fn stop(&self) {
        if let Some(mut c) = self.child.lock().unwrap().take() {
            crate::kill_tree(c.id());
            let _ = c.wait();
        }
        let mut info = self.info.lock().unwrap();
        info.running = false;
        info.players.clear();
    }
}

/// Whether a room answers at an address ("host", "host:port"; the game's default port when none): a TCP connection
/// within four seconds. The time it took, or why not, in words for the window.
pub fn test(server: &str) -> Result<u64, String> {
    let s = server.trim();
    if s.is_empty() {
        return Err("Digite o endereço da sala.".into());
    }
    let with_port = if s.rsplit_once(':').is_some_and(|(_, p)| p.parse::<u16>().is_ok()) {
        s.to_string()
    } else {
        format!("{s}:{DEFAULT_PORT}")
    };
    let addrs: Vec<_> = with_port.to_socket_addrs().map_err(|_| format!("Endereço não encontrado: {s}"))?.collect();
    let start = Instant::now();
    let mut last = String::new();
    for a in addrs {
        match TcpStream::connect_timeout(&a, Duration::from_secs(4)) {
            Ok(_) => return Ok(start.elapsed().as_millis() as u64),
            Err(e) => last = e.to_string(),
        }
    }
    Err(format!("A sala não respondeu em {with_port} ({last})."))
}
