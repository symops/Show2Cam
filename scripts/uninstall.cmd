@echo off
rem Removes the Show2Cam device and its driver packages. Run as Administrator.
net session >nul 2>&1 || (echo Запустите этот файл от имени администратора. & pause & exit /b 1)
cd /d "%~dp0"

rem The installer removes without a window ("/remove"): the package root has the launcher, x64\ / x86\ and an
rem installed copy have the installer itself. ("start /wait": cmd does not wait for a program with a window.)
start "" /wait "Show2Cam-Setup.exe" /remove
if errorlevel 2 (echo Перезагрузите компьютер, чтобы закончить удаление.) else if errorlevel 1 (echo Удаление не удалось — см. %ProgramData%\Show2Cam\logs\setup.log)
echo Готово.
pause
