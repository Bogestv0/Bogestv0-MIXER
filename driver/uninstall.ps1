# Удаление драйвера Bogestv0 Virtual Audio (этап 6.3). Запускать от имени администратора. Тестовый режим не трогает.
$ErrorActionPreference = 'Stop'
$hwid = 'Root\Sysvad_ComponentizedAudioSample'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'ОШИБКА: запустите PowerShell от имени администратора.' -ForegroundColor Red; exit 1
}

Write-Host '1/2 Удаляю устройство...'
$devs = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid }
if (-not $devs) { Write-Host 'Устройство не найдено, пропускаю.' }
foreach ($d in $devs) { pnputil /remove-device $d.InstanceId }

Write-Host '2/2 Удаляю пакеты драйвера (поставщик Bogestv0)...'
# Get-WindowsDriver не зависит от языка Windows, в отличие от разбора вывода pnputil.
$pkgs = Get-WindowsDriver -Online -All | Where-Object { $_.ProviderName -eq 'Bogestv0' }
if (-not $pkgs) { Write-Host 'Пакетов не найдено.' }
foreach ($p in $pkgs) { Write-Host "  $($p.Driver)"; pnputil /delete-driver $p.Driver /uninstall /force }

Write-Host ''
Write-Host 'Готово. Сертификат (package.cer) остаётся в хранилищах; убрать его можно в certmgr.msc (Доверенные корневые, Доверенные издатели).' -ForegroundColor Green
Write-Host "Тестовый режим выключается отдельно: bcdedit /set testsigning off и перезагрузка."
