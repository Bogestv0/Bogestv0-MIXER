# Удаление драйвера Bogestv0 Virtual Audio. Запускать от имени администратора. Тестовый режим не трогает.
# Ключ -Auto: так его вызывает деинсталлятор микшера. При обновлении микшера (деинсталлятор запущен с /UPDATE) драйвер не трогается.
param([switch]$Auto)

# Установщик Tauri 32-битный и запускает 32-битный PowerShell: он не видит bcdedit/pnputil из System32 и не может ставить 64-битный драйвер.
# Поэтому на 64-битной Windows скрипт сам перезапускается в 64-битном PowerShell (папка Sysnative) с теми же ключами.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $sn = Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
    if ($Auto) { $a += '-Auto' }
    & $sn @a
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
$hwid = 'Root\Sysvad_ComponentizedAudioSample'

if ($Auto -and ($env:BOGESTV0_UNINST_CMD -match '/UPDATE')) {
    Write-Host 'Это обновление микшера: драйвер оставляю на месте.'
    exit 0
}

# Настоящее удаление микшера (не обновление): заодно убираем его автозапуск с Windows.
if ($Auto) {
    try { Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Bogestv0 MIXER' -ErrorAction SilentlyContinue } catch {}
}

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

# Служебное: задача доустановки после перезагрузки и отметка об установленной версии.
try { Unregister-ScheduledTask -TaskName 'Bogestv0DriverSetup' -Confirm:$false -ErrorAction SilentlyContinue } catch {}
Remove-Item (Join-Path $env:ProgramData 'Bogestv0\driver.sig') -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host 'Готово. Сертификат (package.cer) остаётся в хранилищах; убрать его можно в certmgr.msc (Доверенные корневые, Доверенные издатели).' -ForegroundColor Green
Write-Host "Тестовый режим выключается отдельно: bcdedit /set testsigning off и перезагрузка."
