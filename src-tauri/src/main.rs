// Не показываем консоль в релизной сборке под Windows.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod obs;

use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
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
        .invoke_handler(tauri::generate_handler![engine_send, engine_info, engine_restart, settings_load, settings_save, obs_probe, obs_save_collection])
        .setup(|app| {
            let engine = Engine { stdin: Mutex::new(None), child: Mutex::new(None), boot: AtomicU64::new(0), gen: AtomicU64::new(0) };
            start_engine(app.handle(), &engine);
            app.manage(engine);

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
            Ok(())
        })
        // Закрытие окна сворачивает в трей: звук продолжает работать.
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.hide();
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
