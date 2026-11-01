# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

(Русская версия ниже.)

## What is signed
- The Windows installer `Bogestv0 MIXER_<version>_x64-setup.exe` and the executables inside it (`Bogestv0 MIXER.exe`, `engine.exe`).
- Only files built from this public repository by GitHub Actions (`.github/workflows/release.yml`) from a release tag `vX.Y.Z`. Local builds are never signed.

## What is not signed by this certificate
- The virtual audio driver (`driver/`). Kernel drivers need a separate Microsoft attestation signature; until then the driver runs in Windows test mode.

## Team roles
| Role | Who |
|---|---|
| Authors (trusted committers) | Bogestv0 (repository owner) |
| Reviewers (review changes from non-committers) | Bogestv0 |
| Approvers (approve every signing request) | Bogestv0 |

Every team member uses multi-factor authentication for GitHub and SignPath.

## Privacy policy
This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it. The only network requests are: update check against GitHub Releases (can be turned off in Settings) and, when the user asks, a connection to OBS Studio on the same computer.

## Open source
MIT license (`LICENSE`), third-party components listed in `THIRD_PARTY_NOTICES.md`, history in `CHANGELOG.md`. The program contains no malware and does not bypass system protection; the driver only creates virtual sound devices.

---

# Политика подписи кода (по-русски)

Бесплатная подпись: SignPath.io, сертификат SignPath Foundation.

**Что подписывается:** установщик Windows и файлы внутри него (`Bogestv0 MIXER.exe`, `engine.exe`), только если они собраны из этого публичного репозитория в GitHub Actions по тегу `vX.Y.Z`. Локальные сборки не подписываются.

**Что не подписывается:** драйвер (`driver/`). Ему нужна отдельная подпись Microsoft (Partner Center, платный сертификат), пока он работает в тестовом режиме Windows.

**Роли:** автор, рецензент и утверждающий подпись — владелец репозитория `Bogestv0`. Двухфакторная защита включена на GitHub и в SignPath.

**Конфиденциальность:** программа ничего не отправляет в сеть, кроме проверки обновлений на GitHub Releases (отключается в настройках) и, по просьбе пользователя, подключения к OBS на этом же компьютере.
