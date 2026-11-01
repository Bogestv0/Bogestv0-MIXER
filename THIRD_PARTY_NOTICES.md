# Сторонние компоненты

Собственный код Bogestv0 MIXER распространяется по лицензии MIT (файл `LICENSE`). В проект входят или при сборке подключаются:

| Компонент | Где | Лицензия |
|---|---|---|
| RNNoise (шумоподавление) | `engine/third_party/rnnoise` (текст в `COPYING`) | BSD-3-Clause |
| miniaudio (звук через WASAPI) | `engine/third_party/miniaudio.h` | на выбор: Unlicense (общественное достояние) или MIT No Attribution |
| Microsoft SysVAD (основа драйвера) | скачивается при сборке драйвера, наши правки в `driver/` (см. `driver/UPSTREAM.md`) | MS-PL: сохраняются уведомления Microsoft, названия Microsoft не используются |
| Tauri и его плагины (оболочка окна) | зависимости `src-tauri/Cargo.toml` | MIT или Apache-2.0 |
