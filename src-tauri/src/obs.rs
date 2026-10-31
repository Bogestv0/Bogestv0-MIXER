//! Связь с OBS: где лежат настройки WebSocket и куда класть готовую коллекцию сцен.
//! Сама работа с OBS идёт из окна (app/obs.js); здесь только то, что окну недоступно: файлы в профиле пользователя.

use serde_json::{json, Value};
use std::path::{Path, PathBuf};

/// Папка настроек OBS: %APPDATA%\obs-studio (обычная установка).
pub fn obs_dir() -> Option<PathBuf> {
    std::env::var_os("APPDATA").map(|a| PathBuf::from(a).join("obs-studio"))
}

fn read_json(path: &Path) -> Option<Value> {
    let text = std::fs::read_to_string(path).ok()?;
    serde_json::from_str(text.trim_start_matches('\u{feff}')).ok()
}

/// Что известно про OBS: есть ли папка и что записано в настройках сервера WebSocket (порт, пароль, включён ли).
/// Пароль нужен окну только чтобы подключиться к OBS на этом же компьютере, дальше он никуда не уходит.
pub fn probe_in(dir: &Path) -> Value {
    let cfg = read_json(&dir.join("plugin_config").join("obs-websocket").join("config.json"));
    let ws = cfg.map(|c| {
        json!({
            "enabled": c["server_enabled"].as_bool().unwrap_or(false),
            "port": c["server_port"].as_u64().filter(|p| (1..=65535).contains(p)).unwrap_or(4455),
            "auth": c["auth_required"].as_bool().unwrap_or(true),
            "password": c["server_password"].as_str().unwrap_or(""),
        })
    });
    json!({ "found": dir.is_dir(), "ws": ws })
}

pub fn probe() -> Value {
    match obs_dir() {
        Some(d) => probe_in(&d),
        None => json!({ "found": false, "ws": null }),
    }
}

/// Имя файла из имени коллекции: только латиница, цифры, «_» и «-».
fn file_stem(name: &str) -> String {
    let s: String = name.chars().map(|c| if c.is_ascii_alphanumeric() || c == '-' { c } else { '_' }).collect();
    let s = s.trim_matches('_').to_string();
    if s.is_empty() { "Bogestv0_MIXER".into() } else { s }
}

/// Кладёт коллекцию сцен в basic\scenes. Старый файл с тем же именем сохраняется рядом как «.json.bak».
/// Возвращает полный путь к новому файлу.
pub fn save_collection_in(dir: &Path, name: &str, json_text: &str) -> Result<PathBuf, String> {
    if !dir.is_dir() {
        return Err("Папка OBS не найдена (%APPDATA%\\obs-studio). Запустите OBS один раз и повторите.".into());
    }
    let v: Value = serde_json::from_str(json_text).map_err(|e| format!("коллекция повреждена: {e}"))?;
    if v["sources"].as_array().map_or(true, |a| a.is_empty()) || v["name"].as_str() != Some(name) {
        return Err("коллекция не прошла проверку (нет источников или имя не совпадает)".into());
    }
    let scenes = dir.join("basic").join("scenes");
    std::fs::create_dir_all(&scenes).map_err(|e| e.to_string())?;
    let path = scenes.join(format!("{}.json", file_stem(name)));
    if path.exists() {
        let _ = std::fs::copy(&path, path.with_extension("json.bak"));
    }
    // Сначала во временный файл: OBS не должен увидеть недописанную коллекцию.
    let tmp = scenes.join(format!("{}.json.tmp", file_stem(name)));
    std::fs::write(&tmp, serde_json::to_string_pretty(&v).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())?;
    Ok(path)
}

pub fn save_collection(name: &str, json_text: &str) -> Result<String, String> {
    let dir = obs_dir().ok_or("Не задана папка профиля (APPDATA)")?;
    save_collection_in(&dir, name, json_text).map(|p| p.display().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("bg-obs-test-{}-{}", tag, std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn probe_reads_websocket_config() {
        let d = tmp("probe");
        let cfgdir = d.join("plugin_config").join("obs-websocket");
        std::fs::create_dir_all(&cfgdir).unwrap();
        // с BOM и русским паролем: оба случая бывают
        std::fs::write(cfgdir.join("config.json"), "\u{feff}{\"alerts_enabled\":false,\"auth_required\":true,\"first_load\":false,\"server_enabled\":true,\"server_password\":\"пароль-1\",\"server_port\":4466}").unwrap();
        let p = probe_in(&d);
        assert_eq!(p["found"], true);
        assert_eq!(p["ws"]["enabled"], true);
        assert_eq!(p["ws"]["port"], 4466);
        assert_eq!(p["ws"]["auth"], true);
        assert_eq!(p["ws"]["password"], "пароль-1");
    }

    #[test]
    fn probe_without_config_or_folder() {
        let d = tmp("none");
        assert_eq!(probe_in(&d)["found"], true);
        assert!(probe_in(&d)["ws"].is_null());
        let missing = d.join("нет-такой");
        assert_eq!(probe_in(&missing)["found"], false);
        // порт вне диапазона и мусор в настройках не должны ломать окно
        let cfgdir = d.join("plugin_config").join("obs-websocket");
        std::fs::create_dir_all(&cfgdir).unwrap();
        std::fs::write(cfgdir.join("config.json"), "{\"server_port\":99999}").unwrap();
        let p = probe_in(&d);
        assert_eq!(p["ws"]["port"], 4455);
        assert_eq!(p["ws"]["enabled"], false);
        std::fs::write(cfgdir.join("config.json"), "не json").unwrap();
        assert!(probe_in(&d)["ws"].is_null());
    }

    #[test]
    fn save_writes_validates_and_backs_up() {
        let d = tmp("save");
        let good = r#"{"name":"Bogestv0 MIXER","sources":[{"id":"scene","name":"Сцена"}]}"#;
        let p = save_collection_in(&d, "Bogestv0 MIXER", good).unwrap();
        assert_eq!(p, d.join("basic").join("scenes").join("Bogestv0_MIXER.json"));
        assert!(std::fs::read_to_string(&p).unwrap().contains("Сцена"));
        assert!(!p.with_extension("json.bak").exists());
        // повторная запись: старая версия остаётся в .bak
        let second = r#"{"name":"Bogestv0 MIXER","sources":[{"id":"scene","name":"Вторая"}]}"#;
        save_collection_in(&d, "Bogestv0 MIXER", second).unwrap();
        assert!(std::fs::read_to_string(p.with_extension("json.bak")).unwrap().contains("Сцена"));
        assert!(std::fs::read_to_string(&p).unwrap().contains("Вторая"));
        // временного файла не остаётся
        assert!(!d.join("basic").join("scenes").join("Bogestv0_MIXER.json.tmp").exists());
        // плохие данные
        assert!(save_collection_in(&d, "Bogestv0 MIXER", "{").is_err());
        assert!(save_collection_in(&d, "Bogestv0 MIXER", r#"{"name":"Bogestv0 MIXER","sources":[]}"#).is_err());
        assert!(save_collection_in(&d, "Другое", good).is_err());
        // нет папки OBS: ничего не создаём
        let missing = d.join("нет-obs");
        assert!(save_collection_in(&missing, "Bogestv0 MIXER", good).is_err());
        assert!(!missing.exists());
    }

    #[test]
    fn stem_is_safe() {
        assert_eq!(file_stem("Bogestv0 MIXER"), "Bogestv0_MIXER");
        assert_eq!(file_stem("../../x"), "x");
        assert_eq!(file_stem("Игры"), "Bogestv0_MIXER");
    }
}
