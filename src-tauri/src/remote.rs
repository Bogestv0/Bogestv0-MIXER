//! Удалённый пульт: маленький веб-сервер внутри программы. Телефон в той же Wi-Fi сети открывает страницу пульта,
//! двигает громкость, переключает сцены и слушает звук выбранной дорожки.
//!
//! Модуль не зависит от Tauri: с программой его связывает трейт `Backend` (отправка строки движку, чтение файла настроек).
//! Благодаря этому сервер проверяется отдельно, на тестовом движке (см. tools/remote-test).
//!
//! Протокол WebSocket (`/ws`), строки JSON:
//!   телефон → программа: auth {pin} · gain {i,v} · mute {i,v} · solo {i,v} · route {i,k,v} · scene {id} · listen {i|null} · ping
//!   программа → телефон: auth {ok,need} · meta {...} · state {...} (как у движка) · m {...} (уровни) · listen {i} · err {msg} · pong
//!   двоичные кадры: байт 1 (звук), байт номера дорожки, дальше 16-битный стерео PCM 48 кГц, little-endian.

use std::net::SocketAddr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, Instant};

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::State;
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::Router;
use base64::Engine as _;
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::sync::{broadcast, watch};

/// Связь модуля с программой.
pub trait Backend: Send + Sync + 'static {
    /// Отправить строку-команду движку (как окно: `set strip 2 gain -6`).
    fn send_engine(&self, line: &str);
    /// Содержимое файла настроек (имена дорожек, сцены). `None`, если файла ещё нет.
    fn settings_json(&self) -> Option<String>;
    /// Телефон переключил сцену: окно должно запомнить её как активную.
    fn scene_applied(&self, id: &str);
}

#[derive(Clone, Debug, PartialEq)]
pub struct Config {
    pub enabled: bool,
    pub port: u16,
    pub require_pin: bool,
    pub pin: String,
    /// Можно ли слушать звук дорожек на телефоне.
    pub listen: bool,
    /// Запас звука на телефоне: fast / balance / safe.
    pub buffer: String,
}

impl Default for Config {
    fn default() -> Self {
        Config { enabled: false, port: 8080, require_pin: true, pin: String::new(), listen: true, buffer: "balance".into() }
    }
}

impl Config {
    /// Читает раздел `app` файла настроек (поля remoteOn, remotePort, ...).
    pub fn from_app(a: &Value) -> Config {
        let d = Config::default();
        let port = a.get("remotePort").and_then(|v| v.as_u64()).filter(|p| (1024..=65535).contains(p)).map(|p| p as u16).unwrap_or(d.port);
        let buffer = match a.get("remoteBuf").and_then(|v| v.as_str()) {
            Some(b @ ("fast" | "balance" | "safe")) => b.to_string(),
            _ => d.buffer.clone(),
        };
        Config {
            enabled: a.get("remoteOn").and_then(|v| v.as_bool()).unwrap_or(d.enabled),
            port,
            require_pin: a.get("remotePinReq").and_then(|v| v.as_bool()).unwrap_or(d.require_pin),
            pin: a.get("remotePin").and_then(|v| v.as_str()).unwrap_or("").chars().filter(|c| c.is_ascii_digit()).take(8).collect(),
            listen: a.get("remoteListen").and_then(|v| v.as_bool()).unwrap_or(d.listen),
            buffer,
        }
    }
}

#[derive(Clone)]
enum Out {
    Text(Arc<str>),
    /// Звук дорожки: номер дорожки и готовый кадр для телефона.
    Pcm(u8, Arc<Vec<u8>>),
}

struct Inner {
    backend: Arc<dyn Backend>,
    cfg: RwLock<Config>,
    tx: broadcast::Sender<Out>,
    /// Увеличивается, когда надо отключить всех (сменили PIN, выключили звук, остановили сервер).
    kick: watch::Sender<u64>,
    clients: AtomicUsize,
    listeners: AtomicUsize,
    last_state: Mutex<Option<Arc<str>>>,
    /// Подряд неудачные попытки ввести PIN (любым устройством) и время, до которого вход закрыт.
    fails: Mutex<(u32, Option<Instant>)>,
    running: Mutex<Running>,
}

#[derive(Default)]
struct Running {
    port: Option<u16>,
    error: String,
    stop: Option<tokio::sync::oneshot::Sender<()>>,
}

pub struct Remote {
    inner: Arc<Inner>,
    rt: tokio::runtime::Runtime,
}

// Файлы страницы пульта лежат внутри программы: телефону не нужен интернет.
const PAGE_HTML: &str = include_str!("../../app/remote.html");
const PAGE_JS: &str = include_str!("../../app/remote.js");
const FONTS_CSS: &str = include_str!("../../app/fonts.css");
const LOGO_PNG: &[u8] = include_bytes!("../../app/logo.png");
macro_rules! font {
    ($n:literal) => {
        ($n, include_bytes!(concat!("../../app/fonts/", $n)) as &'static [u8])
    };
}
const FONTS: &[(&str, &[u8])] = &[
    font!("jetbrains-mono-cyrillic-400-normal.woff2"),
    font!("jetbrains-mono-cyrillic-500-normal.woff2"),
    font!("jetbrains-mono-cyrillic-600-normal.woff2"),
    font!("jetbrains-mono-latin-400-normal.woff2"),
    font!("jetbrains-mono-latin-500-normal.woff2"),
    font!("jetbrains-mono-latin-600-normal.woff2"),
    font!("onest-cyrillic-400-normal.woff2"),
    font!("onest-cyrillic-500-normal.woff2"),
    font!("onest-cyrillic-600-normal.woff2"),
    font!("onest-latin-400-normal.woff2"),
    font!("onest-latin-500-normal.woff2"),
    font!("onest-latin-600-normal.woff2"),
    font!("unbounded-cyrillic-500-normal.woff2"),
    font!("unbounded-cyrillic-600-normal.woff2"),
    font!("unbounded-latin-500-normal.woff2"),
    font!("unbounded-latin-600-normal.woff2"),
];

impl Remote {
    pub fn new(backend: Arc<dyn Backend>) -> Remote {
        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .thread_name("remote")
            .build()
            .expect("не удалось запустить сервер пульта");
        let (tx, _) = broadcast::channel(512);
        let (kick, _) = watch::channel(0u64);
        let inner = Arc::new(Inner {
            backend,
            cfg: RwLock::new(Config::default()),
            tx,
            kick,
            clients: AtomicUsize::new(0),
            listeners: AtomicUsize::new(0),
            last_state: Mutex::new(None),
            fails: Mutex::new((0, None)),
            running: Mutex::new(Running::default()),
        });
        Remote { inner, rt }
    }

    /// Применяет настройки: включает, выключает или перезапускает сервер.
    pub fn apply(&self, cfg: Config) {
        let inner = &self.inner;
        let old = inner.cfg.read().map(|c| c.clone()).unwrap_or_default();
        *inner.cfg.write().unwrap() = cfg.clone();

        let (running_port, has_stop) = {
            let r = inner.running.lock().unwrap();
            (r.port, r.stop.is_some())
        };
        let must_restart = !cfg.enabled || (has_stop && running_port != Some(cfg.port));
        if has_stop && must_restart {
            self.stop_server();
        }
        // Сменили PIN или запретили звук: подключённым устройствам придётся войти заново.
        if old.pin != cfg.pin || old.require_pin != cfg.require_pin || (old.listen && !cfg.listen) {
            inner.kick.send_modify(|v| *v += 1);
        }
        if cfg.enabled && inner.running.lock().unwrap().stop.is_none() {
            self.start_server(cfg.port);
        }
    }

    fn stop_server(&self) {
        let stop = {
            let mut r = self.inner.running.lock().unwrap();
            r.port = None;
            r.stop.take()
        };
        if let Some(s) = stop {
            let _ = s.send(());
        }
        self.inner.kick.send_modify(|v| *v += 1);
        self.inner.stop_listening();
    }

    fn start_server(&self, port: u16) {
        let inner = self.inner.clone();
        let addr = SocketAddr::from(([0, 0, 0, 0], port));
        // Порт занимаем сразу (а не в задаче), чтобы ошибку «порт занят» увидеть в статусе без ожидания.
        let listener = match std::net::TcpListener::bind(addr).and_then(|l| l.set_nonblocking(true).map(|_| l)) {
            Ok(l) => l,
            Err(e) => {
                let msg = if e.kind() == std::io::ErrorKind::AddrInUse { format!("порт {} занят другой программой", port) } else { format!("не удалось открыть порт {}: {}", port, e) };
                let mut r = inner.running.lock().unwrap();
                r.error = msg;
                r.port = None;
                return;
            }
        };
        let (stop_tx, stop_rx) = tokio::sync::oneshot::channel::<()>();
        {
            let mut r = inner.running.lock().unwrap();
            r.error.clear();
            r.port = Some(port);
            r.stop = Some(stop_tx);
        }
        let app = Router::new()
            .route("/", get(page_html))
            .route("/remote.js", get(page_js))
            .route("/fonts.css", get(fonts_css))
            .route("/logo.png", get(logo))
            .route("/fonts/:name", get(font_file))
            .route("/ws", get(ws_upgrade))
            .with_state(inner.clone());
        let app = Router::new().merge(app);
        self.rt.spawn(async move {
            let listener = match tokio::net::TcpListener::from_std(listener) {
                Ok(l) => l,
                Err(e) => {
                    inner.running.lock().unwrap().error = e.to_string();
                    return;
                }
            };
            let _ = axum::serve(listener, app)
                .with_graceful_shutdown(async move {
                    let _ = stop_rx.await;
                })
                .await;
        });
    }

    /// Строка движка. Возвращает `true`, если строка целиком принадлежит пульту (звук) и окну её отдавать не нужно.
    pub fn on_engine_line(&self, line: &str) -> bool {
        self.inner.on_engine_line(line)
    }

    /// Состояние для окна: работает ли сервер, адрес, сколько устройств подключено.
    pub fn status(&self) -> Value {
        let r = self.inner.running.lock().unwrap();
        let port = r.port;
        let ip = local_ip();
        json!({
            "running": port.is_some(),
            "port": port,
            "ip": ip,
            "url": port.map(|p| format!("http://{}:{}", ip, p)),
            "clients": self.inner.clients.load(Ordering::SeqCst),
            "listeners": self.inner.listeners.load(Ordering::SeqCst),
            "error": r.error,
        })
    }

    /// Отключает все подключённые устройства.
    pub fn disconnect_all(&self) {
        self.inner.kick.send_modify(|v| *v += 1);
        self.inner.stop_listening();
    }

    /// QR-код со ссылкой на пульт в виде SVG.
    pub fn qr_svg(&self) -> Option<String> {
        let url = self.status().get("url")?.as_str()?.to_string();
        qr_svg(&url)
    }
}

pub fn qr_svg(text: &str) -> Option<String> {
    use qrcode::render::svg;
    let code = qrcode::QrCode::new(text.as_bytes()).ok()?;
    Some(
        code.render::<svg::Color>()
            .min_dimensions(180, 180)
            .quiet_zone(true)
            .dark_color(svg::Color("#0b0b0c"))
            .light_color(svg::Color("#ffffff"))
            .build(),
    )
}

/// Адрес этого компьютера в локальной сети. Пакеты никуда не отправляются: UDP-«соединение» только выбирает сетевой интерфейс.
pub fn local_ip() -> String {
    for probe in ["8.8.8.8:80", "192.168.1.1:80", "10.0.0.1:80", "172.16.0.1:80"] {
        if let Ok(s) = std::net::UdpSocket::bind("0.0.0.0:0") {
            if s.connect(probe).is_ok() {
                if let Ok(a) = s.local_addr() {
                    if !a.ip().is_loopback() && !a.ip().is_unspecified() {
                        return a.ip().to_string();
                    }
                }
            }
        }
    }
    "127.0.0.1".into()
}

impl Inner {
    fn on_engine_line(&self, line: &str) -> bool {
        // Звук дорожки: {"t":"pcm","i":3,"d":"<base64>"}. Разбираем вручную: строк много, JSON-разбор тут не нужен.
        if line.starts_with(r#"{"t":"pcm""#) {
            if self.listeners.load(Ordering::Relaxed) == 0 {
                // Никто не слушает, а движок ещё шлёт звук (окно закрыли, движок перезапустился): просим замолчать.
                self.backend.send_engine("set listen off");
                return true;
            }
            let idx = line.find(r#""i":"#).and_then(|p| line[p + 4..].split(|c: char| !c.is_ascii_digit()).next()).and_then(|s| s.parse::<u8>().ok());
            let data = line.find(r#""d":""#).map(|p| &line[p + 5..]).and_then(|s| s.split('"').next());
            if let (Some(i), Some(d)) = (idx, data) {
                if let Ok(bytes) = base64::engine::general_purpose::STANDARD.decode(d) {
                    let _ = self.tx.send(Out::Pcm(i, Arc::new(bytes)));
                }
            }
            return true;
        }
        if self.clients.load(Ordering::Relaxed) == 0 && !line.starts_with(r#"{"t":"state""#) && !line.starts_with(r#"{"t":"hello""#) {
            return false;
        }
        if line.starts_with(r#"{"t":"state""#) {
            let s: Arc<str> = Arc::from(line);
            *self.last_state.lock().unwrap() = Some(s.clone());
            let _ = self.tx.send(Out::Text(s));
        } else if line.starts_with(r#"{"t":"m""#) {
            let _ = self.tx.send(Out::Text(Arc::from(line)));
        } else if line.starts_with(r#"{"t":"hello""#) {
            // Движок запущен заново: он ничего не помнит о прослушивании.
            if self.listeners.load(Ordering::SeqCst) > 0 {
                self.listeners.store(0, Ordering::SeqCst);
                self.kick.send_modify(|v| *v += 1);
            }
        }
        false
    }

    /// Уменьшает число слушателей (не уходя ниже нуля). Возвращает `true`, если слушателей не осталось.
    fn dec_listeners(&self) -> bool {
        let prev = self.listeners.fetch_update(Ordering::SeqCst, Ordering::SeqCst, |n| Some(n.saturating_sub(1))).unwrap_or(0);
        prev <= 1
    }

    fn stop_listening(&self) {
        if self.listeners.swap(0, Ordering::SeqCst) > 0 {
            self.backend.send_engine("set listen off");
        }
    }

    fn meta(&self) -> String {
        let cfg = self.cfg.read().unwrap().clone();
        let settings: Value = self.backend.settings_json().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(Value::Null);
        let strips: Vec<Value> = (0..8)
            .map(|i| {
                let s = settings.get("strips").and_then(|a| a.get(i));
                json!({
                    "name": s.and_then(|s| s.get("name")).and_then(|v| v.as_str()).unwrap_or(&format!("Дорожка {}", i + 1)),
                    "tag": s.and_then(|s| s.get("tag")).and_then(|v| v.as_str()).unwrap_or(""),
                    "hidden": s.and_then(|s| s.get("hidden")).and_then(|v| v.as_bool()).unwrap_or(false),
                })
            })
            .collect();
        let scenes: Vec<Value> = settings
            .get("scenes")
            .and_then(|v| v.as_array())
            .map(|a| a.iter().filter_map(|s| Some(json!({ "id": s.get("id")?.as_str()?, "name": s.get("name")?.as_str()? }))).collect())
            .unwrap_or_default();
        json!({
            "t": "meta",
            "strips": strips,
            "scenes": scenes,
            "active": settings.get("active"),
            "listen": cfg.listen,
            "buffer": cfg.buffer,
        })
        .to_string()
    }

    fn apply_scene(&self, id: &str) -> bool {
        let settings: Value = match self.backend.settings_json().and_then(|t| serde_json::from_str(&t).ok()) {
            Some(v) => v,
            None => return false,
        };
        let sc = match settings.get("scenes").and_then(|a| a.as_array()).and_then(|a| a.iter().find(|s| s.get("id").and_then(|v| v.as_str()) == Some(id))) {
            Some(s) => s,
            None => return false,
        };
        if let Some(routes) = sc.get("routes").and_then(|r| r.as_array()) {
            for (i, mask) in routes.iter().take(8).enumerate() {
                let m = mask.as_u64().unwrap_or(0);
                for k in 0..5 {
                    self.backend.send_engine(&format!("set strip {} route {} {}", i, k, (m >> k) & 1));
                }
            }
        }
        self.backend.scene_applied(id);
        true
    }

    fn pin_ok(&self, given: &str) -> bool {
        let cfg = self.cfg.read().unwrap();
        if !cfg.require_pin {
            return true;
        }
        let a = cfg.pin.as_bytes();
        let b = given.as_bytes();
        if a.is_empty() {
            return false; // PIN нужен, но не задан: никого не пускаем
        }
        // Сравнение без раннего выхода: время ответа не выдаёт, сколько цифр угадано.
        let mut diff = a.len() ^ b.len();
        for i in 0..a.len().max(b.len()) {
            diff |= (*a.get(i).unwrap_or(&0) ^ *b.get(i).unwrap_or(&0)) as usize;
        }
        diff == 0
    }

    /// Можно ли сейчас пробовать PIN. После 5 ошибок подряд вход закрывается на 30 секунд для всех устройств.
    fn auth_allowed(&self) -> bool {
        let f = self.fails.lock().unwrap();
        f.1.map(|t| Instant::now() >= t).unwrap_or(true)
    }
    fn auth_failed(&self) {
        let mut f = self.fails.lock().unwrap();
        f.0 += 1;
        if f.0 >= 5 {
            f.0 = 0;
            f.1 = Some(Instant::now() + Duration::from_secs(30));
        }
    }
    fn auth_ok(&self) {
        *self.fails.lock().unwrap() = (0, None);
    }
}

// ---------- HTTP ----------

fn security_headers(h: &mut HeaderMap) {
    h.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    h.insert(header::X_CONTENT_TYPE_OPTIONS, HeaderValue::from_static("nosniff"));
    h.insert("x-frame-options", HeaderValue::from_static("DENY"));
    h.insert("referrer-policy", HeaderValue::from_static("no-referrer"));
    h.insert(
        header::CONTENT_SECURITY_POLICY,
        HeaderValue::from_static("default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'; connect-src 'self' ws: wss:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"),
    );
}

fn reply(ct: &'static str, body: impl IntoResponse) -> Response {
    let mut r = body.into_response();
    r.headers_mut().insert(header::CONTENT_TYPE, HeaderValue::from_static(ct));
    security_headers(r.headers_mut());
    r
}

async fn page_html() -> Response {
    reply("text/html; charset=utf-8", PAGE_HTML)
}
async fn page_js() -> Response {
    reply("text/javascript; charset=utf-8", PAGE_JS)
}
async fn fonts_css() -> Response {
    reply("text/css; charset=utf-8", FONTS_CSS)
}
async fn logo() -> Response {
    reply("image/png", LOGO_PNG)
}
async fn font_file(axum::extract::Path(name): axum::extract::Path<String>) -> Response {
    match FONTS.iter().find(|(n, _)| *n == name) {
        Some((_, bytes)) => {
            let mut r = reply("font/woff2", *bytes);
            r.headers_mut().insert(header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=86400"));
            r
        }
        None => StatusCode::NOT_FOUND.into_response(),
    }
}

/// Браузер сам подставляет Origin в WebSocket. Если страница открыта с другого сайта, а не с нашего адреса, отказываем:
/// иначе любой сайт в браузере на этом же компьютере мог бы управлять микшером.
fn origin_ok(h: &HeaderMap) -> bool {
    let origin = match h.get(header::ORIGIN).and_then(|v| v.to_str().ok()) {
        Some(o) => o,
        None => return true, // не браузер (например, тест)
    };
    let host = h.get(header::HOST).and_then(|v| v.to_str().ok()).unwrap_or("");
    let o = origin.strip_prefix("http://").or_else(|| origin.strip_prefix("https://")).unwrap_or(origin);
    !host.is_empty() && o.eq_ignore_ascii_case(host)
}

async fn ws_upgrade(ws: WebSocketUpgrade, State(inner): State<Arc<Inner>>, headers: HeaderMap) -> Response {
    if !origin_ok(&headers) {
        return StatusCode::FORBIDDEN.into_response();
    }
    ws.max_message_size(4096).on_upgrade(move |sock| session(inner, sock))
}

fn num(v: &Value, key: &str) -> Option<f64> {
    v.get(key).and_then(|x| x.as_f64()).filter(|x| x.is_finite())
}
fn flag(v: &Value) -> Option<bool> {
    match v.get("v")? {
        Value::Bool(b) => Some(*b),
        Value::Number(n) => Some(n.as_f64()? != 0.0),
        _ => None,
    }
}

async fn session(inner: Arc<Inner>, sock: WebSocket) {
    inner.clients.fetch_add(1, Ordering::SeqCst);
    let (mut sink, mut stream) = sock.split();
    let mut rx = inner.tx.subscribe();
    let mut kick = inner.kick.subscribe();
    kick.borrow_and_update();

    let need_pin = inner.cfg.read().unwrap().require_pin;
    let mut authed = !need_pin;
    let mut listening = false;

    async fn send_text(sink: &mut (impl SinkExt<Message> + Unpin), s: String) -> bool {
        sink.send(Message::Text(s)).await.is_ok()
    }
    async fn greet(inner: &Inner, sink: &mut (impl SinkExt<Message> + Unpin)) -> bool {
        if !send_text(sink, json!({"t":"auth","ok":true}).to_string()).await {
            return false;
        }
        if !send_text(sink, inner.meta()).await {
            return false;
        }
        let st = inner.last_state.lock().unwrap().clone();
        match st {
            Some(s) => send_text(sink, s.to_string()).await,
            None => {
                inner.backend.send_engine("get");
                true
            }
        }
    }

    let mut alive = if authed { greet(&inner, &mut sink).await } else { send_text(&mut sink, json!({"t":"auth","ok":false,"need":true}).to_string()).await };

    while alive {
        tokio::select! {
            msg = stream.next() => {
                let text = match msg {
                    Some(Ok(Message::Text(t))) => t,
                    Some(Ok(Message::Close(_))) | None | Some(Err(_)) => break,
                    _ => continue,
                };
                let v: Value = match serde_json::from_str(&text) { Ok(v) => v, Err(_) => continue };
                let t = v.get("t").and_then(|x| x.as_str()).unwrap_or("");
                if t == "ping" {
                    alive = send_text(&mut sink, r#"{"t":"pong"}"#.to_string()).await;
                    continue;
                }
                if t == "auth" {
                    if !inner.auth_allowed() {
                        alive = send_text(&mut sink, json!({"t":"auth","ok":false,"need":true,"msg":"Слишком много попыток. Подождите полминуты."}).to_string()).await;
                        continue;
                    }
                    if inner.pin_ok(v.get("pin").and_then(|x| x.as_str()).unwrap_or("")) {
                        inner.auth_ok();
                        authed = true;
                        alive = greet(&inner, &mut sink).await;
                    } else {
                        inner.auth_failed();
                        tokio::time::sleep(Duration::from_millis(800)).await; // замедляем перебор
                        alive = send_text(&mut sink, json!({"t":"auth","ok":false,"need":true,"msg":"Неверный PIN"}).to_string()).await;
                    }
                    continue;
                }
                if !authed { continue; }
                let strip = v.get("i").and_then(|x| x.as_u64()).filter(|i| *i < 8);
                match (t, strip) {
                    ("gain", Some(i)) => if let Some(g) = num(&v, "v") {
                        inner.backend.send_engine(&format!("set strip {} gain {:.1}", i, g.clamp(-48.0, 12.0)));
                    },
                    ("mute", Some(i)) => if let Some(b) = flag(&v) {
                        inner.backend.send_engine(&format!("set strip {} mute {}", i, b as u8));
                    },
                    ("solo", Some(i)) => if let Some(b) = flag(&v) {
                        inner.backend.send_engine(&format!("set strip {} solo {}", i, b as u8));
                    },
                    ("route", Some(i)) => {
                        let k = v.get("k").and_then(|x| x.as_u64()).filter(|k| *k < 5);
                        if let (Some(k), Some(b)) = (k, flag(&v)) {
                            inner.backend.send_engine(&format!("set strip {} route {} {}", i, k, b as u8));
                        }
                    }
                    ("scene", _) => {
                        let id = v.get("id").and_then(|x| x.as_str()).unwrap_or("");
                        if !inner.apply_scene(id) {
                            alive = send_text(&mut sink, json!({"t":"err","msg":"Сцена не найдена"}).to_string()).await;
                        }
                    }
                    ("listen", _) => {
                        let want = v.get("i").and_then(|x| x.as_u64()).filter(|i| *i < 8);
                        if want.is_some() && !inner.cfg.read().unwrap().listen {
                            alive = send_text(&mut sink, json!({"t":"err","msg":"Прослушивание выключено в настройках на компьютере"}).to_string()).await;
                            continue;
                        }
                        match want {
                            Some(i) => {
                                if !listening { listening = true; inner.listeners.fetch_add(1, Ordering::SeqCst); }
                                inner.backend.send_engine(&format!("set listen {}", i));
                            }
                            None => {
                                if listening {
                                    listening = false;
                                    if inner.dec_listeners() {
                                        inner.backend.send_engine("set listen off");
                                    }
                                }
                            }
                        }
                    }
                    _ => {}
                }
            }
            out = rx.recv() => {
                match out {
                    Ok(Out::Text(s)) => if authed { alive = send_text(&mut sink, s.to_string()).await; },
                    Ok(Out::Pcm(i, bytes)) => if authed && listening {
                        let mut frame = Vec::with_capacity(bytes.len() + 2);
                        frame.push(1);
                        frame.push(i);
                        frame.extend_from_slice(&bytes);
                        alive = sink.send(Message::Binary(frame)).await.is_ok();
                    },
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => break,
                }
            }
            _ = kick.changed() => break,
        }
    }

    if listening && inner.dec_listeners() {
        inner.backend.send_engine("set listen off");
    }
    inner.clients.fetch_sub(1, Ordering::SeqCst);
}
