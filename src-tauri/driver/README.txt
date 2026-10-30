Сюда сборка релиза (.github/workflows/release.yml) кладёт готовый драйвер: .sys, .inf, .cat, package.cer, install.ps1, uninstall.ps1,
install-driver.cmd, uninstall-driver.cmd. Содержимое папки попадает в установщик (папка driver рядом с программой), а установщик сам
запускает install.ps1. В репозитории здесь только этот файл-заглушка: без него `tauri build` на своей машине не найдёт ресурс.
