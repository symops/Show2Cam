@echo off
rem Removes the Show2Cam device and its driver packages. Run as Administrator.
net session >nul 2>&1 || (echo Запустите этот файл от имени администратора. & pause & exit /b 1)
cd /d "%~dp0"

rem The package root has x64\ and x86\ (64-bit / 32-bit Windows); an installed copy has s2cinstall.exe next to it.
set ARCH=x86
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set ARCH=x64
if /i "%PROCESSOR_ARCHITEW6432%"=="AMD64" set ARCH=x64
if exist s2cinstall.exe (s2cinstall.exe remove) else (%ARCH%\s2cinstall.exe remove)
echo Готово.
pause
