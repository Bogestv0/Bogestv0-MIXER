; Хуки установщика Bogestv0 MIXER (Tauri NSIS, bundle.windows.nsis.installerHooks).
; Драйвер виртуальных устройств ставится вместе с программой: пользователь ничего не запускает в консоли.
; Файлы драйвера кладёт в установщик сборка релиза (папка src-tauri/driver, см. .github/workflows/release.yml).
; Ошибка драйвера не отменяет установку программы: микшер работает и без него (тогда нет только виртуальных устройств).
; Коды install.ps1: 0 готово · 3010 нужна перезагрузка · 20 тестовый режим не включился (Secure Boot) · иначе ошибка.

!macro NSIS_HOOK_PREINSTALL
!macroend

!macro NSIS_HOOK_POSTINSTALL
  Push $0
  Push $1
  ; Установщик 32-битный: на 64-битной Windows берём 64-битный PowerShell через Sysnative (иначе нет bcdedit и нельзя ставить драйвер).
  StrCpy $1 "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
  IfFileExists "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe" 0 +2
  StrCpy $1 "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  IfFileExists "$INSTDIR\driver\ComponentizedAudioSample.inf" 0 bogestv0_drv_done
  DetailPrint "Bogestv0: installing virtual audio devices (driver)..."
  nsExec::ExecToLog '"$1" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\driver\install.ps1" -Auto'
  Pop $0
  StrCmp $0 "0" bogestv0_drv_done
  IfSilent bogestv0_drv_done
  StrCmp $0 "3010" bogestv0_drv_reboot
  StrCmp $0 "20" bogestv0_drv_secure
  StrCmp $LANGUAGE "1049" 0 bogestv0_drv_err_en
    MessageBox MB_OK|MB_ICONEXCLAMATION "Программа установлена, но драйвер виртуальных устройств поставить не удалось (код $0).$\r$\n$\r$\nПопробуйте запустить «install-driver.cmd» из папки driver в каталоге программы. Журнал: %ProgramData%\Bogestv0\driver-setup.log"
    Goto bogestv0_drv_done
  bogestv0_drv_err_en:
    MessageBox MB_OK|MB_ICONEXCLAMATION "The app is installed, but the virtual audio driver could not be installed (code $0).$\r$\n$\r$\nTry running install-driver.cmd from the driver folder in the app directory. Log: %ProgramData%\Bogestv0\driver-setup.log"
    Goto bogestv0_drv_done
  bogestv0_drv_reboot:
  StrCmp $LANGUAGE "1049" 0 bogestv0_drv_reboot_en
    MessageBox MB_OK|MB_ICONINFORMATION "Для драйвера включён тестовый режим Windows. Перезагрузите компьютер: после перезагрузки драйвер доустановится сам, ничего запускать не нужно."
    Goto bogestv0_drv_done
  bogestv0_drv_reboot_en:
    MessageBox MB_OK|MB_ICONINFORMATION "Windows test mode has been enabled for the driver. Restart the computer: the driver will finish installing by itself, nothing to run."
    Goto bogestv0_drv_done
  bogestv0_drv_secure:
  StrCmp $LANGUAGE "1049" 0 bogestv0_drv_secure_en
    MessageBox MB_OK|MB_ICONEXCLAMATION "Программа установлена, но Windows не дала включить тестовый режим для драйвера (обычно мешает Secure Boot). Выключите Secure Boot в BIOS/UEFI и запустите «install-driver.cmd» из папки driver в каталоге программы."
    Goto bogestv0_drv_done
  bogestv0_drv_secure_en:
    MessageBox MB_OK|MB_ICONEXCLAMATION "The app is installed, but Windows refused to enable test mode for the driver (usually Secure Boot). Turn Secure Boot off in BIOS/UEFI and run install-driver.cmd from the driver folder in the app directory."
  bogestv0_drv_done:
  Pop $1
  Pop $0
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  Push $0
  Push $1
  Push $2
  IfFileExists "$INSTDIR\driver\uninstall.ps1" 0 bogestv0_un_done
  ; При обновлении программы старый деинсталлятор запускается с /UPDATE: скрипт по этой строке не трогает драйвер.
  ; Командная строка содержит кавычки, поэтому в System::Call она передаётся через регистр, а не текстом.
  StrCpy $2 "$CMDLINE"
  StrCpy $1 "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
  IfFileExists "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe" 0 +2
  StrCpy $1 "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  System::Call 'Kernel32::SetEnvironmentVariable(t "BOGESTV0_UNINST_CMD", t r2)i.r0'
  DetailPrint "Bogestv0: removing virtual audio devices (driver)..."
  nsExec::ExecToLog '"$1" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\driver\uninstall.ps1" -Auto'
  Pop $0
  bogestv0_un_done:
  Pop $2
  Pop $1
  Pop $0
!macroend

!macro NSIS_HOOK_POSTUNINSTALL
!macroend
