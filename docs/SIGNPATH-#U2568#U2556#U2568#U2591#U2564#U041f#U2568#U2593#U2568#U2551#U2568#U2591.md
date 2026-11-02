# Заявка в SignPath Foundation: что сделать и что вставить

Подаётся на https://signpath.org/apply (поля формы я не видел, ниже текст, который подойдёт для описания проекта). Условия: https://signpath.org/terms

## До подачи (чек-лист)
1. Репозиторий публичный, лицензия MIT (`LICENSE` есть).
2. **Релиз уже опубликован** в GitHub Releases (в виде, который будет подписываться): поставить тег `v0.6.0`, дождаться `release.yml`, убедиться, что установщик скачивается и ставится. SignPath подписывает только то, что уже выпускается.
3. На странице проекта (README на GitHub) есть ссылка на `CODE_SIGNING.md`: там роли, политика конфиденциальности и строка «Free code signing provided by SignPath.io, certificate by SignPath Foundation».
4. Включена двухфакторная защита на GitHub (Settings → Password and authentication) и будет в SignPath.
5. Заменить `OWNER`/`Bogestv0/Bogestv0-MIXER` в `app/app.js` и `tauri.conf.json` на реальный репозиторий, если он называется иначе.

## Текст заявки (по-английски)
**Project name:** Bogestv0 MIXER
**Repository:** https://github.com/Bogestv0/Bogestv0-MIXER (замените на свою ссылку)
**Releases:** https://github.com/Bogestv0/Bogestv0-MIXER/releases
**License:** MIT
**Description:** Bogestv0 MIXER is a free virtual audio mixer for Windows, similar in purpose to Voicemeeter. It routes microphones and per-application sound to speakers, headphones, stream and call outputs, with noise suppression (RNNoise), EQ, a soundpad and WAV recording. Built with a C++ audio engine (miniaudio/WASAPI), a Tauri/Rust shell and an HTML UI. All builds are produced from the public repository by GitHub Actions (release.yml) on version tags.
**What would be signed:** the NSIS installer and the executables inside (Bogestv0 MIXER.exe, engine.exe). The virtual audio driver is not covered (it needs a Microsoft attestation signature).
**Privacy:** the program does not transfer information to other systems unless the user asks; it only checks GitHub Releases for updates (can be turned off) and optionally connects to OBS Studio on the same computer.
**Code signing policy:** see CODE_SIGNING.md in the repository.
**Contact:** (ваша почта)

## После одобрения
Пришлите мне: Organization ID, Project slug, Signing policy slug, Artifact configuration slug (если есть) и скажите, что токен `SIGNPATH_API_TOKEN` добавлен в GitHub → Settings → Secrets → Actions. Я подключу шаг подписи в `release.yml` (официальное действие `signpath/github-action-submit-signing-request`).
