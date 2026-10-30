# Установка драйвера Bogestv0 Virtual Audio (этап 6.3). Запускать от имени администратора из папки с файлами драйвера.
# Делает то же, что вручную: сертификат -> пакет драйвера -> устройство Root\Sysvad_ComponentizedAudioSample.
# Повторный запуск безопасен: если устройство уже есть, оно не создаётся второй раз, драйвер обновляется.
$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$inf   = Join-Path $here 'ComponentizedAudioSample.inf'
$cer   = Join-Path $here 'package.cer'
$hwid  = 'Root\Sysvad_ComponentizedAudioSample'

function Stop-With($msg) { Write-Host "ОШИБКА: $msg" -ForegroundColor Red; exit 1 }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Stop-With 'запустите PowerShell от имени администратора.'
}
if (-not (Test-Path $inf)) { Stop-With "не найден $inf. Запускайте скрипт из папки с распакованным драйвером." }
if (-not (Test-Path $cer)) { Stop-With "не найден $cer." }

# 1. Тестовый режим (без него Windows не примет тестовую подпись)
$bcd = (bcdedit /enum '{current}') -join "`n"
if ($bcd -notmatch '(?im)^\s*testsigning\s+Yes') {
    Stop-With "тестовый режим выключен. Выполните 'bcdedit /set testsigning on', перезагрузитесь и запустите скрипт снова (Secure Boot должен быть выключен)."
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
$existing = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue |
    Where-Object { $_.HardwareID -contains $hwid }

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

Write-Host ''
Write-Host 'Готово. Проверьте: Диспетчер устройств -> «Звуковые, игровые и видеоустройства» -> «Bogestv0 Virtual Audio».' -ForegroundColor Green
Write-Host 'ВАЖНО: Windows могла назначить наши устройства по умолчанию. Верните свои колонки и микрофон в Параметры -> Звук.' -ForegroundColor Yellow
if ($reboot) { Write-Host 'Windows просит перезагрузку.' -ForegroundColor Yellow }
