# Установка драйвера Bogestv0 Virtual Audio. Запускать от имени администратора из папки с файлами драйвера.
# Делает то же, что вручную: сертификат -> пакет драйвера -> устройство Root\Sysvad_ComponentizedAudioSample.
# Повторный запуск безопасен: если устройство уже есть, оно не создаётся второй раз, драйвер обновляется.
#
# Обычному пользователю запускать вручную не нужно: установщик микшера вызывает этот скрипт сам (ключ -Auto).
#   -Auto      режим установщика: без вопросов; если тестовый режим выключен, включает его (bcdedit) и просит перезагрузку,
#              а после перезагрузки доустанавливает драйвер сам (задача Windows Bogestv0DriverSetup, живёт до первой удачи).
#              Если драйвер этой же версии уже стоит и работает, ничего не трогает (быстро выходит).
#   -FromTask  служебный: так скрипт запускает задача после перезагрузки.
# Коды выхода: 0 готово · 3010 нужна перезагрузка · 20 тестовый режим не включился (чаще всего Secure Boot) · 21 после перезагрузки
# тестовый режим всё ещё выключен · 1 прочая ошибка. Журнал: %ProgramData%\Bogestv0\driver-setup.log
param([switch]$Auto, [switch]$FromTask)

# Установщик Tauri 32-битный и запускает 32-битный PowerShell: он не видит bcdedit/pnputil из System32 и не может ставить 64-битный драйвер.
# Поэтому на 64-битной Windows скрипт сам перезапускается в 64-битном PowerShell (папка Sysnative) с теми же ключами.
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $sn = Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
    if ($Auto) { $a += '-Auto' }
    if ($FromTask) { $a += '-FromTask' }
    & $sn @a
    exit $LASTEXITCODE
}
$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$inf   = Join-Path $here 'ComponentizedAudioSample.inf'
$cer   = Join-Path $here 'package.cer'
$hwid  = 'Root\Sysvad_ComponentizedAudioSample'
$taskName = 'Bogestv0DriverSetup'

$dataDir = Join-Path $env:ProgramData 'Bogestv0'
try { New-Item -ItemType Directory -Force $dataDir | Out-Null; Start-Transcript -Path (Join-Path $dataDir 'driver-setup.log') -Append | Out-Null } catch {}

function Done($code) { try { Stop-Transcript | Out-Null } catch {}; exit $code }
function Stop-With($msg, $code = 1) { Write-Host "ОШИБКА: $msg" -ForegroundColor Red; Done $code }
function Remove-SetupTask { try { Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue } catch {} }
function Get-OurDevice { Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid } }

Write-Host ("[{0}] install.ps1 Auto={1} FromTask={2}" -f (Get-Date -Format s), $Auto, $FromTask)

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Stop-With 'запустите PowerShell от имени администратора.'
}
if (-not (Test-Path $inf)) { Stop-With "не найден $inf. Запускайте скрипт из папки с распакованным драйвером." }
if (-not (Test-Path $cer)) { Stop-With "не найден $cer." }

# Подпись набора файлов: если она совпала с последней удачной установкой и устройство работает, драйвер не трогаем.
# Так обновление микшера не переставляет драйвер (а с ним и устройства «по умолчанию» в Windows) без нужды.
$sigFile = Join-Path $dataDir 'driver.sig'
$sig = (Get-ChildItem $here -File | Where-Object { $_.Extension -in '.sys', '.inf', '.cat', '.cer', '.dll' } | Sort-Object Name |
        ForEach-Object { $_.Name + ':' + (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ';'
if ($Auto -and (Test-Path $sigFile) -and ((Get-Content $sigFile -Raw).Trim() -eq $sig)) {
    $d = Get-OurDevice
    if ($d -and ($d | Where-Object { $_.Status -eq 'OK' })) {
        Write-Host 'Драйвер этой версии уже установлен и работает. Ничего не делаю.'
        Remove-SetupTask
        Done 0
    }
}

# 1. Тестовый режим (без него Windows не примет тестовую подпись)
$bcd = (bcdedit /enum '{current}') -join "`n"
if ($bcd -notmatch '(?im)^\s*testsigning\s+Yes') {
    if (-not $Auto) {
        Stop-With "тестовый режим выключен. Выполните 'bcdedit /set testsigning on', перезагрузитесь и запустите скрипт снова (Secure Boot должен быть выключен)."
    }
    if ($FromTask) {
        Remove-SetupTask
        Stop-With 'после перезагрузки тестовый режим всё ещё выключен. Выключите Secure Boot в BIOS/UEFI и запустите `install-driver.cmd` из папки driver в каталоге микшера.' 21
    }
    Write-Host 'Тестовый режим выключен, включаю: bcdedit /set testsigning on'
    $out = cmd /c 'bcdedit /set testsigning on 2>&1'
    Write-Host ($out -join "`n")
    if ($LASTEXITCODE -ne 0) {
        Stop-With 'Windows не дала включить тестовый режим. Обычно это Secure Boot: выключите его в BIOS/UEFI и запустите `install-driver.cmd` из папки driver в каталоге микшера.' 20
    }
    # После перезагрузки драйвер доустановит задача Windows (от имени системы), от пользователя ничего не требуется.
    $ps  = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $act = New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Auto -FromTask' -f $PSCommandPath)
    $trg = New-ScheduledTaskTrigger -AtStartup
    $pri = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $taskName -Action $act -Trigger $trg -Principal $pri -Force | Out-Null
    Write-Host 'Тестовый режим включён. После перезагрузки драйвер установится сам.'
    Done 3010
}

# 1b. Устройство уже есть, но драйвер другой версии (иначе мы бы вышли выше): сначала убираем старое. Так новые устройства появляются
# и получают новые имена (Windows запоминает имена конечных точек). Платой за это Windows может снова назначить наши устройства по умолчанию.
if ($Auto -and (Get-OurDevice)) {
    $un = Join-Path $here 'uninstall.ps1'
    if (Test-Path $un) {
        Write-Host 'Стоит драйвер другой версии: удаляю его перед установкой новой (чистая переустановка).'
        $env:BOGESTV0_UNINST_CMD = ''
        $psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $p = Start-Process -FilePath $psExe -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Auto' -f $un) -Wait -PassThru -WindowStyle Hidden
        Write-Host ('Удаление старого драйвера завершено, код {0}.' -f $p.ExitCode)
        Start-Sleep -Seconds 2
    }
}

# 2. Сертификат в оба хранилища
Write-Host '1/3 Сертификат...'
certutil -addstore Root $cer | Out-Null
certutil -addstore TrustedPublisher $cer | Out-Null

# 3. Пакет драйвера в хранилище драйверов
Write-Host '2/3 Пакет драйвера...'
pnputil /add-driver $inf /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { Stop-With "pnputil вернул код $LASTEXITCODE." }

# 4. Устройство
Write-Host '3/3 Устройство...'
$existing = Get-OurDevice

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BogestvSetup {
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
    [DllImport("setupapi.dll", SetLastError=true)] public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid g, IntPtr h);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool SetupDiCreateDeviceInfoW(IntPtr s, string name, ref Guid g, string desc, IntPtr h, int flags, ref SP_DEVINFO_DATA d);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr s, ref SP_DEVINFO_DATA d, int prop, byte[] buf, int size);
    [DllImport("setupapi.dll", SetLastError=true)] public static extern bool SetupDiCallClassInstaller(int fn, IntPtr s, ref SP_DEVINFO_DATA d);
    [DllImport("setupapi.dll", SetLastError=true)] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
    [DllImport("newdev.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr w, string hwid, string inf, int flags, out bool reboot);

    public static string Install(string hwid, string inf, bool create, out bool reboot) {
        reboot = false;
        Guid cls = new Guid("4d36e96c-e325-11ce-bfc1-08002be10318"); // MEDIA
        if (create) {
            IntPtr set = SetupDiCreateDeviceInfoList(ref cls, IntPtr.Zero);
            if (set == new IntPtr(-1)) return "SetupDiCreateDeviceInfoList: " + Marshal.GetLastWin32Error();
            SP_DEVINFO_DATA d = new SP_DEVINFO_DATA(); d.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            if (!SetupDiCreateDeviceInfoW(set, "MEDIA", ref cls, null, IntPtr.Zero, 1, ref d)) return "SetupDiCreateDeviceInfo: " + Marshal.GetLastWin32Error();
            byte[] id = System.Text.Encoding.Unicode.GetBytes(hwid + "\0\0");
            if (!SetupDiSetDeviceRegistryPropertyW(set, ref d, 1, id, id.Length)) return "SetHardwareId: " + Marshal.GetLastWin32Error();
            if (!SetupDiCallClassInstaller(0x19, set, ref d)) return "DIF_REGISTERDEVICE: " + Marshal.GetLastWin32Error();
            SetupDiDestroyDeviceInfoList(set);
        }
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hwid, inf, 1, out reboot)) return "UpdateDriverForPlugAndPlayDevices: " + Marshal.GetLastWin32Error();
        return null;
    }
}
'@

if ($existing) { Write-Host 'Устройство уже есть, обновляю драйвер.' }
$reboot = $false
$err = [BogestvSetup]::Install($hwid, $inf, (-not $existing), [ref]$reboot)
if ($err) { Stop-With "$err (код Windows). Пришлите эту строку." }

Set-Content -Path $sigFile -Value $sig -Encoding ASCII
Remove-SetupTask

Write-Host ''
Write-Host 'Готово. Проверьте: Диспетчер устройств -> «Звуковые, игровые и видеоустройства» -> «Bogestv0 Virtual Audio».' -ForegroundColor Green
Write-Host 'ВАЖНО: Windows могла назначить наши устройства по умолчанию. Верните свои колонки и микрофон в Параметры -> Звук.' -ForegroundColor Yellow
if ($reboot) { Write-Host 'Windows просит перезагрузку.' -ForegroundColor Yellow; Done 3010 }
Done 0
