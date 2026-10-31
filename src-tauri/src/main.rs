// Не показываем консоль в релизной сборке под Windows.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod obs;

use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use tauri::{
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Emitter, Manager, RunEvent, State, WindowEvent,
};

/// Запущенный звуковой движок (engine.exe --serve). Окно шлёт ему строки-команды,
/// а ответы движка (JSON по одной строке) пересылает в интерфейс событием "engine-line".
/// Движок можно перезапустить, не закрывая программу (команда engine_restart).
struct Engine {
    stdin: Mutex<Option<ChildStdin>>,
    child: Mutex<Option<Child>>,
    /// Время запуска движка: интерфейс по нему понимает, что настройки надо отправить заново.
    boot: AtomicU64,
    /// Номер запуска. Старый движок после перезапуска больше ничего не сообщает окну.
    gen: AtomicU64,
}

/// Настройки оболочки с экрана «Настройки» (трей, закрытие окна). Читает обработчик закрытия окна.
struct Prefs {
    /// Закрытие окна сворачивает в трей (если трей включён). Иначе программа закрывается целиком.
    hide_on_close: AtomicBool,
    tray: AtomicBool,
}

fn engine_path() -> std::path::PathBuf {
    let name = if cfg!(windows) { "engine.exe" } else { "engine" };
    // Установленное приложение: engine.exe лежит рядом с основной программой.
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let p = dir.join(name);
            if p.exists() {
                return p;
            }
        }
    }
    // Для разработки: путь можно задать переменной окружения.
    std::env::var("BOGESTV0_ENGINE").map(Into::into).unwrap_or_else(|_| name.into())
}

fn current_gen(app: &AppHandle, fallback: u64) -> u64 {
    app.try_state::<Engine>().map(|e| e.gen.load(Ordering::SeqCst)).unwrap_or(fallback)
}

/// Запускает движок и записывает его в `engine`. Старый движок к этому моменту уже остановлен.
fn start_engine(app: &AppHandle, engine: &Engine) {
    let boot = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
    let gen = engine.gen.fetch_add(1, Ordering::SeqCst) + 1;
    engine.boot.store(boot, Ordering::SeqCst);
    let mut cmd = Command::new(engine_path());
    cmd.arg("--serve").stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: без чёрного окна консоли
    }
    match cmd.spawn() {
        Ok(mut child) => {
            let stdin = child.stdin.take();
            if let Some(out) = child.stdout.take() {
                let handle = app.clone();
                std::thread::spawn(move || {
                    for line in BufReader::new(out).lines().map_while(Result::ok) {
                        if current_gen(&handle, gen) != gen {
                            return; // это уже старый движок
                        }
                        let _ = handle.emit("engine-line", line);
                    }
                    if current_gen(&handle, gen) == gen {
                        let _ = handle.emit("engine-line", r#"{"t":"exit"}"#);
                    }
                });
            }
            if let Ok(mut s) = engine.stdin.lock() {
                *s = stdin;
            }
            if let Ok(mut c) = engine.child.lock() {
                *c = Some(child);
            }
        }
        Err(e) => {
            let _ = app.emit(
                "engine-line",
                format!(r#"{{"t":"err","msg":"движок не запустился: {}"}}"#, e.to_string().replace('"', "'").replace('\\', "/")),
            );
            if let Ok(mut s) = engine.stdin.lock() {
                *s = None;
            }
            if let Ok(mut c) = engine.child.lock() {
                *c = None;
            }
        }
    }
}

/// Просит движок завершиться и ждёт до секунды: так он успевает отпустить звуковые устройства
/// (в монопольном режиме новый движок иначе не смог бы их открыть). Не ушёл, значит, завершаем силой.
fn stop_engine(engine: &Engine) {
    engine.gen.fetch_add(1, Ordering::SeqCst); // старый движок замолкает для окна сразу
    if let Ok(mut s) = engine.stdin.lock() {
        if let Some(stdin) = s.as_mut() {
            let _ = writeln!(stdin, "quit");
            let _ = stdin.flush();
        }
        *s = None;
    }
    if let Ok(mut c) = engine.child.lock() {
        if let Some(mut child) = c.take() {
            let t0 = Instant::now();
            while t0.elapsed() < Duration::from_millis(1000) {
                if matches!(child.try_wait(), Ok(Some(_))) {
                    return;
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn engine_info_json(engine: &Engine) -> String {
    let running = engine.stdin.lock().map(|g| g.is_some()).unwrap_or(false);
    format!(r#"{{"boot":{},"running":{}}}"#, engine.boot.load(Ordering::SeqCst), running)
}

#[tauri::command]
fn engine_send(line: String, engine: State<Engine>) -> Result<(), String> {
    let mut guard = engine.stdin.lock().map_err(|e| e.to_string())?;
    match guard.as_mut() {
        Some(stdin) => {
            let clean = line.replace(['\n', '\r'], " ");
            writeln!(stdin, "{}", clean).and_then(|_| stdin.flush()).map_err(|e| e.to_string())
        }
        None => Err("движок не запущен".into()),
    }
}

#[tauri::command]
fn engine_info(engine: State<Engine>) -> String {
    engine_info_json(&engine)
}

/// Перезапуск движка (Ctrl+R и кнопка «Перезапустить движок»). Окно потом само отправляет ему сохранённые настройки.
#[tauri::command]
async fn engine_restart(app: AppHandle, engine: State<'_, Engine>) -> Result<String, String> {
    stop_engine(&engine);
    start_engine(&app, &engine);
    Ok(engine_info_json(&engine))
}

fn settings_path(app: &AppHandle) -> Result<std::path::PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    Ok(dir.join("bogestv0-settings.json"))
}

/// Файл настроек лежит в профиле пользователя, отдельно от программы: при обновлении его не трогаем.
#[tauri::command]
fn settings_load(app: AppHandle) -> Result<String, String> {
    let path = settings_path(&app)?;
    match std::fs::read_to_string(&path) {
        Ok(s) => Ok(s),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(String::new()),
        Err(e) => Err(e.to_string()),
    }
}

#[tauri::command]
fn settings_save(app: AppHandle, json: String) -> Result<(), String> {
    let path = settings_path(&app)?;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    // Пишем во временный файл и переименовываем: при сбое старый файл остаётся целым.
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, json).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())
}

/// Настройки WebSocket в OBS (порт, пароль, включён ли): окно по ним само подключается к OBS, вводить ничего не надо.
#[tauri::command]
fn obs_probe() -> String {
    obs::probe().to_string()
}

/// Кладёт в папку OBS готовую коллекцию сцен с нашими Out. Возвращает путь к файлу.
#[tauri::command]
fn obs_save_collection(name: String, json: String) -> Result<String, String> {
    obs::save_collection(&name, &json)
}

/// Применяет настройки оболочки: значок в трее, «поверх всех окон» и поведение при закрытии окна.
fn apply_prefs(app: &AppHandle, tray: bool, hide_on_close: bool, on_top: bool) {
    if let Some(p) = app.try_state::<Prefs>() {
        p.tray.store(tray, Ordering::SeqCst);
        p.hide_on_close.store(hide_on_close, Ordering::SeqCst);
    }
    if let Some(t) = app.tray_by_id("main-tray") {
        let _ = t.set_visible(tray);
    }
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.set_always_on_top(on_top);
    }
}

/// Окно присылает сюда настройки «Значок в трее», «Закрытие окна сворачивает в трей» и «Поверх всех окон».
#[tauri::command]
fn app_prefs(app: AppHandle, tray: bool, hide_on_close: bool, on_top: bool) {
    apply_prefs(&app, tray, hide_on_close, on_top);
}

/// Читает раздел `app` файла настроек (без окна): нужно до того, как окно загрузилось.
fn read_app_prefs(app: &AppHandle) -> serde_json::Value {
    settings_path(app)
        .ok()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|v| v.get("app").cloned())
        .unwrap_or(serde_json::Value::Null)
}

fn show_main(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.show();
        let _ = w.unminimize();
        let _ = w.set_focus();
    }
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_updater::Builder::new().build())
        .plugin(tauri_plugin_process::init())
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![engine_send, engine_info, engine_restart, settings_load, settings_save, obs_probe, obs_save_collection, app_prefs])
        .setup(|app| {
            let engine = Engine { stdin: Mutex::new(None), child: Mutex::new(None), boot: AtomicU64::new(0), gen: AtomicU64::new(0) };
            start_engine(app.handle(), &engine);
            app.manage(engine);

            // Настройки оболочки из файла настроек: нужны сразу, до загрузки окна.
            let a = read_app_prefs(app.handle());
            let flag = |key: &str, default: bool| a.get(key).and_then(|v| v.as_bool()).unwrap_or(default);
            let (tray_on, hide_on_close, show_on_start, on_top) = (flag("tray", true), flag("hideOnClose", true), flag("showOnStart", true), flag("onTop", false));
            app.manage(Prefs { hide_on_close: AtomicBool::new(hide_on_close), tray: AtomicBool::new(tray_on) });

            let show = MenuItem::with_id(app, "show", "Открыть микшер", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "Выход", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show, &quit])?;

            TrayIconBuilder::with_id("main-tray")
                .icon(app.default_window_icon().cloned().expect("нет значка приложения"))
                .tooltip("Bogestv0 MIXER")
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "show" => show_main(app),
                    "quit" => app.exit(0),
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        show_main(tray.app_handle());
                    }
                })
                .build(app)?;
            apply_prefs(app.handle(), tray_on, hide_on_close, on_top);
            // Окно создаётся скрытым (tauri.conf.json). Без значка в трее скрытое окно не открыть, поэтому тогда показываем всегда.
            if show_on_start || !tray_on {
                show_main(app.handle());
            }
            Ok(())
        })
        // Закрытие окна сворачивает в трей (звук продолжает работать), если это включено и трей виден. Иначе программа закрывается.
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                let app = window.app_handle();
                let hide = app
                    .try_state::<Prefs>()
                    .map(|p| p.hide_on_close.load(Ordering::SeqCst) && p.tray.load(Ordering::SeqCst))
                    .unwrap_or(true);
                if hide {
                    api.prevent_close();
                    let _ = window.hide();
                } else {
                    app.exit(0);
                }
            }
        })
        .build(tauri::generate_context!())
        .expect("ошибка запуска Bogestv0 MIXER")
        .run(|app, event| {
            // При выходе просим движок завершиться (stop_engine ждёт его и добивает, если завис).
            if let RunEvent::Exit = event {
                if let Some(engine) = app.try_state::<Engine>() {
                    stop_engine(&engine);
                }
            }
        });
}
