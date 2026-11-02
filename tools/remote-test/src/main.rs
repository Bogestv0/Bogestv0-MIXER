// Тестовый стенд удалённого пульта: настоящий remote.rs + «движок» из нескольких десятков строк.
// Движок помнит громкости, Mute, Solo, маршруты и дорожку прослушивания, отвечает строками как engine.exe --serve
// и шлёт синус 440 Гц как звук дорожки. Все принятые команды пишет в журнал (второй аргумент).
#[path = "../../../src-tauri/src/remote.rs"]
mod remote;

use std::io::Write;
use std::sync::mpsc;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

struct Backend {
    tx: Mutex<mpsc::Sender<String>>,
    log: String,
    settings: String,
}

impl remote::Backend for Backend {
    fn send_engine(&self, line: &str) {
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&self.log) {
            let _ = writeln!(f, "{}", line);
        }
        let _ = self.tx.lock().unwrap().send(line.to_string());
    }
    fn settings_json(&self) -> Option<String> {
        std::fs::read_to_string(&self.settings).ok()
    }
    fn scene_applied(&self, id: &str) {
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&self.log) {
            let _ = writeln!(f, "SCENE {}", id);
        }
    }
}

static REMOTE: OnceLock<Arc<remote::Remote>> = OnceLock::new();

#[derive(Clone, Copy)]
struct Strip { g: f64, m: i32, s: i32, r: i32 }

fn state_line(strips: &[Strip], listen: i32) -> String {
    let items: Vec<String> = strips.iter().map(|s| format!(r#"{{"g":{},"m":{},"s":{},"r":{},"src":0}}"#, s.g, s.m, s.s, s.r)).collect();
    format!(r#"{{"t":"state","v":"test","running":1,"listen":{},"strips":[{}]}}"#, listen, items.join(","))
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let port: u16 = args.get(1).and_then(|p| p.parse().ok()).unwrap_or(8099);
    let log = args.get(2).cloned().unwrap_or("/tmp/remote-test.log".into());
    let pin = args.get(3).cloned().unwrap_or("4817".into());
    let require = args.get(4).map(|v| v != "nopin").unwrap_or(true);
    let settings = format!("{}.settings.json", log);
    std::fs::write(&settings, r#"{"strips":[{"name":"Микрофон 1","tag":"HW1"},{"name":"Микрофон 2","tag":"HW2","hidden":true},{"name":"Игры","tag":"V1"},{"name":"Музыка","tag":"V2"},{"name":"Discord","tag":"V3"},{"name":"Донаты","tag":"V4"},{"name":"Браузер","tag":"V5"},{"name":"<b>Браузер 2</b>","tag":"V6"}],"scenes":[{"id":"play","name":"Играю и стримлю","routes":[24,24,10,10,2,10,2,2]},{"id":"call","name":"Звонок","routes":[24,24,2,2,2,2,2,2]}],"active":"play"}"#).unwrap();
    let _ = std::fs::remove_file(&log);

    let (tx, rx) = mpsc::channel::<String>();
    let backend = Arc::new(Backend { tx: Mutex::new(tx), log: log.clone(), settings });
    let remote = Arc::new(remote::Remote::new(backend));
    REMOTE.set(remote.clone()).ok();
    remote.apply(remote::Config { enabled: true, port, require_pin: require, pin, listen: true, buffer: "balance".into() });
    if let Some(svg) = remote.qr_svg() { let _ = std::fs::write(format!("{}.qr.svg", log), svg); }
    println!("READY http://127.0.0.1:{} status={}", port, remote.status());

    // Звук: синус 440 Гц стерео, порции по 20 мс, пока выбрана дорожка.
    let listen = Arc::new(Mutex::new(-1i32));
    {
        let listen = listen.clone();
        let remote = remote.clone();
        std::thread::spawn(move || {
            let mut phase = 0.0f64;
            loop {
                std::thread::sleep(Duration::from_millis(20));
                let idx = *listen.lock().unwrap();
                if idx < 0 { continue; }
                let mut bytes = Vec::with_capacity(960 * 4);
                for _ in 0..960 {
                    let v = ((phase * 2.0 * std::f64::consts::PI).sin() * 0.3 * 32767.0) as i16;
                    phase = (phase + 440.0 / 48000.0).fract();
                    bytes.extend_from_slice(&v.to_le_bytes());
                    bytes.extend_from_slice(&v.to_le_bytes());
                }
                use base64::Engine as _;
                let line = format!(r#"{{"t":"pcm","i":{},"d":"{}"}}"#, idx, base64::engine::general_purpose::STANDARD.encode(&bytes));
                remote.on_engine_line(&line);
            }
        });
    }
    // Уровни ~30 раз в секунду.
    {
        let remote = remote.clone();
        std::thread::spawn(move || loop {
            std::thread::sleep(Duration::from_millis(33));
            remote.on_engine_line(r#"{"t":"m","s":[[0.2,0.2],[0,0],[0.5,0.4],[0.1,0.1],[0,0],[0,0],[0,0],[0,0]],"b":[]}"#);
        });
    }

    let mut strips = [Strip { g: -3.0, m: 0, s: 0, r: 24 }; 8];
    let mut cur_listen = -1i32;
    remote.on_engine_line(&state_line(&strips, cur_listen));
    while let Ok(line) = rx.recv() {
        let p: Vec<&str> = line.split_whitespace().collect();
        let mut changed = true;
        match p.as_slice() {
            ["set", "strip", i, what, rest @ ..] => {
                let i: usize = i.parse().unwrap_or(99);
                if i < 8 {
                    match (*what, rest) {
                        ("gain", [v]) => strips[i].g = v.parse().unwrap_or(0.0),
                        ("mute", [v]) => strips[i].m = v.parse().unwrap_or(0),
                        ("solo", [v]) => { let on: i32 = v.parse().unwrap_or(0); for (j, s) in strips.iter_mut().enumerate() { s.s = if on != 0 && j == i { 1 } else { 0 }; } }
                        ("route", [k, v]) => { let k: i32 = k.parse().unwrap_or(0); if v.parse::<i32>().unwrap_or(0) != 0 { strips[i].r |= 1 << k } else { strips[i].r &= !(1 << k) } }
                        _ => changed = false,
                    }
                }
            }
            ["set", "listen", v] => { cur_listen = if *v == "off" { -1 } else { v.parse().unwrap_or(-1) }; *listen.lock().unwrap() = cur_listen; }
            ["get"] => {}
            _ => changed = false,
        }
        if changed || line == "get" { remote.on_engine_line(&state_line(&strips, cur_listen)); }
    }
}
