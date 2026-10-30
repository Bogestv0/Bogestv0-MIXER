@echo off
rem Ручное удаление драйвера Bogestv0 (удаляет устройства и пакеты драйвера; тестовый режим не трогает).
chcp 65001 >nul
net session >nul 2>&1
if errorlevel 1 (
  echo Запрашиваю права администратора...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" -Auto
echo.
pause
