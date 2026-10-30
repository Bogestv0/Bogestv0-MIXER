@echo off
rem Ручная установка драйвера Bogestv0 (запасной путь: обычно драйвер ставит сам установщик микшера).
chcp 65001 >nul
net session >nul 2>&1
if errorlevel 1 (
  echo Запрашиваю права администратора...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Auto
echo.
echo Код завершения: %errorlevel%  (0 готово, 3010 нужна перезагрузка, 20 не включился тестовый режим)
echo Журнал: %ProgramData%\Bogestv0\driver-setup.log
pause
